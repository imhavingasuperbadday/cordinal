// The Library tab's view row, column browser and visual views on Linux, drawn with the SDL renderer
// as the Windows build draws them (app_browser.cpp, libview.cpp):
//   Tracks          the track list (ui.cpp)
//   Albums & Tracks every album's cover (sized with the grid's covers) beside its heading and tracks
//   Albums          a cover grid in letter sections; clicking an album opens it in place, with its
//                   tracks next to a large cover
//   Artists         one tile per album artist; double-click shows their albums
// The browser's three panes (Genre | Album artist | Album by default) each list the values of their
// field among the tracks the panes to their left let through, with "All" on top. The views show what
// the Library's list would show (browser and search applied), grouped; their selection lives in
// g.selected like the list's, so menus and commands act on it.
#include "libview.h"

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

// --- views --------------------------------------------------------------------------------------------
struct Album {
    StringPool::Handle artist = 0, album = 0, date = 0, genre = 0;
    std::vector<TrackId> tracks;
    double seconds = 0;
    wchar_t letter = L'#';
};
struct Artist {
    StringPool::Handle name = 0;
    std::vector<int> albums;
};
enum class ItemKind { Section, Tile, Panel, AlbumGroup, ArtistTile };
struct Item {
    ItemKind kind;
    Rect rc;  // content coordinates: scrolling subtracts v.scroll
    int index;
};

struct Canvas {
    std::vector<Album> albums;
    std::vector<Artist> artists;
    std::vector<Item> items;
    std::vector<int> albumItem;  // album -> its tile or group item
    float contentHeight = 0, scroll = 0, laidWidth = -1, viewH = 0;
    bool layoutDirty = true;
    int expanded = -1;              // Albums: the open album
    TrackId expandedTrack = kNone;  // ... by its first track, across rebuilds
    std::vector<char> selAlbum, selArtist;
    std::unordered_set<TrackId> selTrack;
    int focus = -1;  // album or artist
    TrackId anchorTrack = kNone;
    uint64_t lastClick = 0;
    int lastClickItem = -1, lastClickRow = -1;
} v;

float F(float px) { return S(px); }
float RowH() { return F(26); }
float Margin() { return F(18); }
float TileArt() { return F((float)g.settings.coverSize); }
float TileH() { return TileArt() + F(44); }
float GroupArt() { return F((float)(g.settings.coverSize + g.settings.coverSize / 4)); }
float GroupHead() { return F(56); }
float GroupX(const Rect& rc) { return rc.x + GroupArt() + F(20); }
Rect GroupRow(const Rect& rc, int t) { return {GroupX(rc) - F(6), rc.y + GroupHead() + t * RowH(), rc.R() - (GroupX(rc) - F(6)), RowH()}; }

std::wstring AlbumTitle(const Album& a) {
    if (a.album) return g_lib.Str(a.album);
    std::wstring t;
    return a.tracks.size() == 1 && LookupField(a.tracks[0], L"title", t) ? t : L"Unknown album";
}
std::wstring ArtistName(StringPool::Handle h) { return h ? g_lib.Str(h) : L"Unknown artist"; }
std::wstring AlbumYear(const Album& a) { return a.date ? g_lib.Str(a.date).substr(0, 4) : L""; }

std::wstring AlbumSubtitle(const Album& a, bool year = true) {
    std::wstring s = ArtistName(a.artist);
    if (year && a.date) s += L"  \x2022  " + AlbumYear(a);
    if (a.genre) s += L"  \x2022  " + g_lib.Str(a.genre);
    s += L"  \x2022  " + std::to_wstring(a.tracks.size()) + (a.tracks.size() == 1 ? L" track, " : L" tracks, ") +
         FormatTotalDuration(a.seconds);
    return s;
}

wchar_t SectionLetter(StringPool::Handle artist) {
    std::wstring s = g_lib.Str(artist);
    size_t i = s.size() > 4 && CompareStringOrdinal(s.c_str(), 4, L"The ", 4, TRUE) == CSTR_EQUAL ? 4 : 0;
    for (; i < s.size(); ++i)
        if (iswalnum(s[i])) {
            wchar_t c = (wchar_t)towupper(s[i]);
            return iswalpha(c) ? c : L'#';
        }
    return L'#';
}

int PanelColumns(const Album& a, float width) {
    float listW = width - Margin() * 2 - F(40) - F(280);
    return a.tracks.size() > 10 && listW > F(760) ? 2 : 1;
}

float PanelHeight(const Album& a, float width) {
    int cols = PanelColumns(a, width);
    int rows = (int)(a.tracks.size() + cols - 1) / cols;
    return std::max(F(280) + F(40), F(84) + rows * RowH() + F(28));
}

