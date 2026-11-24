// Native PE/TLS/DLL fixture: imports, delay imports, DIR64 pointers, static TLS, thread callbacks,
// zero pages, and the cold-code hook: a detached image's module identity, decommit and decode
// again, a thread blocked inside an evicted page, and the cap on resident cold chunks.
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

// cdpack/src/format.h Hook and ColdStatus; the loader fills the hook in, a native run leaves it.
struct ColdStatus {
    unsigned flags, cold_chunks, resident, cap, decodes, evictions, suspended, pad;
    unsigned long long resident_bytes;
    void *stub, *native;
};
struct Hook {
    unsigned char magic[16];
    unsigned version, bytes;
    unsigned (*evict)(const void *, unsigned);
    unsigned (*decodes)(const void *);
    void (*status)(ColdStatus *);
};
__attribute__((aligned(8), used)) volatile Hook fixture_hook = {
    {'C', 'D', 'P', 'H', 'O', 'O', 'K', '1', 0x5a, 0xc3, 0x17, 0x9e, 0x42, 0x08, 0xd1, 0x6b}, 0, 0, 0, 0, 0};

// PROCESS_MEMORY_COUNTERS_EX2, which these headers may not have.
struct Counters {
    DWORD cb, faults;
    SIZE_T peak_working_set, working_set, quota[4], pagefile, peak_pagefile, private_usage,
        private_working_set, shared_commit;
};

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

// Each starts a page of its own in a section the linker places after all other code, in name
// order, so verify.py can make each page a cold chunk of its own and the probe can evict one while
// its own code stays put. The import thunks follow fixture_fence.
__declspec(dllexport) unsigned fixture_far(unsigned);
__declspec(dllexport) unsigned fixture_blocker(HANDLE *events);
__declspec(dllexport) void fixture_fence();

// A new thread's static TLS starts as the template, its pointer relocated into the image.
DWORD WINAPI fixture_thread(void *) {
    if (*tls_value() != 0x31504b4344544c53ull || tls_value()[1] != reinterpret_cast<ULONG_PTR>(&template_value))
        return 1;
    *tls_value() = 17;
    return 0;
}
DWORD WINAPI blocker_thread(void *events) { return fixture_blocker(static_cast<HANDLE *>(events)); }

bool mode(const char *wanted) {
    char value[16];
    const DWORD length = GetEnvironmentVariableA("CDP_FIXTURE", value, sizeof(value));
    unsigned i = 0;
    while (i < length && wanted[i] && value[i] == wanted[i])
        ++i;
    return length && i == length && !wanted[i];
}
DWORD page_state(const void *address, DWORD *protect = nullptr) {
    MEMORY_BASIC_INFORMATION page;
    if (!VirtualQuery(address, &page, sizeof(page)))
        return 0;
    if (protect)
        *protect = page.Protect & 0xff;
    return page.State;
}
bool resident(const void *address) {
    PSAPI_WORKING_SET_EX_INFORMATION page;
    page.VirtualAddress = const_cast<void *>(address);
    page.VirtualAttributes.Flags = 0;
    return K32QueryWorkingSetEx(GetCurrentProcess(), &page, sizeof(page)) && page.VirtualAttributes.Valid;
}
bool counters(Counters &out) {
    out.cb = sizeof(out);
    return K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&out),
                                   sizeof(out));
}

// A detached image is private memory that Windows has been told is the program's module.
unsigned detached_checks() {
    char *self = reinterpret_cast<char *>(&__ImageBase);
    MEMORY_BASIC_INFORMATION page;
    if (!VirtualQuery(reinterpret_cast<const void *>(detached_checks), &page, sizeof(page)) ||
        page.Type != MEM_PRIVATE)
        return 30;
    if (GetModuleHandleW(nullptr) != reinterpret_cast<HMODULE>(self))
        return 31;
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(reinterpret_cast<HMODULE>(self), path, MAX_PATH) < 4 ||
        GetModuleFileNameW(nullptr, path, MAX_PATH) < 4)
        return 32;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&fixture_tls_index), &module) ||
        module != reinterpret_cast<HMODULE>(self))
        return 33;
    void *base = nullptr;
    if (RtlPcToFileHeader(reinterpret_cast<void *>(detached_checks), &base) != self || base != self)
        return 34;
    ColdStatus status;
    fixture_hook.status(&status);
    if (status.native != self || status.stub == self)
        return 35;
    return 0;
}

