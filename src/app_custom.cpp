// Customize layout: the normal window's own arrangement (View > Customize layout, a right-click on
// the player, or the player's handle). The player - transport, shuffle / repeat, the waveform seek
// bar, the times and the volume - docks at any edge of the window:
//   Top / Bottom  a band across the window, the waveform inline between the buttons and the volume
//                 or on a line of its own under them, slim, medium or tall
//   Left / Right  a column: the cover, title and artist at its head, the buttons under them, then
//                 the waveform running down the column, the volume and the level chip at its foot
// Every part can be hidden, as can the now playing line, the level chip, the playlist tabs, the
// search box, the Library's view row, the status bar and the menu bar (Alt or a right-click still
// opens the menus). Up next can join the lyrics as a side panel, on either side. Presets set the
// whole arrangement at once. Drag the player's handle (the dots at its start) to an edge to dock it
// there; click it for this menu.
#include "app.h"

namespace app {

namespace {

constexpr int kWaveHeights[] = {26, 44, 72};     // the band's waveform height per size (96 DPI)
constexpr int kColumnWidths[] = {184, 216, 260};  // the column's width per size
constexpr int kGrip = ui::kGrip;                  // the handle's depth

struct Part {
    bool MainLayout::*on;
    const wchar_t* name;
    bool separatorAfter;
    const wchar_t* key;  // in a pack's layout.ini
};
// The menu's show / hide items, in menu order. The index is the command's offset from
// IDM_LAYOUT_PART_BASE and the key's position in [Layout] Parts, so entries are only ever appended.
const Part kLayoutParts[] = {
    {&MainLayout::transport, L"&Previous, play and next", false, L"transport"},
    {&MainLayout::order, L"&Shuffle and repeat", false, L"order"},
    {&MainLayout::wave, L"&Waveform seek bar", false, L"waveform"},
    {&MainLayout::times, L"&Times", false, L"times"},
    {&MainLayout::volume, L"&Volume", true, L"volume"},
    {&MainLayout::nowPlaying, L"&Now playing (cover, title and artist)", false, L"now_playing"},
    {&MainLayout::largeCover, L"&Large cover", false, L"large_cover"},
    {&MainLayout::points, L"Level and p&oints", true, L"points"},
    {&MainLayout::tabs, L"Playlist t&abs", false, L"tabs"},
    {&MainLayout::search, L"Searc&h box", false, L"search"},
    {&MainLayout::views, L"Library v&iew buttons", false, L"view_buttons"},
    {&MainLayout::upNext, L"&Up next panel", true, L"up_next"},
    {&MainLayout::status, L"Status &bar", false, L"status_bar"},
    {&MainLayout::menuBar, L"&Menu bar", true, L"menu_bar"},
    {&MainLayout::tabCounts, L"Track &counts on tabs", false, L"tab_counts"},
    {&MainLayout::statusDetails, L"Audio &details in the status bar", false, L"status_details"},
    {&MainLayout::hints, L"Playbac&k hints", false, L"hints"},
};
constexpr int kPartCount = (int)std::size(kLayoutParts);

struct Preset {
    const wchar_t* name;
    MainLayout (*make)();
};
const Preset kPresets[] = {
    {L"&Classic (the default)", [] { return MainLayout{}; }},
    {L"Player at the &bottom", [] {
         MainLayout l;
         l.player = Dock::Bottom;
         l.waveSize = 1;
         l.waveOwnRow = true;
         l.largeCover = true;
         return l;
     }},
    {L"Player on the &side", [] {
         MainLayout l;
         l.player = Dock::Left;
         l.waveSize = 1;
         l.upNext = true;
         return l;
     }},
    {L"&Waveform studio", [] {
         MainLayout l;
         l.waveSize = 2;
         l.waveOwnRow = true;
         l.largeCover = true;
         return l;
     }},
    {L"&Minimal", [] {
         MainLayout l;
         l.order = l.nowPlaying = l.points = l.views = l.status = false;
         return l;
     }},
};
constexpr int kPresetCount = (int)std::size(kPresets);
constexpr int kMaxPresets = IDM_WAVE_STYLE_BASE - IDM_LAYOUT_PRESET_BASE;  // built-in and pack presets together

// A pack's layout preset: the default arrangement, then what its [layout] section says. Words name
// the dock and the waveform size; a part it leaves out keeps the default, and 0, no or off hides one.
MainLayout LayoutOfPack(const IniSection& s) {
    MainLayout l;
    auto word = [&](const wchar_t* key, std::initializer_list<const wchar_t*> words, int def) {
        const std::wstring* v = s.Get(key);
        int i = 0;
        for (const wchar_t* w : words) {
            if (v && _wcsicmp(v->c_str(), w) == 0) return i;
            ++i;
        }
        return def;
    };
    auto flag = [&](const wchar_t* key, bool def) {
        const std::wstring* v = s.Get(key);
        if (!v || v->empty()) return def;
        return !(*v == L"0" || _wcsicmp(v->c_str(), L"no") == 0 || _wcsicmp(v->c_str(), L"off") == 0 ||
                 _wcsicmp(v->c_str(), L"false") == 0);
    };
    l.player = (Dock)word(L"player", {L"top", L"bottom", L"left", L"right"}, 0);
    l.waveSize = (uint8_t)word(L"waveform_size", {L"slim", L"medium", L"tall"}, 0);
    l.waveOwnRow = flag(L"waveform_row", false);
    l.panelsLeft = flag(L"side_panels_left", false);
    for (const Part& p : kLayoutParts) l.*p.on = flag(p.key, l.*p.on);
    return l;
}

// The presets menu's entries past the built-in ones.
std::vector<const PackLayout*> PackPresetsInMenu() {
    auto layouts = PackLayoutPresets();
    if (layouts.size() > (size_t)(kMaxPresets - kPresetCount)) layouts.resize(kMaxPresets - kPresetCount);
    return layouts;
}

bool Side() { return g.layout.player == Dock::Left || g.layout.player == Dock::Right; }
bool AnyPlayerPart() {
    const MainLayout& l = g.layout;
    return l.transport || l.order || l.wave || l.times || l.volume;
}

void Show(HWND h, bool on) { ShowWindow(h, on ? SW_SHOWNA : SW_HIDE); }

}  // namespace

// The times read toward the waveform in a band, and centered over and under it in the column.
void AlignTimes(bool column) {
    const struct { HWND h; DWORD band; } times[] = {{g.elapsed, SS_RIGHT}, {g.time, SS_LEFT}};
    for (const auto& t : times) {
        if (!t.h) continue;
        LONG_PTR style = GetWindowLongPtrW(t.h, GWL_STYLE);
        LONG_PTR want = (style & ~(LONG_PTR)SS_TYPEMASK) | (column ? SS_CENTER : t.band);
        if (want == style) continue;
        SetWindowLongPtrW(t.h, GWL_STYLE, want);
        InvalidateRect(t.h, nullptr, TRUE);
    }
}

namespace {

// The now playing line: the cover, the title over the artist line, the level chip on the right.
// Returns its height (0 when hidden).
int PlaceNowPlaying(HDWP& dwp, const RECT& r, int y) {
    const MainLayout& l = g.layout;
    bool chip = l.points && l.nowPlaying;
    Show(g.pointsChip, chip);
    if (!l.nowPlaying) return 0;
    int pad = S(ui::kPad), art = l.largeCover ? S(64) : S(ui::kCover), chipW = S(220);
    g.artRect = {r.left + pad, y, r.left + pad + art, y + art};
    int x0 = g.artRect.right + S(ui::kCoverGap), w = (int)r.right - x0 - pad - (chip ? chipW + S(8) : 0);
    int top = l.largeCover ? y + art / 2 - S(18) : y, line = l.largeCover ? S(18) : S(ui::kLineHeight);
    Place(dwp, g.npTitle, x0, top, w, line);
    Place(dwp, g.npSub, x0, top + line + (l.largeCover ? S(2) : 0), w, line);
    if (chip) Place(dwp, g.pointsChip, r.right - pad - chipW, y + (art - S(32)) / 2, chipW, S(32));
    return art;
}

// One row of the band at `y`, `h` tall: the handle, the buttons, the waveform with its times
// (unless it has a line of its own) and the volume.
void PlaceBandRow(HDWP& dwp, const RECT& r, int y, int h) {
    const MainLayout& l = g.layout;
    // The measurements are shared with the Linux port (ui_spec.h).
    int pad = S(ui::kPad), tb = S(ui::kButton), timeW = S(ui::kTimeWidth), volW = S(ui::kVolumeWidth);
    int by = y + (h - tb) / 2, ty = y + (h - S(ui::kTimeHeight)) / 2 + S(1);
    g.gripRect = {r.left, y, r.left + S(kGrip), y + h};
    int x = r.left + S(kGrip);
    if (l.transport)
        for (int i = 0; i < 3; ++i, x += tb) Place(dwp, g.buttons[i], x, by, tb, tb);
    if (l.order) {
        if (l.transport) x += S(ui::kOrderGap);  // a small gap between the transport and the order buttons
        for (int i = 3; i < 6; ++i, x += tb) Place(dwp, g.buttons[i], x, by, tb, tb);
    }
    if (l.transport || l.order) x += S(ui::kButtonsGap);
    int right = r.right - pad;
    if (l.volume) {
        int volX = right - volW;
        Place(dwp, g.volume, volX, by, volW, tb);
        Place(dwp, g.speaker, volX - S(ui::kSpeakerWidth), by + S(4), S(ui::kSpeakerWidth), S(ui::kTimeHeight));
        right = volX - S(ui::kVolumeGap);
    }
    if (l.wave && l.waveOwnRow) return;
    if (l.times) {
        Place(dwp, g.elapsed, x, ty, timeW, S(ui::kTimeHeight));
        Place(dwp, g.time, right - timeW, ty, timeW, S(ui::kTimeHeight));
        x += timeW + S(ui::kTimeGap);
        right -= timeW + S(ui::kTimeGap);
    }
    if (l.wave) Place(dwp, g.seek, x, y, right - x, h);
}

// The player across the top or bottom of `r`; returns what is left for the Library.
RECT LayoutBand(HDWP& dwp, RECT r) {
    const MainLayout& l = g.layout;
    bool player = AnyPlayerPart(), ownRow = l.wave && l.waveOwnRow;
    int height = ui::kBandWaveHeights[std::clamp(g.bandWaveSize, 0, 5)];
    int pad = S(4), timeW = S(58), waveH = S(height ? height : kWaveHeights[std::min<int>(l.waveSize, 2)]);
    int rowH = ownRow || !l.wave ? S(26) : std::max(S(26), waveH);
    int playerH = player ? rowH + (ownRow ? S(4) + waveH : 0) : 0;
    int art = l.nowPlaying ? (l.largeCover ? S(64) : S(32)) : 0;
    // Its own line: the waveform under the buttons, as wide as the window, between the times.
    auto waveRow = [&](int y) {
        int x = r.left + S(kGrip), right = r.right - pad, ty = y + (waveH - S(18)) / 2 + S(1);
        if (l.times) {
            Place(dwp, g.elapsed, x, ty, timeW, S(18));
            Place(dwp, g.time, right - timeW, ty, timeW, S(18));
            x += timeW + S(6);
            right -= timeW + S(6);
        }
        Place(dwp, g.seek, x, y, right - x, waveH);
    };
    if (l.player == Dock::Top) {
        // The classic order: the player, the now playing line, then the Library.
        int y = r.top + S(ui::kBandTop);
        if (player) {
            PlaceBandRow(dwp, r, y, rowH);
            if (ownRow) waveRow(y + rowH + S(4));
            y += playerH + S(ui::kBandGap);
        } else {
            y = r.top + S(4);
        }
        if (art) y += PlaceNowPlaying(dwp, r, y) + S(ui::kBandGap);
        r.top = y;
        return r;
    }
    // At the bottom the now playing line sits on the player, over a line dividing them from the Library.
    int zone = (art ? S(6) + art : 0) + (player ? S(4) + playerH + S(4) : 0);
    if (!zone) return r;
    int top = r.bottom - zone, y = top + S(6);
    g.dividers.push_back({r.left, top, r.right, top + 1});
    if (art) y += PlaceNowPlaying(dwp, r, y);
    if (player) {
        y = r.bottom - S(4) - playerH;
        PlaceBandRow(dwp, r, y, rowH);
        if (ownRow) waveRow(y + rowH + S(4));
    }
    r.bottom = top;
    return r;
}

// The player as a column down the left or right of `r`; returns what is left for the Library.
RECT LayoutColumn(HDWP& dwp, RECT r) {
    const MainLayout& l = g.layout;
    int colW = std::min(S(kColumnWidths[std::min<int>(l.waveSize, 2)]), (int)(r.right - r.left) / 2);
    bool left = l.player == Dock::Left;
    RECT col = left ? RECT{r.left, r.top, r.left + colW, r.bottom} : RECT{r.right - colW, r.top, r.right, r.bottom};
    if (left) r.left = col.right + 1;
    else r.right = col.left - 1;
    g.dividers.push_back(left ? RECT{col.right, col.top, col.right + 1, col.bottom} : RECT{col.left - 1, col.top, col.left, col.bottom});
    int in = S(12), x0 = col.left + in, w = colW - 2 * in;
    g.gripRect = {col.left, col.top, col.right, col.top + S(kGrip)};
    int y = col.top + S(kGrip) + S(2);
    // The head: a large cover, then the title and the artist line under it.
    if (l.nowPlaying) {
        // At most the size the cover is loaded at (app_art.cpp), and a third of the column, so a
        // short window still leaves the waveform room to run.
        int art = std::max(0, std::min({w, S(176), (int)(col.bottom - col.top) / 3}));
        g.artRect = {col.left + (colW - art) / 2, y, col.left + (colW - art) / 2 + art, y + art};
        y += art + S(8);
        Place(dwp, g.npTitle, x0, y, w, S(18));
        Place(dwp, g.npSub, x0, y + S(19), w, S(18));
        y += S(19) + S(18) + S(10);
    }
    int count = (l.transport ? 3 : 0) + (l.order ? 3 : 0), gap = l.transport && l.order ? S(6) : 0;
    if (count) {
        int b = std::clamp((w - gap) / count, S(26), S(36));
        int x = col.left + (colW - count * b - gap) / 2;
        if (l.transport)
            for (int i = 0; i < 3; ++i, x += b) Place(dwp, g.buttons[i], x, y, b, b);
        x += gap;
        if (l.order)
            for (int i = 3; i < 6; ++i, x += b) Place(dwp, g.buttons[i], x, y, b, b);
        y += b + S(10);
    }
    // The foot, from the bottom up: the level chip, then the volume.
    int bottom = col.bottom - S(6);
    bool chip = l.points && l.nowPlaying;
    Show(g.pointsChip, chip);
    if (chip) {
        Place(dwp, g.pointsChip, x0, bottom - S(32), w, S(32));
        bottom -= S(32) + S(8);
    }
    if (l.volume) {
        Place(dwp, g.speaker, x0, bottom - S(22), S(20), S(18));
        Place(dwp, g.volume, x0 + S(22), bottom - S(26), w - S(22), S(26));
        bottom -= S(26) + S(8);
    }
    // The waveform runs down what is left, the elapsed time over it and the time left under it.
    if (l.times) {
        Place(dwp, g.elapsed, x0, y, w, S(18));
        Place(dwp, g.time, x0, bottom - S(18), w, S(18));
        y += S(18) + S(4);
        bottom -= S(18) + S(4);
    }
    if (l.wave) Place(dwp, g.seek, x0, y, w, bottom - y);
    return r;
}

// Dragging the handle: a translucent band shows where the player would go.
struct Drag {
    bool on = false, moved = false;
    POINT start{};
    Dock target = Dock::Top;
    HWND preview = nullptr;
} drag;

const wchar_t kPreviewClass[] = L"CordinalDockPreview";

LRESULT CALLBACK PreviewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        FillSolid((HDC)wp, rc, Ref(P().accent));
        return 1;
    }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}

