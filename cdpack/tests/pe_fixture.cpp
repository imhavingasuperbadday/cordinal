// Native PE/TLS/DLL fixture: imports, delay imports, DIR64 pointers, static TLS, thread callbacks and
// zero pages.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <delayimp.h>
#include <psapi.h>

extern "C" {
// The fixture links without a C runtime, so it brings the delay-load helper the linker's thunks call
// (tools/build.ps1 delay-loads version.dll). It reads only the descriptor and the image's own RVAs,
// as the toolchains' helpers do; their IAT slots start as relocated pointers to those thunks.
extern IMAGE_DOS_HEADER __ImageBase;
FARPROC WINAPI __delayLoadHelper2(PCImgDelayDescr descriptor, FARPROC *slot) {
    char *base = reinterpret_cast<char *>(&__ImageBase);
    HMODULE *module = reinterpret_cast<HMODULE *>(base + descriptor->rvaHmod);
    if (!*module)
        *module = LoadLibraryExA(base + descriptor->rvaDLLName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const FARPROC *iat = reinterpret_cast<FARPROC *>(base + descriptor->rvaIAT);
    const ULONGLONG name = reinterpret_cast<ULONGLONG *>(base + descriptor->rvaINT)[slot - iat];
    FARPROC function = *module ? GetProcAddress(*module, base + name + 2) : nullptr;
    if (!function)
        ExitProcess(16);
    return *slot = function;
}

struct Template {
    unsigned long long value;
    const void *pointer;
};
extern Template template_value;
__attribute__((section(".tls"))) Template template_value = {0x31504b4344544c53ull, &template_value};
DWORD fixture_tls_index;
volatile LONG process_attach, thread_attach, thread_detach;
const void *volatile relocated_pointer = &template_value;
// One DIR64 fixup inside the code section, so a test that marks that page cold also proves the
// loader applies a cold chunk's relocations before the page becomes executable.
__attribute__((section(".text"), used)) const void *const fixture_text_pointer = &template_value;
// Zero-initialized and longer than pack.py's ZERO_GAP_PAGES: a packed image leaves these pages
// out of its chunks, so like a native one it must start with them demand-zero (not resident), yet
// readable as zero and writable.
volatile unsigned char fixture_zero[40 * 4096];

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
    if (fixture_text_pointer != &template_value)
        return 15;
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
    // The array's first and last pages may hold other variables; the ones between hold only it.
    const uintptr_t interior = (reinterpret_cast<uintptr_t>(fixture_zero) + 4095) & ~uintptr_t(4095);
    PSAPI_WORKING_SET_EX_INFORMATION pages[32];  // no initializer: this fixture has no memset
    for (unsigned i = 0; i < 32; ++i) {
        pages[i].VirtualAddress = reinterpret_cast<void *>(interior + i * 4096);
        pages[i].VirtualAttributes.Flags = 0;
    }
    if (!K32QueryWorkingSetEx(GetCurrentProcess(), pages, sizeof(pages)))
        return 6;
    for (const auto &item : pages)
        if (item.VirtualAttributes.Valid)
            return 7;
    for (unsigned i = 0; i < sizeof(fixture_zero); ++i)
        if (fixture_zero[i])
            return 8;
    fixture_zero[sizeof(fixture_zero) / 2] = 17;
    if (fixture_zero[sizeof(fixture_zero) / 2] != 17)
        return 9;
    // A delay-loaded DLL maps on the first call through its thunk, not when the image loads.
    if (GetModuleHandleA("version.dll"))
        return 17;
    if (!GetFileVersionInfoSizeA("kernel32.dll", nullptr) || !GetModuleHandleA("version.dll"))
        return 18;
    return 0;
}
#ifdef CDP_TEST_DLL
BOOL WINAPI fixture_entry(HINSTANCE, DWORD, void *) { return TRUE; }
#else
void fixture_entry() { ExitProcess(fixture_probe()); }
#endif
}