// Pages of fixture_far, fixture_blocker and fixture_fence are each a cold chunk.
unsigned eviction_checks() {
    const void *away = reinterpret_cast<const void *>(fixture_far);
    const void *blocker = reinterpret_cast<const void *>(fixture_blocker);
    DWORD protect = 0;
    // A cold chunk nothing has touched is reserved, not committed, and not resident.
    const void *untouched[2] = {away, blocker};
    for (const void *page : untouched)
        if (page_state(page) != MEM_RESERVE || resident(page) || fixture_hook.decodes(page) != 0)
            return 40;
    // Its first call decodes it: committed, execute-read, resident.
    unsigned (*volatile call)(unsigned) = fixture_far;
    if (call(4) != 13)
        return 42;
    if (fixture_hook.decodes(away) != 1 || page_state(away, &protect) != MEM_COMMIT ||
        protect != PAGE_EXECUTE_READ || !resident(away))
        return 41;
    // Evicting it decommits it: reserved again, gone from the working set and from the commit.
    Counters before, after;
    // Idle eviction keeps it: its page is in the working set, so it ran since the last trim.
    if (fixture_hook.evict(away, 1) != 0 || page_state(away) != MEM_COMMIT)
        return 52;
    if (!counters(before) || fixture_hook.evict(away, 0) != 1 || !counters(after))
        return 43;
    if (page_state(away) != MEM_RESERVE || resident(away))
        return 44;
    if (before.private_usage - after.private_usage < 4096 ||
        before.private_working_set - after.private_working_set < 4096)
        return 45;
    // The next call faults it back, decoded a second time with its fixups, execute-read.
    if (call(5) != 16 || fixture_hook.decodes(away) != 2 || page_state(away, &protect) != MEM_COMMIT ||
        protect != PAGE_EXECUTE_READ || fixture_text_pointer != &template_value)
        return 46;
    // A thread blocked inside a cold function returns through its evicted page, which faults back.
    HANDLE events[2] = {CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE thread = CreateThread(nullptr, 0, blocker_thread, events, 0, nullptr);
    if (!events[0] || !events[1] || !thread || WaitForSingleObject(events[0], 5000) != WAIT_OBJECT_0)
        return 47;
    ColdStatus status;
    if (fixture_hook.decodes(blocker) != 1 || fixture_hook.evict(blocker, 0) != 1 || page_state(blocker) != MEM_RESERVE)
        return 48;
    // The eviction suspended the blocked thread (and the loader's own, if any are left).
    fixture_hook.status(&status);
    if (status.suspended < 1 || status.evictions != 2)
        return 49;
    SetEvent(events[1]);
    DWORD exit = 0;
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &exit) || exit != 42 ||
        fixture_hook.decodes(blocker) != 2 || page_state(blocker) != MEM_COMMIT)
        return 50;
    CloseHandle(thread);
    CloseHandle(events[0]);
    CloseHandle(events[1]);
    // Evicting everything idle: both, and the fence's page if its import thunks ran.
    if (fixture_hook.evict(nullptr, 0) < 2 || page_state(away) != MEM_RESERVE || page_state(blocker) != MEM_RESERVE)
        return 51;
    return 0;
}

// With a cap of two resident cold chunks, decoding a third decommits the one decoded longest ago.
unsigned cap_checks() {
    const void *away = reinterpret_cast<const void *>(fixture_far);
    const void *fence = reinterpret_cast<const void *>(fixture_fence);
    ColdStatus status;
    unsigned (*volatile call)(unsigned) = fixture_far;
    if (call(1) != 4)
        return 59;
    void (*volatile touch)() = fixture_fence;  // a direct call to an empty function is dropped
    touch();
    fixture_hook.status(&status);
    if (status.cap != 2 || status.resident != 2 || page_state(away) != MEM_COMMIT || page_state(fence) != MEM_COMMIT)
        return 60;
    if (fixture_blocker(nullptr) != 7)
        return 61;
    fixture_hook.status(&status);
    if (status.resident != 2 || status.evictions != 1 || page_state(away) != MEM_RESERVE)
        return 62;
    if (call(1) != 4 || fixture_hook.decodes(away) != 2 || page_state(fence) != MEM_RESERVE)
        return 63;
    fixture_hook.status(&status);
    if (status.resident != 2 || status.evictions != 2)
        return 64;
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
    if (mode("cap"))
        return cap_checks();  // touches nothing else cold: the order of decodes is the test
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
    const uintptr_t far_page = reinterpret_cast<uintptr_t>(fixture_far) & ~uintptr_t(4095);
    if (far_page == (reinterpret_cast<uintptr_t>(fixture_probe) & ~uintptr_t(4095)))
        return 19;
    if (fixture_hook.status) {
        ColdStatus status;
        fixture_hook.status(&status);
        if (status.flags & 1) {
            if (const unsigned failed = detached_checks())
                return failed;
        }
    } else if (mode("evict")) {
        return 20;  // the packed run must have filled the hook in
    }
    if (mode("evict"))
        return eviction_checks();
    return 0;
}
#ifdef CDP_TEST_DLL
BOOL WINAPI fixture_entry(HINSTANCE, DWORD, void *) { return TRUE; }
#else
void fixture_entry() { ExitProcess(fixture_probe()); }
#endif
__attribute__((noinline, aligned(4096), section(".text$zc1"))) unsigned fixture_far(unsigned x) { return x * 3 + 1; }
// Signals the first event, waits for the second, returns 42; with no events it returns 7 at once.
__attribute__((noinline, aligned(4096), section(".text$zc2"))) unsigned fixture_blocker(HANDLE *events) {
    if (!events)
        return 7;
    SetEvent(events[0]);
    WaitForSingleObject(events[1], INFINITE);
    return 42;
}
// Ends fixture_blocker's page: the import thunks would otherwise land beside it.
__attribute__((noinline, used, aligned(4096), section(".text$zc3"))) void fixture_fence() {}
}