// The edge of the client area nearest the cursor, each measured against the window's own size.
Dock DockAt(POINT pt) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    double w = std::max(1L, rc.right), h = std::max(1L, rc.bottom);
    // The sides need the cursor nearer: a column is the bigger change.
    double d[] = {pt.y / h, (h - pt.y) / h, pt.x / w * 1.6, (w - pt.x) / w * 1.6};
    return (Dock)(std::min_element(std::begin(d), std::end(d)) - std::begin(d));
}

// Where the player would sit if dropped at `dock`, in screen coordinates.
RECT PreviewRect(Dock dock) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    int depth = dock == Dock::Top || dock == Dock::Bottom ? S(76) : S(kColumnWidths[std::min<int>(g.layout.waveSize, 2)]);
    RECT r = rc;
    switch (dock) {
        case Dock::Top: r.bottom = r.top + depth; break;
        case Dock::Bottom: r.top = r.bottom - depth; break;
        case Dock::Left: r.right = r.left + depth; break;
        default: r.left = r.right - depth; break;
    }
    MapWindowPoints(g.hwnd, nullptr, (POINT*)&r, 2);
    return r;
}

void EndDrag() {
    drag.on = false;
    if (drag.preview) DestroyWindow(drag.preview);
    drag.preview = nullptr;
}

