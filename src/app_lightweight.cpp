// Lightweight mode (View > Lightweight mode, Ctrl+Shift+L, or the notification-area icon): cordinal
// keeps playing, effects and all, with its window put away, so it can run in the background on
// little memory and CPU. What stays on screen is a preference: nothing but the notification-area
// icon, the mini player, or the compact window. Going in releases the UI's caches (the reduced
// memory policy of app_memory.cpp), pauses background library work, slows the UI timer while
// nothing is shown and, a moment later, gives unused memory back to Windows (app_memory.cpp, which
// keeps doing so in every mode). Library records,
// playlists, the queue and the engine stay exactly as they were, so playback never notices.
//
// While it is on, the numpad is a remote from any program (NumpadKeyIndex below), with Num Lock off
// or always, as Preferences says: its bare keys are the "from any program" bindings of File >
// Keyboard shortcuts (app_keys.cpp), 5 pausing and 0 opening the song picker unless you change them.
// The hook is only installed while the mode is on, so no other program loses a key the rest of the
// time. The picker's own key (Win+Alt+Space unless changed) is an ordinary key from any program.
#include "app.h"

namespace app {

namespace {

constexpr UINT kHiddenTick = 1000, kShownTick = 250;

struct State {
    bool openedMini = false;     // the mode opened the mini player, so leaving closes it
    bool changedSize = false;    // the mode made the window compact from `sizeBefore`
    ViewSize sizeBefore = ViewSize::Normal;
    bool changingSize = false;   // the mode itself is switching the size
    // The numpad hook runs on a thread of its own, so a busy UI thread never delays typing in
    // other programs: the hook only decides and posts.
    HANDLE thread = nullptr;
    DWORD threadId = 0;
} s;

std::atomic<int> g_hookMode{0};
std::atomic<uint32_t> g_hookKeys{0};  // NumpadKeysBound(): the numpad keys that have a command
std::atomic<HWND> g_hookTarget{nullptr};
bool g_down[256];  // hook thread only: which captured keys are held, to tell a repeat

LRESULT CALLBACK NumpadHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        const DWORD vk = k->vkCode;
        if (vk >= 256) return CallNextHookEx(nullptr, code, wp, lp);
        if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
            if (!g_down[vk]) return CallNextHookEx(nullptr, code, wp, lp);
            g_down[vk] = false;  // its press was cordinal's, so its release is too
            return 1;
        }
        const bool modified = GetAsyncKeyState(VK_CONTROL) < 0 || GetAsyncKeyState(VK_MENU) < 0 ||
                              GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0;
        int index = modified ? -1 : NumpadKeyIndex(vk, (k->flags & LLKHF_EXTENDED) != 0, g_hookMode.load());
        if (index >= 0 && !(g_hookKeys.load() & (1u << index))) index = -1;  // a key with nothing to do types
        // In the song picker and the shortcut window the numpad's keys move, type and get assigned.
        if (index >= 0 && !g_down[vk]) {
            wchar_t name[32]{};
            GetClassNameW(GetForegroundWindow(), name, (int)std::size(name));
            if (!wcscmp(name, L"CordinalPalette") || !wcscmp(name, L"CordinalKeys")) index = -1;
        }
        if (index >= 0) {
            const bool repeat = g_down[vk];
            g_down[vk] = true;
            PostMessageW(g_hookTarget.load(), WM_APP_KEYS, (WPARAM)index, repeat);  // the command decides on repeats
            return 1;  // the key is cordinal's while the mode is on: no other program sees it
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI HookThread(void* ready) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // a queue, before StartKeys returns
    memset(g_down, 0, sizeof(g_down));
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, NumpadHook, g.inst, 0);
    SetEvent((HANDLE)ready);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    }
    if (hook) UnhookWindowsHookEx(hook);
    return 0;
}

void StartNumpad() {
    if (s.thread || g.lightweightNumpad == (int)LightweightNumpad::Off || !g.hwnd || !NumpadKeysBound()) return;
    g_hookMode = g.lightweightNumpad;
    g_hookKeys = NumpadKeysBound();
    g_hookTarget = g.hwnd;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready) return;
    s.thread = CreateThread(nullptr, 64 * 1024, HookThread, ready, STACK_SIZE_PARAM_IS_A_RESERVATION, &s.threadId);
    if (s.thread) WaitForSingleObject(ready, 2000);
    CloseHandle(ready);
}

void StopNumpad() {
    if (!s.thread) return;
    PostThreadMessageW(s.threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(s.thread, 2000);
    CloseHandle(s.thread);
    s.thread = nullptr;
    s.threadId = 0;
}

// A key another program holds can't reach cordinal; say so where someone with no window will see it.
void StartKeys() {
    static bool told = false;  // once a session: the shortcut window lists them too
    if (const auto& taken = TakenGlobalKeys(); !taken.empty() && !told) {
        told = true;
        std::wstring keys;
        for (const std::wstring& k : taken) keys += (keys.empty() ? L"" : L", ") + k;
        TrayNotice(taken.size() == 1 ? keys + L" is taken" : L"Some keys are taken",
                   L"Another program uses " + keys + L", so cordinal can't have it. Click cordinal's icon by the clock "
                   L"instead, or choose another key in File \x203A Keyboard shortcuts.");
    }
    StartNumpad();
}

void StopKeys() { StopNumpad(); }

void SetSizeQuietly(ViewSize size) {
    s.changingSize = true;
    SetViewSize(size);
    s.changingSize = false;
}

}  // namespace

