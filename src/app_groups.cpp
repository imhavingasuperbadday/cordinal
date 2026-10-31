// Album group headers in the track lists.
//
// Consecutive tracks of one album (two or more) get a two-row header: a small cover, the album,
// and its artist, year, track count and length. The tracks under it leave out what the header
// already says (album, album artist, and the artist and year when they match), so a 14-track
// album reads as 14 titles instead of 14 copies of the same artist and album.
//
// List rows are g.view positions with each group's two header rows in front of its first track.
// Nothing is stored per track: a row is mapped through the (sorted) groups by binary search.
#include "app.h"
#include "thumbs.h"

#include <list>

namespace app {

namespace {

// Small covers for the headers, drawn with GDI; decoded and cached on disk by ThumbLoader.
struct CoverCache {
    ThumbLoader loader;
    bool started = false;
    std::unordered_map<uint64_t, std::pair<ArtImage, std::list<uint64_t>::iterator>> images;  // key ^ size
    std::list<uint64_t> lru;  // most recently drawn first
    size_t bytes = 0;
};
CoverCache& Covers() {
    static CoverCache c;
    return c;
}
constexpr size_t kMaxCovers = 192;
constexpr size_t kMaxCoverBytes = 8u << 20;

void TrimCovers(size_t budget) {
    auto& c = Covers();
    while (!c.images.empty() && (c.images.size() > ViewMemoryBudget(kMaxCovers) || c.bytes > budget)) {
        auto it = c.images.find(c.lru.back());
        c.bytes -= it->second.first.pixels.capacity() * sizeof(uint32_t);
        c.images.erase(it);
        c.lru.pop_back();
    }
}

bool SortKeepsAlbums(int column) {
    // #, Artist, Album, Year, Album artist, Disc: sorting by these keeps an album's tracks together.
    return column <= 0 || column == 1 || column == 3 || column == 4 || column == 5 || column == 10 || column == 13;
}

bool SameAlbum(const Track& a, const Track& b) {
    if (a.album != b.album) return false;
    // With album artists, they decide; without, the folder does (compilations keep together).
    if (a.albumArtist || b.albumArtist) return a.albumArtist == b.albumArtist;
    return a.dir == b.dir;
}

// Index of the last group whose header starts at or before `row`, or -1.
int GroupAtRow(int row) {
    auto it = std::upper_bound(g.groups.begin(), g.groups.end(), (uint32_t)row,
                               [](uint32_t r, const TrackGroup& gr) { return r < gr.firstRow; });
    return (int)(it - g.groups.begin()) - 1;
}

// The header cover for `gr`, or nullptr while it is still decoding (the loader posts
// WM_APP_GROUP_ART when a request completes). Keyed by path + file time + size, kept in an LRU.
const ArtImage* Cover(const TrackGroup& gr, int size) {
    // While the mini player is the only visible window its covers are not drawn here. If the main
    // window is shown too (tray menu), keep loading them for its album canvas.
    if (MiniPlayerWindow() && !IsWindowVisible(g.hwnd)) return nullptr;
    auto& c = Covers();
    if (!c.started) {
        PWSTR local = nullptr;
        std::wstring cache = g.dataDir + L"\\thumbs";
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            cache = std::wstring(local) + L"\\cordinal\\thumbs";
            CoTaskMemFree(local);
        }
        c.loader.Start(g.hwnd, WM_APP_GROUP_ART, cache, 1, ReducedMemoryMode());
        c.started = true;
    }
    TrackId id = Active().items[g.view[gr.firstView]];
    if (const AlbumGroup* group = AlbumGroupForTrack(id); group && group->artTrack != kInvalidTrack && g_lib.Contains(group->artTrack))
        id = group->artTrack;
    uint64_t base = ThumbLoader::Key(g_lib.Path(id), g_lib[id].fileTime);
    auto it = c.images.find(base ^ (uint64_t)size);
    if (it != c.images.end()) {
        c.lru.splice(c.lru.begin(), c.lru, it->second.second);
        return &it->second.first;
    }
    c.loader.Request(base, g_lib.Path(id), size);
    return nullptr;
}

void DrawText1(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF color, UINT flags) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

}  // namespace

bool GroupingActive() {
    if (CanvasActive() || !SortKeepsAlbums(g.sortColumn)) return false;
    return (g.groupAlbums || FoobarMode()) && !ApolloMode();  // foobar2000 always groups its playlists; Apollo never
}

