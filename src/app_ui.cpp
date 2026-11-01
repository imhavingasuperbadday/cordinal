// Fonts, palette colors, cover tiles and the drawing helpers shared by the other modules. The dark
// menu bar, the player bars and the control subclasses live in app_ui_menu.cpp,
// app_ui_trackbar.cpp and app_ui_controls.cpp.
#include "app.h"
#include "app_ui_internal.h"

namespace app {

// WM_CTLCOLOR* helpers: set the control's text/background colors from the active
// palette and return the brush to use, wrapped as the message's LRESULT.
LRESULT FieldControlColor(HDC dc) {
    SetTextColor(dc, TextColor());
    SetBkColor(dc, Ref(P().surface2));
    return (LRESULT)g.fieldBrush;
}

LRESULT SurfaceControlColor(HDC dc, COLORREF text) {
    SetTextColor(dc, text);
    SetBkColor(dc, SurfaceColor());
    return (LRESULT)g.surfaceBrush;
}

const wchar_t* s_fontFace = nullptr;  // the theme font the UI fonts were last made with (none: the system's)
// The dark menu bar's font: Windows lays the bar's items out for the system menu font, so its text
// stays in that font even when a theme brings its own (a wider pixel font would overrun the items).
HFONT s_menuFont = nullptr;

// The menu bar's font for app_ui_menu.cpp, which cannot name the static above.
HFONT MenuFont() { return s_menuFont; }

// Makes a font built into the executable (RCDATA `id`) available to this process, once.
bool LoadThemeFont(int id) {
    static std::vector<int> loaded;
    if (std::find(loaded.begin(), loaded.end(), id) != loaded.end()) return true;
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    void* bytes = data ? LockResource(data) : nullptr;
    DWORD count = 0;
    if (!bytes || !AddFontMemResourceEx(bytes, SizeofResource(nullptr, res), nullptr, &count)) return false;
    loaded.push_back(id);
    return true;
}

// (Re)creates every UI font for the current g.dpi and theme; callers follow with ApplyFonts().
// SystemParametersInfoForDpi keeps the base font in step with the OS chrome; the two
// Segoe fonts supply the symbol and Segoe MDL2 Assets transport glyphs.
void CreateFonts() {
    for (HFONT* f : {&g.font, &g.fontBold, &g.fontTitle, &g.fontSymbol, &g.fontArt, &g.fontIcons, &g.fontIconsBig, &g.fontIconsHuge})
        if (*f) {
            DeleteObject(*f);
            *f = nullptr;
        }
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi);
    if (s_menuFont) DeleteObject(s_menuFont);
    s_menuFont = CreateFontIndirectW(&ncm.lfMenuFont);
    LOGFONTW lf = ncm.lfMessageFont;
    // The theme's own font. A built-in pixel font stays crisp: whole pixels per font pixel, no
    // smoothing. A system face keeps ClearType and scales like the rest of the UI.
    const Theme& theme = ThemeInfo(g.themeMode);
    s_fontFace = theme.font;
    if (theme.font && theme.fontResource && LoadThemeFont(theme.fontResource)) {
        wcscpy_s(lf.lfFaceName, theme.font);
        lf.lfHeight = -theme.fontPx * std::max(1, ((int)g.dpi + 48) / 96);
        lf.lfQuality = NONANTIALIASED_QUALITY;
    } else if (theme.font && !theme.fontResource) {
        wcscpy_s(lf.lfFaceName, theme.font);
        if (theme.fontPx) lf.lfHeight = -S(theme.fontPx);
    }
    g.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    g.fontBold = CreateFontIndirectW(&lf);
    g.fontTitle = CreateFontIndirectW(&lf);  // compact: now-playing title is just the bold UI font
    LOGFONTW sym = ncm.lfMessageFont;
    wcscpy_s(sym.lfFaceName, L"Segoe UI Symbol");
    sym.lfHeight = -MulDiv(10, (int)g.dpi, 72);
    g.fontSymbol = CreateFontIndirectW(&sym);
    sym.lfHeight = -S(20);
    g.fontArt = CreateFontIndirectW(&sym);
    wcscpy_s(sym.lfFaceName, IconFace());
    sym.lfHeight = -S(13);
    g.fontIcons = CreateFontIndirectW(&sym);
    sym.lfHeight = -S(17);
    g.fontIconsBig = CreateFontIndirectW(&sym);
    sym.lfHeight = -S(21);
    g.fontIconsHuge = CreateFontIndirectW(&sym);
}