// The numpad's keys as Windows reports them, as keymap numpad indexes. With Num Lock off the digits
// arrive as the navigation keys they are printed with (4 is Left), which the arrow-key block also
// sends, but with the "extended" flag; only the numpad's unflagged copies count. With Num Lock on
// they are the VK_NUMPAD digits, and the operators and Enter are the same either way: cordinal takes
// those only when Preferences says Always, since it can't see Num Lock from the hook.
int NumpadKeyIndex(DWORD vk, bool extended, int mode) {
    if (mode == (int)LightweightNumpad::Off) return -1;
    static constexpr DWORD kNav[11] = {VK_INSERT, VK_END, VK_DOWN, VK_NEXT, VK_LEFT, VK_CLEAR,
                                       VK_RIGHT,  VK_HOME, VK_UP,  VK_PRIOR, VK_DELETE};
    for (int i = 0; i < 11; ++i)
        if (vk == kNav[i] && !extended) return i;
    if (mode != (int)LightweightNumpad::Always) return -1;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (int)(vk - VK_NUMPAD0);
    static constexpr DWORD kOthers[] = {VK_DECIMAL, VK_ADD, VK_SUBTRACT, VK_MULTIPLY, VK_DIVIDE};
    for (int i = 0; i < 5; ++i)
        if (vk == kOthers[i]) return 10 + i;
    if (vk == VK_RETURN && extended) return 15;
    return -1;
}

bool LightweightHidesWindow() {
    return g.lightweight && g.lightweightShows != (int)LightweightShows::Compact;
}

bool LightweightPausesWork() { return g.lightweight && g.lightweightPause; }

void EnterLightweight() {
    if (g.lightweight || !g.hwnd) return;
    if (PaletteOpen()) ClosePalette();
    g.lightweight = true;
    s.openedMini = s.changedSize = false;
    const auto shows = (LightweightShows)g.lightweightShows;
    if (shows == LightweightShows::Mini) {
        if (!MiniPlayerWindow()) {
            ToggleMiniPlayer();
            s.openedMini = MiniPlayerWindow() != nullptr;
        }
        if (!MiniPlayerWindow()) ShowWindow(g.hwnd, SW_HIDE);  // no mini player: still put the window away
    } else {
        if (MiniPlayerWindow()) {  // the mode shows something else: the mini player goes too
            g.lightweight = false;
            ToggleMiniPlayer();
            g.lightweight = true;
        }
        if (shows == LightweightShows::Compact) {
            if (IsIconic(g.hwnd)) ShowWindow(g.hwnd, SW_RESTORE);
            if (g.size != ViewSize::Compact) {
                s.sizeBefore = g.size;
                s.changedSize = true;
                SetSizeQuietly(ViewSize::Compact);
            }
        } else {
            ShowWindow(g.hwnd, SW_HIDE);
        }
    }
    UpdateViewMemory();  // ReducedMemoryMode() is now true
    TrimMemory();
    if (LightweightHidesWindow()) SetTimer(g.hwnd, kUiTimer, kHiddenTick, nullptr);
    GiveMemoryBackSoon();
    RegisterGlobalKeys();  // cordinal's own keys from any program, held while the mode is on
    StartKeys();
    UpdateTray();
}

// `show` false is for exiting: the window's size goes back for next time, but nothing appears.
void LeaveLightweight(bool show) {
    if (!g.lightweight) return;
    g.lightweight = false;  // first, so closing the mini player below doesn't come back here
    StopKeys();
    RegisterGlobalKeys();
    SetTimer(g.hwnd, kUiTimer, kShownTick, nullptr);
    if (PaletteOpen()) ClosePalette();
    if (s.openedMini && MiniPlayerWindow()) {
        if (show) ToggleMiniPlayer();  // brings the main window back
        else DestroyWindow(MiniPlayerWindow());
    }
    if (s.changedSize && g.size == ViewSize::Compact) SetSizeQuietly(s.sizeBefore);
    s.openedMini = s.changedSize = false;
    UpdateViewMemory();
    WaveformTrackChanged(true);  // its peaks were left for later while nothing showed them
    if (show && !MiniPlayerWindow()) {
        ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g.hwnd);
    }
    UpdateTray();
}

void ToggleLightweight() {
    if (g.lightweight) LeaveLightweight();
    else EnterLightweight();
}

// Something showed the main window while the mode had put it away (another copy of cordinal was
// started, say): that is the way back, so the mode ends with it.
void LightweightWindowShown() {
    if (LightweightHidesWindow()) PostMessageW(g.hwnd, WM_COMMAND, IDM_LIGHTWEIGHT, 0);
}

// The window was made a size other than compact (Ctrl+=, a menu): the mode is over, and the size
// chosen stays.
void LightweightSizeChanged(ViewSize size) {
    if (!g.lightweight || s.changingSize || size == ViewSize::Compact ||
        g.lightweightShows != (int)LightweightShows::Compact)
        return;
    s.changedSize = false;
    LeaveLightweight();
}

// The preferences changed while the mode is on: what shows and the numpad follow at once.
void LightweightPreferencesChanged() {
    if (!g.lightweight) return;
    LeaveLightweight(false);
    EnterLightweight();
}

// The mini player opened or closed while the mode is on (the icon's menu does both without leaving
// it): what it made and dropped goes back to Windows a moment later, as on the way in.
void LightweightMiniChanged() {
    if (g.lightweight) GiveMemoryBackSoon();
}

}  // namespace app
