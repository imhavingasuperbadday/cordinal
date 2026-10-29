// Crash dump writer behind crashdump.h: installs the unhandled-exception, CRT and terminate
// handlers, and writes a minidump into the crash folder when one fires. The dump is written by a
// dedicated helper thread while the faulting thread waits, so even a stack-overflow crash can still
// be captured. Only the newest dumps are kept.
#include "sort.h"
#include "crashdump.h"

#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>

#include <stdlib.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

namespace {

constexpr size_t kKeepDumps = 5;
constexpr DWORD kCrashCode = 0xE0C0D000;  // exception code for CRT and terminate crashes
constexpr DWORD kDumpWaitMs = 20000;     // the faulting thread waits this long for the dump
const wchar_t kMarkerName[] = L"seen.txt";

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// dbghelp is loaded by name rather than linked: the exe stays free of its import library, and
// the crash path never depends on a search order that could pick up another dbghelp.dll.
MiniDumpWriteDumpFn Writer() {
    static MiniDumpWriteDumpFn fn = [] {
        HMODULE lib = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return lib ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(lib, "MiniDumpWriteDump")) : nullptr;
    }();
    return fn;
}

struct CrashState {
    std::wstring dir;
    HANDLE request = nullptr;  // signalled by a crashing thread; the helper thread writes the dump
    HANDLE done = nullptr;     // signalled when the dump is written (or given up on)
    EXCEPTION_POINTERS* ep = nullptr;
    DWORD faultingThread = 0;
    LONG busy = 0;             // the first crash writes the dump; any later one just ends the process
};
CrashState g;  // process-wide; filled in by InstallCrashHandler before any handler can run

bool IsDumpName(const std::wstring& name) {
    return name.size() > 13 && name.compare(0, 9, L"cordinal-") == 0 && name.compare(name.size() - 4, 4, L".dmp") == 0;
}

// Newest dumps first, judged by write time.
std::vector<std::pair<ULONGLONG, std::wstring>> ListDumps(const std::wstring& dir) {
    std::vector<std::pair<ULONGLONG, std::wstring>> dumps;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\cordinal-*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return dumps;
    do {
        if (IsDumpName(fd.cFileName) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            ULARGE_INTEGER t{};
            t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            dumps.emplace_back(t.QuadPart, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    SortObjects(dumps.begin(), dumps.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    return dumps;
}

void Prune(const std::wstring& dir) {
    auto dumps = ListDumps(dir);
    for (size_t i = kKeepDumps; i < dumps.size(); ++i) DeleteFileW((dir + L"\\" + dumps[i].second).c_str());
}

bool WriteAndPrune(const std::wstring& dir, EXCEPTION_POINTERS* ep, DWORD threadId, std::wstring* path) {
    MiniDumpWriteDumpFn write = Writer();
    if (!write) return false;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    SYSTEMTIME st;
    GetSystemTime(&st);  // UTC, as the name says
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    // Two dumps in the same second (a self-test, or a second crash) get a counter.
    std::wstring file;
    for (int n = 1; n < 100; ++n) {
        file = dir + L"\\cordinal-" + stamp + (n > 1 ? L"-" + std::to_wstring(n) : std::wstring()) + L".dmp";
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = threadId;
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    const auto type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
    BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), h, type, ep ? &mei : nullptr, nullptr, nullptr);
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(file.c_str());
        return false;
    }
    Prune(dir);
    if (path) *path = file;
    return true;
}

// Runs on the helper thread: the crashed thread is parked in RequestDump, so its state is intact.
DWORD WINAPI CrashWriterThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g.request, INFINITE);
        WriteAndPrune(g.dir, g.ep, g.faultingThread, nullptr);
        SetEvent(g.done);
    }
}

// Faulting thread's side of the handshake: it publishes its exception state and parks until the
// helper thread has written the dump. Only the first crash does this; a later one returns at once so
// the process can reach TerminateProcess.
void RequestDump(EXCEPTION_POINTERS* ep) {
    if (!g.request || InterlockedExchange(&g.busy, 1)) return;
    g.ep = ep;
    g.faultingThread = GetCurrentThreadId();
    SetEvent(g.request);
    WaitForSingleObject(g.done, kDumpWaitMs);
}

// The CRT and terminate handlers get no EXCEPTION_POINTERS, so one is made from the caller's context.
[[noreturn]] void CrashNow(DWORD code) {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = code;
    rec.ExceptionAddress = reinterpret_cast<PVOID>(&CrashNow);
    EXCEPTION_POINTERS ep{&rec, &ctx};
    RequestDump(&ep);
    // The state is no longer trustworthy, so end the process here instead of returning to it.
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(INFINITE);
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    RequestDump(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

void InvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    CrashNow(kCrashCode + 1);
}

void PureCall() {
    CrashNow(kCrashCode + 2);
}

void Terminate() {
    CrashNow(kCrashCode + 3);
}

}  // namespace

void InstallCrashHandler(const std::wstring& dumpDir) {
    static bool installed = false;
    if (installed) return;
    installed = true;
    g.dir = dumpDir;
    SHCreateDirectoryExW(nullptr, dumpDir.c_str(), nullptr);
    Writer();  // load dbghelp now, not while the process is dying
    g.request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE thread = g.request && g.done ? CreateThread(nullptr, 0, CrashWriterThread, nullptr, 0, nullptr) : nullptr;
    if (thread) CloseHandle(thread);
    SetUnhandledExceptionFilter(UnhandledFilter);
    _set_invalid_parameter_handler(InvalidParameter);
    _set_purecall_handler(PureCall);
    std::set_terminate(Terminate);
}

bool TakeNewCrashDump(std::wstring& path) {
    if (g.dir.empty()) return false;
    auto dumps = ListDumps(g.dir);
    if (dumps.empty()) return false;
    const std::wstring& newest = dumps[0].second;

    std::wstring markerPath = g.dir + L"\\" + kMarkerName;
    std::string seen;
    HANDLE h = CreateFileW(markerPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char buf[256];
        DWORD got = 0;
        if (ReadFile(h, buf, sizeof(buf), &got, nullptr)) seen.assign(buf, got);
        CloseHandle(h);
    }
    std::string name;  // dump names are ASCII
    for (wchar_t c : newest) name += (char)c;
    if (seen == name) return false;

    h = CreateFileW(markerPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, name.data(), (DWORD)name.size(), &written, nullptr);
        CloseHandle(h);
    }
    path = g.dir + L"\\" + newest;
    return true;
}

bool WriteCrashDumpNow(const std::wstring& dumpDir, EXCEPTION_POINTERS* ep, std::wstring* path) {
    return WriteAndPrune(dumpDir, ep, GetCurrentThreadId(), path);
}