void BuildGroups() {
    v.albums.clear();
    v.artists.clear();
    std::unordered_map<uint64_t, size_t> byKey;
    for (TrackId id : g.rows) {
        const Track& t = g_lib[id];
        StringPool::Handle artist = t.albumArtist ? t.albumArtist : t.artist;
        // Albums are album artist + album; tracks without an album group per artist.
        auto [it, added] = byKey.try_emplace((uint64_t)artist << 32 | t.album, v.albums.size());
        if (added) {
            Album a;
            a.artist = artist;
            a.album = t.album;
            a.date = t.date;
            a.genre = t.genre;
            a.letter = SectionLetter(artist);
            v.albums.push_back(std::move(a));
        }
        Album& a = v.albums[it->second];
        a.tracks.push_back(id);
        a.seconds += t.Duration();
        if (!a.genre) a.genre = t.genre;
    }
    if (g.settings.libraryMode == LibraryMode::Artists) {
        // "Toby Fox; Geoffrey Day" is two artists: the album shows under each of them.
        std::unordered_map<StringPool::Handle, uint32_t> byArtist;
        std::vector<StringPool::Handle> names;
        for (int i = 0; i < (int)v.albums.size(); ++i) {
            BrowserValues(v.albums[i].tracks[0], BrowseField::AlbumArtist, names);
            for (StringPool::Handle name : names) {
                auto [it, added] = byArtist.try_emplace(name, (uint32_t)v.artists.size());
                if (added) v.artists.push_back(Artist{name, {}});
                v.artists[it->second].albums.push_back(i);
            }
        }
        // Split names join the list where they belong alphabetically ("Unknown artist" last).
        std::vector<std::wstring> keys;
        for (const Artist& a : v.artists) keys.push_back(g_lib.Str(a.name));
        std::vector<int> order(v.artists.size());
        for (int i = 0; i < (int)order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            if (keys[a].empty() != keys[b].empty()) return keys[b].empty();
            return StrCmpLogicalW(keys[a].c_str(), keys[b].c_str()) < 0;
        });
        std::vector<Artist> sorted;
        for (int i : order) sorted.push_back(std::move(v.artists[i]));
        v.artists.swap(sorted);
    }
}

// Grid layout shared by album and artist tiles: columns that fill the width evenly.
void GridMetrics(float width, float itemW, int& cols, float& gap) {
    float avail = width - Margin() * 2;
    gap = F(16);
    cols = std::max(1, (int)((avail + gap) / (itemW + gap)));
    gap = cols > 1 ? std::min((avail - cols * itemW) / (cols - 1), F(32)) : 0;
}

void LayoutItems(float W) {
    v.items.clear();
    v.albumItem.assign(v.albums.size(), -1);
    float y = F(8);
    const LibraryMode mode = g.settings.libraryMode;
    if (mode == LibraryMode::Albums) {
        int cols;
        float gap;
        GridMetrics(W, TileArt(), cols, gap);
        // Letter sections, once there are enough albums per letter to be worth a heading.
        bool sections = g.playlists[g.active].sortColumn <= 0 && v.albums.size() >= 40;
        if (sections) {
            std::bitset<65536> letters;
            for (const auto& a : v.albums) letters.set((uint16_t)a.letter);
            sections = v.albums.size() >= letters.count() * 3;
        }
        wchar_t letter = 0;
        int col = 0, rowStart = 0;
        auto endRow = [&](int lastAlbum) {
            y += TileH() + F(16);
            if (v.expanded >= rowStart && v.expanded <= lastAlbum) {
                float h = PanelHeight(v.albums[v.expanded], W);
                v.items.push_back({ItemKind::Panel, {0, y - F(6), W, h}, v.expanded});
                y += h + F(10);
            }
        };
        for (int i = 0; i < (int)v.albums.size(); ++i) {
            if (sections && v.albums[i].letter != letter) {
                if (col) endRow(i - 1);
                col = 0;
                letter = v.albums[i].letter;
                v.items.push_back({ItemKind::Section, {Margin(), y, W - 2 * Margin(), F(40)}, (int)letter});
                y += F(44);
            }
            if (col == 0) rowStart = i;
            float x = Margin() + col * (TileArt() + gap);
            v.albumItem[i] = (int)v.items.size();
            v.items.push_back({ItemKind::Tile, {x, y, TileArt(), TileH()}, i});
            if (++col == cols) {
                endRow(i);
                col = 0;
            }
        }
        if (col) endRow((int)v.albums.size() - 1);
    } else if (mode == LibraryMode::AlbumTracks) {
        for (int i = 0; i < (int)v.albums.size(); ++i) {
            float h = std::max(GroupHead() + v.albums[i].tracks.size() * RowH(), GroupArt()) + F(8);
            v.albumItem[i] = (int)v.items.size();
            v.items.push_back({ItemKind::AlbumGroup, {Margin(), y, W - 2 * Margin(), h}, i});
            y += h + F(24);
        }
    } else if (mode == LibraryMode::Artists) {
        float tw = F(140);
        int cols;
        float gap;
        GridMetrics(W, tw, cols, gap);
        for (int i = 0; i < (int)v.artists.size(); ++i) {
            int c = i % cols;
            if (i && c == 0) y += tw + F(64);
            v.items.push_back({ItemKind::ArtistTile, {Margin() + c * (tw + gap), y, tw, tw + F(52)}, i});
        }
        if (!v.artists.empty()) y += tw + F(64);
    }
    v.contentHeight = y + F(12);
    v.laidWidth = W;
    v.layoutDirty = false;
}

// The views keep a scroll bar's width on the right whether or not they scroll, as the Windows canvas
// does (its scroll bar is disabled, not hidden, when everything fits).
float Gutter() { return S(17); }

