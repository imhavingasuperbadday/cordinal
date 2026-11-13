// The Library tab's view row and column browser on Linux, drawn with the SDL renderer as the Windows build
// draws them (app_browser.cpp): the four views' buttons and the browser toggle, then the browser's three
// panes (Genre | Album artist | Album by default). Each pane lists the values of its field among the tracks
// the panes to its left let through, with "All" on top. The visual views are libview_canvas.cpp.
#include "alternate_ui.h"
#include "libview.h"

#include "album_editions.h"
#include "text.h"

#include <bitset>
#include <cmath>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace lx {
namespace {

// --- browser ------------------------------------------------------------------------------------------
const char* const kFieldNames[] = {"Genre", "Album artist", "Artist", "Album", "Composer", "Year", "Label", "Grouping", "Folder"};
const char* const kFieldPlurals[] = {"genres", "album artists", "artists", "albums", "composers", "years", "labels", "groupings", "folders"};
static_assert(std::size(kFieldNames) == (size_t)BrowseField::Count);
static_assert(std::size(kFieldPlurals) == (size_t)BrowseField::Count);
const char* const kModeNames[] = {"Tracks", "Albums & Tracks", "Albums", "Artists"};

struct Pane {
    std::vector<StringPool::Handle> values;
    std::vector<uint32_t> counts;
    std::vector<StringPool::Handle> selected;
    float scroll = 0;
    int anchor = -1;  // the row shift-clicks extend from
    int cursor = 0;   // the row the keyboard moves from (0 is "All")
};
Pane g_panes[3];
int g_focusPane = -1;  // the pane the keyboard drives, -1 when the focus is elsewhere
float g_paneRowsH = 0;  // the panes' visible height, for paging and keeping the cursor in view
std::string g_typed;    // type to jump: what has been typed lately
Uint64 g_typedTick = 0;

// Per-tag splits ("A; B" -> A, B) and years, worked out once per library revision.
std::unordered_map<StringPool::Handle, std::vector<StringPool::Handle>> g_splitCache;
std::unordered_map<StringPool::Handle, StringPool::Handle> g_yearCache;
uint32_t g_cacheRevision = 0;

void SplitTo(StringPool::Handle h, std::vector<StringPool::Handle>& out) {
    std::string_view s = g_lib.View(h);
    if (!h || s.find(';') == std::string_view::npos) {
        out.push_back(h);
        return;
    }
    auto [it, added] = g_splitCache.emplace(h, std::vector<StringPool::Handle>());
    if (added) {
        for (size_t start = 0; start <= s.size();) {
            size_t sep = s.find(';', start);
            if (sep == std::string_view::npos) sep = s.size();
            std::string_view part = s.substr(start, sep - start);
            while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
            while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
            if (!part.empty() || it->second.empty()) it->second.push_back(g_lib.Intern(part));
            start = sep + 1;
        }
    }
    out.insert(out.end(), it->second.begin(), it->second.end());
}

StringPool::Handle YearOf(StringPool::Handle date) {
    std::string_view d = g_lib.View(date);
    if (d.size() <= 4 || !isdigit((unsigned char)d[0])) return date;
    auto [it, added] = g_yearCache.emplace(date, 0);
    if (added) it->second = g_lib.Intern(d.substr(0, 4));
    return it->second;
}

}  // namespace

// Every value of `field` for one track: several for a multi-value tag, handle 0 for an empty one.
void BrowserValues(TrackId id, BrowseField field, std::vector<StringPool::Handle>& out) {
    const Track& t = g_lib[id];
    out.clear();
    switch (field) {
        case BrowseField::Genre: SplitTo(t.genre, out); break;
        case BrowseField::AlbumArtist: SplitTo(t.albumArtist ? t.albumArtist : t.artist, out); break;
        case BrowseField::Artist: SplitTo(t.artist, out); break;
        case BrowseField::Album: out.push_back(t.album); break;
        case BrowseField::Composer: SplitTo(t.composer, out); break;
        case BrowseField::Year: out.push_back(YearOf(t.date)); break;
        case BrowseField::Label: out.push_back(t.label); break;
        case BrowseField::Grouping: out.push_back(t.grouping); break;
        case BrowseField::Folder: out.push_back(t.dir); break;  // the full folder: namesakes stay apart
        default: break;
    }
    if (out.empty()) out.push_back(0);
}

