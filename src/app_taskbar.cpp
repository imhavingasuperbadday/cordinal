// Taskbar integration: previous / play-pause / next buttons under the window's thumbnail, and
// the playback position on the taskbar button (green while playing, yellow while paused).
#include "app.h"

namespace app {

namespace {

UINT g_buttonCreated = 0;  // "TaskbarButtonCreated": Explorer (re)created our button
// ITaskbarList3 is created when Explorer announces the button; null until then or when it is
// unavailable. HICONs are rebuilt on every DPI change.
ComPtr<ITaskbarList3> g_taskbar;
bool g_buttonsAdded = false;
HICON g_icons[4] = {};  // previous, play, pause, next
enum class Shown { None, Playing, Paused } g_shown = Shown::None;
ULONGLONG g_lastValue = UINT64_MAX;

// A white glyph from Segoe MDL2 Assets as an icon. The taskbar draws thumbnail buttons on its own
// dark or light surface, so white with alpha reads well on both (it's what Windows' players do).
HICON GlyphIcon(const wchar_t* glyph, int size) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color) {
        DeleteDC(dc);
        return nullptr;
    }
    HGDIOBJ oldBmp = SelectObject(dc, color);
    LOGFONTW lf{};
    lf.lfHeight = -size * 3 / 4;
    lf.lfQuality = ANTIALIASED_QUALITY;  // grayscale coverage becomes the alpha channel
    wcscpy_s(lf.lfFaceName, IconFace());
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT r{0, 0, size, size};
    DrawTextW(dc, glyph, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    auto* px = static_cast<uint32_t*>(bits);
    for (int i = 0; i < size * size; ++i) {
        uint32_t a = px[i] & 0xFF;  // white text on black: any channel is the coverage
        px[i] = (a << 24) | (a << 16) | (a << 8) | a;  // premultiplied white
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(font);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    return icon;
}

// (Re)draws the four glyph icons at the current DPI, discarding any previous ones.
void MakeIcons() {
    for (HICON& i : g_icons)
        if (i) {
            DestroyIcon(i);
            i = nullptr;
        }
    int size = GetSystemMetricsForDpi(SM_CXSMICON, g.dpi);
    const wchar_t* glyphs[] = {L"\xE892", L"\xE768", L"\xE769", L"\xE893"};
    for (int i = 0; i < 4; ++i) g_icons[i] = GlyphIcon(glyphs[i], size);
}

// A thumbnail button with its icon, tooltip and flags filled in.
THUMBBUTTON Button(UINT id, HICON icon, const wchar_t* tip) {
    THUMBBUTTON b{};
    b.dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    b.iId = id;
    b.hIcon = icon;
    wcsncpy_s(b.szTip, tip, _TRUNCATE);
    b.dwFlags = THBF_ENABLED;
    return b;
}

// True while audio is actually playing: a track is loaded and not paused.
bool Playing() { return (g.playing != kNone || RadioPlaying()) && !g.engine.Paused(); }

// Adds the thumbnail buttons the first time, otherwise updates their icons and tooltips in place.
void SetButtons(bool add) {
    if (!g_taskbar) return;
    bool playing = Playing();
    THUMBBUTTON buttons[] = {
        Button(IDM_PREV, g_icons[0], L"Previous"),
        Button(IDM_PLAY_PAUSE, g_icons[playing ? 2 : 1], playing ? L"Pause" : L"Play"),
        Button(IDM_NEXT, g_icons[3], L"Next"),
    };
    if (add) g_buttonsAdded = SUCCEEDED(g_taskbar->ThumbBarAddButtons(g.hwnd, 3, buttons));
    else if (g_buttonsAdded) g_taskbar->ThumbBarUpdateButtons(g.hwnd, 3, buttons);
}

}  // namespace

// Wires the taskbar button messages: the created notice plus the two filter exceptions below.
void InitTaskbar() {
    g_buttonCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
    // Lets the message through when cordinal runs elevated and Explorer doesn't.
    ChangeWindowMessageFilterEx(g.hwnd, g_buttonCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g.hwnd, WM_COMMAND, MSGFLT_ALLOW, nullptr);
}

// True if `msg` was the taskbar's "button created" notice (handled here).
bool OnTaskbarMessage(UINT msg) {
    if (!g_buttonCreated || msg != g_buttonCreated) return false;
    g_taskbar.Reset();
    g_buttonsAdded = false;
    g_shown = Shown::None;
    g_lastValue = UINT64_MAX;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_taskbar))) &&
        SUCCEEDED(g_taskbar->HrInit())) {
        MakeIcons();
        SetButtons(true);
        UpdateTaskbar();
    } else {
        g_taskbar.Reset();
    }
    return true;
}

// The window moved to a monitor with another DPI: redraw the icons and update the buttons.
void TaskbarDpiChanged() {
    if (!g_taskbar) return;
    MakeIcons();
    SetButtons(false);
}

// Called with every status update (a few times a second): cheap when nothing changed.
void UpdateTaskbar() {
    if (!g_taskbar) return;
    static bool wasRadio = false;
    bool radio = RadioPlaying();
    if (wasRadio != radio) { wasRadio = radio; g_shown = Shown::None; }
    Shown shown = g.playing == kNone && !radio ? Shown::None : g.engine.Paused() ? Shown::Paused : Shown::Playing;
    if (shown != g_shown) {
        bool buttons = (shown == Shown::Playing) != (g_shown == Shown::Playing);
        g_shown = shown;
        if (buttons) SetButtons(false);
        if (!g.taskbarProgress || shown == Shown::None || radio) {
            g_taskbar->SetProgressState(g.hwnd, TBPF_NOPROGRESS);
            g_lastValue = UINT64_MAX;
        } else {
            g_taskbar->SetProgressState(g.hwnd, shown == Shown::Paused ? TBPF_PAUSED : TBPF_NORMAL);
        }
    }
    if (!g.taskbarProgress || shown == Shown::None || radio) return;
    double dur = CurrentDuration();
    if (dur <= 0) return;
    // 1000 steps: no more precision than a taskbar button can show, and few redundant calls.
    ULONGLONG value = (ULONGLONG)std::clamp(g.engine.Position() / dur * 1000.0, 0.0, 1000.0);
    if (value != g_lastValue) {
        g_lastValue = value;
        g_taskbar->SetProgressValue(g.hwnd, value, 1000);
    }
}

// Show progress on the taskbar (Preferences > View): keeps the taskbar button's progress bar in step
// with playback.
void SetTaskbarProgress(bool on) {
    g.taskbarProgress = on;
    IniWrite(L"Window", L"TaskbarProgress", on ? L"1" : L"0");
    g_shown = Shown::None;  // forces the state to be set again
    if (g_taskbar && !on) g_taskbar->SetProgressState(g.hwnd, TBPF_NOPROGRESS);
    UpdateTaskbar();
}

// Releases the taskbar object and its icons; safe when no buttons were ever added.
void ShutdownTaskbar() {
    g_taskbar.Reset();
    for (HICON& i : g_icons)
        if (i) {
            DestroyIcon(i);
            i = nullptr;
        }
}

}  // namespace app
