// A music icon in the taskbar's notification area, drawn with the real Windows 11 icon font
// (Segoe Fluent Icons, U+EC4F) in the same monochrome style Windows uses for its own tray
// icons, following the light/dark taskbar. Left-click toggles the mini player (in Lightweight mode
// it opens the song picker by the clock); right-click opens a menu that can run cordinal with no
// window at all: the transport, Up next and the playlists, shuffle, repeat, volume and effects,
// and Lightweight mode.
//
// The icon is owned by this process, so it is on the taskbar while cordinal runs. "Start with
// Windows" (View menu, or the icon's own menu) is how it is there without opening the player.
#include "app.h"

namespace app {

namespace {

constexpr UINT kTrayCallback = WM_APP_TRAY;
constexpr UINT kTrayId = 1;

// Windows 11 shipped Segoe Fluent Icons; Windows 10 has only Segoe MDL2 Assets. Both carry
// the same music note at U+EC4F.
constexpr wchar_t kFluentIcons[] = L"Segoe Fluent Icons";
constexpr wchar_t kMdl2Icons[] = L"Segoe MDL2 Assets";
constexpr wchar_t kMusicNote[] = L"\xEC4F";

const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"cordinal";

NOTIFYICONDATAW g_nid{};
HICON g_icon = nullptr;
bool g_added = false;
UINT g_taskbarCreated = 0;  // "TaskbarCreated": explorer restarted, re-add the icon
std::wstring g_tip;
TrackId g_tipTrack = kNone;  // what the last tooltip described
bool g_tipPaused = false;

// The taskbar is light or dark from its own setting, which is not the app's theme.
bool LightTaskbar() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value != 0;
    return false;  // Windows 11's taskbar is dark unless light mode is on
}

// GDI substitutes a face it cannot find, so ask it what it actually got.
bool FaceInstalled(const wchar_t* face) {
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfHeight = -12;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ old = SelectObject(dc, font);
    wchar_t actual[LF_FACESIZE]{};
    GetTextFaceW(dc, LF_FACESIZE, actual);
    SelectObject(dc, old);
    DeleteObject(font);
    ReleaseDC(nullptr, dc);
    return _wcsicmp(actual, face) == 0;
}

// Draw the glyph into a 32-bit DIB. GDI paints white-on-black, so any channel is the glyph's
// coverage and becomes the alpha; the colour is washed in separately for the taskbar's theme.
// (The same trick app_taskbar.cpp uses for the thumbnail buttons.)
HICON RenderNote(const wchar_t* face, int cx, int cy) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), cx, -cy, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) {
        DeleteDC(dc);
        return nullptr;
    }
    HGDIOBJ oldBmp = SelectObject(dc, color);
    LOGFONTW lf{};
    lf.lfHeight = -cy;
    lf.lfQuality = ANTIALIASED_QUALITY;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT r{0, 0, cx, cy};
    DrawTextW(dc, kMusicNote, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(font);

    uint8_t level = LightTaskbar() ? 0x00 : 0xFF;  // black note on a light bar
    auto* px = static_cast<uint32_t*>(bits);
    for (int i = 0; i < cx * cy; ++i) {
        uint32_t a = px[i] & 0xFF;
        px[i] = (a << 24) | (level << 16) | (level << 8) | level;
    }

    HBITMAP mask = CreateBitmap(cx, cy, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    return icon;
}

HICON MakeIcon() {
    int cx = GetSystemMetricsForDpi(SM_CXSMICON, g.dpi ? g.dpi : GetDpiForSystem());
    int cy = GetSystemMetricsForDpi(SM_CYSMICON, g.dpi ? g.dpi : GetDpiForSystem());
    if (cx <= 0) cx = 16;
    if (cy <= 0) cy = 16;

    if (FaceInstalled(kFluentIcons))
        if (HICON icon = RenderNote(kFluentIcons, cx, cy)) return icon;
    if (FaceInstalled(kMdl2Icons))
        if (HICON icon = RenderNote(kMdl2Icons, cx, cy)) return icon;

    // Neither icon font is present: cordinal's own icon is better than an empty square.
    HICON fallback = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, cx, cy, 0);
    return fallback ? fallback : LoadIconW(g.inst, MAKEINTRESOURCEW(IDI_APP));
}

std::wstring Tip() {
    if (RadioPlaying()) return L"cordinal - " + RadioStationName() + (g.engine.Paused() ? L" (paused)" : L"");
    if (g.playing == kNone) return L"cordinal";
    auto lookup = Lookup(g.playing, true);
    const Pack* pack = PackFormatOwner(kPackTray);  // a pack's "tray tip" format
    std::wstring title = FormatTitle(pack ? pack->formats[kPackTray] : L"[%artist% - ]%title%", lookup);
    if (title.empty()) return L"cordinal";
    if (g.engine.Paused()) return L"cordinal (paused) - " + title;
    return L"cordinal - " + title;
}

