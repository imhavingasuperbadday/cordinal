// Window sizes, smallest first; Ctrl+- and Ctrl+= step through them:
//   mini player (app_mini.cpp)  a round window on its own; the main window hides
//   Compact     the main window made small around the player: the Library's view row goes; the
//               cover, buttons and seek bar get bigger. It has a size and place of its own; the
//               others share the full window's.
//   Normal      the usual window, the only one with the menu bar, status bar and points chip
//               (the others show the menus on a right-click), and the only one whose layout can
//               be customized (app_custom.cpp)
//   Spotlight   ("Big mode plus" in the menu) just the music: Now playing (in the colors of the cover) over a large playback
//               strip in the middle of the window, then Up next (app_panels.cpp)
//   Big         ("Big mode") the panels in tabs, docked above and below that strip (app_big.cpp)
#include "app.h"

namespace app {

namespace {

HMENU g_menuBar = nullptr;  // detached but for the normal size

// The compact window's normal-position rectangle (workspace coordinates, like WINDOWPLACEMENT):
// where it was last time, else the full window's top left corner.
RECT CompactRect(const RECT& full) {
    auto v = ParseInts(IniRead(L"Window", L"CompactPlacement", L""));
    if (v.size() == 4) {
        RECT r{v[0], v[1], v[2], v[3]};
        if (r.right - r.left >= S(300) && r.bottom - r.top >= S(260) && MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return r;
    }
    return {full.left, full.top, full.left + S(440), full.top + S(600)};
}

void Apply(ViewSize size) {
    bool reduced = ReducedMemoryMode();
    g.size = size;
    if (reduced != ReducedMemoryMode()) UpdateViewMemory();
    SyncMenuBar();
    // The strip's controls hide until the mouse is over it: watch for that.
    if (size >= ViewSize::Spotlight) SetTimer(g.hwnd, kStripTimer, 60, nullptr);
    else KillTimer(g.hwnd, kStripTimer);
    g.stripHover = false;
    // The button glyphs change size; the seek and volume bars change style.
    for (HWND h : g.buttons) InvalidateRect(h, nullptr, FALSE);
    for (HWND h : {g.seek, g.volume}) InvalidateRect(h, nullptr, TRUE);
    Layout();
}

// The spotlight and big mode want room: a smaller window grows (within its screen).
void MakeRoom(int w, int h) {
    RECT r;
    if (IsZoomed(g.hwnd) || !IsWindowVisible(g.hwnd) || !GetWindowRect(g.hwnd, &r)) return;
    if (r.right - r.left >= w && r.bottom - r.top >= h) return;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    w = std::min(std::max(w, (int)(r.right - r.left)), (int)(work.right - work.left));
    h = std::min(std::max(h, (int)(r.bottom - r.top)), (int)(work.bottom - work.top));
    int x = std::clamp((int)r.left, (int)work.left, (int)work.right - w);
    int y = std::clamp((int)r.top, (int)work.top, (int)work.bottom - h);
    SetWindowPos(g.hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace

// The menu bar shows in the normal window unless its layout hides it; detached, it is kept for the
// right-click menus (ShowCompactMenu).
void SyncMenuBar() {
    if (g.size != ViewSize::Normal || !g.layout.menuBar || ApolloMode()) {
        if (HMENU bar = GetMenu(g.hwnd)) g_menuBar = bar;
        SetMenu(g.hwnd, nullptr);
    } else if (g_menuBar && !GetMenu(g.hwnd)) {
        SetMenu(g.hwnd, g_menuBar);
    }
}

HMENU MainMenuBar() {
    HMENU bar = GetMenu(g.hwnd);
    return bar ? bar : g_menuBar;
}

void ReplaceMenuBar(HMENU bar) {
    if (GetMenu(g.hwnd)) SetMenu(g.hwnd, bar);
    g_menuBar = bar;
    SyncMenuBar();
}

// Switches view size. Compact remembers the full placement so leaving it restores the normal window
// exactly; entering it swaps in the stored compact rectangle (or a default based on the full one).
void SetViewSize(ViewSize size) {
    if (size == g.size || size >= ViewSize::Count) return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    bool visible = IsWindowVisible(g.hwnd) != 0;
    if (size == ViewSize::Compact) {
        g.fullPlacement = {sizeof(WINDOWPLACEMENT)};
        GetWindowPlacement(g.hwnd, &g.fullPlacement);
        wp = g.fullPlacement;
        wp.rcNormalPosition = CompactRect(g.fullPlacement.rcNormalPosition);
        wp.showCmd = visible ? SW_SHOWNORMAL : SW_HIDE;
        Apply(size);
        SetWindowPlacement(g.hwnd, &wp);
    } else if (g.size == ViewSize::Compact) {
        SaveViewSize();  // the compact place, for next time
        wp = g.fullPlacement;
        if (wp.length != sizeof(wp)) GetWindowPlacement(g.hwnd, &wp);
        if (wp.showCmd != SW_SHOWMAXIMIZED) wp.showCmd = SW_SHOWNORMAL;
        if (!visible) wp.showCmd = SW_HIDE;
        Apply(size);
        SetWindowPlacement(g.hwnd, &wp);
    } else {
        Apply(size);
    }
    if (size == ViewSize::Big) MakeRoom(S(1120), S(780));
    else if (size == ViewSize::Spotlight) MakeRoom(S(760), S(640));
    Layout();
    SaveViewSize();
}

// Called once at startup with the placement the window was created with; applies the approved size and,
// for Compact, substitutes the stored compact rectangle while keeping the full placement for later.
void RestoreViewSize(WINDOWPLACEMENT full, int showCmd) {
    ViewSize size = g.defaultSize;
    bool mini = g.defaultMiniPlayer && showCmd != SW_HIDE && showCmd != SW_SHOWMINIMIZED &&
                showCmd != SW_MINIMIZE && showCmd != SW_SHOWMINNOACTIVE;
    WINDOWPLACEMENT wp = full;
    if (size == ViewSize::Compact) {
        g.fullPlacement = full;
        wp.rcNormalPosition = CompactRect(full.rcNormalPosition);
        wp.showCmd = showCmd == SW_SHOWMAXIMIZED ? SW_SHOWNORMAL : showCmd;
    }
    Apply(size);
    if (mini) wp.showCmd = SW_HIDE;
    SetWindowPlacement(g.hwnd, &wp);
    if (mini) {
        ToggleMiniPlayer();
        // The mini player is fallible; never leave the process with no visible window.
        if (!MiniPlayerWindow()) ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
    }
    ResetPreferenceSuggestions();
}

// Remember geometry separately from the startup preference: trying a size is not consent to keep it.
void SaveViewSize() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (g.size == ViewSize::Compact && GetWindowPlacement(g.hwnd, &wp)) {
        const RECT& r = wp.rcNormalPosition;
        IniWrite(L"Window", L"CompactPlacement", JoinInts({(int)r.left, (int)r.top, (int)r.right, (int)r.bottom}));
    }
}

void ShowCompactMenu(POINT pt) {
    if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_LARGER, L"&Larger\tCtrl+=");
    AppendMenuW(m, MF_STRING, IDM_SMALLER, L"&Smaller\tCtrl+-");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // The menu bar's menus, borrowed: taken out again before the popup is destroyed.
    int borrowed = 0;
    if (g_menuBar) {
        for (int i = 0, n = GetMenuItemCount(g_menuBar); i < n; ++i) {
            wchar_t text[64] = {};
            GetMenuStringW(g_menuBar, i, text, (int)std::size(text), MF_BYPOSITION);
            if (HMENU sub = GetSubMenu(g_menuBar, i)) {
                AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, text);
                ++borrowed;
            }
        }
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
    // The borrowed popups were appended after the first three items (Larger, Smaller, separator),
    // so the first of them sits at index 3.
    for (int i = 0; i < borrowed; ++i) RemoveMenu(m, 3, MF_BYPOSITION);
    DestroyMenu(m);
    if (cmd) OnCommand(cmd, 0, nullptr);
}

void ViewSmaller() {
    if (g.size != ViewSize::Compact) SetViewSize((ViewSize)((int)g.size - 1));
    else if (!MiniPlayerWindow()) ToggleMiniPlayer();
}

void ViewLarger() {
    if (MiniPlayerWindow()) ToggleMiniPlayer();  // the main window comes back as it was left
    else if (g.size != ViewSize::Big) SetViewSize((ViewSize)((int)g.size + 1));
}

}  // namespace app