// Header rows in front of each group's tracks: two (cover, album, artist line), or the foobar2000
// theme's one-line "Artist - [Year] Album".
int HeaderRows() { return FoobarMode() ? 1 : 2; }

namespace {

uint32_t GroupSpan(const TrackGroup& gr) { return (uint32_t)HeaderRows() + gr.count + gr.pad; }

bool SameFoobarGroup(const Track& a, const Track& b) {
    if (!a.album || !b.album) return !a.album && !b.album;  // untagged runs share one "? - ?" group
    return SameAlbum(a, b);
}

}  // namespace

// Rebuilds g.groups from the current sorted/filtered view. Runs after any change that can join or
// break an album run; groups store firstView/firstRow and are found by binary search thereafter.
// Default look: albums of two or more tracks. foobar2000: every run, singles and untagged tracks
// too, each padded with empty rows so its tracks are at least as tall as its cover. A pack's `by`
// format groups runs of two or more tracks whose formatted text matches instead.
void BuildRows() {
    g.groups.clear();
    if (!GroupingActive() || g.playlists.empty()) return;
    const auto& items = Active().items;
    bool foobar = FoobarMode();
    uint32_t n = (uint32_t)g.view.size(), extra = 0, coverRows = foobar ? FoobarCoverRows() : 0;
    const PackGroups* look = foobar ? nullptr : PackGroupLook();
    std::vector<std::wstring> keys;
    if (look && !look->by.empty())
        for (uint32_t p = 0; p < n; ++p) keys.push_back(FormatTitle(look->by, Lookup(items[g.view[p]], false)));
    for (uint32_t p = 0; p < n;) {
        const Track& first = g_lib[items[g.view[p]]];
        uint32_t q = p + 1;
        if (!keys.empty())
            while (q < n && keys[q] == keys[p]) ++q;
        else if (foobar)
            while (q < n && SameFoobarGroup(first, g_lib[items[g.view[q]]])) ++q;
        else if (first.album)
            while (q < n && SameAlbum(first, g_lib[items[g.view[q]]])) ++q;
        if (foobar || ((first.album || !keys.empty()) && q - p >= 2)) {
            TrackGroup gr{p, q - p, p + extra};
            gr.album = first.album;
            gr.artist = first.albumArtist;
            gr.date = first.date;
            if (foobar) gr.pad = coverRows > gr.count ? coverRows - gr.count : 0;
            bool oneArtist = true;
            for (uint32_t k = p; k < q; ++k) {
                const Track& t = g_lib[items[g.view[k]]];
                gr.seconds += t.Duration();
                oneArtist &= t.artist == first.artist;
                if (!gr.date) gr.date = t.date;
            }
            if (!gr.artist) gr.artist = oneArtist ? first.artist : 0;
            extra += (uint32_t)HeaderRows() + gr.pad;
            g.groups.push_back(gr);
        }
        p = q;
    }
}

int RowCount() {
    if (g.groups.empty()) return (int)g.view.size();
    const TrackGroup& last = g.groups.back();
    return (int)(last.firstRow + GroupSpan(last) + (g.view.size() - last.firstView - last.count));
}

// The view position of a list row; header and pad rows give -1, or with `closest` the group's first
// (header) or last (pad) track.
static int ViewPosAt(int row, bool closest) {
    if (row < 0 || row >= RowCount()) return -1;
    int k = GroupAtRow(row);
    if (k < 0) return row;
    const TrackGroup& gr = g.groups[k];
    int local = row - (int)gr.firstRow, h = HeaderRows();
    if (local < h) return closest ? (int)gr.firstView : -1;
    local -= h;
    if (local < (int)gr.count) return (int)gr.firstView + local;
    local -= (int)gr.count;
    if (local < (int)gr.pad) return closest ? (int)(gr.firstView + gr.count) - 1 : -1;
    return (int)(gr.firstView + gr.count) + local - (int)gr.pad;
}

int ViewPosOfRow(int row) { return ViewPosAt(row, false); }
int ViewPosNear(int row) { return ViewPosAt(row, true); }