// (Re)creates the shell icon and adds it to the notification area, upgrading to protocol
// version 4 (hover tooltips and richer callbacks).
void AddIcon() {
    if (!g.hwnd) return;
    if (g_icon) {
        DestroyIcon(g_icon);
        g_icon = nullptr;
    }
    g_icon = MakeIcon();
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g.hwnd;
    g_nid.uID = kTrayId;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = kTrayCallback;
    g_nid.hIcon = g_icon;
    g_tip = Tip();
    g_tipTrack = g.playing;
    g_tipPaused = (g.playing != kNone) && g.engine.Paused();
    wcsncpy_s(g_nid.szTip, g_tip.c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_added = false;
        return;
    }
    g_added = true;
    // Version 4 puts the event in LOWORD(lParam) and the cursor in wParam, and NIF_SHOWTIP
    // shows the tooltip on hover (earlier versions only showed it on click).
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

// The menu's own items, returned rather than sent as commands: a song of Up next, a playlist, a
// volume. Above every IDM_* range and below the system commands (0xF000).
constexpr UINT kUpNextBase = 0xE000, kPlaylistBase = 0xE100, kVolumeBase = 0xE800, kShowId = 0xE900;
constexpr int kVolumes[] = {100, 75, 50, 25, 10};

// "Artist - Title", short enough for a menu.
std::wstring MenuSong(TrackId id) {
    std::wstring title = g_lib.Str(g_lib[id].title), artist = g_lib.Str(g_lib[id].artist);
    if (title.empty()) title = g_lib.Str(g_lib[id].file);
    std::wstring text = artist.empty() ? title : artist + L" - " + title;
    if (text.size() > 60) text = text.substr(0, 57) + L"...";
    std::wstring escaped;  // '&' would underline the next letter
    for (wchar_t c : text) escaped += c == L'&' ? std::wstring(L"&&") : std::wstring(1, c);
    return escaped;
}

// The icon's right-click menu: everything a background player needs without its window.
void ShowTrayMenu(POINT pt) {
    bool idle = g.playing == kNone && !RadioPlaying();
    bool paused = !idle && g.engine.Paused();
    const UINT idleFlag = idle ? MF_GRAYED : 0;
    HMENU m = CreatePopupMenu();
    if (g.playing != kNone && g.playing < g_lib.Count())  // what is playing, as a heading
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (std::wstring(paused ? L"Paused: " : L"") + MenuSong(g.playing)).c_str());
    AppendMenuW(m, MF_STRING, IDM_PLAY_PAUSE, idle ? L"&Play" : paused ? L"&Resume" : L"&Pause");
    AppendMenuW(m, MF_STRING | idleFlag, IDM_PREV, L"Pre&vious");
    AppendMenuW(m, MF_STRING | idleFlag, IDM_NEXT, L"&Next");
    AppendMenuW(m, MF_STRING | idleFlag, IDM_STOP, L"&Stop");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const std::wstring pickKey = GlobalKeyText(IDM_QUICK_PICK);
    AppendMenuW(m, MF_STRING, IDM_QUICK_PICK, (L"Play a s&ong..." + (pickKey.empty() ? L"" : L"\t" + pickKey)).c_str());
    // Up next: a click plays that song now.
    HMENU upNext = CreatePopupMenu();
    std::vector<TrackId> upcoming = UpcomingTracks();
    for (size_t i = 0; i < upcoming.size(); ++i)
        if (upcoming[i] < g_lib.Count()) AppendMenuW(upNext, MF_STRING, kUpNextBase + (UINT)i, MenuSong(upcoming[i]).c_str());
    if (upcoming.empty()) AppendMenuW(upNext, MF_STRING | MF_GRAYED, 0, L"Nothing yet");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)upNext, L"&Up next");
    HMENU lists = CreatePopupMenu();
    for (int i = 0; i < (int)g.playlists.size() && i < 0x700; ++i) {
        const Playlist& pl = g.playlists[i];
        if (pl.items.empty() || pl.kind == PlaylistKind::Inbox) continue;
        std::wstring name;
        for (wchar_t c : pl.name) name += c == L'&' ? std::wstring(L"&&") : std::wstring(1, c);
        AppendMenuW(lists, MF_STRING | (i == g.playingPl && !idle ? MF_CHECKED : 0), kPlaylistBase + (UINT)i, name.c_str());
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)lists, L"Play pla&ylist");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SHUFFLE, L"S&huffle");
    HMENU repeat = CreatePopupMenu();
    const wchar_t* repeatNames[] = {L"&Off", L"Repeat &all", L"Repeat &one"};
    for (int i = 0; i < (int)Repeat::Count; ++i) AppendMenuW(repeat, MF_STRING, IDM_REPEAT_BASE + i, repeatNames[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)repeat, L"&Repeat");
    // Volume: cordinal's own level, when it has one (with it off, Windows' volume is the control).
    HMENU volume = CreatePopupMenu();
    AppendMenuW(volume, MF_STRING, IDM_MUTE, L"&Mute");
    if (g.softwareVolume) {
        AppendMenuW(volume, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < std::size(kVolumes); ++i)
            AppendMenuW(volume, MF_STRING | (g.volumePercent == kVolumes[i] ? MF_CHECKED : 0), kVolumeBase + (UINT)i,
                        (std::to_wstring(kVolumes[i]) + L"%").c_str());
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)volume, L"&Volume");
    // Effects keep working in Lightweight mode; these are the ones worth changing from here.
    HMENU effects = CreatePopupMenu();
    const wchar_t* reverbNames[] = {L"No reverb", L"&Room reverb", L"&Hall reverb", L"&Plate reverb"};
    static_assert(std::size(reverbNames) == (size_t)ReverbPreset::Count);
    for (int i = 0; i < (int)ReverbPreset::Count; ++i) AppendMenuW(effects, MF_STRING, IDM_REVERB_BASE + i, reverbNames[i]);
    AppendMenuW(effects, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(effects, MF_STRING, IDM_EQUALIZER, L"&Equalizer...");
    AppendMenuW(effects, MF_STRING, IDM_NIGHT_MODE, L"&Night mode");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)effects, L"&Effects");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g.lightweight ? MF_CHECKED : 0), IDM_LIGHTWEIGHT, L"&Lightweight mode\tCtrl+Shift+L");
    if (g.lightweight || !IsWindowVisible(g.hwnd)) AppendMenuW(m, MF_STRING, kShowId, L"Show &cordinal");
    AppendMenuW(m, MF_STRING | (MiniPlayerWindow() ? MF_CHECKED : 0), IDM_MINI_PLAYER, L"&Mini player");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (StartWithWindows() ? MF_CHECKED : 0), IDM_START_WITH_WINDOWS, L"Start with &Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
    // Checkmarks as the menu bar shows them (UpdateMainMenu).
    CheckMenuItem(m, IDM_SHUFFLE, MF_BYCOMMAND | (g.shuffle == kShuffleTracks ? MF_CHECKED : 0));
    CheckMenuItem(m, IDM_MUTE, MF_BYCOMMAND | (g.volumePercent == 0 ? MF_CHECKED : 0));
    CheckMenuItem(m, IDM_NIGHT_MODE, MF_BYCOMMAND | (g.nightMode ? MF_CHECKED : 0));
    CheckMenuItem(m, IDM_EQUALIZER, MF_BYCOMMAND | (g.eq.enabled || g.eq.crossfeed ? MF_CHECKED : 0));
    CheckMenuRadioItem(m, IDM_REPEAT_BASE, IDM_REPEAT_BASE + (int)Repeat::Count - 1, IDM_REPEAT_BASE + (int)g.repeat, MF_BYCOMMAND);
    CheckMenuRadioItem(m, IDM_REVERB_BASE, IDM_REVERB_BASE + (int)ReverbPreset::Count - 1,
                       IDM_REVERB_BASE + (int)g.reverb.preset, MF_BYCOMMAND);
    SetForegroundWindow(g.hwnd);  // or the menu stays up after a click elsewhere
    const UINT cmd = (UINT)TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD, pt.x, pt.y, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);  // and its submenus
    if (!cmd) return;
    if (cmd == kShowId) {
        ShowMainWindow();
    } else if (cmd >= kVolumeBase && cmd < kVolumeBase + std::size(kVolumes)) {
        SetVolumePercent(kVolumes[cmd - kVolumeBase]);
    } else if (cmd >= kPlaylistBase && cmd < kVolumeBase) {
        int pl = (int)(cmd - kPlaylistBase);
        if (pl < (int)g.playlists.size() && !g.playlists[pl].items.empty())
            PlayTrack(pl, g.shuffle == kShuffleTracks ? (int)(GetTickCount64() % g.playlists[pl].items.size()) : 0, true);
    } else if (cmd >= kUpNextBase && cmd < kPlaylistBase) {
        // The list may have moved on while the menu was open: play it only if it is still coming.
        const size_t i = cmd - kUpNextBase;
        if (i < upcoming.size()) {
            std::vector<TrackId> now = UpcomingTracks();
            auto at = std::find(now.begin(), now.end(), upcoming[i]);
            if (at != now.end() && (size_t)(at - now.begin()) < g.queue.size() && g.queue[at - now.begin()] == *at) {
                PlayQueued((int)(at - now.begin()));
            } else if (at != now.end() && g.playingPl >= 0 && g.playingPl < (int)g.playlists.size()) {
                const auto& items = g.playlists[g.playingPl].items;
                auto in = std::find(items.begin(), items.end(), *at);
                if (in != items.end()) PlayTrack(g.playingPl, (int)(in - items.begin()), true);
            }
        }
    } else if (cmd == IDM_MINI_PLAYER) {
        TrayMiniPlayer();
    } else {
        OnCommand((int)cmd, 0, nullptr);
    }
    UpdateTray();
}

}  // namespace