void Relayout(const Rect& area) {
    const float width = std::max(1.0f, area.w - Gutter());
    if (v.layoutDirty || v.laidWidth != width) LayoutItems(width);
    v.viewH = area.h;
    v.scroll = std::clamp(v.scroll, 0.0f, std::max(0.0f, v.contentHeight - area.h));
}

Rect PanelArea(const Item& it) {
    float x = Margin() + F(28), right = it.rc.R() - Margin() - F(28) - F(280);
    return {x, it.rc.y + F(84), right - x, it.rc.h - F(104)};
}

Rect PanelRow(const Album& a, const Rect& area, int t) {
    int cols = PanelColumns(a, v.laidWidth);
    int rows = (int)(a.tracks.size() + cols - 1) / cols;
    int c = t / rows, r = t % rows;
    float colW = (area.w - (cols - 1) * F(24)) / cols;
    return {area.x + c * (colW + F(24)), area.y + r * RowH(), colW, RowH()};
}

Rect PlayButton(const Item& it) {
    float d = F(34);
    return {it.rc.R() - F(8) - d, it.rc.y + TileArt() - F(8) - d, d, d};
}
Rect PanelClose(const Item& it) { return {it.rc.x + Margin(), it.rc.y + F(14), F(22), F(22)}; }

// Selection, as the views show it, into g.selected (and the focus row, for Play and the menus).
void SyncSelection() {
    std::unordered_set<TrackId> ids(v.selTrack);
    for (int a = 0; a < (int)v.albums.size(); ++a)
        if (v.selAlbum[a]) ids.insert(v.albums[a].tracks.begin(), v.albums[a].tracks.end());
    for (int i = 0; i < (int)v.artists.size(); ++i)
        if (v.selArtist[i])
            for (int a : v.artists[i].albums) ids.insert(v.albums[a].tracks.begin(), v.albums[a].tracks.end());
    g.selected.assign(g.rows.size(), 0);
    g.focus = -1;
    for (size_t r = 0; r < g.rows.size(); ++r)
        if (ids.count(g.rows[r])) {
            g.selected[r] = 1;
            if (g.focus < 0 || g.rows[r] == v.anchorTrack) g.focus = (int)r;
        }
    g.anchor = g.focus;
    g.redraw = true;
}

void ClearCanvasSelection() {
    std::fill(v.selAlbum.begin(), v.selAlbum.end(), (char)0);
    std::fill(v.selArtist.begin(), v.selArtist.end(), (char)0);
    v.selTrack.clear();
}

std::vector<TrackId> ViewTracks() {
    std::vector<TrackId> all;
    for (auto& a : v.albums) all.insert(all.end(), a.tracks.begin(), a.tracks.end());
    return all;
}

void PlayAlbum(int album, int track) {
    if (album >= 0 && album < (int)v.albums.size()) PlayTracks(v.albums[album].tracks, track);
}

void ActivateRow(int album, int track) {
    if (g.settings.libraryMode == LibraryMode::AlbumTracks) {
        // A track row in Albums & Tracks plays the whole view from there, like the track list.
        int index = 0;
        for (int a = 0; a < album; ++a) index += (int)v.albums[a].tracks.size();
        PlayTracks(ViewTracks(), index + track);
    } else {
        PlayAlbum(album, track);
    }
}

// Double-click an artist: point the browser's album-artist pane at them (the second pane if none
// lists album artists), show it, and switch to Albums.
void ShowArtist(int artist) {
    if (artist < 0 || artist >= (int)v.artists.size()) return;
    StringPool::Handle name = v.artists[artist].name;
    int pane = 1;
    for (int i = 0; i < 3; ++i)
        if (g.settings.browserFields[i] == (uint8_t)BrowseField::AlbumArtist) pane = i;
    g.settings.browserFields[pane] = (uint8_t)BrowseField::AlbumArtist;
    for (int i = 0; i < 3; ++i) g_panes[i].selected.clear();
    g_panes[pane].selected.push_back(name);
    g.settings.browserVisible = true;
    SetLibraryMode(LibraryMode::Albums);
}

struct Hit {
    int item = -1, row = -1;
    bool play = false, close = false, stars = false;
    float starsX = 0;
};

Hit HitTest(const Rect& area, float x, float y) {
    Hit h;
    float cx = x - area.x, cy = y - area.y + v.scroll;
    for (int i = 0; i < (int)v.items.size(); ++i) {
        const Item& it = v.items[i];
        Rect r = it.rc;
        if (it.kind == ItemKind::Tile || it.kind == ItemKind::ArtistTile) r = {r.x - F(6), r.y - F(6), r.w + F(12), r.h + F(6)};
        if (!r.In(cx, cy)) continue;
        h.item = i;
        auto rowAt = [&](const Rect& row, int t) {
            if (!row.In(cx, cy)) return;
            h.row = t;
            float starsRight = row.R() - F(6) - F(56);  // as DrawTrackRow places them
            if (cx >= starsRight - F(64) && cx < starsRight + F(4)) {
                h.stars = true;
                h.starsX = cx - (starsRight - F(64));
            }
        };
        if (it.kind == ItemKind::Tile) {
            h.play = PlayButton(it).In(cx, cy);
        } else if (it.kind == ItemKind::Panel) {
            h.close = PanelClose(it).In(cx, cy);
            const Album& a = v.albums[it.index];
            Rect area2 = PanelArea(it);
            for (int t = 0; t < (int)a.tracks.size() && h.row < 0; ++t) rowAt(PanelRow(a, area2, t), t);
        } else if (it.kind == ItemKind::AlbumGroup) {
            const Album& a = v.albums[it.index];
            for (int t = 0; t < (int)a.tracks.size() && h.row < 0; ++t) rowAt(GroupRow(it.rc, t), t);
        }
        return h;
    }
    return h;
}

