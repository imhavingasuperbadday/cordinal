// The Library tab's visual views on Linux, drawn with the SDL renderer as the Windows build draws them
// (libview.cpp, libview_draw.cpp):
//   Albums & Tracks every album's cover (sized with the grid's covers) beside its heading and tracks
//   Albums          a cover grid in letter sections; clicking an album opens it in place, with its
//                   tracks next to a large cover
//   Artists         one tile per album artist; double-click shows their albums
// The views show what the Library's list would show (browser and search applied, libview.cpp), grouped;
// their selection lives in g.selected like the list's, so menus and commands act on it.
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

// --- views --------------------------------------------------------------------------------------------
struct Album {
    StringPool::Handle artist = 0, album = 0, date = 0, genre = 0;
    std::vector<TrackId> tracks;  // the shown edition's tracks
    double seconds = 0;
    wchar_t letter = L'#';
    uint32_t group = 0, edition = 0;
    int editions = 0;
    TrackId artTrack = kNone;
    std::wstring canonicalTitle;
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
    float scrollGoal = 0;           // Smooth scrolling: where the wheel or paging is taking the view
    bool scrollGliding = false;     // TickCanvasScroll moves the view toward scrollGoal
    bool layoutDirty = true;
    int expanded = -1;              // Albums: the open album
    TrackId expandedTrack = kNone;  // ... by its first track, across rebuilds
    std::unordered_map<uint32_t, uint32_t> chosenEdition;  // group -> edition chosen this session
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
// The narrowest a column of track rows gets beside a cover; a narrow window shrinks the cover
// rather than squeezing the rows under it (as on Windows).
float MinRowsW() { return F(220); }
// Albums & Tracks: the grid's cover a quarter larger, smaller when the album item (`width`) is narrow.
float GroupArt(float width) {
    return std::clamp(width - F(20) - MinRowsW(), F(48), F((float)(g.settings.coverSize + g.settings.coverSize / 4)));
}
float GroupHead() { return F(56); }
float GroupX(const Rect& rc) { return rc.x + GroupArt(rc.w) + F(20); }
// A row shows its rating stars only when the title keeps a useful width beside them.
bool RowStars(const Rect& r) { return r.w >= F(34 + 120 + 72 + 56 + 6); }
Rect GroupRow(const Rect& rc, int t) { return {GroupX(rc) - F(6), rc.y + GroupHead() + t * RowH(), rc.R() - (GroupX(rc) - F(6)), RowH()}; }

std::wstring AlbumTitle(const Album& a) {
    if (!a.canonicalTitle.empty()) return a.canonicalTitle;
    if (a.album) return g_lib.Str(a.album);
    std::wstring t;
    return a.tracks.size() == 1 && LookupField(a.tracks[0], L"title", t) ? t : L"Unknown album";
}
std::wstring ArtistName(StringPool::Handle h) { return h ? g_lib.Str(h) : L"Unknown artist"; }
TrackId ArtTrackOf(const Album& a) {
    return a.artTrack != kNone && g_lib.Contains(a.artTrack) ? a.artTrack : a.tracks.front();
}
std::wstring AlbumYear(const Album& a) { return a.date ? g_lib.Str(a.date).substr(0, 4) : L""; }

std::wstring AlbumSubtitle(const Album& a, bool year = true) {
    std::wstring s = ArtistName(a.artist);
    if (year && a.date) s += L"  \x2022  " + AlbumYear(a);
    if (a.genre) s += L"  \x2022  " + g_lib.Str(a.genre);
    s += L"  \x2022  " + std::to_wstring(a.tracks.size()) + (a.tracks.size() == 1 ? L" track, " : L" tracks, ") +
         FormatTotalDuration(a.seconds);
    if (a.editions > 1) s += L"  \x2022  " + std::to_wstring(a.editions) + L" editions";
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

// The open album panel's large cover: up to 280 px, smaller in a narrow view, and left out (the
// tile above shows it) when it would leave the rows too little room.
float PanelCover(float width) {
    float side = std::min(F(280), width - Margin() * 2 - F(56) - MinRowsW());
    return side >= F(96) ? side : 0;
}
// The right edge of the panel's heading and rows: left of the cover, with a gap, when there is one.
float PanelRight(const Rect& rc) {
    float side = PanelCover(rc.w);
    return rc.R() - Margin() - (side ? F(28) + side : F(10));
}

int PanelColumns(const Album& a, float width) {
    float listW = PanelRight({0, 0, width, 0}) - Margin() - F(40);
    return a.tracks.size() > 10 && listW > F(760) ? 2 : 1;
}

// The open album panel's colour: its cover's hue (once the large cover has arrived) over the theme's surface.
uint32_t PanelSurface(const Album& a, float width, const Palette& p) {
    return AlbumPanelSurface(p, a.tracks.empty() ? -1 : AlbumCoverHue(ArtTrackOf(a), a.album, PanelCover(width)));
}

float PanelHeight(const Album& a, float width) {
    int cols = PanelColumns(a, width);
    int rows = (int)(a.tracks.size() + cols - 1) / cols;
    return std::max(PanelCover(width) + F(40), F(84) + rows * RowH() + F(28));
}

void BuildGroups() {
    v.albums.clear();
    v.artists.clear();
    struct ShowEditions {
        std::vector<std::pair<uint32_t, std::vector<TrackId>>> visible;
    };
    std::vector<ShowEditions> editions;
    std::unordered_map<uint64_t, size_t> byKey;
    std::unordered_map<uint32_t, size_t> byGroup;
    for (TrackId id : g.rows) {
        const Track& t = g_lib[id];
        const AlbumGroup* group = AlbumGroupForTrack(id);
        const AlbumEdition* edition = group ? AlbumEditionForTrack(id) : nullptr;
        if (group && edition) {
            auto [it, added] = byGroup.try_emplace(group->id, v.albums.size());
            if (added) {
                Album a;
                a.group = group->id;
                a.editions = (int)group->editions.size();
                a.artist = t.albumArtist ? t.albumArtist : t.artist;
                a.date = t.date;
                a.genre = t.genre;
                a.letter = SectionLetter(a.artist);
                std::string title = group->title.empty() ? edition->album : group->title;
                a.canonicalTitle = FromUtf8(title);
                v.albums.push_back(std::move(a));
                editions.emplace_back();
            }
            auto& slot = editions[it->second];
            auto found = std::find_if(slot.visible.begin(), slot.visible.end(),
                                      [&](const auto& item) { return item.first == edition->id; });
            if (found == slot.visible.end()) slot.visible.push_back({edition->id, {}});
            else found->second.push_back(id);
            continue;
        }
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
            a.artTrack = id;
            v.albums.push_back(std::move(a));
            editions.emplace_back();
        }
        Album& a = v.albums[it->second];
        a.tracks.push_back(id);
        a.seconds += t.Duration();
        if (!a.genre) a.genre = t.genre;
    }
    std::vector<size_t> drop;
    for (size_t index = 0; index < v.albums.size(); ++index) {
        Album& a = v.albums[index];
        if (!a.group) continue;
        const AlbumGroup* group = FindAlbumGroupConst(a.group);
        if (!group) { drop.push_back(index); continue; }
        auto& visible = editions[index].visible;
        if (visible.empty()) { drop.push_back(index); continue; }
        auto hasVisible = [&](uint32_t editionId) {
            return std::any_of(visible.begin(), visible.end(), [&](const auto& item) { return item.first == editionId; });
        };
        uint32_t preferred = PreferredEditionId(*group), chosen = 0;
        auto remembered = v.chosenEdition.find(a.group);
        if (remembered != v.chosenEdition.end() && hasVisible(remembered->second)) chosen = remembered->second;
        if (!chosen && hasVisible(preferred)) chosen = preferred;
        if (!chosen) chosen = visible.front().first;
        const AlbumEdition* shown = FindAlbumEdition(*group, chosen);
        if (!shown) shown = FindAlbumEdition(*group, visible.front().first);
        if (!shown) { drop.push_back(index); continue; }
        std::unordered_set<TrackId> allowed;
        for (const auto& item : visible) if (item.first == shown->id) allowed.insert(item.second.begin(), item.second.end());
        if (allowed.empty()) {
            shown = FindAlbumEdition(*group, visible.front().first);
            if (!shown) { drop.push_back(index); continue; }
            chosen = shown->id;
            for (const auto& item : visible) if (item.first == shown->id) allowed.insert(item.second.begin(), item.second.end());
        }
        a.tracks.clear();
        a.seconds = 0;
        a.edition = chosen;
        a.editions = (int)group->editions.size();
        v.chosenEdition[a.group] = chosen;
        for (TrackId id : shown->tracks) {
            if (!allowed.count(id)) continue;
            a.tracks.push_back(id);
            a.seconds += g_lib[id].Duration();
            if (!a.genre) a.genre = g_lib[id].genre;
        }
        if (a.tracks.empty()) { drop.push_back(index); continue; }
        a.artTrack = group->artTrack != kInvalidTrack && g_lib.Contains(group->artTrack)
                         ? group->artTrack
                         : (shown->tracks.empty() ? a.tracks.front() : shown->tracks.front());
        std::string title = group->title.empty() ? shown->album : group->title;
        a.canonicalTitle = FromUtf8(title);
    }
    for (auto it = drop.rbegin(); it != drop.rend(); ++it) v.albums.erase(v.albums.begin() + (ptrdiff_t)*it);
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
    // Cover spacing (Preferences > Library) is the least space; the spread stops at twice that.
    const float least = F((float)std::clamp(g.settings.coverGap, 4, 48));
    gap = least;
    cols = std::max(1, (int)((avail + gap) / (itemW + gap)));
    gap = cols > 1 ? std::min((avail - cols * itemW) / (cols - 1), least * 2) : 0;
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
            float h = std::max(GroupHead() + v.albums[i].tracks.size() * RowH(), GroupArt(W - 2 * Margin())) + F(8);
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
    float x = Margin() + F(28), right = PanelRight(it.rc);
    return {x, it.rc.y + F(84), std::max(0.0f, right - x), it.rc.h - F(104)};
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
bool HasEditionSwitch(const Album& a) { return a.group && a.editions > 1; }
Rect PanelEditionRect(const Item& it) {
    float right = PanelRight(it.rc), left = std::max(right - F(210), Margin() + F(28));
    return {left, it.rc.y + F(48), std::max(0.0f, right - left), F(24)};
}

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
    BrowserShowValue(BrowseField::AlbumArtist, v.artists[artist].name);
    SetLibraryMode(LibraryMode::Albums);
}

struct Hit {
    int item = -1, row = -1;
    bool play = false, close = false, stars = false, editions = false;
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
            float starsRight = row.R() - F(6) - F(56);  // as DrawTrackRow places them, when it shows them
            if (RowStars(row) && cx >= starsRight - F(64) && cx < starsRight + F(4)) {
                h.stars = true;
                h.starsX = cx - (starsRight - F(64));
            }
        };
        if (it.kind == ItemKind::Tile) {
            h.play = PlayButton(it).In(cx, cy);
        } else if (it.kind == ItemKind::Panel) {
            h.close = PanelClose(it).In(cx, cy);
            const Album& a = v.albums[it.index];
            if (HasEditionSwitch(a)) h.editions = PanelEditionRect(it).In(cx, cy);
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

float CanvasMaxScroll() { return std::max(0.0f, v.contentHeight - v.viewH); }

// Where a wheel notch or paging key starts from: the pending goal while a glide is under way.
float CanvasScrollBase() { return v.scrollGliding ? v.scrollGoal : v.scroll; }

// Wheel and paging with Smooth scrolling on: the goal moves now and TickCanvasScroll eases to it.
// Scrollbar-free jumps (Home, End, focus moves) set the view directly.
void CanvasScrollTo(float y) {
    if (!g.settings.smoothScroll) {
        v.scroll = y;
        v.scrollGliding = false;
        return;
    }
    v.scrollGoal = std::clamp(y, 0.0f, CanvasMaxScroll());
    v.scrollGliding = std::fabs(v.scrollGoal - v.scroll) > 0.5f;
    if (!v.scrollGliding) v.scroll = v.scrollGoal;
}

}  // namespace

// Declared in app.h: the main loop asks whether a glide is under way and ticks it.
bool CanvasScrollAnimating() { return v.scrollGliding; }

// One frame of the glide: close a quarter of the gap each tick and land exactly on the goal.
void TickCanvasScroll() {
    if (!v.scrollGliding) return;
    if (!g.settings.smoothScroll) {  // turned off mid-glide: stop where the view is
        v.scrollGliding = false;
        return;
    }
    v.scrollGoal = std::clamp(v.scrollGoal, 0.0f, CanvasMaxScroll());  // the content may have shrunk
    float gap = v.scrollGoal - v.scroll;
    if (std::fabs(gap) <= 0.5f) {
        v.scroll = v.scrollGoal;
        v.scrollGliding = false;
    } else {
        v.scroll += gap * 0.25f;
    }
    g.redraw = true;
}

namespace {

void EnsureVisible(const Rect& r, float viewH) {
    v.scrollGliding = false;
    if (r.y - F(10) < v.scroll) v.scroll = r.y - F(10);
    else if (r.B() + F(10) > v.scroll + viewH) v.scroll = r.B() + F(10) - viewH;
    v.scroll = std::clamp(v.scroll, 0.0f, std::max(0.0f, v.contentHeight - viewH));
}

// --- drawing ------------------------------------------------------------------------------------------
float g_mx = -1, g_my = -1;
Hit g_hover;

// Five small stars 12 pixels apart, as Windows draws them (its symbol font's star is smaller than the
// UI font's here): the rating's in the accent, a half star filled on its left, the rest faint. An
// unrated track's row shows nothing until it is hovered or selected (ui::ShowEmptyStars).
void DrawStars(const Rect& r, uint8_t rating, bool selected, bool hover, const Palette& p) {
    if (!ui::ShowEmptyStars(rating, hover, selected)) return;
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
// The stars show only when the title keeps a useful width beside them (RowStars).
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
    if (RowStars(r)) {
        DrawStars({right - F(64), r.y, F(64), r.h}, tr.rating, sel, hover, p);
        right -= F(72);
    }
    if (tr.artist && tr.artist != a.artist) {
        float aw = std::min(F(220), std::max(0.0f, right - r.x - F(34)) * 0.4f);
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
            // The open album stands on its panel's colour, so the tile and the panel read as one piece.
            Rect bg{rc.x - F(6), rc.y - F(6), rc.w + F(12), rc.h + F(8)};
            if (sel) Fill(bg, p.select);
            else if (open) Fill(bg, PanelSurface(a, v.laidWidth, p));
            else if (hover) Fill(bg, p.hover);
            Rect art{rc.x, rc.y, rc.w, TileArt()};
            std::wstring title = AlbumTitle(a);
            DrawAlbumCover(ArtTrackOf(a), a.album, title, art, p);
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
            std::wstring sub = ArtistName(a.artist);
            if (a.editions > 1) sub += L"  \x2022  " + std::to_wstring(a.editions) + L" editions";
            DrawText(Font::Small, Utf8(sub), rc.x, art.B() + F(24), sc, rc.w);
            break;
        }
        case ItemKind::Panel: {
            const Album& a = v.albums[it.index];
            // The cover's colour at the top, settling halfway into the page by the bottom.
            uint32_t tint = PanelSurface(a, rc.w, p), foot = Mix(tint, p.bg, 50);
            SDL_Vertex fade[4] = {{{rc.x, rc.y}, FColor(tint), {0, 0}}, {{rc.R(), rc.y}, FColor(tint), {0, 0}},
                                  {{rc.R(), rc.B()}, FColor(foot), {0, 0}}, {{rc.x, rc.B()}, FColor(foot), {0, 0}}};
            const int quad[6] = {0, 1, 2, 0, 2, 3};
            SDL_RenderGeometry(g.renderer, nullptr, fade, 4, quad, 6);
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
            float x = Margin() + F(28), right = PanelRight(rc);
            DrawText(Font::Title, Utf8(AlbumTitle(a) + L"  \x25B8"), x, rc.y + F(16), p.text, right - x);
            float subtitleRight = right;
            if (HasEditionSwitch(a)) {
                Rect chip = PanelEditionRect(it);
                subtitleRight = chip.x - F(10);
                Fill(chip, hover && g_hover.editions ? p.hover : p.surface2);
                Frame(chip, p.border);
                const AlbumGroup* group = FindAlbumGroupConst(a.group);
                const AlbumEdition* edition = group ? FindAlbumEdition(*group, a.edition) : nullptr;
                std::wstring label = edition ? EditionLabel(*edition) : L"Edition";
                int position = 1;
                if (group) for (size_t i = 0; i < group->editions.size(); ++i)
                    if (group->editions[i].id == a.edition) position = (int)i + 1;
                label = std::to_wstring(position) + L"/" + std::to_wstring(a.editions) + L"  " + label + L"  \x25BE";
                DrawText(Font::Small, Utf8(label), chip.x + F(8), TextY(chip, Font::Small), p.text, chip.w - F(14));
            }
            DrawText(Font::Normal, Utf8(AlbumSubtitle(a)), x, rc.y + F(52), p.sub, subtitleRight - x);
            Rect area = PanelArea(it);
            for (int t = 0; t < (int)a.tracks.size(); ++t) DrawTrackRow(a, t, PanelRow(a, area, t), hover && g_hover.row == t, p);
            float side = std::min(PanelCover(rc.w), rc.h - F(40));
            if (side > 0) DrawAlbumCover(ArtTrackOf(a), a.album, AlbumTitle(a), {rc.R() - Margin() - side, rc.y + F(20), side, side}, p);
            break;
        }
        case ItemKind::AlbumGroup: {
            const Album& a = v.albums[it.index];
            float side = GroupArt(rc.w);
            DrawAlbumCover(ArtTrackOf(a), a.album, AlbumTitle(a), {rc.x, rc.y + F(4), side, side}, p);
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
            DrawAlbumCover(ArtTrackOf(first), first.album, ArtistName(ar.name), art, p);
            uint32_t tc = sel ? p.selectText : p.text, sc = sel ? p.selectText : p.sub;
            DrawText(Font::Bold, Utf8(ArtistName(ar.name)), rc.x, art.B() + F(9), tc, rc.w);
            DrawText(Font::Small, std::to_string(ar.albums.size()) + (ar.albums.size() == 1 ? " album" : " albums"), rc.x,
                     art.B() + F(27), sc, rc.w);
            break;
        }
    }
}

}  // namespace