std::wstring PartKey(const MainLayout& layout) {
    std::wstring s;
    for (const Part& p : kLayoutParts) s += layout.*p.on ? L'1' : L'0';
    return s;
}

}  // namespace

bool SeekVertical() { return g.size == ViewSize::Normal && Side() && !ApolloMode() && !FoobarMode(); }

// [Layout]: Player (Dock), Wave (size), WaveRow, PanelsLeft, and Parts: one 0/1 per kLayoutParts
// entry, so a part added later keeps its default in an older file. Revision 1 made the level and
// points chip opt-in: a file saved before it had the chip on by default, so it starts hidden once.
constexpr int kLayoutRevision = 1;

void LoadMainLayout() {
    MainLayout& l = g.layout;
    l = MainLayout{};  // missing/new parts retain their defaults when reloading a profile
    l.player = (Dock)std::clamp(IniInt(L"Layout", L"Player", 0), 0, (int)Dock::Count - 1);
    l.waveSize = (uint8_t)std::clamp(IniInt(L"Layout", L"Wave", 0), 0, 2);
    l.waveOwnRow = IniInt(L"Layout", L"WaveRow", 0) != 0;
    l.panelsLeft = IniInt(L"Layout", L"PanelsLeft", 0) != 0;
    std::wstring parts = IniRead(L"Layout", L"Parts", L"");
    for (int i = 0; i < kPartCount && i < (int)parts.size(); ++i) l.*kLayoutParts[i].on = parts[i] == L'1';
    if (IniInt(L"Layout", L"Revision", 0) < 1) l.points = false;
    g.defaultLayout = l;
    LoadWaveLook();
}