void EnsureVisible(const Rect& r, float viewH) {
    if (r.y - F(10) < v.scroll) v.scroll = r.y - F(10);
    else if (r.B() + F(10) > v.scroll + viewH) v.scroll = r.B() + F(10) - viewH;
    v.scroll = std::clamp(v.scroll, 0.0f, std::max(0.0f, v.contentHeight - viewH));
}

// --- drawing ------------------------------------------------------------------------------------------
float g_mx = -1, g_my = -1;
Hit g_hover;

// Five small stars 12 pixels apart, as Windows draws them (its symbol font's star is smaller than the
// UI font's here): the rating's in the accent, a half star filled on its left, the rest faint.
void DrawStars(const Rect& r, uint8_t rating, bool selected, const Palette& p) {
    const float w = F(12), outer = F(4.6f), inner = outer * 0.42f, cy = r.y + r.h / 2;
    SDL_Rect area{};
    const bool clipped = SDL_RenderClipEnabled(g.renderer);
    if (clipped) SDL_GetRenderClipRect(g.renderer, &area);
    for (int i = 0; i < 5; ++i) {
        const float cx = r.x + i * w + w / 2;
        std::vector<SDL_FPoint> star{{cx, cy}};
        for (int k = 0; k <= 10; ++k) {
            float a = (float)(k * M_PI / 5 - M_PI / 2), rad = k % 2 ? inner : outer;
            star.push_back({cx + rad * std::cos(a), cy + rad * std::sin(a)});
        }
        int fill = std::clamp((int)rating - i * 20, 0, 20);
        if (fill < 20) Polygon(star, selected ? p.sub : p.faint);
        if (!fill) continue;
        if (fill < 20) {  // the left half only
            SDL_Rect half{(int)(cx - outer - 1), (int)(cy - outer - 1), (int)(outer + 1), (int)(2 * outer + 2)};
            if (clipped) SDL_GetRectIntersection(&half, &area, &half);
            SDL_SetRenderClipRect(g.renderer, &half);
        }
        Polygon(star, selected ? p.selectText : p.accent);
        if (fill < 20) SDL_SetRenderClipRect(g.renderer, clipped ? &area : nullptr);
    }
}

void PlayTriangle(const Rect& r, uint32_t color) {
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2, s = r.h * 0.22f;
    Polygon({{cx - s * 0.8f, cy - s}, {cx + s, cy}, {cx - s * 0.8f, cy + s}}, color);
}

// One track row: number, title (and the artist when it differs from the album's), stars, length.
void DrawTrackRow(const Album& a, int t, const Rect& r, bool hover, const Palette& p) {
    TrackId id = a.tracks[t];
    const Track& tr = g_lib[id];
    bool sel = v.selTrack.count(id) != 0, playing = id == g.playing && g.playing != kNone;
    if (sel) Fill(r, p.select);
    else if (hover) Fill(r, p.hover);
    uint32_t text = sel ? p.selectText : playing ? p.playing : p.text, sub = sel ? p.selectText : p.sub;
    Rect num{r.x + F(4), r.y, F(22), r.h};
    if (playing) PlayTriangle({num.x + F(4), r.y, F(20), r.h}, sel ? p.selectText : p.playing);
    else if (tr.trackNo) DrawText(Font::Small, std::to_string(tr.trackNo), num.x, TextY(num, Font::Small), sub, num.w, 2);
    float right = r.R() - F(6);
    DrawText(Font::Small, Utf8(FormatDuration(tr.Duration())), right - F(48), TextY(r, Font::Small), sub, F(48), 2);
    right -= F(56);
    DrawStars({right - F(64), r.y, F(64), r.h}, tr.rating, sel, p);
    right -= F(72);
    if (tr.artist && tr.artist != a.artist) {
        float aw = std::min(F(220), (right - r.x) * 0.4f);
        DrawText(Font::Small, std::string(g_lib.View(tr.artist)), right - aw, TextY(r, Font::Small), sub, aw);
        right -= aw + F(8);
    }
    std::wstring title;
    LookupField(id, L"title", title);
    Font f = playing ? Font::Bold : Font::Normal;
    DrawText(f, Utf8(title), r.x + F(34), TextY(r, f), text, std::max(0.0f, right - r.x - F(34)));
}

