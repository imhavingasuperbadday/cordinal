// A music icon in the taskbar's notification area, drawn with the real Windows 11 icon font
// (Segoe Fluent Icons, U+EC4F) in the same monochrome style Windows uses for its own tray
// icons, following the light/dark taskbar. Left-click toggles the mini player; right-click
// opens the transport.
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

// The icon's right-click menu: transport, mini player, start-with-Windows and exit.
void ShowTrayMenu(POINT pt) {
    bool idle = g.playing == kNone && !RadioPlaying();
    bool paused = !idle && g.engine.Paused();
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_PLAY_PAUSE, paused ? L"&Resume" : L"&Pause");
    AppendMenuW(m, MF_STRING | (idle ? MF_GRAYED : 0), IDM_PREV, L"Pre&vious");
    AppendMenuW(m, MF_STRING | (idle ? MF_GRAYED : 0), IDM_NEXT, L"&Next");
    AppendMenuW(m, MF_STRING | (idle ? MF_GRAYED : 0), IDM_STOP, L"&Stop");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_MINI_PLAYER, L"&Mini player");
    AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, L"Show &cordinal");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (StartWithWindows() ? MF_CHECKED : 0), IDM_START_WITH_WINDOWS, L"Start with &Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"E&xit");
    SetForegroundWindow(g.hwnd);  // or the menu stays up after a click elsewhere
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

}  // namespace

// The font for icon glyphs: Segoe MDL2 Assets, which Windows 10 and 11 both have. Under Wine
// (Linux) the installer can only add Segoe Fluent Icons, which has the same code points.
const wchar_t* IconFace() {
    static const wchar_t* face = !FaceInstalled(kMdl2Icons) && FaceInstalled(kFluentIcons) ? kFluentIcons : kMdl2Icons;
    return face;
}

// Bring the main window up (and forward) from the tray or the icon's menu.
void ShowMainWindow() {
    if (!g.hwnd) return;
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
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(exe) + L"\"";
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
            ToggleMiniPlayer();
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

}  // namespace app
