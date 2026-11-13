// Loads the delay-loaded system DLLs (the /DELAYLOAD list in tools/build.ps1) from System32 only.
// Those DLLs serve features that may never run in a session (network lookups, the web player, the
// iPod, headset controls), so the player maps them on first use instead of at startup; each one
// mapped costs private pages and heap whether it is used or not. The linker's own helper would look
// for them with the ordinary search order, which starts in the program's folder, so this hook
// names System32 instead.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <delayimp.h>

namespace {

FARPROC WINAPI DelayLoadHook(unsigned notify, PDelayLoadInfo info) {
    if (notify != dliNotePreLoadLibrary) return nullptr;
    return (FARPROC)LoadLibraryExA(info->szDll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

}  // namespace

extern "C" const PfnDliHook __pfnDliNotifyHook2 = DelayLoadHook;