void ApplyFonts() {
    if (g.tabs) TabCtrl_SetPadding(g.tabs, S(18), S(3));
    for (HWND h : {g.list, g.tabs, g.search, g.status, g.npSub, g.time, g.elapsed})
        SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
    SendMessageW(g.npTitle, WM_SETFONT, (WPARAM)g.fontTitle, TRUE);
    SendMessageW(g.speaker, WM_SETFONT, (WPARAM)g.fontSymbol, TRUE);
    for (HWND b : g.buttons) SendMessageW(b, WM_SETFONT, (WPARAM)g.fontSymbol, TRUE);
    RadioFonts();
}

void FillSolid(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

namespace {
// `h` 0..360, `s` and `v` 0..1.
Rgb Hsv(double h, double s, double v) {
    double c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60, 2) - 1)), m = v - c;
    double red = 0, green = 0, blue = 0;
    if (h < 60) red = c, green = x;
    else if (h < 120) red = x, green = c;
    else if (h < 180) green = c, blue = x;
    else if (h < 240) green = x, blue = c;
    else if (h < 300) red = x, blue = c;
    else red = c, blue = x;
    auto byte = [&](double u) { return (Rgb)std::lround((u + m) * 255); };
    return byte(red) << 16 | byte(green) << 8 | byte(blue);
}
}  // namespace

CoverTile CoverTileFor(const std::wstring& name) {
    if (name.empty()) return {P().surface2, P().faint};
    uint32_t hash = 2166136261u;  // FNV-1a over the lowercased name
    for (wchar_t c : name) hash = (hash ^ (uint32_t)towlower(c)) * 16777619u;
    double hue = hash % 360;
    return P().dark ? CoverTile{Hsv(hue, 0.38, 0.30), Hsv(hue, 0.28, 0.86)} : CoverTile{Hsv(hue, 0.20, 0.90), Hsv(hue, 0.50, 0.42)};
}

std::wstring CoverInitial(const std::wstring& name) {
    auto first = std::find_if(name.begin(), name.end(), [](wchar_t c) { return iswalnum(c) != 0; });
    return first == name.end() ? L"\x266B" : std::wstring(1, (wchar_t)towupper(*first));
}

void DrawCoverArt(HDC dc, const RECT& r, const ArtImage& art) {
    int side = std::min(art.width, art.height);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(bi.bmiHeader), art.width, -art.height, 1, 32};
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    // The crop is centred both ways, so it is the same whichever end of a top-down DIB the
    // source y counts from.
    StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, (art.width - side) / 2, (art.height - side) / 2,
                  side, side, art.pixels.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

void DrawCoverPlaceholder(HDC dc, const RECT& r, const std::wstring& name) {
    CoverTile t = CoverTileFor(name);
    FillSolid(dc, r, Ref(t.tile));
    LOGFONTW lf{};
    GetObjectW(g.font, sizeof(lf), &lf);
    lf.lfHeight = -std::max(8, (int)(r.bottom - r.top) * 9 / 20);
    lf.lfWidth = 0;
    lf.lfWeight = FW_SEMIBOLD;
    HFONT font = CreateFontIndirectW(&lf);
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Ref(t.ink));
    std::wstring initial = CoverInitial(name);
    RECT tr = r;
    DrawTextW(dc, initial.c_str(), (int)initial.size(), &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(font);
}