// The font for icon glyphs: Segoe MDL2 Assets, which Windows 10 and 11 both have. Under Wine
// (Linux) the installer can only add Segoe Fluent Icons, which has the same code points.
const wchar_t* IconFace() {
    static const wchar_t* face = !FaceInstalled(kMdl2Icons) && FaceInstalled(kFluentIcons) ? kFluentIcons : kMdl2Icons;
    return face;
}

// Bring the main window up (and forward) from the tray or the icon's menu; in Lightweight mode
// that is the way out of it.
void ShowMainWindow() {
    if (!g.hwnd) return;
    if (g.lightweight) {
        LeaveLightweight();
        return;
    }
    ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.hwnd);
}

// True when the Run key holds a non-empty value for cordinal.
bool StartWithWindows() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    wchar_t value[MAX_PATH * 2]{};
    DWORD size = sizeof(value), type = 0;
    LSTATUS status = RegQueryValueExW(key, kRunValue, nullptr, &type, reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ && value[0] != L'\0';
}

// Writes this executable's own path, so the copy started at logon runs the same build.
void ToggleStartWithWindows() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return;
    if (StartWithWindows()) {
        RegDeleteValueW(key, kRunValue);
    } else {
        std::wstring exe(32768, L'\0');
        DWORD n = GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size());
        if (!n || n >= exe.size()) {  // no usable path: leave the logon entry off
            RegCloseKey(key);
            return;
        }
        exe.resize(n);
        std::wstring command = L"\"" + exe + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       (DWORD)((command.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
}

void InitTray() {
    // Explorer restarts (and shell updates) destroy every tray icon; it tells the world with
    // this broadcast so applications can put theirs back.
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddIcon();
}

// Removes the icon and frees it; called on exit.
void ShutdownTray() {
    if (g_added) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_added = false;
    }
    if (g_icon) {
        DestroyIcon(g_icon);
        g_icon = nullptr;
    }
    g_tip.clear();
}