int RowOfViewPos(int pos) {
    if (pos < 0 || pos >= (int)g.view.size()) return -1;
    auto it = std::upper_bound(g.groups.begin(), g.groups.end(), (uint32_t)pos,
                               [](uint32_t p, const TrackGroup& gr) { return p < gr.firstView; });
    if (it == g.groups.begin()) return pos;
    const TrackGroup& gr = *(it - 1);
    int h = HeaderRows(), into = pos - (int)gr.firstView;
    if (into < (int)gr.count) return (int)gr.firstRow + h + into;
    return (int)(gr.firstRow + h + gr.count + gr.pad) + into - (int)gr.count;
}

int HeaderGroupOfRow(int row) {
    int k = GroupAtRow(row);
    return k >= 0 && row >= 0 && (uint32_t)row < g.groups[k].firstRow + HeaderRows() ? k : -1;
}

// The group whose header, tracks or pad rows include `row`, or -1.
int GroupOfRow(int row) {
    int k = GroupAtRow(row);
    return k >= 0 && row >= 0 && (uint32_t)row < g.groups[k].firstRow + GroupSpan(g.groups[k]) ? k : -1;
}

const TrackGroup* GroupOfViewPos(int pos) {
    auto it = std::upper_bound(g.groups.begin(), g.groups.end(), (uint32_t)pos,
                               [](uint32_t p, const TrackGroup& gr) { return p < gr.firstView; });
    if (it == g.groups.begin()) return nullptr;
    const TrackGroup& gr = *(it - 1);
    return (uint32_t)pos < gr.firstView + gr.count ? &gr : nullptr;
}

TrackId RowTrack(int row) {
    int p = ViewPosOfRow(row);
    return p < 0 ? kNone : Active().items[g.view[p]];
}

// What a grouped track's cell leaves to its header. The foobar2000 theme's columns already say
// only what the header doesn't.
bool GroupHidesCell(int pos, int column, TrackId id) {
    if (FoobarMode()) return false;
    if (const PackGroups* look = PackGroupLook(); look && !look->by.empty()) return false;  // not album groups
    const TrackGroup* gr = GroupOfViewPos(pos);
    if (!gr) return false;
    const Track& t = g_lib[id];
    switch (column) {
        case 4: case 10: return true;                // Album, Album artist
        case 3: return gr->artist && t.artist == gr->artist;
        case 5: return t.date == gr->date;          // Year
        default: return false;
    }
}

void SelectGroup(int group, bool add) {
    if (group < 0 || group >= (int)g.groups.size()) return;
    const TrackGroup& gr = g.groups[group];
    if (!add) ClearSelection();
    int h = HeaderRows(), first = (int)gr.firstRow + h;
    for (int r = first; r < first + (int)gr.count; ++r) ListView_SetItemState(g.list, r, LVIS_SELECTED, LVIS_SELECTED);
    for (int r = 0; r < h; ++r) ListView_SetItemState(g.list, gr.firstRow + r, 0, LVIS_SELECTED);
    ListView_SetItemState(g.list, first, LVIS_FOCUSED, LVIS_FOCUSED);
    g.statsDirty = true;
}

// The cover for a group (see Cover), for the foobar2000 theme's cover column.
const ArtImage* GroupCover(const TrackGroup& gr, int size) { return Cover(gr, size); }

namespace {

// A pack's header line: its format over the group's first track, with %group_tracks% and
// %group_length% for the group itself.
std::wstring PackHeaderText(const std::wstring& format, const TrackGroup& gr) {
    FieldLookup track = Lookup(Active().items[g.view[gr.firstView]], false);
    return FormatTitle(format, [&](const std::wstring& name, std::wstring& out) {
        if (name == L"group_tracks") out = std::to_wstring(gr.count);
        else if (name == L"group_length") out = FormatDuration(gr.seconds);
        else return track(name, out);
        return true;
    });
}

}  // namespace

