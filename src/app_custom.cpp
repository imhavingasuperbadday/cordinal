// The Layout menu: the normal window's own arrangement (the menu bar's Layout menu, a right-click on
// the player, or the player's handle). The player - transport, shuffle / repeat, the waveform seek
// bar, the times and the volume - docks at any edge of the window:
//   Top / Bottom  a band across the window, the waveform inline between the buttons and the volume
//                 or on a line of its own under them, slim, medium or tall
//   Left / Right  a column: the cover, title and artist at its head, the buttons under them, then
//                 the waveform running down the column, the volume and the level chip at its foot
// Every part can be hidden, as can the now playing line, the level chip, the playlist tabs, the
// search box, the Library's view row, the status bar and the menu bar (Alt or a right-click still
// opens the menus). Up next can join the lyrics as a side panel, on either side. Presets set the
// whole arrangement at once, a blank canvas hides every part to build up from, and Save this layout
// keeps up to eight of your own (settings.ini [Layout] Saved1 to Saved8). Drag the player's handle (the dots at its start) to an edge to dock it
// there; click it for this menu.
#include "app.h"
#include <commdlg.h>

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
    {&MainLayout::transport, L"Previous, play and ne&xt", false, L"transport"},
    {&MainLayout::order, L"S&huffle and repeat", false, L"order"},
    {&MainLayout::wave, L"&Waveform seek bar", false, L"waveform"},
    {&MainLayout::times, L"&Times", false, L"times"},
    {&MainLayout::volume, L"&Volume", true, L"volume"},
    {&MainLayout::nowPlaying, L"&Now playing (cover, title and artist)", false, L"now_playing"},
    {&MainLayout::largeCover, L"Lar&ge cover", false, L"large_cover"},
    {&MainLayout::points, L"&Level and points", true, L"points"},
    {&MainLayout::tabs, L"Playlist t&abs", false, L"tabs"},
    {&MainLayout::search, L"Search b&ox", false, L"search"},
    {&MainLayout::views, L"Library v&iew buttons", false, L"view_buttons"},
    {&MainLayout::upNext, L"&Up next panel", true, L"up_next"},
    {&MainLayout::status, L"Status &bar", false, L"status_bar"},
    {&MainLayout::menuBar, L"&Menu bar", true, L"menu_bar"},
    {&MainLayout::tabCounts, L"Track &counts on tabs", false, L"tab_counts"},
    {&MainLayout::statusDetails, L"Audio &details in the status bar", false, L"status_details"},
    {&MainLayout::hints, L"Playbac&k hints", false, L"hints"},
};
constexpr int kPartCount = (int)std::size(kLayoutParts);

// The built-in presets live in ui_spec.h, in a pack's words, shared with the Linux port.
constexpr int kPresetCount = (int)std::size(ui::kLayoutPresets);
constexpr int kMaxPresets = IDM_WAVE_STYLE_BASE - IDM_LAYOUT_PRESET_BASE;  // built-in and pack presets together
static_assert(ui::kSavedLayouts <= IDM_LAYOUT_FORGET_BASE - IDM_LAYOUT_SAVED_BASE);

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
    if (const std::wstring* v = s.Get(L"side_width")) l.sideWidth = std::clamp(_wtoi(v->c_str()), 180, 480);
    if (const std::wstring* v = s.Get(L"pad")) l.pad = std::clamp(_wtoi(v->c_str()), 0, 16);
    for (const Part& p : kLayoutParts) l.*p.on = flag(p.key, l.*p.on);
    return l;
}

// A layout written in a pack's words, where ';' may stand for the line breaks (a saved layout's line).
MainLayout LayoutOfText(std::string text) {
    std::replace(text.begin(), text.end(), ';', '\n');
    std::vector<IniSection> sections = ParseIni(text);
    return sections.empty() ? MainLayout{} : LayoutOfPack(sections.front());
}

// The built-in presets, read once.
const std::vector<MainLayout>& PresetLayouts() {
    static const std::vector<MainLayout> layouts = [] {
        std::vector<MainLayout> out;
        for (const ui::LayoutPreset& p : ui::kLayoutPresets) out.push_back(LayoutOfText(p.text));
        return out;
    }();
    return layouts;
}

