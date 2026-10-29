// Crash dumps: when the process dies of an unhandled exception, a CRT invalid parameter, a pure
// virtual call or std::terminate, a minidump is written to the crash folder. The dump is written
// by a helper thread, so a stack overflow can still produce one. The newest few are kept.
#pragma once
#include <string>

struct _EXCEPTION_POINTERS;

// Installs the handlers. Dumps go to dumpDir (created if needed); only the newest 5 are kept.
// Call once early, before the UI is built, so a startup crash is caught; repeat calls do nothing.
void InstallCrashHandler(const std::wstring& dumpDir);

// True once for each crash dump written since the last call, so the UI can say "closed
// unexpectedly last time" a single time. `path` receives the dump's full path. Call it at startup.
bool TakeNewCrashDump(std::wstring& path);

// Writes a minidump of this process into dumpDir on the calling thread, then prunes the folder.
// With ep null the dump has no exception record. Used by the self-test.
bool WriteCrashDumpNow(const std::wstring& dumpDir, _EXCEPTION_POINTERS* ep, std::wstring* path);
