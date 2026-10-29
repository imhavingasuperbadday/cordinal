// Creates the main window's native child controls: transport buttons, seek and volume bars, the
// playlist tabs and search box, the list view, the browser/lyrics panels, the status bar and the
// tooltips. Every control starts at zero size; app_layout.cpp places them. Handles live in `g`.
#include "app.h"

namespace app {

void CreateControls(HWND hwnd) {
    auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)id, g.inst,
                               nullptr);
    };
    g.npTitle = make(0, WC_STATICW, kAppName, SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX | SS_NOTIFY, IDC_NP_TITLE);
    g.npSub = make(0, WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX | SS_NOTIFY, IDC_NP_SUB);
    // The glyphs are drawn from Segoe MDL2 Assets in DrawToolbarButton; the text is for accessibility.
    const wchar_t* glyphs[] = {L"Previous", L"Play", L"Next", L"Shuffle", L"Repeat", L"Resume"};
    const wchar_t* tips[] = {L"Previous (Ctrl+Left)", L"Play / Pause (Space)", L"Next (Ctrl+Right)",
                             L"Shuffle (Ctrl+H)", L"Repeat (Ctrl+R)", L"Resume where you left off"};
    const int buttonIds[] = {IDC_PREV, IDC_PLAY, IDC_NEXT, IDC_SHUFFLE, IDC_REPEAT, IDC_RESUME};
    for (int i = 0; i < 6; ++i) {
        g.buttons[i] = make(0, WC_BUTTONW, glyphs[i], BS_OWNERDRAW | WS_TABSTOP, buttonIds[i]);
        SetWindowSubclass(g.buttons[i], ButtonSubclass, 0, 0);
    }
    // The foobar2000 theme's toolbar adds stop and pause (shown only there, app_foobar.cpp).
    const int fbIds[] = {IDC_STOP, IDC_PAUSE};
    const wchar_t* fbNames[] = {L"Stop", L"Pause"};
    for (int i = 0; i < 2; ++i) {
        g.fbButtons[i] = make(0, WC_BUTTONW, fbNames[i], BS_OWNERDRAW | WS_TABSTOP, fbIds[i]);
        ShowWindow(g.fbButtons[i], SW_HIDE);
        SetWindowSubclass(g.fbButtons[i], ButtonSubclass, 0, 0);
    }
    // The seek bar is a fixed 0..1000 scale (thousandths of the track): playback maps position and
    // duration onto it, so its unit is not milliseconds.
    g.seek = make(0, TRACKBAR_CLASSW, L"Seek", TBS_HORZ | TBS_NOTICKS | TBS_BOTH | WS_TABSTOP, IDC_SEEK);
    SendMessageW(g.seek, TBM_SETRANGEMAX, FALSE, 1000);
    SendMessageW(g.seek, TBM_SETPAGESIZE, 0, 50);
    g.elapsed = make(0, WC_STATICW, L"", SS_RIGHT | SS_NOPREFIX, IDC_ELAPSED);
    g.time = make(0, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_NOTIFY, IDC_TIME);
    g.speaker = make(0, WC_STATICW, L"\U0001F50A", SS_CENTER | SS_NOPREFIX | SS_NOTIFY, IDC_SPEAKER);
    g.volume = make(0, TRACKBAR_CLASSW, L"Volume", TBS_HORZ | TBS_NOTICKS | TBS_BOTH | TBS_TOOLTIPS | WS_TABSTOP, IDC_VOLUME);
    SendMessageW(g.volume, TBM_SETRANGEMAX, FALSE, 100);
    SendMessageW(g.volume, TBM_SETPOS, TRUE, g.volumePercent);
    SetWindowSubclass(g.seek, TrackbarSubclass, 0, 0);
    SetWindowSubclass(g.volume, TrackbarSubclass, 0, 0);

    // Tabs and search. The tab strip paints itself (app_tabstrip.cpp); the search box shows a cue
    // banner (UpdateSearchCue) and is subclassed for its Escape / Enter handling.
    g.tabs = make(0, WC_TABCONTROLW, L"", TCS_TOOLTIPS | WS_TABSTOP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, IDC_TABS);
    SetWindowSubclass(g.tabs, TabsSubclass, 0, 0);
    g.search = make(WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_SEARCH);
    UpdateSearchCue();
    SendMessageW(g.search, EM_SETLIMITTEXT, 4096, 0);
    SetWindowSubclass(g.search, SearchSubclass, 0, 0);
    CreateRadio(hwnd);

    // Virtual list view: LVS_OWNERDATA means the app supplies every row (g.view) on demand. The
    // header is subclassed for the sort arrows and drag feedback.
    g.list = make(0, WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP, IDC_LIST);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP |
                                                  LVS_EX_LABELTIP);
    SetWindowSubclass(g.list, ListSubclass, 0, 0);
    g.header = ListView_GetHeader(g.list);
    SetWindowSubclass(g.header, HeaderSubclass, 0, 0);
    LoadColumns();
    SetupColumns();

    CreateBrowser(hwnd);
    CreateLibView(hwnd);
    CreateLyricsPanel(hwnd);
    // No size grip: the unthemed bar draws a light 3D grip that no palette reaches, and the
    // window's own corner resizes it anyway.
    g.status = make(0, STATUSCLASSNAMEW, L"", 0, IDC_STATUS);
    SetWindowSubclass(g.status, StatusSubclass, 0, 0);

    g.tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT,
                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, g.inst, nullptr);
    for (int i = 0; i < 6; ++i) {
        TTTOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hwnd;
        ti.uId = (UINT_PTR)g.buttons[i];
        ti.lpszText = const_cast<wchar_t*>(tips[i]);
        SendMessageW(g.tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    for (int i = 0; i < 2; ++i) {
        TTTOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hwnd;
        ti.uId = (UINT_PTR)g.fbButtons[i];
        ti.lpszText = const_cast<wchar_t*>(fbNames[i]);
        SendMessageW(g.tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    // The status bar has no child windows for its parts, so its tools are keyed by index
    // (100 + part) rather than by handle, unlike the window-keyed tools elsewhere.
    const wchar_t* statusTips[] = {L"ReplayGain: click to switch between off, track and album gain",
                                   L"Equalizer: click to open it (Ctrl+E)", L"Output: click to switch between shared and exclusive"};
    for (int i = 0; i < 3; ++i) {
        TTTOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g.status;
        ti.uId = 100 + i;
        ti.lpszText = const_cast<wchar_t*>(statusTips[i]);
        SendMessageW(g.tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    for (HWND h : {g.npTitle, g.npSub, g.speaker, g.time}) {
        TTTOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hwnd;
        ti.uId = (UINT_PTR)h;
        ti.lpszText = const_cast<wchar_t*>(h == g.speaker ? L"Mute / unmute (Ctrl+M)"
                                           : h == g.time  ? L"Click to switch between the time left and the length"
                                                          : L"Find the playing track, or continue where you stopped");
        SendMessageW(g.tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

}  // namespace app