// Draws one of a header's two rows. `rc` is the row; each row paints its own half of the header
// (the cover is clipped to the row), so partial repaints line up.
void DrawGroupRow(HDC dc, int row, RECT rc) {
    int k = HeaderGroupOfRow(row);
    if (k < 0) return;
    if (FoobarMode()) return DrawFoobarHeader(dc, k, rc);
    const TrackGroup& gr = g.groups[k];
    int part = row - (int)gr.firstRow, h = rc.bottom - rc.top;
    RECT client;
    GetClientRect(g.list, &client);
    RECT all{rc.left, rc.top - part * h, std::min(rc.right, client.right), rc.top - part * h + 2 * h};
    int save = SaveDC(dc);
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    FillSolid(dc, rc, WindowBg());
    if (part == 0 && gr.firstRow > 0) FillSolid(dc, {all.left, all.top, all.right, all.top + 1}, Ref(P().border));
    int size = 2 * h - S(8);
    const PackGroups* look = PackGroupLook();
    RECT cover{all.left + S(8), all.top + S(5), all.left + S(8) + size, all.top + S(5) + size};
    std::wstring album = g_lib.Str(gr.album);
    // The cover fills its square; an album without art gets its own tile, and one still decoding a
    // plain one, so the tile's color doesn't flash before the art.
    if (!look || look->cover) {
        const ArtImage* art = Cover(gr, size);
        if (art && !art->Empty()) DrawCoverArt(dc, cover, *art);
        else if (art) DrawCoverPlaceholder(dc, cover, album);
        else FillSolid(dc, cover, Ref(P().surface2));
        DrawCoverFrame(dc, cover);
    } else {
        cover.right = cover.left - S(10);
    }
    SetBkMode(dc, TRANSPARENT);
    // Line 1: the album, and the track count and length on the right.
    // Line 2: the artist and year.
    int x = cover.right + S(10), right = all.right - S(10);
    std::wstring artist = gr.artist ? g_lib.Str(gr.artist) : L"Various artists";
    std::wstring year = g_lib.Str(gr.date).substr(0, 4);
    std::wstring stats = std::to_wstring(gr.count) + L" tracks  \x00B7  " + FormatDuration(gr.seconds);
    std::wstring line2text = year.empty() ? artist : artist + L"  \x00B7  " + year;
    std::wstring edition = TrackEditionLabel(Active().items[g.view[gr.firstView]]);
    if (!edition.empty()) line2text += L"  \x00B7  " + edition;
    if (look) {
        if (!look->title.empty()) album = PackHeaderText(look->title, gr);
        if (!look->subtitle.empty()) line2text = PackHeaderText(look->subtitle, gr);
        if (!look->right.empty()) stats = PackHeaderText(look->right, gr);
    }
    RECT line1{x, all.top + S(3), right, all.top + h + S(1)}, line2{x, all.top + h - S(1), right, all.bottom - S(3)};
    RECT statsRect = line1;
    HGDIOBJ old = SelectObject(dc, g.font);
    SIZE ext{};
    GetTextExtentPoint32W(dc, stats.c_str(), (int)stats.size(), &ext);
    SelectObject(dc, old);
    statsRect.left = std::max(x, (int)(right - ext.cx));
    DrawText1(dc, stats, statsRect, g.font, SubColor(), DT_RIGHT);
    line1.right = statsRect.left - S(12);
    DrawText1(dc, album, line1, g.fontBold, TextColor(), DT_LEFT);
    DrawText1(dc, line2text, line2, g.font, SubColor(), DT_LEFT);
    RestoreDC(dc, save);
}

// UI thread: drains decoded covers from the loader, evicting the least-recently drawn past the cap,
// and repaints the list if any arrived.
void OnGroupArt() {
    auto& c = Covers();
    bool any = false;
    while (auto r = c.loader.TakeResult()) {
        if (MiniPlayerWindow() && !IsWindowVisible(g.hwnd)) continue;
        uint64_t key = r->key ^ (uint64_t)r->size;
        if (c.images.count(key)) continue;
        if (r->image.pixels.capacity() * sizeof(uint32_t) > ViewMemoryBudget(kMaxCoverBytes)) continue;
        c.lru.push_front(key);
        c.bytes += r->image.pixels.capacity() * sizeof(uint32_t);
        c.images.emplace(key, std::make_pair(std::move(r->image), c.lru.begin()));
        TrimCovers(ViewMemoryBudget(kMaxCoverBytes));
        any = true;
    }
    if (any && g.list && !g.groups.empty()) InvalidateRect(g.list, nullptr, FALSE);
}

size_t GroupArtBytes() { return Covers().bytes; }

void ReleaseGroupArt() {
    auto& c = Covers();
    c.loader.DropPending();
    decltype(c.images)().swap(c.images);
    c.lru.clear();
    c.bytes = 0;
}

void StopGroupArt() {
    auto& c = Covers();
    if (c.started) c.loader.Stop();
    c.started = false;
    ReleaseGroupArt();
}

}  // namespace app