namespace {

std::string PaneRowText(const Pane& pane, BrowseField field, int row) {
    if (row == 0) return "All (" + std::to_string(pane.values.size()) + " " + kFieldPlurals[(int)field] + ")";
    StringPool::Handle h = pane.values[row - 1];
    return h ? std::string(g_lib.View(h)) : "[Empty]";
}

struct PaneGeometry {
    Rect header, rows;
};
PaneGeometry PaneRect(const Rect& area, int i) {
    float w = std::floor(area.w / 3), x = area.x + i * w;
    if (i == 2) w = area.R() - x;
    const float headerH = S(24);
    return {{x, area.y, w - (i < 2 ? 1 : 0), headerH}, {x, area.y + headerH, w - (i < 2 ? 1 : 0), area.h - headerH}};
}

float PaneRowH() { return S(ui::kRow); }

void ClampPane(Pane& pane, const Rect& rows) {
    pane.scroll = std::clamp(pane.scroll, 0.0f, std::max(0.0f, (pane.values.size() + 1) * PaneRowH() - rows.h));
}

// A long folder keeps its first segment and its end, with "..." between, as Windows' path ellipsis.
std::string PathEllipsis(const std::string& text, float maxW) {
    if (TextWidth(Font::Normal, text) <= maxW) return text;
    size_t head = text.find('/', 1);
    if (head == std::string::npos) return text;
    const std::string prefix = text.substr(0, head + 1) + "...";
    for (size_t cut = text.find('/', head + 1); cut != std::string::npos; cut = text.find('/', cut + 1)) {
        std::string tail = text.substr(cut);
        if (TextWidth(Font::Normal, prefix + tail) <= maxW) return prefix + tail;
    }
    return text;  // not even the last folder fits: the plain ellipsis at the end will do
}

void BrowserChanged() {
    g.selected.assign(g.rows.size(), 0);
    g.focus = g.anchor = -1;
    g.scroll = 0;
    RebuildRows();
}

}  // namespace

// --- view row and browser --------------------------------------------------------------------------
const char* ViewButtonLabel(int i) { return i < (int)LibraryMode::Count ? kModeNames[i] : "Browser"; }

void BrowserShowValue(BrowseField field, StringPool::Handle value) {
    int pane = 1;
    for (int i = 0; i < 3; ++i)
        if (g.settings.browserFields[i] == (uint8_t)field) pane = i;
    g.settings.browserFields[pane] = (uint8_t)field;
    for (Pane& p : g_panes) p.selected.clear();
    g_panes[pane].selected.push_back(value);
    g.settings.browserVisible = true;
}

void ToggleBrowser() {
    g.settings.browserVisible = !g.settings.browserVisible;
    if (!g.settings.browserVisible)  // a hidden browser filters nothing
        for (Pane& pane : g_panes) pane.selected.clear();
    SaveSettings();
    BrowserChanged();
}

bool BrowserShown() {
    if (AlternateMode()) return false;
    return g.settings.browserVisible && !g.playlists.empty() && g.playlists[g.active].kind == PlaylistKind::Library;
}

float BrowserHeight() { return S((float)g.settings.browserHeight); }
void SetBrowserHeight(float pixels) { g.settings.browserHeight = std::clamp((int)std::lround(pixels / g.scale), 60, 2000); }