void SetLibraryMode(LibraryMode mode) {
    if (g.settings.libraryMode != mode) v.scroll = 0;
    g.settings.libraryMode = mode;
    SaveSettings();
    RebuildRows();
}

// --- views ------------------------------------------------------------------------------------------
bool CanvasActive() {
    if (AlternateMode()) return false;
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

void ChooseEdition(uint32_t group, uint32_t edition) {
    const AlbumGroup* found = FindAlbumGroupConst(group);
    if (!found || !FindAlbumEdition(*found, edition)) return;
    v.chosenEdition[group] = edition;
    RebuildRows();
    for (int i = 0; i < (int)v.albums.size(); ++i)
        if (v.albums[i].group == group && !v.albums[i].tracks.empty()) {
            v.expanded = i;
            v.expandedTrack = v.albums[i].tracks.front();
            break;
        }
    v.layoutDirty = true;
    g.redraw = true;
}

void OpenEditionChoice(int album, float x, float y) {
    if (album < 0 || album >= (int)v.albums.size() || !v.albums[album].group) return;
    const Album& shown = v.albums[album];
    const AlbumGroup* group = FindAlbumGroupConst(shown.group);
    if (!group) return;
    std::vector<std::string> labels;
    std::vector<uint32_t> ids;
    int selected = 0;
    for (const auto& edition : group->editions) {
        std::string label = ToUtf8(EditionLabel(edition)) + "  (" + std::to_string(edition.tracks.size()) +
                            (edition.tracks.size() == 1 ? " track)" : " tracks)");
        labels.push_back(std::move(label));
        ids.push_back(edition.id);
        if (edition.id == shown.edition) selected = (int)ids.size() - 1;
    }
    uint32_t chosenGroup = shown.group;
    OpenChoice(x, y, labels, selected, [chosenGroup, ids](int pick) {
        if (pick >= 0 && pick < (int)ids.size()) ChooseEdition(chosenGroup, ids[(size_t)pick]);
    });
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
            if (h.editions) {
                OpenEditionChoice(it.index, x, y);
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

void CanvasRelayout() {
    v.layoutDirty = true;
    g.redraw = true;
}

void CanvasWheel(const Rect& area, float dy, bool ctrl) {
    if (ctrl) {  // Ctrl+wheel: bigger or smaller covers
        g.settings.coverSize = std::clamp(g.settings.coverSize + (dy > 0 ? 20 : -20), 80, 300);
        SaveSettings();
        v.layoutDirty = true;
    } else {
        CanvasScrollTo(CanvasScrollBase() - dy * S(120));
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
    auto cycleEdition = [&](int direction) {
        if (v.expanded < 0 || v.expanded >= (int)v.albums.size()) return false;
        Album& album = v.albums[v.expanded];
        const AlbumGroup* group = album.group ? FindAlbumGroupConst(album.group) : nullptr;
        if (!group || group->editions.size() < 2) return false;
        int current = 0;
        for (size_t i = 0; i < group->editions.size(); ++i)
            if (group->editions[i].id == album.edition) current = (int)i;
        ChooseEdition(group->id, group->editions[(size_t)((current + direction + (int)group->editions.size()) %
                                                          (int)group->editions.size())].id);
        return true;
    };
    switch (e.key) {
        case SDLK_LEFTBRACKET: if (cycleEdition(-1)) return true; return false;
        case SDLK_RIGHTBRACKET: if (cycleEdition(1)) return true; return false;
        case SDLK_LEFT: if (ctrl) return false; MoveCanvasFocus(-1, 0, width, viewH); return true;
        case SDLK_RIGHT: if (ctrl) return false; MoveCanvasFocus(1, 0, width, viewH); return true;
        case SDLK_UP: MoveCanvasFocus(0, -1, width, viewH); return true;
        case SDLK_DOWN: MoveCanvasFocus(0, 1, width, viewH); return true;
        case SDLK_HOME: CanvasScrollTo(0); break;
        case SDLK_END: CanvasScrollTo(v.contentHeight); break;
        case SDLK_PAGEUP: CanvasScrollTo(CanvasScrollBase() - viewH + F(40)); break;
        case SDLK_PAGEDOWN: CanvasScrollTo(CanvasScrollBase() + viewH - F(40)); break;
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

std::vector<AlbumSelection> CanvasSelectedAlbums() {
    std::vector<AlbumSelection> result;
    if (!CanvasActive()) return result;
    for (int i = 0; i < (int)v.albums.size(); ++i) {
        if (!v.selAlbum[i] || v.albums[i].tracks.empty()) continue;
        const Album& album = v.albums[i];
        AlbumSelection selection;
        selection.groupId = album.group;
        selection.editionId = album.edition;
        selection.tracks = album.tracks;
        const Track& first = g_lib[album.tracks.front()];
        selection.artist = first.albumArtist ? g_lib.View(first.albumArtist) : g_lib.View(first.artist);
        selection.album = album.canonicalTitle.empty() ? g_lib.View(first.album) : ToUtf8(album.canonicalTitle);
        selection.date = g_lib.View(first.date);
        result.push_back(std::move(selection));
    }
    return result;
}

bool CanvasFocusedEdition(uint32_t& group, uint32_t& edition) {
    if (!CanvasActive()) return false;
    int index = v.expanded;
    if (index < 0 && v.focus >= 0 && v.focus < (int)v.albums.size() && v.selAlbum[v.focus]) index = v.focus;
    if (index < 0 || index >= (int)v.albums.size() || !v.albums[index].group) return false;
    group = v.albums[index].group;
    edition = v.albums[index].edition;
    return true;
}

void CanvasChooseEdition(uint32_t group, uint32_t edition) { ChooseEdition(group, edition); }

bool CanvasWantsHand(const Rect& area, float x, float y) {
    if (!area.In(x, y)) return false;
    Hit h = HitTest(area, x, y);
    return h.play || h.close || h.stars || h.editions;
}

}  // namespace lx