// A layout as a pack's layout.ini writes it, read back by LayoutOfPack.
std::wstring LayoutIniText(const MainLayout& l) {
    const wchar_t* docks[] = {L"top", L"bottom", L"left", L"right"};
    const wchar_t* sizes[] = {L"slim", L"medium", L"tall"};
    std::wstring s = L"player = " + std::wstring(docks[(int)l.player]) + L"\nwaveform size = " + sizes[std::min<int>(l.waveSize, 2)] +
                     L"\nwaveform row = " + (l.waveOwnRow ? L"1" : L"0") + L"\nside panels left = " + (l.panelsLeft ? L"1" : L"0") + L"\n";
    for (const Part& p : kLayoutParts) s += std::wstring(p.key) + L" = " + (l.*p.on ? L"1" : L"0") + L"\n";
    return s;
}

void SaveMainLayout() {
    const MainLayout& l = g.defaultLayout;
    IniWrite(L"Layout", L"Player", std::to_wstring((int)l.player));
    IniWrite(L"Layout", L"Wave", std::to_wstring(l.waveSize));
    IniWrite(L"Layout", L"WaveRow", l.waveOwnRow ? L"1" : L"0");
    IniWrite(L"Layout", L"PanelsLeft", l.panelsLeft ? L"1" : L"0");
    IniWrite(L"Layout", L"Parts", PartKey(l));
    IniWrite(L"Layout", L"Revision", std::to_wstring(kLayoutRevision));
}