// Handles the tray callback and the TaskbarCreated broadcast; returns true when consumed.
bool OnTrayMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated && msg == g_taskbarCreated) {
        g_added = false;
        AddIcon();
        return true;
    }
    if (msg != kTrayCallback) return false;

    // NOTIFYICON_VERSION_4: the event is LOWORD(lParam), the cursor wParam.
    UINT event = LOWORD(lp);
    POINT pt{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)};
    switch (event) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (g.lightweight) ShowQuickPick(true);  // music without bringing the window back
            else ToggleMiniPlayer();
            return true;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            if (pt.x == 0 && pt.y == 0) GetCursorPos(&pt);  // older versions carry no coordinates
            ShowTrayMenu(pt);
            return true;
        default:
            return true;
    }
}

// Called from the UI timer and after a track or state change: nothing is sent unless it changed.
void UpdateTray() {
    if (!g_added) return;
    // Building the tooltip parses a title format, so only do it when the track or state moved.
    static bool wasRadio = false;
    bool radio = RadioPlaying();
    bool paused = (g.playing != kNone || radio) && g.engine.Paused();
    if (!radio && !wasRadio && g.playing == g_tipTrack && paused == g_tipPaused) return;
    wasRadio = radio;
    g_tipTrack = g.playing;
    g_tipPaused = paused;
    std::wstring tip = Tip();
    if (tip == g_tip) return;
    g_tip = tip;
    wcsncpy_s(g_nid.szTip, g_tip.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.hIcon = g_icon;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// The taskbar's light/dark setting changed: redraw the note and swap the icon in.
void TrayThemeChanged() {
    if (!g_added) return;
    HICON previous = g_icon;
    g_icon = MakeIcon();
    if (!g_icon) {
        g_icon = previous;
        return;
    }
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.hIcon = g_icon;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (previous) DestroyIcon(previous);
}

// The icon size follows the DPI, so a redraw is enough.
void TrayDpiChanged() {
    if (!g_added) return;
    TrayThemeChanged();
}

// A notification from the icon, for news that must reach someone whose window is put away. Windows
// keeps it quiet during focus assist and full-screen programs.
void TrayNotice(const std::wstring& title, const std::wstring& text) {
    if (!g_added) return;
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

}  // namespace app