const MainLayout& BlankCanvas() {
    static const MainLayout blank = LayoutOfText(ui::kBlankCanvas);
    return blank;
}

// Your saved layouts, [Layout] Saved1 to Saved8: empty where a slot is free.
std::wstring SavedKey(int slot) { return L"Saved" + std::to_wstring(slot + 1); }
std::wstring SavedName(int slot) { return L"My layout " + std::to_wstring(slot + 1); }
bool SavedLayout(int slot, MainLayout& out) {
    std::wstring text = IniRead(L"Layout", SavedKey(slot).c_str(), L"");
    if (text.empty()) return false;
    out = LayoutOfText(ToUtf8(text));
    return true;
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
    int pad = S(std::clamp(l.pad, 0, 16)), art = S(l.largeCover ? ui::kLargeCover : ui::kCover), chipW = S(220);
    g.artRect = {r.left + pad, y, r.left + pad + art, y + art};
    int x0 = g.artRect.right + S(ui::kCoverGap), w = (int)r.right - x0 - pad - (chip ? chipW + S(8) : 0);
    int line = S(l.largeCover ? ui::kLargeLineHeight : ui::kLineHeight), gap = l.largeCover ? S(ui::kLargeLineGap) : 0;
    int top = y + ui::NowPlayingTextTop(art, line, gap);
    Place(dwp, g.npTitle, x0, top, w, line);
    Place(dwp, g.npSub, x0, top + line + gap, w, line);
    if (chip) Place(dwp, g.pointsChip, r.right - pad - chipW, y + (art - S(32)) / 2, chipW, S(32));
    return art;
}