void DrawItem(const Item& item, int index, float ox, float oy, const Palette& p) {
    Rect rc{item.rc.x + ox, item.rc.y + oy, item.rc.w, item.rc.h};
    Item it{item.kind, rc, item.index};
    bool hover = index == g_hover.item;
    switch (it.kind) {
        case ItemKind::Section: {
            DrawText(Font::Title, Utf8(std::wstring(1, (wchar_t)it.index)), rc.x, rc.y + F(4), p.text);
            Fill({rc.x + F(34), rc.B() - F(18), rc.w - F(34), std::max(1.0f, F(1))}, p.border);
            break;
        }
        case ItemKind::Tile: {
            const Album& a = v.albums[it.index];
            bool sel = v.selAlbum[it.index], open = it.index == v.expanded;
            Rect bg{rc.x - F(6), rc.y - F(6), rc.w + F(12), rc.h + F(8)};
            if (sel || open) Fill(bg, sel ? p.select : p.hover);
            else if (hover) Fill(bg, p.hover);
            if (open) Fill({bg.x, bg.B() - F(2), bg.w, F(2)}, p.border);
            Rect art{rc.x, rc.y, rc.w, TileArt()};
            std::wstring title = AlbumTitle(a);
            DrawAlbumCover(a.tracks[0], a.album, title, art, p);
            bool playing = std::find(a.tracks.begin(), a.tracks.end(), g.playing) != a.tracks.end();
            if (hover || playing) {
                Rect button = PlayButton(it);
                Fill(button, hover && g_hover.play ? p.accent2 : p.accent);
                // A note while its album plays, as on Windows, and the play triangle otherwise.
                if (playing && !IsPaused())
                    DrawText(Font::Bold, "\xE2\x99\xAB", button.x, TextY(button, Font::Bold), TextOn(p.accent), button.w, 1);
                else PlayTriangle(button, TextOn(p.accent));
            }
            uint32_t tc = sel ? p.selectText : playing ? p.playing : p.text, sc = sel ? p.selectText : p.sub;
            DrawText(Font::Bold, Utf8(title), rc.x, art.B() + F(6), tc, rc.w);
            DrawText(Font::Small, Utf8(ArtistName(a.artist)), rc.x, art.B() + F(24), sc, rc.w);
            break;
        }
        case ItemKind::Panel: {
            const Album& a = v.albums[it.index];
            uint32_t tint = Mix(p.surface2, p.accent, p.dark ? 16 : 10);
            Fill(rc, tint);
            int tileItem = v.albumItem[it.index];
            if (tileItem >= 0) {  // a notch pointing at the open tile
                float cx = v.items[tileItem].rc.x + ox + v.items[tileItem].rc.w / 2;
                Polygon({{cx - F(10), rc.y}, {cx, rc.y - F(10)}, {cx + F(10), rc.y}}, tint);
            }
            Rect close = PanelClose(it);
            if (hover && g_hover.close) Fill(close, p.hover);
            float m = F(5);
            ThickLine(close.x + m, close.y + m, close.R() - m, close.B() - m, std::max(1.0f, F(1.2f)), p.sub);
            ThickLine(close.R() - m, close.y + m, close.x + m, close.B() - m, std::max(1.0f, F(1.2f)), p.sub);
            float x = Margin() + F(28), right = rc.R() - Margin() - F(28) - F(280);
            DrawText(Font::Title, Utf8(AlbumTitle(a) + L"  \x25B8"), x, rc.y + F(16), p.text, right - x);
            DrawText(Font::Normal, Utf8(AlbumSubtitle(a)), x, rc.y + F(52), p.sub, right - x);
            Rect area = PanelArea(it);
            for (int t = 0; t < (int)a.tracks.size(); ++t) DrawTrackRow(a, t, PanelRow(a, area, t), hover && g_hover.row == t, p);
            float side = std::min(F(280), rc.h - F(40));
            DrawAlbumCover(a.tracks[0], a.album, AlbumTitle(a), {rc.R() - Margin() - side, rc.y + F(20), side, side}, p);
            break;
        }
        case ItemKind::AlbumGroup: {
            const Album& a = v.albums[it.index];
            DrawAlbumCover(a.tracks[0], a.album, AlbumTitle(a), {rc.x, rc.y + F(4), GroupArt(), GroupArt()}, p);
            // The heading: the title and the year on the right, then the details and a rule.
            float x = GroupX(rc), right = rc.R() - F(6);
            std::wstring year = AlbumYear(a);
            if (!year.empty()) {
                DrawText(Font::Small, Utf8(year), right - F(60), rc.y + F(5), p.sub, F(60), 2);
                right -= F(68);
            }
            DrawText(Font::Title, Utf8(AlbumTitle(a)), x, rc.y + F(2), p.text, right - x);
            DrawText(Font::Small, Utf8(AlbumSubtitle(a, false)), x, rc.y + F(29), p.sub, rc.R() - F(6) - x);
            Fill({x - F(6), rc.y + GroupHead() - F(6), rc.R() - x + F(6), std::max(1.0f, F(1))}, p.border);
            for (int t = 0; t < (int)a.tracks.size(); ++t) DrawTrackRow(a, t, GroupRow(rc, t), hover && g_hover.row == t, p);
            break;
        }
        case ItemKind::ArtistTile: {
            const Artist& ar = v.artists[it.index];
            bool sel = v.selArtist[it.index];
            if (sel || hover) Fill({rc.x - F(6), rc.y - F(6), rc.w + F(12), rc.h + F(6)}, sel ? p.select : p.hover);
            Rect art{rc.x, rc.y, rc.w, rc.w};
            const Album& first = v.albums[ar.albums[0]];
            DrawAlbumCover(first.tracks[0], first.album, ArtistName(ar.name), art, p);
            uint32_t tc = sel ? p.selectText : p.text, sc = sel ? p.selectText : p.sub;
            DrawText(Font::Bold, Utf8(ArtistName(ar.name)), rc.x, art.B() + F(9), tc, rc.w);
            DrawText(Font::Small, std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album" : " albums"), rc.x,
                     art.B() + F(27), sc, rc.w);
            break;
        }
    }
}

}  // namespace