void FilterLibraryTracks(std::vector<TrackId>& items) {
    if (g_cacheRevision != g_lib.Revision()) {
        g_splitCache.clear();
        g_yearCache.clear();
        g_cacheRevision = g_lib.Revision();
    }
    if (!g.settings.browserVisible) return;
    std::vector<StringPool::Handle> vals, keys;
    for (int i = 0; i < 3; ++i) {
        Pane& pane = g_panes[i];
        const BrowseField field = (BrowseField)g.settings.browserFields[i];
        keys.clear();
        for (TrackId id : items) {
            BrowserValues(id, field, vals);
            keys.insert(keys.end(), vals.begin(), vals.end());
        }
        std::sort(keys.begin(), keys.end());
        std::vector<std::pair<StringPool::Handle, uint32_t>> counts;
        for (size_t a = 0; a < keys.size();) {
            size_t b = a + 1;
            while (b < keys.size() && keys[b] == keys[a]) ++b;
            counts.emplace_back(keys[a], (uint32_t)(b - a));
            a = b;
        }
        auto countOf = [&](StringPool::Handle h) -> const uint32_t* {
            auto it = std::lower_bound(counts.begin(), counts.end(), h, [](const auto& e, StringPool::Handle x) { return e.first < x; });
            return it != counts.end() && it->first == h ? &it->second : nullptr;
        };
        // Natural order, the empty value first (as "[Empty]").
        std::vector<std::wstring> text;
        for (auto& [h, n] : counts) text.push_back(h ? g_lib.Str(h) : std::wstring());
        std::vector<size_t> order(counts.size());
        for (size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (text[a].empty() != text[b].empty()) return text[a].empty();
            return StrCmpLogicalW(text[a].c_str(), text[b].c_str()) < 0;
        });
        pane.values.clear();
        pane.counts.clear();
        for (size_t k : order) {
            pane.values.push_back(counts[k].first);
            pane.counts.push_back(counts[k].second);
        }
        // A selected value that no longer exists upstream drops out of the selection.
        std::erase_if(pane.selected, [&](StringPool::Handle h) { return !countOf(h); });
        if (!pane.selected.empty()) {
            std::unordered_set<StringPool::Handle> sel(pane.selected.begin(), pane.selected.end());
            std::erase_if(items, [&](TrackId id) {
                BrowserValues(id, field, vals);
                return std::none_of(vals.begin(), vals.end(), [&](StringPool::Handle x) { return sel.count(x) != 0; });
            });
        }
    }
}

// The panes as the Windows list views: a header with the field's name (click it to choose another
// field), "All" in bold on top, and each value with its count on the right.
void DrawBrowser(const Rect& area, const Palette& p, float mouseX, float mouseY) {
    Fill(area, p.bg);
    g_paneRowsH = area.h - S(24);
    for (int i = 0; i < 3; ++i) {
        const Pane& pane = g_panes[i];
        const BrowseField field = (BrowseField)g.settings.browserFields[i];
        PaneGeometry G = PaneRect(area, i);
        Fill(G.header, p.bg);
        DrawText(Font::Normal, kFieldNames[(int)field], G.header.x + S(6), TextY(G.header), G.header.In(mouseX, mouseY) ? p.text : p.sub,
                 G.header.w - S(12));
        Fill({G.header.x, G.header.B() - S(1), G.header.w, S(1)}, p.border);
        if (i < 2) Fill({G.header.R(), area.y, 1, area.h}, p.border);
        // The count column's edge, through the header and the rows, as the Windows list views draw it.
        Fill({G.header.R() - S(48) - S(1), area.y + S(4), 1, area.h - S(4)}, Mix(p.bg, p.border, 50));
        SDL_Rect clip{(int)G.rows.x, (int)G.rows.y, (int)G.rows.w, (int)G.rows.h};
        SDL_SetRenderClipRect(g.renderer, &clip);
        const float rh = PaneRowH();
        int first = (int)(pane.scroll / rh), rows = (int)pane.values.size() + 1;
        std::unordered_set<StringPool::Handle> sel(pane.selected.begin(), pane.selected.end());
        for (int row = first; row < rows && row <= first + (int)(G.rows.h / rh) + 1; ++row) {
            Rect r{G.rows.x, G.rows.y + row * rh - pane.scroll, G.rows.w, rh};
            bool selected = row == 0 ? pane.selected.empty() : sel.count(pane.values[row - 1]) != 0;
            if (selected) Fill(r, p.select);
            else if (r.In(mouseX, mouseY)) Fill(r, p.hover);
            const float countW = S(48);
            Font f = row == 0 ? Font::Bold : Font::Normal;
            std::string label = PaneRowText(pane, field, row);
            if (field == BrowseField::Folder && row > 0) label = PathEllipsis(label, r.w - countW - S(12));
            DrawText(f, label, r.x + S(6), TextY(r, f), selected ? p.selectText : p.text, r.w - countW - S(12));
            if (g_focusPane == i && row == pane.cursor) Frame({r.x, r.y, r.w - 1, r.h - 1}, p.sub);
            if (row > 0)
                DrawText(Font::Normal, std::to_string(pane.counts[row - 1]), r.R() - countW - S(6), TextY(r), selected ? p.selectText : p.sub,
                         countW, 2);
        }
        SDL_SetRenderClipRect(g.renderer, nullptr);
    }
}