// One row of the band at `y`, `h` tall: the handle, the buttons, the waveform with its times
// (unless it has a line of its own) and the volume.
void PlaceBandRow(HDWP& dwp, const RECT& r, int y, int h) {
    const MainLayout& l = g.layout;
    // The measurements are shared with the Linux port (ui_spec.h).
    int pad = S(std::clamp(l.pad, 0, 16)), tb = S(ui::kButton), timeW = S(ui::kTimeWidth), volW = S(ui::kVolumeWidth);
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
    int pad = S(std::clamp(l.pad, 0, 16)), timeW = S(58), waveH = S(height ? height : kWaveHeights[std::min<int>(l.waveSize, 2)]);
    int rowH = ownRow || !l.wave ? S(26) : std::max(S(26), waveH);
    int playerH = player ? rowH + (ownRow ? S(4) + waveH : 0) : 0;
    int art = l.nowPlaying ? S(l.largeCover ? ui::kLargeCover : ui::kCover) : 0;
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
        g.playerRect = {r.left, r.top, r.right, y};
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
    g.playerRect = {r.left, top + 1, r.right, r.bottom};
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
    g.playerRect = col;
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
// Revision 2 turned the status bar's audio details back on by default: files saved while they were
// off by default (October 19 to November 19, 2026) show them once.
constexpr int kLayoutRevision = 2;

void LoadMainLayout() {
    MainLayout& l = g.layout;
    l = MainLayout{};  // missing/new parts retain their defaults when reloading a profile
    l.player = (Dock)std::clamp(IniInt(L"Layout", L"Player", 0), 0, (int)Dock::Count - 1);
    l.waveSize = (uint8_t)std::clamp(IniInt(L"Layout", L"Wave", 0), 0, 2);
    l.waveOwnRow = IniInt(L"Layout", L"WaveRow", 0) != 0;
    l.panelsLeft = IniInt(L"Layout", L"PanelsLeft", 0) != 0;
    l.sideWidth = std::clamp(IniInt(L"Layout", L"SideWidth", 300), 180, 480);
    l.pad = std::clamp(IniInt(L"Layout", L"Pad", ui::kPad), 0, 16);
    std::wstring parts = IniRead(L"Layout", L"Parts", L"");
    for (int i = 0; i < kPartCount && i < (int)parts.size(); ++i) l.*kLayoutParts[i].on = parts[i] == L'1';
    const int revision = IniInt(L"Layout", L"Revision", 0);
    if (revision < 1) l.points = false;
    if (revision < 2) l.statusDetails = true;
    g.defaultLayout = l;
    LoadWaveLook();
}

// A layout as a pack's layout.ini writes it, read back by LayoutOfPack.
std::wstring LayoutIniText(const MainLayout& l) {
    const wchar_t* docks[] = {L"top", L"bottom", L"left", L"right"};
    const wchar_t* sizes[] = {L"slim", L"medium", L"tall"};
    std::wstring s = L"player = " + std::wstring(docks[(int)l.player]) + L"\nwaveform size = " + sizes[std::min<int>(l.waveSize, 2)] +
                     L"\nwaveform row = " + (l.waveOwnRow ? L"1" : L"0") + L"\nside panels left = " + (l.panelsLeft ? L"1" : L"0") + L"\n";
    if (l.sideWidth != 300) s += L"side width = " + std::to_wstring(l.sideWidth) + L"\n";
    if (l.pad != ui::kPad) s += L"pad = " + std::to_wstring(l.pad) + L"\n";
    for (const Part& p : kLayoutParts) s += std::wstring(p.key) + L" = " + (l.*p.on ? L"1" : L"0") + L"\n";
    return s;
}

void SaveMainLayout() {
    const MainLayout& l = g.defaultLayout;
    IniWrite(L"Layout", L"Player", std::to_wstring((int)l.player));
    IniWrite(L"Layout", L"Wave", std::to_wstring(l.waveSize));
    IniWrite(L"Layout", L"WaveRow", l.waveOwnRow ? L"1" : L"0");
    IniWrite(L"Layout", L"PanelsLeft", l.panelsLeft ? L"1" : L"0");
    IniWrite(L"Layout", L"SideWidth", std::to_wstring(l.sideWidth));
    IniWrite(L"Layout", L"Pad", std::to_wstring(l.pad));
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

// Forward declarations for menus used by the live Layout Top Bar.
namespace {
HMENU PresetsMenu();
HMENU PartsMenu();
}

// Interactive Layout Top Bar: live one-click customization of presets, player dock, waveform,
// parts, Light/Dark theme & accent colour, and Save/Reset/Done.
namespace {
enum BarId {
    kBarPresets = 1,
    kBarDock0, kBarDock1, kBarDock2, kBarDock3,
    kBarWave0, kBarWave1, kBarWave2, kBarWaveRow,
    kBarParts,
    kBarLight, kBarDark, kBarAccent, kBarColours,
    kBarSave, kBarReset, kBarDone,
};

struct BarItem {
    RECT rect{};
    int id = 0;
    std::wstring label;
    bool active = false;
    bool primary = false;
    bool disabled = false;
    bool hasDot = false;
    Rgb dot = 0;
};

int barHover = 0;

std::vector<BarItem> BuildBarItems(int W) {
    std::vector<BarItem> out;
    const MainLayout& l = g.layout;
    const int barH = S(ui::kLayoutBarHeight), pad = S(8), bh = S(24), y = (barH - bh) / 2, gap = S(4);
    HDC dc = GetDC(g.hwnd);
    HGDIOBJ old = SelectObject(dc, g.font);
    auto measure = [&](const std::wstring& s, int extra = 0) {
        RECT r{};
        DrawTextW(dc, s.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        return (int)r.right + S(14) + extra;
    };

    // Right-aligned action buttons first so we know how much width remains on the left.
    const std::pair<int, const wchar_t*> rightDefs[] = {
        {kBarDone, L"Done"},
        {kBarReset, L"Reset"},
        {kBarSave, L"Save"},
    };
    int rx = W - pad;
    std::vector<BarItem> rightItems;
    for (const auto& d : rightDefs) {
        int bw = measure(d.second);
        if (rx - bw < pad) break;
        rx -= bw;
        rightItems.push_back({{rx, y, rx + bw, y + bh}, d.first, d.second, false, d.first == kBarDone, false});
        rx -= gap;
    }

    int x = pad;
    auto push = [&](int id, const std::wstring& label, bool active, bool hasDot = false, Rgb dot = 0, int space = 4) {
        int bw = measure(label, hasDot ? S(12) : 0);
        if (x + bw > rx) return false;
        out.push_back({{x, y, x + bw, y + bh}, id, label, active, false, false, hasDot, dot});
        x += bw + S(space);
        return true;
    };

    push(kBarPresets, L"Presets \x25BE", false, false, 0, 8);
    const wchar_t* docks[] = {L"Top", L"Bot", L"Left", L"Right"};
    for (int i = 0; i < 4; ++i) push(kBarDock0 + i, docks[i], (int)l.player == i, false, 0, i == 3 ? 8 : 2);
    const wchar_t* waves[] = {L"Slim", L"Med", L"Tall"};
    for (int i = 0; i < 3; ++i) push(kBarWave0 + i, waves[i], l.waveSize == i && l.wave, false, 0, 2);
    push(kBarWaveRow, L"Row", !Side() && l.wave && l.waveOwnRow, false, 0, 8);
    push(kBarParts, L"Parts \x25BE", false, false, 0, 8);
    const bool lightActive = g.themeMode == ThemeMode::Light || (g.themeMode == ThemeMode::System && !g.dark);
    const bool darkActive = g.themeMode == ThemeMode::Dark || (g.themeMode == ThemeMode::System && g.dark);
    push(kBarLight, L"Light", lightActive, false, 0, 2);
    push(kBarDark, L"Dark", darkActive, false, 0, 2);
    push(kBarAccent, L"Accent \x25BE", false, true, P().accent, 2);
    push(kBarColours, L"Colours...", false, false, 0, 4);

    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);
    for (const BarItem& item : rightItems) out.push_back(item);
    return out;
}

void ShowAccentPopup(POINT pt) {
    HMENU m = CreatePopupMenu();
    const Rgb cur = CustomThemeColors(g.dark).accent;
    for (int i = 0; i < (int)std::size(kAccentPresets); ++i) {
        const Rgb target = g.dark ? kAccentPresets[i].darkAccent : kAccentPresets[i].lightAccent;
        AppendMenuW(m, MF_STRING | (cur == target ? MF_CHECKED : MF_UNCHECKED), 100 + i, kAccentPresets[i].name);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 200, L"Custom colour...");
    AppendMenuW(m, MF_STRING, 201, L"Reset all colours for this mode");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 100 && cmd < 100 + (int)std::size(kAccentPresets)) {
        const AccentPreset& p = kAccentPresets[cmd - 100];
        CustomThemeColors(g.dark).accent = g.dark ? p.darkAccent : p.lightAccent;
        if (g.themeMode != ThemeMode::Light && g.themeMode != ThemeMode::Dark && g.themeMode != ThemeMode::System)
            g.themeMode = g.dark ? ThemeMode::Dark : ThemeMode::Light;
        ApplyTheme();
        SavePreferences();
    } else if (cmd == 200) {
        static COLORREF custom[16]{};
        CHOOSECOLORW cc{sizeof(cc), g.hwnd, nullptr, Ref(P().accent), custom, CC_RGBINIT | CC_FULLOPEN, 0, nullptr, nullptr};
        if (ChooseColorW(&cc)) {
            CustomThemeColors(g.dark).accent =
                ((Rgb)GetRValue(cc.rgbResult) << 16) | ((Rgb)GetGValue(cc.rgbResult) << 8) | (Rgb)GetBValue(cc.rgbResult);
            if (g.themeMode != ThemeMode::Light && g.themeMode != ThemeMode::Dark && g.themeMode != ThemeMode::System)
                g.themeMode = g.dark ? ThemeMode::Dark : ThemeMode::Light;
            ApplyTheme();
            SavePreferences();
        }
    } else if (cmd == 201) {
        OnCommand(g.dark ? IDM_THEME_RESET_DARK : IDM_THEME_RESET_LIGHT, 0, nullptr);
    }
}

void RunBarItem(const BarItem& item) {
    POINT pt{item.rect.left, item.rect.bottom};
    ClientToScreen(g.hwnd, &pt);
    switch (item.id) {
        case kBarPresets: {
            HMENU m = PresetsMenu();
            InsertMenuW(m, 0, MF_BYPOSITION | MF_STRING, IDM_LAYOUT_BLANK, L"Start from a &blank canvas");
            InsertMenuW(m, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
            UpdateLayoutMenu(m);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
            DestroyMenu(m);
            if (cmd) OnCommand(cmd, 0, nullptr);
            break;
        }
        case kBarDock0: case kBarDock1: case kBarDock2: case kBarDock3:
            OnCommand(IDM_LAYOUT_DOCK_BASE + (item.id - kBarDock0), 0, nullptr);
            break;
        case kBarWave0: case kBarWave1: case kBarWave2:
            if (!g.layout.wave) g.layout.wave = true;
            OnCommand(IDM_LAYOUT_WAVE_BASE + (item.id - kBarWave0), 0, nullptr);
            break;
        case kBarWaveRow:
            if (!Side()) OnCommand(IDM_LAYOUT_WAVE_ROW, 0, nullptr);
            break;
        case kBarParts: {
            HMENU m = PartsMenu();
            UpdateLayoutMenu(m);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
            DestroyMenu(m);
            if (cmd) OnCommand(cmd, 0, nullptr);
            break;
        }
        case kBarLight:
            OnCommand(IDM_THEME_BASE + (int)ThemeMode::Light, 0, nullptr);
            break;
        case kBarDark:
            OnCommand(IDM_THEME_BASE + (int)ThemeMode::Dark, 0, nullptr);
            break;
        case kBarAccent:
            ShowAccentPopup(pt);
            break;
        case kBarColours:
            ShowPreferences(g.dark ? "theme.dark_accent" : "theme.light_accent");
            break;
        case kBarSave:
            OnCommand(IDM_LAYOUT_SAVE, 0, nullptr);
            break;
        case kBarReset:
            OnCommand(IDM_LAYOUT_PRESET_BASE, 0, nullptr);
            break;
        case kBarDone:
            OnCommand(IDM_LAYOUT_BAR, 0, nullptr);
            break;
    }
}
}  // namespace

// The window's own paint under the controls: the dividers, the Layout Top Bar and the player's handle.
void PaintLayoutChrome(HDC dc) {
    if (g.layoutBar) {
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        int barH = S(ui::kLayoutBarHeight);
        RECT bar{0, 0, rc.right, barH};
        FillSolid(dc, bar, Ref(P().surface));
        FillSolid(dc, {0, barH - 1, rc.right, barH}, Ref(P().border));
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(dc, g.font);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        for (const BarItem& item : BuildBarItems(rc.right)) {
            bool hovered = item.id == barHover;
            Rgb fill = item.primary || item.active ? P().accent : hovered ? P().hover : P().surface2;
            Rgb border = item.primary || item.active ? P().accent : P().border;
            Rgb fg = item.primary || item.active ? TextOn(fill) : P().text;
            SetDCPenColor(dc, Ref(border));
            SetDCBrushColor(dc, Ref(fill));
            RoundRect(dc, item.rect.left, item.rect.top, item.rect.right, item.rect.bottom, S(5), S(5));
            RECT tr = item.rect;
            if (item.hasDot) {
                int ds = S(8), dx = tr.left + S(7), dy = (tr.top + tr.bottom - ds) / 2;
                SetDCPenColor(dc, Ref(P().border));
                SetDCBrushColor(dc, Ref(item.dot));
                Ellipse(dc, dx, dy, dx + ds, dy + ds);
                tr.left += ds + S(4);
            }
            SetTextColor(dc, Ref(fg));
            DrawTextW(dc, item.label.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldFont);
    }
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

// The handle's and Layout Top Bar's mouse messages (window coordinates).
bool LayoutMouse(UINT msg, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (g.size == ViewSize::Normal && g.layoutBar && !drag.on) {
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        int barH = S(ui::kLayoutBarHeight);
        if (msg == WM_MOUSEMOVE) {
            int nextHover = 0;
            if (pt.y >= 0 && pt.y < barH) {
                for (const BarItem& item : BuildBarItems(rc.right))
                    if (PtInRect(&item.rect, pt)) { nextHover = item.id; break; }
            }
            if (nextHover != barHover) {
                barHover = nextHover;
                RECT bar{0, 0, rc.right, barH};
                InvalidateRect(g.hwnd, &bar, FALSE);
            }
            if (pt.y >= 0 && pt.y < barH) return true;
        } else if (msg == WM_LBUTTONDOWN && pt.y >= 0 && pt.y < barH) {
            for (const BarItem& item : BuildBarItems(rc.right)) {
                if (PtInRect(&item.rect, pt)) {
                    RunBarItem(item);
                    return true;
                }
            }
            return true;
        }
    }
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
    if (g.size == ViewSize::Normal && g.layoutBar && !drag.on && pt.y >= 0 && pt.y < S(ui::kLayoutBarHeight)) {
        RECT rc;
        GetClientRect(g.hwnd, &rc);
        for (const BarItem& item : BuildBarItems(rc.right)) {
            if (PtInRect(&item.rect, pt)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return true;
            }
        }
    }
    if (g.size != ViewSize::Normal || (!drag.on && !PtInRect(&g.gripRect, pt))) return false;
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    return true;
}

namespace {

// Start from a preset: the built-in ones, the packs', then the ones you saved, and Reset the layout.
HMENU PresetsMenu() {
    HMENU presets = CreatePopupMenu();
    for (int i = 0; i < kPresetCount; ++i) AppendMenuW(presets, MF_STRING, IDM_LAYOUT_PRESET_BASE + i, ui::kLayoutPresets[i].name);
    auto packs = PackPresetsInMenu();
    if (!packs.empty()) AppendMenuW(presets, MF_SEPARATOR, 0, nullptr);
    for (size_t i = 0; i < packs.size(); ++i)
        AppendMenuW(presets, MF_STRING, IDM_LAYOUT_PRESET_BASE + kPresetCount + i, packs[i]->name.c_str());
    // Your own, each with its number as its access key.
    HMENU forget = CreatePopupMenu();
    bool any = false;
    MainLayout saved;
    for (int i = 0; i < ui::kSavedLayouts; ++i) {
        if (!SavedLayout(i, saved)) continue;
        if (!any) AppendMenuW(presets, MF_SEPARATOR, 0, nullptr);
        any = true;
        std::wstring name = L"My layout &" + std::to_wstring(i + 1);
        AppendMenuW(presets, MF_STRING, IDM_LAYOUT_SAVED_BASE + i, name.c_str());
        AppendMenuW(forget, MF_STRING, IDM_LAYOUT_FORGET_BASE + i, name.c_str());
    }
    AppendMenuW(presets, MF_SEPARATOR, 0, nullptr);
    if (any) AppendMenuW(presets, MF_POPUP, (UINT_PTR)forget, L"&Forget a saved layout");
    else DestroyMenu(forget);
    // The classic preset again, by the name people look for.
    AppendMenuW(presets, MF_STRING, IDM_LAYOUT_PRESET_BASE, L"&Reset the layout");
    return presets;
}

// Show: every part the normal window can hide, with the Library's own toggles in its group.
HMENU PartsMenu() {
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < kPartCount; ++i) {
        AppendMenuW(m, MF_STRING, IDM_LAYOUT_PART_BASE + i, kLayoutParts[i].name);
        // The column browser and the lyrics after the view buttons, and which side the side panels
        // take after Up next.
        if (kLayoutParts[i].on == &MainLayout::views) {
            AppendMenuW(m, MF_STRING, IDC_VIEW4, L"Column b&rowser");
            AppendMenuW(m, MF_STRING, IDM_LYRICS, L"L&yrics panel\tCtrl+L");
        }
        if (kLayoutParts[i].on == &MainLayout::upNext) AppendMenuW(m, MF_STRING, IDM_LAYOUT_PANELS_LEFT, L"Side panels on the l&eft");
        if (kLayoutParts[i].separatorAfter) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    return m;
}

// The menu bar's Layout menu, and the right-click menu of the player. Start from a preset or from a
// blank canvas, then dock the player, size the waveform and show each part you want; Save this layout
// keeps the result among the presets. The window's other arrangements follow: Up next, the window's
// size, Big mode's panels and the mini player. The checks come from UpdateLayoutMenu.
void FillLayoutMenu(HMENU m) {
    AppendMenuW(m, MF_STRING, IDM_LAYOUT_BAR, L"&Customize layout bar");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)PresetsMenu(), L"Start from a &preset");
    AppendMenuW(m, MF_STRING, IDM_LAYOUT_BLANK, L"Start from a &blank canvas");
    AppendMenuW(m, MF_STRING, IDM_LAYOUT_SAVE, L"Sa&ve this layout");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU dock = CreatePopupMenu();
    const wchar_t* docks[] = {L"At the &top", L"At the &bottom", L"On the &left", L"On the &right"};
    for (int i = 0; i < (int)Dock::Count; ++i) AppendMenuW(dock, MF_STRING, IDM_LAYOUT_DOCK_BASE + i, docks[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)dock, L"P&layer position");
    HMENU wave = CreatePopupMenu();
    const wchar_t* sizes[] = {L"&Slim", L"&Medium", L"&Tall"};
    if (Side()) {
        for (int i = 0; i < 3; ++i) AppendMenuW(wave, MF_STRING, IDM_LAYOUT_WAVE_BASE + i, sizes[i]);
    } else AppendMenuW(wave, MF_STRING, IDM_LAYOUT_WAVE_SIZE, L"&Size...");
    AppendMenuW(wave, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(wave, MF_STRING, IDM_LAYOUT_WAVE_ROW, L"On a line of its &own");
    AppendWaveLookMenu(wave);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)wave, L"Wave&form");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)PartsMenu(), L"&Show");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_QUEUE_SHOW, L"&Up next\tCtrl+Shift+Q");
    HMENU windowSizes = CreatePopupMenu();
    const wchar_t* sizeNames[] = {L"&Compact", L"&Normal", L"Big mode &plus", L"&Big mode"};
    for (int i = 0; i < (int)ViewSize::Count; ++i) AppendMenuW(windowSizes, MF_STRING, IDM_SIZE_BASE + i, sizeNames[i]);
    AppendMenuW(windowSizes, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(windowSizes, MF_STRING, IDM_SMALLER, L"S&maller\tCtrl+-");
    AppendMenuW(windowSizes, MF_STRING, IDM_LARGER, L"&Larger\tCtrl+=");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)windowSizes, L"Window si&ze");
    HMENU panels = CreatePopupMenu();
    const wchar_t* panelNames[] = {L"&Library", L"&Now playing", L"&Up next"};
    for (int i = 0; i < kPanelCount; ++i) AppendMenuW(panels, MF_STRING, IDM_PANEL_BASE + i, panelNames[i]);
    AppendMenuW(panels, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(panels, MF_STRING, IDM_PANELS_RESET, L"&Reset the layout");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)panels, L"Big mo&de panels");
    AppendMenuW(m, MF_STRING, IDM_MINI_PLAYER, L"&Mini player\tCtrl+Shift+M");
}

}  // namespace

