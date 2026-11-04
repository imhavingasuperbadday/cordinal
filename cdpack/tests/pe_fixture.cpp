// Native PE/TLS/DLL fixture: imports, DIR64 pointers, static TLS and thread callbacks.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
struct Template {
    unsigned long long value;
    const void *pointer;
};
extern Template template_value;
__attribute__((section(".tls"))) Template template_value = {0x31504b4344544c53ull, &template_value};
DWORD fixture_tls_index;
volatile LONG process_attach, thread_attach, thread_detach;
const void *volatile relocated_pointer = &template_value;

unsigned long long *tls_value() {
    void **slots;
    __asm__("movq %%gs:0x58,%0" : "=r"(slots));
    return static_cast<unsigned long long *>(slots[fixture_tls_index]);
}
void NTAPI fixture_tls(void *, DWORD reason, void *) {
    if (reason == DLL_PROCESS_ATTACH)
        InterlockedIncrement(&process_attach);
    if (reason == DLL_THREAD_ATTACH)
        InterlockedIncrement(&thread_attach);
    if (reason == DLL_THREAD_DETACH)
        InterlockedIncrement(&thread_detach);
}
const PIMAGE_TLS_CALLBACK fixture_callbacks[] = {fixture_tls, nullptr};
__attribute__((used)) extern const IMAGE_TLS_DIRECTORY64 _tls_used = {
    reinterpret_cast<ULONGLONG>(&template_value),
    reinterpret_cast<ULONGLONG>(&template_value) + sizeof(template_value),
    reinterpret_cast<ULONGLONG>(&fixture_tls_index),
    reinterpret_cast<ULONGLONG>(fixture_callbacks),
    0,
    0x400000};

DWORD WINAPI fixture_thread(void *) {
    if (*tls_value() != 0x31504b4344544c53ull)
        return 1;
    *tls_value() = 17;
    return 0;
}
__declspec(dllexport) unsigned fixture_probe() {
    if (process_attach != 1)
        return 100 + unsigned(process_attach);
    if (relocated_pointer != &template_value)
        return 12;
    if (*tls_value() != 0x31504b4344544c53ull)
        return 13;
    if (tls_value()[1] != reinterpret_cast<ULONG_PTR>(&template_value))
        return 14;
    HANDLE thread = CreateThread(nullptr, 0, fixture_thread, nullptr, 0, nullptr);
    if (!thread || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
        return 2;
    DWORD exit = 99;
    if (!GetExitCodeThread(thread, &exit))
        return 3;
    CloseHandle(thread);
    if (exit || thread_attach != 1 || thread_detach != 1 || *tls_value() != 0x31504b4344544c53ull)
        return 4;
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(reinterpret_cast<const void *>(fixture_probe), &page, sizeof(page)) ||
        (page.Protect & 0xff) != PAGE_EXECUTE_READ)
        return 5;
    return 0;
}
#ifdef CDP_TEST_DLL
BOOL WINAPI fixture_entry(HINSTANCE, DWORD, void *) { return TRUE; }
#else
void fixture_entry() { ExitProcess(fixture_probe()); }
#endif
}
