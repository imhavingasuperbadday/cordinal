// Fonts, palette colors, dark menu bar and the flat custom-drawn controls.
#include "app.h"

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

// ---------------------------------------------------------------------------
// Dark menu bar (the undocumented UAH messages Windows uses to draw the menu bar)

constexpr UINT WM_UAHDRAWMENU = 0x0091, WM_UAHDRAWMENUITEM = 0x0092;
struct UahMenu {
    HMENU hmenu;
    HDC hdc;
    DWORD flags;
};
struct UahMenuItem {
    int position;
    DWORD metrics[8];
    DWORD popupMetrics[5];
};
struct UahDrawMenuItem {
    DRAWITEMSTRUCT dis;
    UahMenu um;
    UahMenuItem umi;
};

bool HandleDarkMenuBar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
    if (!g.dark) return false;
    switch (msg) {
        case WM_UAHDRAWMENU: {
            auto* um = reinterpret_cast<UahMenu*>(lp);
            MENUBARINFO mbi{sizeof(mbi)};
            GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi);
            RECT wr;
            GetWindowRect(hwnd, &wr);
            RECT rc = mbi.rcBar;
            OffsetRect(&rc, -wr.left, -wr.top);
            FillRect(um->hdc, &rc, g.surfaceBrush);
            result = TRUE;
            return true;
        }
        case WM_UAHDRAWMENUITEM: {
            auto* item = reinterpret_cast<UahDrawMenuItem*>(lp);
            wchar_t text[128] = {};
            MENUITEMINFOW mii{sizeof(mii)};
            mii.fMask = MIIM_STRING;
            mii.dwTypeData = text;
            mii.cch = (UINT)std::size(text) - 1;
            GetMenuItemInfoW(item->um.hmenu, item->umi.position, TRUE, &mii);
            UINT state = item->dis.itemState;
            bool hot = state & (ODS_HOTLIGHT | ODS_SELECTED);
            HBRUSH bg = CreateSolidBrush(hot ? Ref(P().hover) : Ref(P().surface));
            FillRect(item->um.hdc, &item->dis.rcItem, bg);
            DeleteObject(bg);
            SetBkMode(item->um.hdc, TRANSPARENT);
            SetTextColor(item->um.hdc, (state & (ODS_INACTIVE | ODS_GRAYED)) ? Ref(P().faint)
                                       : g.themeMode == ThemeMode::Foobar ? Ref(kFoobarChromeText) : Ref(P().text));
            HGDIOBJ old = SelectObject(item->um.hdc, s_menuFont ? s_menuFont : g.font);
            DrawTextW(item->um.hdc, text, -1, &item->dis.rcItem,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER | ((state & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
            SelectObject(item->um.hdc, old);
            result = TRUE;
            return true;
        }
        case WM_NCPAINT:
        case WM_NCACTIVATE: {
            result = DefWindowProcW(hwnd, msg, wp, lp);
            // Paint over the light line Windows draws under the menu bar.
            MENUBARINFO mbi{sizeof(mbi)};
            if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return true;
            RECT client, wr;
            GetClientRect(hwnd, &client);
            MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
            GetWindowRect(hwnd, &wr);
            OffsetRect(&client, -wr.left, -wr.top);
            RECT line = client;
            line.bottom = line.top;
            line.top--;
            HDC dc = GetWindowDC(hwnd);
            FillRect(dc, &line, g.surfaceBrush);
            ReleaseDC(hwnd, dc);
            return true;
        }
    }
    return false;
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

// The seek bar's waveform with its section over it; false while there is none to
// draw (the flat channel stands in). `rc` is the whole bar as drawn.
bool DrawSeekWaveform(HDC dc, const RECT& rc, const RECT& channel, double frac) {
    if (FountainShown()) PaintFountain(dc, rc, channel.left, channel.right, frac);
    else if (!DrawWaveform(dc, rc, channel, frac)) return false;
    DrawRegion(dc, rc, channel);  // the section, over the peaks (app_region.cpp)
    return true;
}

// The seek bar of a player docked at a side runs down its column. The common control has no
// vertical bar that keeps the horizontal one's messages, so the bar stays horizontal underneath and
// is painted here through a transposed world transform: the horizontal drawing code then lays
// time out top to bottom. Its channel is the whole height, inset a little at each end.
RECT VerticalChannel(const RECT& client) { return {client.top + S(4), client.left, client.bottom - S(4), client.right}; }

void PaintPlayerBar(HWND bar, HDC printDC = nullptr) {
    PAINTSTRUCT ps{};
    HDC dc = printDC ? printDC : BeginPaint(bar, &ps);
    RECT client;
    GetClientRect(bar, &client);
    int lo = (int)SendMessageW(bar, TBM_GETRANGEMIN, 0, 0), hi = (int)SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);
    int pos = (int)SendMessageW(bar, TBM_GETPOS, 0, 0);
    double frac = hi > lo ? (double)(pos - lo) / (hi - lo) : 0;
    // Drawn off screen and copied once: the Dark Fountain repaints 30 times a second.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1, (int)client.right), std::max(1, (int)client.bottom));
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    if (bar == g.volume || (!SeekVertical() && !FountainShown())) {
        // Paint the whole control so the native trackbar cannot leave chrome behind: its own
        // background fill kept the palette brush of the theme it started in (black after a switch
        // from dark to light).
        NMCUSTOMDRAW draw{};
        draw.hdr.hwndFrom = bar;
        draw.hdc = mem;
        draw.dwDrawStage = CDDS_ITEMPREPAINT;
        draw.dwItemSpec = TBCD_CHANNEL;
        SendMessageW(bar, TBM_GETCHANNELRECT, 0, (LPARAM)&draw.rc);
        DrawTrackbar(&draw);
        draw.dwItemSpec = TBCD_THUMB;
        SendMessageW(bar, TBM_GETTHUMBRECT, 0, (LPARAM)&draw.rc);
        POINT cursor;
        GetCursorPos(&cursor);
        ScreenToClient(bar, &cursor);
        draw.uItemState = GetCapture() == bar ? CDIS_SELECTED : PtInRect(&draw.rc, cursor) ? CDIS_HOT : 0;
        DrawTrackbar(&draw);
        if (g.showFocusBoxes && GetFocus() == bar && !(SendMessageW(bar, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
            DrawFocusRect(mem, &client);
    } else if (SeekVertical()) {
        RECT ch = VerticalChannel(client);
        bool fountain = FountainShown();
        if (fountain) PaintFountain(mem, client, ch.left, ch.right, frac);
        int oldMode = SetGraphicsMode(mem, GM_ADVANCED);
        XFORM transpose{0, 1, 1, 0, 0, 0};  // x' = y, y' = x
        SetWorldTransform(mem, &transpose);
        RECT rc{client.top, client.left, client.bottom, client.right};
        if (fountain) {
            DrawRegion(mem, rc, ch);
        } else if (!DrawSeekWaveform(mem, rc, ch, frac)) {
            FillSolid(mem, rc, SurfaceColor());
        }
        ModifyWorldTransform(mem, nullptr, MWT_IDENTITY);
        SetGraphicsMode(mem, oldMode);
    } else {
        RECT channel{}, thumb{};
        SendMessageW(bar, TBM_GETCHANNELRECT, 0, (LPARAM)&channel);
        SendMessageW(bar, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
        int halfThumb = (thumb.right - thumb.left) / 2;
        channel.left += halfThumb;
        channel.right -= halfThumb;
        DrawSeekWaveform(mem, client, channel, frac);
    }
    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    if (!printDC) EndPaint(bar, &ps);
}

// Where along `h` the point falls, 0..1: the thumb-centre mapping the stock bar uses, or for the
// vertical seek bar its channel top to bottom. DragBarTo seeks by it and the section menu marks
// by it (RegionTimeAt), so both land on the same spot.
double BarFracAt(HWND h, POINT pt) {
    if (FoobarMode() && (h == g.seek || h == g.volume)) {
        RECT track = FoobarSliderTrack(h);
        return std::clamp((double)(pt.x - track.left) / std::max(1L, track.right - track.left), 0.0, 1.0);
    }
    if (h == g.seek && SeekVertical()) {
        RECT client;
        GetClientRect(h, &client);
        RECT ch = VerticalChannel(client);
        return std::clamp((double)(pt.y - ch.left) / std::max(1, (int)(ch.right - ch.left)), 0.0, 1.0);
    }
    RECT thumb{}, channel{};
    SendMessageW(h, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
    SendMessageW(h, TBM_GETCHANNELRECT, 0, (LPARAM)&channel);
    int thumbW = thumb.right - thumb.left;
    int width = std::max(1, (int)(channel.right - channel.left) - thumbW);
    return std::clamp(((double)pt.x - channel.left - thumbW / 2) / width, 0.0, 1.0);
}

// Flat trackbar for both themes: square channel with an accent fill, classic foobar feel.
// Position/range logic is untouched so seek/volume length behavior is identical.
LRESULT DrawTrackbar(NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    HDC dc = cd->hdc;
    HWND bar = cd->hdr.hwndFrom;
    if (bar == g.seek && LiveWaveformShown() && !FountainShown()) {
        if (cd->dwItemSpec == TBCD_CHANNEL) {
            RECT all{}; GetClientRect(bar, &all);
            int lo = (int)SendMessageW(bar, TBM_GETRANGEMIN, 0, 0), hi = (int)SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);
            int pos = (int)SendMessageW(bar, TBM_GETPOS, 0, 0);
            DrawWaveform(dc, all, cd->rc, hi > lo ? (double)(pos - lo) / (hi - lo) : 0);
        }
        return CDRF_SKIPDEFAULT;
    }
    if (ApolloMode() && (bar == g.seek || bar == g.volume)) return ApolloDrawTrackbar(cd);
    if (FoobarMode() && (bar == g.seek || bar == g.volume)) {
        // foobar2000's sliders: a sunken groove and a small blue thumb (none on a stopped seek bar).
        if (cd->dwItemSpec == TBCD_CHANNEL) {
            RECT all;
            GetClientRect(bar, &all);
            FillSolid(dc, all, SurfaceColor());
            RECT groove = FoobarSliderTrack(bar);
            FillSolid(dc, groove, WindowBg());
            FillSolid(dc, {groove.left, groove.top, groove.right, groove.top + 1}, Ref(P().border));
            FillSolid(dc, {groove.left, groove.top, groove.left + 1, groove.bottom}, Ref(P().border));
            return CDRF_SKIPDEFAULT;
        }
        if (cd->dwItemSpec == TBCD_THUMB) {
            if (!IsWindowEnabled(bar)) return CDRF_SKIPDEFAULT;
            // The native thumb rectangle can lag TBM_SETPOS(FALSE) because this control paints
            // itself. Read the position directly, using the same geometry as clicks and drags.
            RECT track = FoobarSliderTrack(bar);
            int lo = (int)SendMessageW(bar, TBM_GETRANGEMIN, 0, 0), hi = (int)SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);
            int pos = (int)SendMessageW(bar, TBM_GETPOS, 0, 0);
            double frac = hi > lo ? std::clamp((double)(pos - lo) / (hi - lo), 0.0, 1.0) : 0;
            int w = S(ui::kFoobarThumb), h = S(ui::kFoobarThumbHeight);
            int cx = track.left + (int)std::lround(frac * (track.right - track.left)), cy = (track.top + track.bottom) / 2;
            FillSolid(dc, {cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h}, Ref(P().accent));
            return CDRF_SKIPDEFAULT;
        }
        return cd->dwItemSpec == TBCD_TICS ? CDRF_SKIPDEFAULT : CDRF_DODEFAULT;
    }
    if (cd->dwItemSpec == TBCD_CHANNEL) {
        RECT r = cd->rc;
        bool vert = (GetWindowLongW(bar, GWL_STYLE) & TBS_VERT) != 0;
        int lo = (int)SendMessageW(bar, TBM_GETRANGEMIN, 0, 0);
        int hi = (int)SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);
        int pos = (int)SendMessageW(bar, TBM_GETPOS, 0, 0);
        double frac = hi > lo ? (double)(pos - lo) / (hi - lo) : 0;
        RECT all;
        GetClientRect(bar, &all);
        if (bar == g.seek && DrawSeekWaveform(dc, all, r, frac)) return CDRF_SKIPDEFAULT;
        // Paint the bar's own background: the channel item is the only thing repainted over an
        // invalidated bar, so a track whose waveform has gone (or isn't known yet) would
        // otherwise leave the previous track's columns standing outside the slim channel.
        FillSolid(dc, all, SurfaceColor());
        COLORREF fill = Ref(P().accent);
        if (!IsWindowEnabled(bar)) fill = Ref(P().faint);
        // Slim flat channel, centered.
        RECT t = r;
        if (vert) {
            int w = std::min<long>(S(4), t.right - t.left);
            int cx = (t.left + t.right) / 2;
            t.left = cx - w / 2;
            t.right = t.left + w;
        } else {
            int h = std::min<long>(S(4), t.bottom - t.top);
            int cy = (t.top + t.bottom) / 2;
            t.top = cy - h / 2;
            t.bottom = t.top + h;
        }
        FillSolid(dc, t, Ref(P().border));  // the empty track, no outline
        // Filled portion: left->thumb (horizontal) or bottom->thumb (vertical).
        // EQ sliders fill from zero dB. The preamp has extra attenuation for headphone profiles;
        // native vertical trackbars put position zero at the top.
        RECT f = t;
        bool bipolar = vert && lo == 0 && (hi == 48 || hi == 168);
        if (vert) {
            if (bipolar) {
                double zero = hi == 168 ? 48.0 / 168 : 0.5;
                int mid = t.top + (int)((t.bottom - t.top) * zero);
                int y = t.top + (int)((t.bottom - t.top) * frac);
                if (y < mid) f = {t.left, y, t.right, mid};
                else f = {t.left, mid, t.right, y};
            } else {
                int y = t.top + (int)((t.bottom - t.top) * (1.0 - frac));
                f.top = y;
            }
        } else {
            f.right = f.left + (int)((f.right - f.left) * frac);
        }
        if (f.right > f.left && f.bottom > f.top) FillSolid(dc, f, fill);
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_THUMB) {
        // Over a waveform the thumb isn't drawn: the split between the two shades is the playhead.
        // The spotlight's and big mode's bars have no thumb at all: the played color shows the
        // position.
        if (bar == g.seek && (WaveformShown() || FountainShown())) return CDRF_SKIPDEFAULT;
        if (g.size >= ViewSize::Spotlight && (bar == g.seek || bar == g.volume)) return CDRF_SKIPDEFAULT;
        bool enabled = IsWindowEnabled(bar);
        bool hot = (cd->uItemState & CDIS_HOT) != 0;
        bool pressed = (cd->uItemState & CDIS_SELECTED) != 0;
        // A slim upright bar centred in the stock thumb's rect: the text color at rest, the accent
        // while hovered or held. DELTARUNE keeps its square, yellow-edged thumb.
        RECT r = cd->rc;
        if (g.themeMode == ThemeMode::Deltarune) {
            COLORREF face = !enabled ? Ref(P().surface) : pressed ? Ref(P().accent) : hot ? Ref(P().hover) : Ref(P().surface);
            COLORREF edge = !enabled ? Ref(P().faint) : bar == g.volume ? Ref(P().accent) : Ref(P().border);
            SetDCBrushColor(dc, face);
            SetDCPenColor(dc, edge);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
            Rectangle(dc, r.left, r.top, r.right + 1, r.bottom + 1);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            return CDRF_SKIPDEFAULT;
        }
        int w = std::max(2, S(3)), hgt = std::min<int>(S(14), r.bottom - r.top);
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        FillSolid(dc, {cx - w / 2, cy - hgt / 2, cx - w / 2 + w, cy - hgt / 2 + hgt},
                  !enabled ? Ref(P().faint) : pressed || hot ? Ref(P().accent) : TextColor());
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_TICS) {
        // No ticks; the flat channel reads cleaner.
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

static HWND s_barDrag = nullptr;  // the bar whose channel (not thumb) the mouse is down on

// Moves `h` to the position under `pt` (BarFracAt) and tells the parent, which previews the time
// while a drag is in progress.
void DragBarTo(HWND h, POINT pt) {
    int lo = (int)SendMessageW(h, TBM_GETRANGEMIN, 0, 0), hi = (int)SendMessageW(h, TBM_GETRANGEMAX, 0, 0);
    int pos = lo + (int)std::lround(BarFracAt(h, pt) * (hi - lo));
    SendMessageW(h, TBM_SETPOS, TRUE, pos);
    // The stock bar only repaints the strip the thumb left; over a waveform it isn't drawn at
    // all and the split is the playhead, so repaint the whole bar (without the background
    // erase, which blinks the waveform away).
    InvalidateRect(h, nullptr, FALSE);
    SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, pos), (LPARAM)h);
}

LRESULT CALLBACK TrackbarSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_LBUTTONDOWN) {
        // The stock bar pages on a channel click, so the click never landed where it was aimed,
        // and its own capture then moved the bar a second time: two seeks, the second one wrong,
        // and a pressed state repainting over the waveform. A channel press is therefore driven
        // here from press to release; the thumb keeps the stock drag, which already seeks once.
        // The vertical seek bar has no thumb of its own: every press is a channel press.
        RECT thumb{};
        bool vertical = h == g.seek && SeekVertical();
        if (!vertical) SendMessageW(h, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (vertical || (FoobarMode() && (h == g.seek || h == g.volume)) || !PtInRect(&thumb, pt)) {
            s_barDrag = h;
            SetFocus(h);
            SetCapture(h);
            DragBarTo(h, pt);
            return 0;
        }
    } else if (msg == WM_MOUSEMOVE && s_barDrag == h) {
        DragBarTo(h, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    } else if (msg == WM_TIMER && h == g.seek && (FountainStep(wp) || WaveformStep(wp))) {
        InvalidateRect(h, nullptr, FALSE);  // the next live waveform or fountain frame
        return 0;
    } else if ((msg == WM_PAINT || msg == WM_ERASEBKGND || msg == WM_PRINTCLIENT) && (h == g.seek || h == g.volume)) {
        if (msg != WM_ERASEBKGND) PaintPlayerBar(h, msg == WM_PRINTCLIENT ? (HDC)wp : nullptr);
        return msg == WM_ERASEBKGND;  // the paint fills the whole bar
    } else if ((msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) && s_barDrag == h) {
        HWND parent = GetParent(h);
        if (msg == WM_LBUTTONUP) DragBarTo(h, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});  // the release decides the position
        int pos = (int)SendMessageW(h, TBM_GETPOS, 0, 0);
        s_barDrag = nullptr;  // before ReleaseCapture: its WM_CAPTURECHANGED must not fire again
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        // MainScroll seeks on TB_ENDTRACK alone, so a lost capture still has to end the drag, or
        // g.seeking would stay set and the bar would stop following playback.
        SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, pos), (LPARAM)h);
        return 0;
    } else if (msg == WM_NCDESTROY) {
        if (s_barDrag == h) s_barDrag = nullptr;
        RemoveWindowSubclass(h, TrackbarSubclass, 0);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Search box key handling: Ctrl+A selects all, Esc clears, Enter applies the query
// (and plays the first match unless Ctrl), Down drops focus into the list. The
// WM_CHAR clauses swallow the control chars produced by keys already handled in
// WM_KEYDOWN so they neither beep nor insert text.
LRESULT CALLBACK SearchSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (RadioActive() && wp == VK_RETURN) { RadioSearch(); return 0; }
        if (RadioActive() && wp == VK_DOWN) { RadioFocus(); return 0; }
        if (RadioActive() && wp == VK_ESCAPE) { SetWindowTextW(h, L""); RadioFocus(); return 0; }
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            SetWindowTextW(h, L"");
            FlushSearch();
            SetFocus(CanvasActive() ? g.libView : g.list);
            return 0;
        }
        if (wp == VK_RETURN) {
            FlushSearch();
            if (GetKeyState(VK_CONTROL) >= 0) PlayRow(0);
            return 0;
        }
        if (wp == VK_DOWN) {
            FlushSearch();
            if (g.view.empty()) return 0;
            if (CanvasActive()) {
                LibViewFocusFirst();
            } else {
                SetFocus(g.list);
                ClearSelection();
                int row = RowOfViewPos(0);
                ListView_SetItemState(g.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(g.list, 0, FALSE);
                ListView_EnsureVisible(g.list, row, FALSE);
            }
            return 0;
        }
    } else if (msg == WM_CHAR && (wp == L'\r' || wp == 27 || wp == 1)) {
        return 0;
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, SearchSubclass, 0);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Flat list header in both themes: the list's own background so header and rows read as one
// surface, quiet labels, short faint dividers (enough to find a column edge) and the sort arrow.
LRESULT CALLBACK HeaderSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (h == g.header && (msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK)) {
        // Let the header finish resizing/reordering before capturing the resulting columns.
        LRESULT result = DefSubclassProc(h, msg, wp, lp);
        SaveColumns();
        if (FoobarMode()) RefreshViewKeepingSelection();  // the cover column's width sets the groups' heights
        return result;
    }
    if (msg == WM_PAINT || msg == WM_ERASEBKGND) {
        if (msg == WM_ERASEBKGND) return 1;
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (ApolloMode()) {
            ApolloPaintHeader(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        RECT rc;
        GetClientRect(h, &rc);
        bool foobar = FoobarMode();  // foobar2000's header: near black, light labels, full dividers
        FillSolid(dc, rc, foobar ? Ref(kFoobarHeaderBg) : WindowBg());
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, g.font);
        COLORREF divider = foobar ? Ref(P().border) : RGB((GetRValue(WindowBg()) + GetRValue(Ref(P().border))) / 2,
                               (GetGValue(WindowBg()) + GetGValue(Ref(P().border))) / 2,
                               (GetBValue(WindowBg()) + GetBValue(Ref(P().border))) / 2);
        int n = Header_GetItemCount(h);
        for (int i = 0; i < n; ++i) {
            RECT r;
            if (!Header_GetItemRect(h, i, &r)) continue;
            HDITEMW item{};
            wchar_t text[128] = {};
            item.mask = HDI_TEXT | HDI_FORMAT;
            item.pszText = text;
            item.cchTextMax = (int)std::size(text);
            Header_GetItem(h, i, &item);
            // Column divider.
            FillSolid(dc, {r.right - 1, r.top + (foobar ? 0 : S(6)), r.right, r.bottom - (foobar ? 0 : S(6))}, divider);
            UINT fmt = item.fmt & (HDF_LEFT | HDF_CENTER | HDF_RIGHT);
            UINT dt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
            dt |= fmt == HDF_CENTER ? DT_CENTER : fmt == HDF_RIGHT ? DT_RIGHT : DT_LEFT;
            RECT tr = r;
            tr.left += S(5);
            tr.right -= S(5);
            bool sorted = (item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) != 0;
            if (sorted) tr.right -= S(14);
            SetTextColor(dc, foobar ? Ref(kFoobarChromeText) : sorted ? TextColor() : SubColor());
            DrawTextW(dc, text, -1, &tr, dt);
            if (sorted) {
                bool up = (item.fmt & HDF_SORTUP) != 0;
                int cx = r.right - S(12), cy = (r.top + r.bottom) / 2;
                POINT tri[3];
                if (up) {
                    tri[0] = {cx - S(4), cy + S(2)};
                    tri[1] = {cx + S(4), cy + S(2)};
                    tri[2] = {cx, cy - S(3)};
                } else {
                    tri[0] = {cx - S(4), cy - S(2)};
                    tri[1] = {cx + S(4), cy - S(2)};
                    tri[2] = {cx, cy + S(3)};
                }
                HBRUSH ab = CreateSolidBrush(Ref(P().text));
                HPEN ap = CreatePen(PS_SOLID, 1, Ref(P().text));
                HGDIOBJ ob = SelectObject(dc, ab), op = SelectObject(dc, ap);
                Polygon(dc, tri, 3);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(ab);
                DeleteObject(ap);
            }
        }
        FillSolid(dc, {rc.left, rc.bottom - 1, rc.right, rc.bottom}, Ref(P().border));
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, HeaderSubclass, 0);
    return DefSubclassProc(h, msg, wp, lp);
}

// Flat toolbar buttons: track hover so the owner draw can show a subtle hot state.
HWND g_hotButton = nullptr;
LRESULT CALLBACK ButtonSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE && g_hotButton != h) {
        HWND old = g_hotButton;
        g_hotButton = h;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        if (old) InvalidateRect(old, nullptr, FALSE);
        InvalidateRect(h, nullptr, FALSE);
    } else if (msg == WM_MOUSELEAVE && g_hotButton == h) {
        g_hotButton = nullptr;
        InvalidateRect(h, nullptr, FALSE);
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, ButtonSubclass, 0);
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        InvalidateRect(h, nullptr, FALSE);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Owner-drawn toolbar button. Glyphs are Segoe MDL2 Assets code points; the icon
// font size follows the button height so compact and big modes stay proportional.
void DrawToolbarButton(const DRAWITEMSTRUCT* di) {
    bool pressed = (di->itemState & ODS_SELECTED) != 0, hot = di->hwndItem == g_hotButton;
    int id = GetDlgCtrlID(di->hwndItem);
    // Shuffle, repeat and resume show that they are on with an accent glyph and a dot under it.
    bool active = (id == IDC_SHUFFLE && g.shuffle) || (id == IDC_REPEAT && g.repeat != Repeat::Off) ||
                  (id == IDC_RESUME && g.resumeOn);
    RECT r = di->rcItem;
    if (FoobarMode()) {
        DrawFoobarButton(di);
        return;
    }
    FillSolid(di->hDC, r, pressed ? Ref(P().select) : hot ? Ref(P().hover) : SurfaceColor());
    if (pressed || hot) {
        HGDIOBJ ob = SelectObject(di->hDC, GetStockObject(NULL_BRUSH));
        HGDIOBJ op = SelectObject(di->hDC, GetStockObject(DC_PEN));
        SetDCPenColor(di->hDC, Ref(P().border));
        Rectangle(di->hDC, r.left, r.top, r.right, r.bottom);
        SelectObject(di->hDC, ob);
        SelectObject(di->hDC, op);
    }
    // Segoe MDL2 Assets code points.
    const wchar_t* glyph = L"";
    switch (id) {
        case IDC_PREV: glyph = L"\xE892"; break;
        case IDC_PLAY: glyph = (g.playing != kNone || RadioPlaying()) && !g.engine.Paused() ? L"\xE769" : L"\xE768"; break;
        case IDC_NEXT: glyph = L"\xE893"; break;
        case IDC_SHUFFLE: glyph = L"\xE8B1"; break;
        case IDC_REPEAT: glyph = g.repeat == Repeat::One ? L"\xE8ED" : L"\xE8EE"; break;
        case IDC_RESUME: glyph = L"\xE81C"; break;  // History
    }
    COLORREF color = (di->itemState & ODS_DISABLED) ? Ref(P().faint) : active ? Ref(P().accent) : TextColor();
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, color);
    RECT gr = r;
    if (active) OffsetRect(&gr, 0, -S(2));
    int height = r.bottom - r.top;  // the glyph grows with the button (compact and big sizes)
    HGDIOBJ old = SelectObject(di->hDC, height >= S(46) ? g.fontIconsHuge : height >= S(34) ? g.fontIconsBig : g.fontIcons);
    DrawTextW(di->hDC, glyph, -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(di->hDC, old);
    if (active) {
        int cx = (r.left + r.right) / 2, cy = r.bottom - S(5), d = std::max(2, S(3));
        RECT dot{cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d};
        FillSolid(di->hDC, dot, color);
    }
    if (g.showFocusBoxes && (di->itemState & ODS_FOCUS)) {
        InflateRect(&r, -S(2), -S(2));
        DrawFocusRect(di->hDC, &r);
    }
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