// A layout setting changed: the menu bar, the seek bar's orientation and every place follow.
void ApplyMainLayout() {
    SyncMenuBar();
    RefreshTabs();       // the counts on the tabs
    UpdateNowPlaying();  // the hints in the now playing line
    Layout();
    InvalidateRect(g.seek, nullptr, TRUE);
    FountainWake();
    SaveMainLayout();
}

// The normal window, inside `r` (the client area above the status bar).
void LayoutNormal(HDWP& dwp, RECT r) {
    const MainLayout& l = g.layout;
    for (int i = 0; i < 3; ++i) Show(g.buttons[i], l.transport);
    for (int i = 3; i < 6; ++i) Show(g.buttons[i], l.order);
    Show(g.seek, l.wave);
    for (HWND h : {g.elapsed, g.time}) Show(h, l.times);
    for (HWND h : {g.speaker, g.volume}) Show(h, l.volume);
    for (HWND h : {g.npTitle, g.npSub}) Show(h, l.nowPlaying);
    AlignTimes(Side());
    r = Side() ? LayoutColumn(dwp, r) : LayoutBand(dwp, r);
    if (!AnyPlayerPart() && !(Side() && l.nowPlaying)) g.gripRect = {};
    LayoutLibrary(dwp, r, true);
}

// The window's own paint under the controls: the dividers and the player's handle (a few dots).
void PaintLayoutChrome(HDC dc) {
    for (const RECT& d : g.dividers) FillSolid(dc, d, Ref(P().border));
    if (IsRectEmpty(&g.gripRect)) return;
    const RECT& r = g.gripRect;
    bool across = r.right - r.left > r.bottom - r.top;  // the column's handle lies across its head
    int dot = std::max(2, S(2)), step = dot + std::max(2, S(2)), cols = across ? 4 : 2, rows = across ? 2 : 3;
    int x = (r.left + r.right - (cols * step - (step - dot))) / 2, y = (r.top + r.bottom - (rows * step - (step - dot))) / 2;
    COLORREF c = Ref(drag.on ? P().accent : P().faint);
    for (int i = 0; i < cols; ++i)
        for (int j = 0; j < rows; ++j) FillSolid(dc, {x + i * step, y + j * step, x + i * step + dot, y + j * step + dot}, c);
}