HMENU LayoutMenu() {
    HMENU m = CreatePopupMenu();
    FillLayoutMenu(m);
    return m;
}

// The menu bar keeps its Layout menu between openings, but what it lists follows the layout (the
// waveform's sizes on a side, the layouts you saved), so it is filled again each time it opens.
void RefreshLayoutPopup(HMENU popup) {
    HMENU bar = MainMenuBar();
    if (!bar || !popup || GetSubMenu(bar, ui::kLayoutMenu) != popup) return;
    while (GetMenuItemCount(popup) > 0) DeleteMenu(popup, 0, MF_BYPOSITION);
    FillLayoutMenu(popup);
    ApplyShortcutLabels(popup);
}

void UpdateLayoutMenu(HMENU m) {
    const MainLayout& l = g.layout;
    CheckMenuRadioItem(m, IDM_LAYOUT_DOCK_BASE, IDM_LAYOUT_DOCK_BASE + (int)Dock::Count - 1,
                       IDM_LAYOUT_DOCK_BASE + (int)l.player, MF_BYCOMMAND);
    if (Side()) CheckMenuRadioItem(m, IDM_LAYOUT_WAVE_BASE, IDM_LAYOUT_WAVE_BASE + 2,
                                  IDM_LAYOUT_WAVE_BASE + l.waveSize, MF_BYCOMMAND);
    auto check = [&](UINT id, bool on) { CheckMenuItem(m, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED)); };
    auto enable = [&](UINT id, bool on) { EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    check(IDM_LAYOUT_BAR, g.layoutBar);
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
    // The start the window's layout still is, if any: a preset, the blank canvas or one of yours.
    int preset = -1;
    for (int i = 0; i < kPresetCount && preset < 0; ++i)
        if (PresetLayouts()[i] == l) preset = i;
    auto packs = PackPresetsInMenu();
    for (size_t i = 0; i < packs.size() && preset < 0; ++i)
        if (LayoutOfPack(packs[i]->values) == l) preset = kPresetCount + (int)i;
    for (int i = 0; i < kPresetCount + (int)packs.size(); ++i) check(IDM_LAYOUT_PRESET_BASE + i, i == preset);
    const bool blank = l == BlankCanvas();
    check(IDM_LAYOUT_BLANK, blank);
    int mine = -1;
    MainLayout saved;
    for (int i = 0; i < ui::kSavedLayouts; ++i) {
        bool same = mine < 0 && preset < 0 && !blank && SavedLayout(i, saved) && saved == l;
        if (same) mine = i;
        check(IDM_LAYOUT_SAVED_BASE + i, same);
    }
    // Saving is for a layout of your own, not one the menu already starts from.
    enable(IDM_LAYOUT_SAVE, preset < 0 && !blank && mine < 0);
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
        l = PresetLayouts()[id - IDM_LAYOUT_PRESET_BASE];
    } else if (id >= IDM_LAYOUT_PRESET_BASE + kPresetCount && id < IDM_LAYOUT_PRESET_BASE + kMaxPresets) {
        auto packs = PackPresetsInMenu();
        size_t i = id - IDM_LAYOUT_PRESET_BASE - kPresetCount;
        if (i >= packs.size()) return true;
        l = LayoutOfPack(packs[i]->values);
    } else if (id == IDM_LAYOUT_BLANK) {
        l = BlankCanvas();
        ShowMessage(L"A blank canvas. Add the parts you want from Layout > Show.");
    } else if (id == IDM_LAYOUT_SAVE) {
        int slot = -1;
        MainLayout saved;
        for (int i = 0; i < ui::kSavedLayouts && slot < 0; ++i)
            if (!SavedLayout(i, saved)) slot = i;
        if (slot < 0) {
            ShowMessage(L"Eight layouts are saved. Forget one under Layout > Start from a preset first.");
            return true;
        }
        std::wstring text = LayoutIniText(l);
        std::replace(text.begin(), text.end(), L'\n', L';');
        if (IniWrite(L"Layout", SavedKey(slot).c_str(), text))
            ShowMessage(SavedName(slot) + L" saved. It is under Layout > Start from a preset.");
        return true;
    } else if (id >= IDM_LAYOUT_SAVED_BASE && id < IDM_LAYOUT_SAVED_BASE + ui::kSavedLayouts) {
        MainLayout saved;
        if (!SavedLayout(id - IDM_LAYOUT_SAVED_BASE, saved)) return true;
        l = saved;
    } else if (id >= IDM_LAYOUT_FORGET_BASE && id < IDM_LAYOUT_FORGET_BASE + ui::kSavedLayouts) {
        IniDelete(L"Layout", SavedKey(id - IDM_LAYOUT_FORGET_BASE).c_str());
        ShowMessage(SavedName(id - IDM_LAYOUT_FORGET_BASE) + L" forgotten. The window keeps the layout it has.");
        return true;
    } else if (id == IDM_LAYOUT_BAR) {
        if (g.size != ViewSize::Normal) OnCommand(IDM_SIZE_BASE + (int)ViewSize::Normal, 0, nullptr);
        g.layoutBar = !g.layoutBar;
        Layout();
        return true;
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