// --- view row and browser --------------------------------------------------------------------------
const char* ViewButtonLabel(int i) { return i < (int)LibraryMode::Count ? kModeNames[i] : "Browser"; }

void SetLibraryMode(LibraryMode mode) {
    if (g.settings.libraryMode != mode) v.scroll = 0;
    g.settings.libraryMode = mode;
    SaveSettings();
    RebuildRows();
}

void ToggleBrowser() {
    g.settings.browserVisible = !g.settings.browserVisible;
    if (!g.settings.browserVisible)  // a hidden browser filters nothing
        for (Pane& pane : g_panes) pane.selected.clear();
    SaveSettings();
    BrowserChanged();
}

bool BrowserShown() {
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

// --- views ------------------------------------------------------------------------------------------
bool CanvasActive() {
    return !g.playlists.empty() && g.playlists[g.active].kind == PlaylistKind::Library && g.settings.libraryMode != LibraryMode::Tracks;
}

// Groups the rows again and carries the selection and the open album over from g.selected.
void CanvasRebuild() {
    if (!CanvasActive()) {
        if (!v.albums.empty()) v = Canvas{};
        return;
    }
    BuildGroups();
    v.selAlbum.assign(v.albums.size(), 0);
    v.selArtist.assign(v.artists.size(), 0);
    v.selTrack.clear();
    v.expanded = -1;
    std::unordered_set<TrackId> selected;
    for (size_t r = 0; r < g.rows.size() && r < g.selected.size(); ++r)
        if (g.selected[r]) selected.insert(g.rows[r]);
    const LibraryMode mode = g.settings.libraryMode;
    for (int a = 0; a < (int)v.albums.size(); ++a) {
        const auto& tracks = v.albums[a].tracks;
        if (tracks.front() == v.expandedTrack) v.expanded = a;
        bool all = std::all_of(tracks.begin(), tracks.end(), [&](TrackId id) { return selected.count(id) != 0; });
        if (mode == LibraryMode::Albums && all) v.selAlbum[a] = 1;
        else if (mode != LibraryMode::Artists)
            for (TrackId id : tracks)
                if (selected.count(id)) v.selTrack.insert(id);
    }
    if (mode == LibraryMode::Artists)
        for (int i = 0; i < (int)v.artists.size(); ++i) {
            bool all = !selected.empty();
            for (int a : v.artists[i].albums)
                for (TrackId id : v.albums[a].tracks) all = all && selected.count(id) != 0;
            v.selArtist[i] = all;
        }
    if (v.focus >= (mode == LibraryMode::Artists ? (int)v.artists.size() : (int)v.albums.size())) v.focus = -1;
    v.layoutDirty = true;
    g.redraw = true;
}

void DrawCanvas(const Rect& area, const Palette& p, float mouseX, float mouseY) {
    Relayout(area);
    g_mx = mouseX;
    g_my = mouseY;
    g_hover = area.In(mouseX, mouseY) ? HitTest(area, mouseX, mouseY) : Hit{};
    Fill(area, p.bg);
    SDL_Rect clip{(int)area.x, (int)area.y, (int)area.w, (int)area.h};
    SDL_SetRenderClipRect(g.renderer, &clip);
    if (v.items.empty()) {
        std::string hint = !g.search.empty() ? "No tracks match the search." :
                           g.settings.folders.empty() ? "Your library is empty: add your music folder with Library \xE2\x96\xB8 Library folders, or drop folders here."
                           : g.scanning ? "Looking for music..." : "Nothing here yet.";
        DrawText(Font::Normal, hint, area.x, area.y + S(40), p.sub, area.w, 1);
    }
    const float top = v.scroll, bottom = v.scroll + area.h, ox = area.x, oy = area.y - v.scroll;
    for (int i = 0; i < (int)v.items.size(); ++i) {
        const Item& it = v.items[i];
        if (it.rc.B() + F(12) < top || it.rc.y - F(12) > bottom) continue;
        DrawItem(it, i, ox, oy, p);
    }
    // The scroll bar at rest, as the list's: a slim thumb, or only faint arrows when everything fits.
    const float cx = area.R() - Gutter() / 2;
    if (v.contentHeight > area.h) {
        float thumbH = std::max(S(24), area.h * area.h / v.contentHeight);
        float thumbY = area.y + S(2) + (area.h - S(4) - thumbH) * v.scroll / std::max(1.0f, v.contentHeight - area.h);
        Fill({std::round(cx - S(1)), thumbY, S(2), thumbH}, p.faint);
    } else {
        Polygon({{cx - S(4), area.y + S(10)}, {cx, area.y + S(6)}, {cx + S(4), area.y + S(10)}}, p.faint);
        Polygon({{cx - S(4), area.B() - S(10)}, {cx + S(4), area.B() - S(10)}, {cx, area.B() - S(6)}}, p.faint);
    }
    SDL_SetRenderClipRect(g.renderer, nullptr);
}

void CanvasMouseDown(const Rect& area, float x, float y, int button, int clicks, SDL_Keymod mod) {
    Relayout(area);
    Hit h = HitTest(area, x, y);
    bool ctrl = mod & SDL_KMOD_CTRL, shift = mod & SDL_KMOD_SHIFT;
    if (button == SDL_BUTTON_RIGHT) {
        // Right-clicking outside the selection selects what's under the mouse first.
        if (h.item >= 0) {
            const Item& it = v.items[h.item];
            bool selected = (it.kind == ItemKind::Tile && v.selAlbum[it.index]) || (it.kind == ItemKind::ArtistTile && v.selArtist[it.index]) ||
                            (h.row >= 0 && v.selTrack.count(v.albums[it.index].tracks[h.row]));
            if (!selected && it.kind != ItemKind::Section) CanvasMouseDown(area, x, y, SDL_BUTTON_LEFT, 1, SDL_KMOD_NONE);
        }
        if (std::any_of(g.selected.begin(), g.selected.end(), [](char c) { return c != 0; })) OpenTrackMenu(x, y);
        return;
    }
    if (button != SDL_BUTTON_LEFT) return;
    bool dbl = clicks >= 2 && h.item == v.lastClickItem && h.row == v.lastClickRow;
    v.lastClickItem = h.item;
    v.lastClickRow = h.row;
    if (h.item < 0) {
        if (!ctrl && !shift) ClearCanvasSelection();
        SyncSelection();
        return;
    }
    const Item it = v.items[h.item];
    switch (it.kind) {
        case ItemKind::Section: break;
        case ItemKind::Tile: {
            int a = it.index;
            if (h.play || dbl) {
                PlayAlbum(a, 0);
                break;
            }
            if (shift && v.focus >= 0 && v.focus < (int)v.albums.size()) {
                if (!ctrl) ClearCanvasSelection();
                for (int i = std::min(v.focus, a); i <= std::max(v.focus, a); ++i) v.selAlbum[i] = 1;
            } else if (ctrl) {
                v.selAlbum[a] = !v.selAlbum[a];
                v.focus = a;
            } else {
                bool wasOpen = v.expanded == a;
                ClearCanvasSelection();
                v.selAlbum[a] = 1;
                v.focus = a;
                v.expanded = wasOpen ? -1 : a;
                v.expandedTrack = wasOpen ? kNone : v.albums[a].tracks.front();
                LayoutItems(area.w);
                if (v.expanded >= 0)
                    for (const Item& panel : v.items)
                        if (panel.kind == ItemKind::Panel) EnsureVisible({panel.rc.x, it.rc.y, panel.rc.w, panel.rc.B() - it.rc.y}, area.h);
            }
            break;
        }
        case ItemKind::Panel:
        case ItemKind::AlbumGroup: {
            const Album& a = v.albums[it.index];
            if (h.close) {
                v.expanded = -1;
                v.expandedTrack = kNone;
                LayoutItems(area.w);
                break;
            }
            if (h.row < 0) {
                if (it.kind == ItemKind::AlbumGroup && !dbl) {  // the heading or cover selects the album
                    if (!ctrl) ClearCanvasSelection();
                    v.selTrack.insert(a.tracks.begin(), a.tracks.end());
                }
                if (dbl) PlayAlbum(it.index, 0);
                break;
            }
            TrackId id = a.tracks[h.row];
            if (dbl) {
                ActivateRow(it.index, h.row);
                break;
            }
            if (h.stars && !ctrl && !shift) {
                int halves = std::clamp((int)std::ceil(h.starsX / F(12) * 2), 1, 10);
                uint8_t rating = (uint8_t)(halves * 10);
                RateTracks({id}, rating == g_lib[id].rating ? 0 : rating);
                break;
            }
            if (shift && v.anchorTrack != kNone) {
                auto all = ViewTracks();
                auto p1 = std::find(all.begin(), all.end(), v.anchorTrack), p2 = std::find(all.begin(), all.end(), id);
                if (p1 != all.end() && p2 != all.end()) {
                    if (!ctrl) ClearCanvasSelection();
                    if (p1 > p2) std::swap(p1, p2);
                    for (auto q = p1; q <= p2; ++q) v.selTrack.insert(*q);
                }
            } else if (ctrl) {
                if (!v.selTrack.erase(id)) v.selTrack.insert(id);
                v.anchorTrack = id;
            } else {
                ClearCanvasSelection();
                v.selTrack.insert(id);
                v.anchorTrack = id;
            }
            break;
        }
        case ItemKind::ArtistTile: {
            if (dbl) {
                ShowArtist(it.index);
                return;
            }
            if (!ctrl) ClearCanvasSelection();
            v.selArtist[it.index] = ctrl ? !v.selArtist[it.index] : 1;
            v.focus = it.index;
            break;
        }
    }
    SyncSelection();
}

void CanvasWheel(const Rect& area, float dy, bool ctrl) {
    if (ctrl) {  // Ctrl+wheel: bigger or smaller covers
        g.settings.coverSize = std::clamp(g.settings.coverSize + (dy > 0 ? 20 : -20), 80, 300);
        SaveSettings();
        v.layoutDirty = true;
    } else {
        v.scroll -= dy * S(120);
    }
    Relayout(area);
    g.redraw = true;
}

namespace {

// Arrow keys: through the tiles (a row at a time up and down), or through Albums & Tracks' rows.
void MoveCanvasFocus(int dx, int dy, float width, float viewH) {
    const LibraryMode mode = g.settings.libraryMode;
    if (mode == LibraryMode::AlbumTracks) {
        std::vector<std::pair<int, int>> rows;
        for (int a = 0; a < (int)v.albums.size(); ++a)
            for (int t = 0; t < (int)v.albums[a].tracks.size(); ++t) rows.emplace_back(a, t);
        if (rows.empty()) return;
        int cur = -1;
        for (int i = 0; i < (int)rows.size(); ++i)
            if (v.albums[rows[i].first].tracks[rows[i].second] == v.anchorTrack) cur = i;
        int next = std::clamp(cur < 0 ? 0 : cur + dy + dx, 0, (int)rows.size() - 1);
        auto [a, t] = rows[next];
        ClearCanvasSelection();
        v.anchorTrack = v.albums[a].tracks[t];
        v.selTrack.insert(v.anchorTrack);
        const Rect& rc = v.items[v.albumItem[a]].rc;
        Rect row = GroupRow(rc, t);
        EnsureVisible({rc.x, t == 0 ? rc.y : row.y, rc.w, row.B() - (t == 0 ? rc.y : row.y)}, viewH);
    } else {
        bool artists = mode == LibraryMode::Artists;
        int count = artists ? (int)v.artists.size() : (int)v.albums.size();
        if (!count) return;
        int cols;
        float gap;
        GridMetrics(width, artists ? F(140) : TileArt(), cols, gap);
        int next = std::clamp(v.focus < 0 ? 0 : v.focus + dx + dy * cols, 0, count - 1);
        ClearCanvasSelection();
        (artists ? v.selArtist : v.selAlbum)[next] = 1;
        v.focus = next;
        for (const Item& it : v.items)
            if (it.kind == (artists ? ItemKind::ArtistTile : ItemKind::Tile) && it.index == next) EnsureVisible(it.rc, viewH);
    }
    SyncSelection();
}

}  // namespace

bool CanvasKey(const SDL_KeyboardEvent& e) {
    // The keys act on the canvas as it was last drawn.
    const float width = v.laidWidth > 0 ? v.laidWidth : 800, viewH = std::max(1.0f, v.viewH);
    const bool ctrl = e.mod & SDL_KMOD_CTRL;
    switch (e.key) {
        case SDLK_LEFT: if (ctrl) return false; MoveCanvasFocus(-1, 0, width, viewH); return true;
        case SDLK_RIGHT: if (ctrl) return false; MoveCanvasFocus(1, 0, width, viewH); return true;
        case SDLK_UP: MoveCanvasFocus(0, -1, width, viewH); return true;
        case SDLK_DOWN: MoveCanvasFocus(0, 1, width, viewH); return true;
        case SDLK_HOME: v.scroll = 0; break;
        case SDLK_END: v.scroll = v.contentHeight; break;
        case SDLK_PAGEUP: v.scroll -= viewH - F(40); break;
        case SDLK_PAGEDOWN: v.scroll += viewH - F(40); break;
        case SDLK_ESCAPE:
            if (v.expanded < 0) return false;
            v.expanded = -1;
            v.expandedTrack = kNone;
            v.layoutDirty = true;
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: {
            if (ctrl) return false;
            const LibraryMode mode = g.settings.libraryMode;
            if (mode == LibraryMode::Artists && v.focus >= 0) ShowArtist(v.focus);
            else if (mode == LibraryMode::Albums && v.focus >= 0 && v.selTrack.empty()) PlayAlbum(v.focus, 0);
            else if (g.focus >= 0) PlayRow(g.focus);
            else PlayAlbum(0, 0);
            return true;
        }
        case SDLK_A:
            if (!ctrl) return false;
            CanvasSelectAll();
            return true;
        default: return false;
    }
    v.scroll = std::clamp(v.scroll, 0.0f, std::max(0.0f, v.contentHeight - viewH));
    g.redraw = true;
    return true;
}

void CanvasSelectAll() {
    ClearCanvasSelection();
    const LibraryMode mode = g.settings.libraryMode;
    if (mode == LibraryMode::Artists) std::fill(v.selArtist.begin(), v.selArtist.end(), (char)1);
    else if (mode == LibraryMode::AlbumTracks) {
        auto all = ViewTracks();
        v.selTrack.insert(all.begin(), all.end());
    } else std::fill(v.selAlbum.begin(), v.selAlbum.end(), (char)1);
    SyncSelection();
}

bool CanvasWantsHand(const Rect& area, float x, float y) {
    if (!area.In(x, y)) return false;
    Hit h = HitTest(area, x, y);
    return h.play || h.close || h.stars;
}

}  // namespace lx