void DrawCoverFrame(HDC dc, const RECT& r) {
    COLORREF bg = WindowBg(), text = TextColor();
    COLORREF edge = RGB(GetRValue(bg) + (GetRValue(text) - GetRValue(bg)) * 14 / 100,
                        GetGValue(bg) + (GetGValue(text) - GetGValue(bg)) * 14 / 100,
                        GetBValue(bg) + (GetBValue(text) - GetBValue(bg)) * 14 / 100);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, edge);
    Rectangle(dc, r.left - 1, r.top - 1, r.right + 1, r.bottom + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

void ThemeEqControls();

// Resolves light/dark from the mode (System follows the OS setting) and repaints all
// themed chrome from the active Palette. Safe to call before the main window exists:
// it then only builds the brushes and menu theme, which is how startup applies the
// theme before creating any window.
void ApplyTheme() {
    g.pal = &ThemePalette(g.themeMode);
    g.dark = g.pal->dark;
    ApplyMenuTheme(g.dark);
    for (HBRUSH* b : {&g.fieldBrush, &g.surfaceBrush})
        if (*b) DeleteObject(*b);
    g.fieldBrush = CreateSolidBrush(Ref(P().surface2));
    g.surfaceBrush = CreateSolidBrush(SurfaceColor());
    if (!g.hwnd) return;
    // A theme with a font of its own changes every font, and the layout with them.
    const bool fontsChanged = ThemeInfo(g.themeMode).font != s_fontFace;
    if (fontsChanged) {
        CreateFonts();
        ApplyFonts();
    }
    ApplyWindowTheme(g.hwnd, P());
    if (g.themeMode == ThemeMode::Foobar) {  // foobar2000's caption text is the system's white, not the palette's green
        COLORREF caption = Ref(0xFFFFFF);
        DwmSetWindowAttribute(g.hwnd, 36 /* DWMWA_TEXT_COLOR */, &caption, sizeof(caption));
    }
    if (g.eqWnd) {
        ApplyWindowTheme(g.eqWnd, P());
        ThemeEqControls();
    }
    if (g.headphonesWnd) SendMessageW(g.headphonesWnd, WM_THEMECHANGED, 0, 0);
    ThemeBrowser();
    LibViewTheme();
    LyricsTheme();
    FoobarTheme();
    MiniPlayerTick();
    ApplyControlTheme(g.list, g.dark);
    ApplyControlTheme(g.header, g.dark);
    ListView_SetBkColor(g.list, WindowBg());
    ListView_SetTextBkColor(g.list, WindowBg());
    ListView_SetTextColor(g.list, TextColor());
    SetWindowTheme(g.search, g.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
    ApplyControlTheme(g.tips, g.dark);
    // Status bar and tabs are painted by us in both modes, from the palette.
    SetWindowTheme(g.status, L"", L"");
    SendMessageW(g.status, SB_SETBKCOLOR, 0, (LPARAM)SurfaceColor());
    SetWindowTheme(g.tabs, L"", L"");
    ApolloTheme();
    // Player arrangements can change even when both themes use the system font.
    if (fontsChanged || !g.startupTheme) Layout();
    // The first pass runs while the window is still hidden and about to paint from scratch, so
    // the frame recalculation, fountain wake and invalidations would only be repeated work.
    if (!g.startupTheme) {
        SetWindowPos(g.hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        WaveformTrackChanged(true);
        FountainWake();  // a theme change starts or stops the DELTARUNE-only effect immediately
        RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
        if (g.eqWnd) RedrawWindow(g.eqWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    }
}

// A FieldLookup for the title-format engine. withPlayback adds the two pseudo-fields
// that only exist for the playing track: playback_time and playback_time_remaining.
FieldLookup Lookup(TrackId id, bool withPlayback) {
    return [id, withPlayback](const std::wstring& name, std::wstring& out) {
        if (withPlayback) {
            if (name == L"playback_time") {
                out = FormatDuration(g.engine.Position());
                return true;
            }
            if (name == L"playback_time_remaining") {
                out = FormatDuration(std::max(0.0, g.engine.Duration() - g.engine.Position()));
                return true;
            }
        }
        if (name.starts_with(L"apollo_")) return ApolloField(id, name, out);
        // A pack's own field (fields.ini) fills in where cordinal has no field of that name.
        return LookupField(id, name, out) || PackField(id, name, out);
    };
}

std::wstring WindowText(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring text(len + 1, L'\0');
    GetWindowTextW(h, text.data(), len + 1);
    text.resize(len);
    return text;
}

void SetTextIfChanged(HWND h, const std::wstring& text) {
    if (WindowText(h) != text) SetWindowTextW(h, text.c_str());
}

void SetStatusPart(int part, const std::wstring& text) {
    if (g.statusParts[part] == text) return;
    g.statusParts[part] = text;
    // Owner drawn so the text color follows the theme; borderless, so the parts read as one flat
    // strip split by the hairlines DrawMainItem draws, not as three sunken boxes.
    SendMessageW(g.status, SB_SETTEXTW, part | SBT_OWNERDRAW | SBT_NOBORDERS, (LPARAM)part);
}

void ShowMessage(const std::wstring& text) {
    g.message = ThemeInfo(g.themeMode).messagePrefix + text;
    g.messageUntil = GetTickCount64() + 6000;
    SetStatusPart(ApolloMode() ? 1 : 0, g.message);  // Apollo's first part always counts the tracks
    MessageBlip();
}

std::wstring EmptyViewText() {
    if (!g.filter.empty()) return L"No tracks match your search. Press Esc in the search box to clear it.";
    if (LibraryActive()) {
        for (const auto& pane : g.panes)
            if (!pane.selected.empty()) return L"No tracks match the browser filters. Select All in each browser column to reset.";
        return L"Your library is empty. Add a music folder with Library > Library folders to keep it up to date, "
               L"or drop music here to add what it holds now.";
    }
    if (Active().Editable()) return L"This playlist is empty. Drop music here, or press Ctrl+O to add files.";
    if (Active().kind == PlaylistKind::Auto) return L"No tracks match this auto-playlist. Change its rules from the Playlist menu.";
    return L"Nothing here yet.";
}

bool TranslateMainKey(MSG* msg) {
    if (g.rename && msg->hwnd == g.rename) return false;
    bool main = msg->hwnd == g.hwnd || IsChild(g.hwnd, msg->hwnd);
    if (main && msg->message == WM_KEYDOWN) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, alt = GetKeyState(VK_MENU) < 0;
        bool shift = GetKeyState(VK_SHIFT) < 0;
        wchar_t cls[32]{};
        GetClassNameW(msg->hwnd, cls, (int)std::size(cls));
        bool editing = _wcsicmp(cls, WC_EDITW) == 0 || _wcsnicmp(cls, L"RichEdit", 8) == 0;
        if (RadioActive() && msg->hwnd == RadioView() && !ctrl && !alt && !shift &&
            (msg->wParam == VK_LEFT || msg->wParam == VK_RIGHT)) return false;
        if (!ctrl && !alt && !shift && msg->wParam == VK_SPACE &&
            (msg->hwnd == RadioTab() || (RadioActive() && IsChild(RadioView(), msg->hwnd) && !editing))) return false;
        // Editing a query or playlist name must keep the normal edit-control shortcuts.
        if (editing &&
            ((ctrl && (msg->wParam == 'Z' || msg->wParam == VK_LEFT || msg->wParam == VK_RIGHT ||
                       msg->wParam == VK_RETURN)) || msg->wParam == VK_F2))
            return false;
        if (!editing && !ctrl && !alt && !shift) {
            if (msg->wParam == VK_LEFT || msg->wParam == VK_RIGHT) {
                SeekBy(msg->wParam == VK_LEFT ? -5.0 : 5.0);
                return true;
            }
            if (msg->wParam == VK_SPACE) {
                if (!(msg->lParam & (1LL << 30))) TogglePause();
                return true;
            }
        }
        if (msg->wParam == VK_TAB && !ctrl && !alt) {
            std::vector<HWND> stops;
            for (HWND b : g.buttons) stops.push_back(b);
            stops.insert(stops.end(), {g.seek, g.volume, RadioTab(), g.search});
            if (RadioActive()) {
                auto radio = RadioTabStops();
                stops.insert(stops.end(), radio.begin(), radio.end());
            } else {
                for (HWND b : g.viewButtons) stops.push_back(b);
                for (const auto& pane : g.panes) stops.push_back(pane.list);
                stops.push_back(CanvasActive() ? g.libView : g.list);
            }
            std::erase_if(stops, [](HWND h) { return !h || !IsWindowVisible(h) || !IsWindowEnabled(h); });
            if (stops.empty()) return false;
            auto it = std::find(stops.begin(), stops.end(), GetFocus());
            int i = it == stops.end() ? -1 : (int)(it - stops.begin());
            int count = (int)stops.size();
            int next = GetKeyState(VK_SHIFT) < 0 ? (i < 0 ? count - 1 : (i + count - 1) % count) : (i + 1) % count;
            SendMessageW(g.hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
            SetFocus(stops[next]);
            return true;
        }
    }
    return TranslateAcceleratorW(g.hwnd, g.accel, msg) != 0;
}

void TrimMemory() {
    ReleaseGroupArt();
    CompactLibrary();
}

// Registers a window class once; registering again fails harmlessly, so callers
// can invoke this on every show without a per-site "registered" flag.
void RegisterAppClass(const wchar_t* cls, WNDPROC proc, UINT style, bool appIcon) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = style;
    wc.lpfnWndProc = proc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (appIcon) wc.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
}

// Runs a window's drawing through an off-screen bitmap and blits it once, so a redraw
// never shows a half-painted frame.
void DoubleBufferedPaint(HWND hwnd, void (*draw)(HDC, RECT)) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc, area = ps.rcPaint;
    GetClientRect(hwnd, &rc);
    int w = area.right - area.left, h = area.bottom - area.top;
    if (w > 0 && h > 0) {
        // The bitmap holds only the area being repainted; the viewport shifts drawing into it.
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        SetViewportOrgEx(mem, -area.left, -area.top, nullptr);
        draw(mem, rc);
        BitBlt(dc, area.left, area.top, w, h, mem, area.left, area.top, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    EndPaint(hwnd, &ps);
}

}  // namespace app