// The selection a click or a key makes on one row: "All" clears, shift extends from the anchor, ctrl toggles.
static void SelectPaneRow(Pane& pane, int row, SDL_Keymod mod) {
    if (row == 0) {
        pane.selected.clear();
        pane.anchor = -1;
    } else {
        StringPool::Handle h = pane.values[row - 1];
        if ((mod & SDL_KMOD_SHIFT) && pane.anchor > 0) {
            if (!(mod & SDL_KMOD_CTRL)) pane.selected.clear();
            for (int r = std::min(pane.anchor, row); r <= std::max(pane.anchor, row); ++r) pane.selected.push_back(pane.values[r - 1]);
        } else if (mod & SDL_KMOD_CTRL) {
            auto it = std::find(pane.selected.begin(), pane.selected.end(), h);
            if (it != pane.selected.end()) pane.selected.erase(it);
            else pane.selected.push_back(h);
            pane.anchor = row;
        } else {
            pane.selected = {h};
            pane.anchor = row;
        }
    }
    BrowserChanged();
}

void BrowserMouseDown(const Rect& area, float x, float y, int button, int clicks, SDL_Keymod mod) {
    for (int i = 0; i < 3; ++i) {
        PaneGeometry G = PaneRect(area, i);
        if (G.header.In(x, y)) {
            std::vector<std::string> labels(std::begin(kFieldNames), std::end(kFieldNames));
            OpenChoice(G.header.x, G.header.B(), labels, g.settings.browserFields[i], [i](int f) {
                g.settings.browserFields[i] = (uint8_t)f;
                g_panes[i].selected.clear();
                g_panes[i].scroll = 0;
                SaveSettings();
                BrowserChanged();
            });
            return;
        }
        if (!G.rows.In(x, y) || button != SDL_BUTTON_LEFT) continue;
        Pane& pane = g_panes[i];
        int row = (int)((y - G.rows.y + pane.scroll) / PaneRowH());
        if (row < 0 || row > (int)pane.values.size()) return;
        if (clicks >= 2) {  // a value's double-click plays everything under it
            if (!g.rows.empty()) PlayRow(0);
            return;
        }
        g_focusPane = i;
        pane.cursor = row;
        SDL_StartTextInput(g.window);  // type to jump
        SelectPaneRow(pane, row, mod);
        return;
    }
}

void BrowserBlur() {
    if (g_focusPane < 0) return;
    g_focusPane = -1;
    if (!g.searchFocused) SDL_StopTextInput(g.window);
    g.redraw = true;
}

bool BrowserFocused() { return g_focusPane >= 0 && BrowserShown(); }