// The handle's mouse messages (window coordinates). A click opens the layout menu; a drag docks the
// player at the edge it is let go nearest. True when the message was the handle's.
bool LayoutMouse(UINT msg, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
        case WM_LBUTTONDOWN:
            if (g.size != ViewSize::Normal || !PtInRect(&g.gripRect, pt)) return false;
            drag = {};
            drag.on = true;
            drag.start = pt;
            drag.target = g.layout.player;
            SetCapture(g.hwnd);
            InvalidateRect(g.hwnd, &g.gripRect, FALSE);
            return true;
        case WM_MOUSEMOVE:
            if (!drag.on) return false;
            if (!drag.moved && std::abs(pt.x - drag.start.x) + std::abs(pt.y - drag.start.y) < S(6)) return true;
            if (!drag.moved) {
                drag.moved = true;
                RegisterAppClass(kPreviewClass, PreviewProc, 0, false);
                drag.preview = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                               kPreviewClass, L"", WS_POPUP, 0, 0, 0, 0, g.hwnd, nullptr, g.inst, nullptr);
                if (drag.preview) SetLayeredWindowAttributes(drag.preview, 0, 80, LWA_ALPHA);
                drag.target = Dock::Count;  // forces the first placement below
            }
            if (Dock d = DockAt(pt); d != drag.target) {
                drag.target = d;
                RECT r = PreviewRect(d);
                if (drag.preview)
                    SetWindowPos(drag.preview, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            return true;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED: {
            if (!drag.on) return false;
            bool released = msg == WM_LBUTTONUP, moved = drag.moved;
            Dock target = drag.target;
            EndDrag();  // before ReleaseCapture: its WM_CAPTURECHANGED must find the drag over
            if (released) ReleaseCapture();
            InvalidateRect(g.hwnd, &g.gripRect, FALSE);
            if (!released) return true;
            if (!moved) {
                POINT at{g.gripRect.left, g.gripRect.bottom};
                ClientToScreen(g.hwnd, &at);
                ShowLayoutMenu(at);
            } else if (target != g.layout.player && target < Dock::Count) {
                g.layout.player = target;
                ApplyMainLayout();
            }
            return true;
        }
    }
    return false;
}