static void EnsureCursorVisible(Pane& pane) {
    const float rh = PaneRowH(), top = pane.cursor * rh;
    if (top < pane.scroll) pane.scroll = top;
    else if (top + rh > pane.scroll + g_paneRowsH) pane.scroll = top + rh - g_paneRowsH;
    pane.scroll = std::clamp(pane.scroll, 0.0f, std::max(0.0f, (pane.values.size() + 1) * rh - g_paneRowsH));
}

static void MovePaneCursor(Pane& pane, int row, bool extend) {
    row = std::clamp(row, 0, (int)pane.values.size());
    pane.cursor = row;
    EnsureCursorVisible(pane);
    SelectPaneRow(pane, row, extend ? SDL_KMOD_SHIFT : 0);
}

bool BrowserKey(const SDL_KeyboardEvent& e) {
    if (!BrowserFocused()) return false;
    Pane& pane = g_panes[g_focusPane];
    const bool shift = e.mod & SDL_KMOD_SHIFT;
    const int last = (int)pane.values.size(), page = std::max(1, (int)(g_paneRowsH / PaneRowH()) - 1);
    pane.cursor = std::clamp(pane.cursor, 0, last);
    auto focusPane = [&](int i) {
        g_focusPane = std::clamp(i, 0, 2);
        g_panes[g_focusPane].cursor = std::clamp(g_panes[g_focusPane].cursor, 0, (int)g_panes[g_focusPane].values.size());
    };
    switch (e.key) {
        case SDLK_UP: MovePaneCursor(pane, pane.cursor - 1, shift); return true;
        case SDLK_DOWN: MovePaneCursor(pane, pane.cursor + 1, shift); return true;
        case SDLK_PAGEUP: MovePaneCursor(pane, pane.cursor - page, shift); return true;
        case SDLK_PAGEDOWN: MovePaneCursor(pane, pane.cursor + page, shift); return true;
        case SDLK_HOME: MovePaneCursor(pane, 0, shift); return true;
        case SDLK_END: MovePaneCursor(pane, last, shift); return true;
        case SDLK_LEFT: focusPane(g_focusPane - 1); return true;
        case SDLK_RIGHT: focusPane(g_focusPane + 1); return true;
        case SDLK_TAB: focusPane((g_focusPane + (shift ? 2 : 1)) % 3); return true;
        case SDLK_ESCAPE: BrowserBlur(); return true;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            if (!g.rows.empty()) PlayRow(0);
            return true;
        case SDLK_SPACE:  // a space inside what is being typed; otherwise pause as everywhere else
            return !g_typed.empty() && SDL_GetTicks() - g_typedTick < 1000;
        default: return false;
    }
}

// Type to jump: the first value starting with what was typed in the last second.
void BrowserText(const char* text) {
    if (!BrowserFocused() || !text) return;
    Pane& pane = g_panes[g_focusPane];
    const Uint64 now = SDL_GetTicks();
    if (now - g_typedTick > 1000) g_typed.clear();
    g_typed += text;
    g_typedTick = now;
    auto lower = [](unsigned char c) { return (char)(c < 128 ? std::tolower(c) : c); };
    for (int i = 0; i < (int)pane.values.size(); ++i) {
        std::string_view v = pane.values[i] ? g_lib.View(pane.values[i]) : std::string_view();
        if (v.size() < g_typed.size()) continue;
        bool match = true;
        for (size_t k = 0; k < g_typed.size() && match; ++k) match = lower(v[k]) == lower(g_typed[k]);
        if (!match) continue;
        MovePaneCursor(pane, i + 1, false);
        break;
    }
    g.redraw = true;
}

void BrowserWheel(const Rect& area, float x, float y, float dy) {
    for (int i = 0; i < 3; ++i) {
        PaneGeometry G = PaneRect(area, i);
        if (!Rect{G.header.x, area.y, G.header.w, area.h}.In(x, y)) continue;
        g_panes[i].scroll -= dy * PaneRowH() * 3;
        ClampPane(g_panes[i], G.rows);
        g.redraw = true;
    }
}

}  // namespace lx