bool LayoutSetCursor(POINT pt) {
    if (g.size != ViewSize::Normal || (!drag.on && !PtInRect(&g.gripRect, pt))) return false;
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    return true;
}

// View > Customize layout, and the right-click menu of the player. Built fresh each time; the
// checks come from UpdateLayoutMenu.
HMENU LayoutMenu() {
    HMENU m = CreatePopupMenu();
    HMENU dock = CreatePopupMenu();
    const wchar_t* docks[] = {L"At the &top", L"At the &bottom", L"On the &left", L"On the &right"};
    for (int i = 0; i < (int)Dock::Count; ++i) AppendMenuW(dock, MF_STRING, IDM_LAYOUT_DOCK_BASE + i, docks[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)dock, L"&Player position");
    HMENU wave = CreatePopupMenu();
    const wchar_t* sizes[] = {L"&Slim", L"&Medium", L"&Tall"};
    if (Side()) {
        for (int i = 0; i < 3; ++i) AppendMenuW(wave, MF_STRING, IDM_LAYOUT_WAVE_BASE + i, sizes[i]);
    } else AppendMenuW(wave, MF_STRING, IDM_LAYOUT_WAVE_SIZE, L"&Size...");
    AppendMenuW(wave, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(wave, MF_STRING, IDM_LAYOUT_WAVE_ROW, L"On a line of its &own");
    AppendWaveLookMenu(wave);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)wave, L"Wa&veform");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < kPartCount; ++i) {
        AppendMenuW(m, MF_STRING, IDM_LAYOUT_PART_BASE + i, kLayoutParts[i].name);
        // The Library's own toggles join its group: the column browser and the lyrics after the
        // view buttons, and which side the side panels take after Up next.
        if (kLayoutParts[i].on == &MainLayout::views) {
            AppendMenuW(m, MF_STRING, IDC_VIEW4, L"Column b&rowser");
            AppendMenuW(m, MF_STRING, IDM_LYRICS, L"L&yrics panel\tCtrl+L");
        }
        if (kLayoutParts[i].on == &MainLayout::upNext) AppendMenuW(m, MF_STRING, IDM_LAYOUT_PANELS_LEFT, L"Side panels on the l&eft");
        if (kLayoutParts[i].separatorAfter) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU presets = CreatePopupMenu();
    for (int i = 0; i < kPresetCount; ++i) AppendMenuW(presets, MF_STRING, IDM_LAYOUT_PRESET_BASE + i, kPresets[i].name);
    auto packs = PackPresetsInMenu();
    if (!packs.empty()) AppendMenuW(presets, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < packs.size(); ++i)
        AppendMenuW(presets, MF_STRING, IDM_LAYOUT_PRESET_BASE + kPresetCount + i, packs[i]->name.c_str());
    AppendMenuW(m, MF_POPUP, (UINT_PTR)presets, L"Pre&sets");
    AppendMenuW(m, MF_STRING, IDM_LAYOUT_PRESET_BASE, L"Rese&t the layout");
    return m;
}

void UpdateLayoutMenu(HMENU m) {
    const MainLayout& l = g.layout;
    CheckMenuRadioItem(m, IDM_LAYOUT_DOCK_BASE, IDM_LAYOUT_DOCK_BASE + (int)Dock::Count - 1,
                       IDM_LAYOUT_DOCK_BASE + (int)l.player, MF_BYCOMMAND);
    if (Side()) CheckMenuRadioItem(m, IDM_LAYOUT_WAVE_BASE, IDM_LAYOUT_WAVE_BASE + 2,
                                  IDM_LAYOUT_WAVE_BASE + l.waveSize, MF_BYCOMMAND);
    auto check = [&](UINT id, bool on) { CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    auto enable = [&](UINT id, bool on) { EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    check(IDM_LAYOUT_WAVE_ROW, l.waveOwnRow);
    UpdateWaveLookMenu(m);
    check(IDM_LAYOUT_PANELS_LEFT, l.panelsLeft);
    check(IDC_VIEW4, g.browserVisible);
    for (int i = 0; i < kPartCount; ++i) check(IDM_LAYOUT_PART_BASE + i, l.*kLayoutParts[i].on);
    // Items that only mean something in some arrangements are grayed rather than hidden.
    enable(IDM_LAYOUT_WAVE_ROW, !Side() && l.wave);
    auto partId = [](bool MainLayout::*on) {
        int i = 0;
        while (i < kPartCount && kLayoutParts[i].on != on) ++i;
        return (UINT)(IDM_LAYOUT_PART_BASE + i);
    };
    enable(partId(&MainLayout::largeCover), !Side() && l.nowPlaying);  // the column's cover is always large
    enable(partId(&MainLayout::points), l.nowPlaying);                 // the chip sits with Now playing
    enable(IDC_VIEW4, LibraryActive());
    int preset = -1;
    for (int i = 0; i < kPresetCount && preset < 0; ++i)
        if (kPresets[i].make() == l) preset = i;
    auto packs = PackPresetsInMenu();
    for (size_t i = 0; i < packs.size() && preset < 0; ++i)
        if (LayoutOfPack(packs[i]->values) == l) preset = kPresetCount + (int)i;
    for (int i = 0; i < kPresetCount + (int)packs.size(); ++i) check(IDM_LAYOUT_PRESET_BASE + i, i == preset);
}

// OnCommand's share: true when `id` was a layout command.
bool LayoutCommand(int id) {
    if (id == IDM_LAYOUT_WAVE_SIZE) { ShowPreferences("view.band_wave_size"); return true; }
    MainLayout& l = g.layout;
    if (id >= IDM_LAYOUT_DOCK_BASE && id < IDM_LAYOUT_DOCK_BASE + (int)Dock::Count) {
        l.player = (Dock)(id - IDM_LAYOUT_DOCK_BASE);
    } else if (id >= IDM_LAYOUT_WAVE_BASE && id <= IDM_LAYOUT_WAVE_BASE + 2) {
        l.waveSize = (uint8_t)(id - IDM_LAYOUT_WAVE_BASE);
    } else if (id == IDM_LAYOUT_WAVE_ROW) {
        l.waveOwnRow = !l.waveOwnRow;
    } else if (id == IDM_LAYOUT_PANELS_LEFT) {
        l.panelsLeft = !l.panelsLeft;
    } else if (id >= IDM_LAYOUT_PART_BASE && id < IDM_LAYOUT_PART_BASE + kPartCount) {
        bool& on = l.*kLayoutParts[id - IDM_LAYOUT_PART_BASE].on;
        on = !on;
        if (&on == &l.menuBar && !on) ShowMessage(L"Menu bar hidden. Press Alt, or right-click the player, for the menus.");
    } else if (id >= IDM_LAYOUT_PRESET_BASE && id < IDM_LAYOUT_PRESET_BASE + kPresetCount) {
        l = kPresets[id - IDM_LAYOUT_PRESET_BASE].make();
    } else if (id >= IDM_LAYOUT_PRESET_BASE + kPresetCount && id < IDM_LAYOUT_PRESET_BASE + kMaxPresets) {
        auto packs = PackPresetsInMenu();
        size_t i = id - IDM_LAYOUT_PRESET_BASE - kPresetCount;
        if (i >= packs.size()) return true;
        l = LayoutOfPack(packs[i]->values);
    } else if (id == IDM_LAYOUT_MENUS) {
        POINT pt;
        GetCursorPos(&pt);
        ShowCompactMenu(pt);
        return true;
    } else {
        return false;
    }
    ApplyMainLayout();
    return true;
}

// The right-click menu of the normal window's player and its now playing line: the layout, and the
// menu bar's menus too while the bar is hidden.
void ShowLayoutMenu(POINT pt) {
    if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
    HMENU m = LayoutMenu();
    if (!GetMenu(g.hwnd)) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_LAYOUT_MENUS, L"Player me&nus...\tAlt");
    }
    UpdateLayoutMenu(m);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd) OnCommand(cmd, 0, nullptr);
}

}  // namespace app
