// The Library tab's visual views, drawn with Direct2D:
//   Albums          a cover grid in letter sections; clicking an album opens it in place, with
//                   its tracks next to a large cover (MusicBee's album view)
//   Albums & Tracks every album's cover (sized with the grid's covers) beside its heading and tracks
//   Artists         one round tile per album artist; double-click shows their albums
//
// The views show what the Library tab's list would show (browser and search applied), grouped.
// Covers come from ThumbLoader and are kept as Direct2D bitmaps in a small LRU cache.
// This file builds the albums and artists and lays them out, and holds the public calls; drawing and input
// have their own files (libview_internal.h).
#include "libview_internal.h"

#include <bitset>

namespace app::lv {

LibView v;

// Decodes a pool string into `out`, reusing its buffer. Painting used to build a fresh wstring
// for every title on every frame, and the heap kept those blocks after they were freed.
void AssignPool(std::wstring& out, StringPool::Handle h) {
    if (!h) {
        out.clear();
        return;
    }
    const std::string_view utf8 = g_lib.View(h);
    if (utf8.empty()) {
        out.clear();
        return;
    }
    if (out.capacity() < utf8.size()) out.reserve(utf8.size());  // one UTF-16 unit per byte is enough
    out.resize(utf8.size());
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), (int)out.size());
    if (n <= 0) out.clear();
    else out.resize((size_t)n);
}

void AppendPool(std::wstring& out, StringPool::Handle h) {
    if (!h) return;
    const std::string_view utf8 = g_lib.View(h);
    if (utf8.empty()) return;
    const size_t at = out.size();
    out.resize(at + utf8.size());
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data() + at, (int)utf8.size());
    if (n <= 0) out.resize(at);
    else out.resize(at + (size_t)n);
}

const std::wstring& AlbumTitle(const Album& a) {
    if (!a.canonicalTitle.empty()) return a.canonicalTitle;
    static std::wstring title;
    if (a.album) {
        AssignPool(title, a.album);
        return title;
    }
    title.clear();
    if (a.trackCount == 1) {
        const Track& track = g_lib[Tracks(a)[0]];
        if (track.title) AssignPool(title, track.title);
        else title = StemOf(g_lib.Str(track.file));
        if (!title.empty()) return title;
    }
    title.assign(L"Unknown album", 13);
    return title;
}
const std::wstring& ArtistName(StringPool::Handle h) {
    static std::wstring name;
    if (!h) name.assign(L"Unknown artist", 14);
    else AssignPool(name, h);
    return name;
}

wchar_t SectionLetter(StringPool::Handle artist) {
    std::wstring s = g_lib.Str(artist);
    size_t i = 0;
    if (s.size() > 4 && CompareStringOrdinal(s.c_str(), 4, L"The ", 4, TRUE) == CSTR_EQUAL) i = 4;
    for (; i < s.size(); ++i)
        if (iswalnum(s[i])) {
            wchar_t c = (wchar_t)towupper(s[i]);
            return iswalpha(c) ? c : L'#';
        }
    return L'#';
}

// The open album panel's large cover, on the right: up to 280 px, smaller in a narrow view, and
// left out (the tile above shows it) when it would leave the rows too little room.
float PanelCover(float width) {
    float side = std::min(F(280), width - Margin() * 2 - F(56) - MinRowsW());
    return side >= F(96) ? side : 0;
}
// The right edge of the panel's heading and rows: left of the cover, with a gap, when there is one.
float PanelRight(float width) {
    float side = PanelCover(width);
    return width - Margin() - (side ? F(28) + side : F(10));
}

// Track rows two columns wide when an open album has many tracks and the panel is wide.
int PanelColumns(const Album& a, float width) {
    float listW = PanelRight(width) - Margin() - F(40);
    return a.trackCount > 10 && listW > F(760) ? 2 : 1;
}

float PanelHeight(const Album& a, float width) {
    int cols = PanelColumns(a, width);
    int rows = (int)(a.trackCount + cols - 1) / cols;
    return std::max(PanelCover(width) + F(40), F(84) + rows * RowH() + F(28));
}

// ---------------------------------------------------------------------------
// Data

void BuildGroups() {
    v.albums.clear();
    v.artists.clear();
    const Playlist& pl = Active();
    struct ShowEditions {
        std::vector<std::pair<uint32_t, std::vector<TrackId>>> visible;  // edition id -> visible tracks
    };
    std::vector<ShowEditions> editions;
    std::vector<std::vector<TrackId>> lists;  // parallel to v.albums, packed into v.albumTracks at the end
    std::unordered_map<uint64_t, size_t> byKey;
    std::unordered_map<uint32_t, size_t> byGroup;
    for (uint32_t row : g.view) {
        TrackId id = pl.items[row];
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
                lists.emplace_back();
            }
            // Keep the edition's own album order later; this just records which tracks survived
            // the browser and search filters.
            auto& slot = editions[it->second];
            auto found = std::find_if(slot.visible.begin(), slot.visible.end(),
                                      [&](const auto& item) { return item.first == edition->id; });
            if (found == slot.visible.end()) slot.visible.push_back({edition->id, {}});
            else found->second.push_back(id);
            continue;
        }
        StringPool::Handle artist = t.albumArtist ? t.albumArtist : t.artist;
        // Albums are album artist + album; tracks without an album group per artist.
        uint64_t key = (uint64_t)artist << 32 | t.album;
        auto [it, added] = byKey.try_emplace(key, v.albums.size());
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
            lists.emplace_back();
        }
        Album& a = v.albums[it->second];
        lists[it->second].push_back(id);
        a.seconds += t.Duration();
        if (!a.genre) a.genre = t.genre;
    }
    // A grouped album is one tile. Its tracks belong to one edition: the one last chosen in this
    // session, else the preferred edition, else the first edition visible in the current filter.
    // Displaying the preferred edition's tracks makes the tile's play button, cover and panel agree.
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
        lists[index].clear();
        a.seconds = 0;
        a.edition = chosen;
        a.editions = (int)group->editions.size();
        v.chosenEdition[a.group] = chosen;
        for (TrackId id : shown->tracks) {
            if (!allowed.count(id)) continue;
            lists[index].push_back(id);
            a.seconds += g_lib[id].Duration();
            if (!a.genre) a.genre = g_lib[id].genre;
        }
        if (lists[index].empty()) { drop.push_back(index); continue; }
        a.artTrack = group->artTrack != kInvalidTrack && g_lib.Contains(group->artTrack)
                         ? group->artTrack
                         : (shown->tracks.empty() ? lists[index].front() : shown->tracks.front());
        std::string title = group->title.empty() ? shown->album : group->title;
        a.canonicalTitle = FromUtf8(title);
    }
    for (auto it = drop.rbegin(); it != drop.rend(); ++it) {
        v.albums.erase(v.albums.begin() + (ptrdiff_t)*it);
        lists.erase(lists.begin() + (ptrdiff_t)*it);
    }
    // Albums can interleave, so the per-album lists are packed only once the edition pass is done.
    // The temporary lists then go away; scrolling does not allocate one heap block per album.
    std::vector<TrackId> packed;
    size_t total = 0;
    for (const auto& list : lists) total += list.size();
    packed.reserve(total);
    for (size_t i = 0; i < lists.size(); ++i) {
        v.albums[i].trackBegin = (uint32_t)packed.size();
        v.albums[i].trackCount = (uint32_t)lists[i].size();
        packed.insert(packed.end(), lists[i].begin(), lists[i].end());
    }
    v.albumTracks.swap(packed);
    if (g.libMode == LibraryMode::Artists) {
        // "Toby Fox; Geoffrey Day" is two artists: the album shows under each of them.
        std::unordered_map<StringPool::Handle, uint32_t> byArtist;
        std::vector<StringPool::Handle> names;
        for (int i = 0; i < (int)v.albums.size(); ++i) {
            BrowserValues(Tracks(v.albums[i])[0], BrowseField::AlbumArtist, names);
            for (StringPool::Handle name : names) {
                auto [it, added] = byArtist.try_emplace(name, (uint32_t)v.artists.size());
                if (added) v.artists.push_back(Artist{name, {}, 0});
                v.artists[it->second].albums.push_back(i);
                v.artists[it->second].tracks += v.albums[i].trackCount;
            }
        }
        // Split names join the list where they belong alphabetically ("Unknown artist" last).
        std::vector<std::wstring> keys;
        keys.reserve(v.artists.size());
        for (const Artist& a : v.artists) keys.push_back(g_lib.Str(a.name));
        std::vector<int> order(v.artists.size());
        for (int i = 0; i < (int)order.size(); ++i) order[i] = i;
        SortRecords(order.begin(), order.end(), [&](int a, int b) {
            if (keys[a].empty() != keys[b].empty()) return keys[b].empty();
            int c = StrCmpLogicalW(keys[a].c_str(), keys[b].c_str());
            return c ? c < 0 : a < b;
        });
        std::vector<Artist> sorted;
        sorted.reserve(order.size());
        for (int i : order) sorted.push_back(std::move(v.artists[i]));
        v.artists.swap(sorted);
    }
    v.selAlbum.assign(v.albums.size(), 0);
    v.selArtist.assign(v.artists.size(), 0);
    if (v.expanded >= (int)v.albums.size()) v.expanded = -1;
}

// Grid layout shared by album and artist tiles: columns that fill the width evenly.
void GridMetrics(float itemW, int& cols, float& gap) {
    float avail = v.width - Margin() * 2;
    // A compact gallery: covers keep an even rhythm and spread a little to fill the width, but never
    // so far apart that a row stops reading as one. Cover spacing (Preferences > Library) sets the
    // least space; the spread stops at twice that.
    const float least = (float)F(std::clamp(g.coverGap, 4, 48));
    gap = least;
    cols = std::max(1, (int)((avail + gap) / (itemW + gap)));
    gap = cols > 1 ? (avail - cols * itemW) / (cols - 1) : 0;
    gap = std::min(gap, least * 2);
}

void LayoutItems() {
    v.items.clear();
    v.albumItem.assign(v.albums.size(), -1);
    float y = F(8);
    float W = (float)v.width;
    if (g.libMode == LibraryMode::Albums) {
        int cols;
        float gap;
        GridMetrics(TileArt(), cols, gap);
        // Letter sections in library order, once there are enough albums per letter to be worth a
        // heading (a small library reads better as one continuous grid).
        bool sections = Active().sortColumn <= 0 && v.albums.size() >= 40;
        if (sections) {
            std::bitset<65536> letters;  // Every UTF-16 code unit, without one heap node per letter.
            for (const auto& a : v.albums) letters.set(a.letter);
            sections = v.albums.size() >= letters.count() * 3;
        }
        wchar_t letter = 0;
        int col = 0;
        int rowStart = 0;  // first album of the current row
        auto endRow = [&](int lastAlbum) {
            y += TileH() + F(16);
            if (v.expanded >= rowStart && v.expanded <= lastAlbum) {
                float h = PanelHeight(v.albums[v.expanded], W);
                v.items.push_back({ItemKind::Panel, {0, y - F(6), W, y - F(6) + h}, v.expanded});
                y += h + F(10);
            }
        };
        for (int i = 0; i < (int)v.albums.size(); ++i) {
            if (sections && v.albums[i].letter != letter) {
                if (col) endRow(i - 1);
                col = 0;
                letter = v.albums[i].letter;
                v.items.push_back({ItemKind::Section, {Margin(), y, W - Margin(), y + F(40)}, (int)letter});
                y += F(44);
            }
            if (col == 0) rowStart = i;
            float x = Margin() + col * (TileArt() + gap);
            v.albumItem[i] = (int)v.items.size();
            v.items.push_back({ItemKind::Tile, {x, y, x + TileArt(), y + TileH()}, i});
            if (++col == cols) {
                endRow(i);
                col = 0;
            }
        }
        if (col) endRow((int)v.albums.size() - 1);
    } else if (g.libMode == LibraryMode::AlbumTracks) {
        for (int i = 0; i < (int)v.albums.size(); ++i) {
            float h = std::max(GroupHead() + v.albums[i].trackCount * RowH(), GroupArt(W - Margin() * 2)) + F(8);
            v.albumItem[i] = (int)v.items.size();
            v.items.push_back({ItemKind::AlbumGroup, {Margin(), y, W - Margin(), y + h}, i});
            y += h + F(24);
        }
    } else if (g.libMode == LibraryMode::Artists) {
        float tw = F(140);
        int cols;
        float gap;
        GridMetrics(tw, cols, gap);
        for (int i = 0; i < (int)v.artists.size(); ++i) {
            int c = i % cols;
            if (i && c == 0) y += tw + F(64);
            float x = Margin() + c * (tw + gap);
            v.items.push_back({ItemKind::ArtistTile, {x, y, x + tw, y + tw + F(52)}, i});
        }
        if (!v.artists.empty()) y += tw + F(64);
    }
    v.contentHeight = y + F(12);
    int maxScroll = std::max(0, (int)v.contentHeight - v.height);
    v.scroll = std::clamp(v.scroll, 0, maxScroll);
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL, 0, (int)v.contentHeight, (UINT)v.height,
                  v.scroll};
    SetScrollInfo(v.hwnd, SB_VERT, &si, TRUE);
}

}  // namespace app::lv

namespace app {
using namespace lv;

StringPool::Handle Split1(StringPool::Handle h) {
    // The first value of a multi-value tag ("A; B" -> "A").
    std::string_view s = g_lib.View(h);
    size_t sep = s.find(';');
    return sep == std::string_view::npos ? h : g_lib.Intern(s.substr(0, sep));
}

// Creates only the control. Direct2D, text formats and thumbnail threads are deferred until its
// first paint, so the ordinary track list never pays for the album/artist renderer.
void CreateLibView(HWND parent) {
    RegisterAppClass(kLibViewClass, LibViewProc, CS_DBLCLKS, false);
    g.coverSize = g.defaultCoverSize = std::clamp(IniInt(L"Library", L"CoverSize", 160), 80, 300);
    v.tile = g.coverSize;
    v.hwnd = CreateWindowExW(0, kLibViewClass, L"", WS_CHILD | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 0, parent,
                             (HMENU)(INT_PTR)IDC_LIBVIEW, g.inst, nullptr);
    g.libView = v.hwnd;
}

void LibViewRebuild() {
    if (!v.hwnd || !CanvasActive()) return;
    BuildGroups();
    v.hoverItem = -1;
    LayoutItems();
    InvalidateRect(v.hwnd, nullptr, FALSE);
}

// A Preferences change or Ctrl+wheel: the live tile size for this session.
void LibViewSetCoverSize(int pixels) {
    const int size = std::clamp(pixels, 80, 300);
    if (g.coverSize == size && v.tile == size) return;
    g.coverSize = v.tile = size;
    if (!v.hwnd) return;
    DropBitmaps();
    LayoutItems();
    InvalidateRect(v.hwnd, nullptr, FALSE);
}

void LibViewRelayout() {
    if (!v.hwnd) return;
    LayoutItems();
    InvalidateRect(v.hwnd, nullptr, FALSE);
}

void LibViewReleaseGraphics() {
    v.loader.Stop();
    v.loaderStarted = false;
    DropBitmaps();
    v.bitmaps.rehash(0);
    ReleaseTarget();
    v.ellipsis.Reset();
    v.fTitle.Reset(); v.fText.Reset(); v.fSmall.Reset(); v.fNum.Reset(); v.fHeading.Reset(); v.fSection.Reset();
    v.fBig.Reset(); v.fStars.Reset(); v.fGlyph.Reset();
    v.dw.Reset(); v.d2d.Reset();
}

// Compact / mini: free the Direct2D target (GPU vs software changes with ReducedMemoryMode) and
// drop pending thumbs, but keep the decode workers. A full Stop() would join them on the UI thread.
void LibViewTrimMemory() {
    if (v.loaderStarted) {
        v.loader.DropPending();
        v.loader.SetReducedMemory(ReducedMemoryMode());
    }
    DropBitmaps();
    v.bitmaps.rehash(0);
    ReleaseTarget();
    v.ellipsis.Reset();
    v.fTitle.Reset(); v.fText.Reset(); v.fSmall.Reset(); v.fNum.Reset(); v.fHeading.Reset(); v.fSection.Reset();
    v.fBig.Reset(); v.fStars.Reset(); v.fGlyph.Reset();
    v.dw.Reset(); v.d2d.Reset();
}

// kBusyTimer (app_badges.cpp): keeps a shown spinner turning, clears one that stopped, and looks
// for newly busy tracks once a second (a full repaint each frame would cost too much for that).
void LibViewBusyTick(bool any) {
    if (!v.hwnd || !IsWindowVisible(v.hwnd)) return;
    const ULONGLONG now = GetTickCount64();
    if (v.busyDrawn || (any && now - v.busyProbe >= 1000)) {
        v.busyProbe = now;
        InvalidateRect(v.hwnd, nullptr, FALSE);
    }
}

void LibViewInvalidate() {
    if (v.hwnd && IsWindowVisible(v.hwnd)) InvalidateRect(v.hwnd, nullptr, FALSE);
}

// Turning Smooth scrolling off ends a glide already under way; the view stays where it is.
void LibViewSmoothScrollChanged() {
    if (!g.smoothScroll) StopSmoothScroll();
}

void LibViewForgetCovers(const std::vector<TrackId>& ids) {
    for (TrackId id : ids)
        if (g_lib.Contains(id)) v.loader.Forget(ThumbLoader::Key(g_lib.Path(id), g_lib[id].fileTime));
    if (!v.hwnd) return;
    DropBitmaps();
    LibViewInvalidate();
}

void LibViewTheme() {
    if (!v.hwnd) return;
    ApplyControlTheme(v.hwnd, g.dark);
    LibViewInvalidate();
}

void LibViewDpiChanged() {
    if (!v.hwnd) return;
    if (v.dw) CreateFormats();
    DropBitmaps();
    LayoutItems();
}

std::vector<TrackId> LibViewSelection() {
    std::vector<TrackId> r;
    for (int a = 0; a < (int)v.albums.size(); ++a) {
        bool whole = v.selAlbum[a];
        for (TrackId id : Tracks(v.albums[a]))
            if (whole || v.selTrack.count(id)) r.push_back(id);
    }
    for (int i = 0; i < (int)v.artists.size(); ++i)
        if (v.selArtist[i])
            for (int a : v.artists[i].albums) {
                auto slice = Tracks(v.albums[a]);
                r.insert(r.end(), slice.begin(), slice.end());
            }
    return r;
}

std::vector<AlbumSelection> LibViewSelectedAlbums() {
    std::vector<AlbumSelection> result;
    if (!CanvasActive()) return result;
    for (int i = 0; i < (int)v.albums.size(); ++i) {
        if (!v.selAlbum[i] || !v.albums[i].trackCount) continue;
        const Album& album = v.albums[i];
        auto slice = Tracks(album);
        AlbumSelection selection;
        selection.groupId = album.group;
        selection.editionId = album.edition;
        selection.tracks.assign(slice.begin(), slice.end());
        const Track& first = g_lib[slice.front()];
        selection.artist = first.albumArtist ? g_lib.View(first.albumArtist) : g_lib.View(first.artist);
        selection.album = album.canonicalTitle.empty() ? g_lib.View(first.album) : ToUtf8(album.canonicalTitle);
        selection.date = g_lib.View(first.date);
        result.push_back(std::move(selection));
    }
    return result;
}

bool LibViewFocusedEdition(uint32_t& group, uint32_t& edition) {
    if (!CanvasActive()) return false;
    int index = v.expanded;
    if (index < 0 && v.focus >= 0 && v.focus < (int)v.albums.size() && v.selAlbum[v.focus]) index = v.focus;
    if (index < 0 || index >= (int)v.albums.size() || !v.albums[index].group) return false;
    group = v.albums[index].group;
    edition = v.albums[index].edition;
    return true;
}

void LibViewChooseEdition(uint32_t group, uint32_t edition) {
    const AlbumGroup* found = FindAlbumGroupConst(group);
    if (!found || !FindAlbumEdition(*found, edition)) return;
    v.chosenEdition[group] = edition;
    LibViewRebuild();
}

void LibViewShowGroup(uint32_t group) {
    for (int i = 0; i < (int)v.albums.size(); ++i) {
        if (v.albums[i].group != group) continue;
        v.expanded = i;
        v.focus = i;
        LayoutItems();
        for (const auto& item : v.items)
            if (item.kind == ItemKind::Panel) EnsureVisible(item.rc);
        InvalidateRect(v.hwnd, nullptr, FALSE);
        return;
    }
}

void LibViewSelectAll() {
    ClearCanvasSelection();
    if (g.libMode == LibraryMode::Artists)
        std::fill(v.selArtist.begin(), v.selArtist.end(), (char)1);
    else if (g.libMode == LibraryMode::AlbumTracks)
        for (TrackId id : ViewTracks()) v.selTrack.insert(id);
    else
        std::fill(v.selAlbum.begin(), v.selAlbum.end(), (char)1);
    g.statsDirty = true;
    InvalidateRect(v.hwnd, nullptr, FALSE);
}

void LibViewFocusFirst() {
    ClearCanvasSelection();
    v.focus = -1;
    v.anchorTrack = kNone;
    MoveFocus(0, 1);
    SetFocus(v.hwnd);
}

// Remembers where the view was (scroll, open album, focus/selection) so it can be restored after a
// rebuild; scroll is stored in 96-DPI units so it survives a DPI change.
void LibViewRememberPosition(PlaylistPosition& position) {
    position.canvasScroll = MulDiv(v.scroll, 96, (int)g.dpi);
    if (v.expanded >= 0 && v.expanded < (int)v.albums.size())
        position.expanded = Tracks(v.albums[v.expanded]).front();
    if (g.libMode == LibraryMode::AlbumTracks) position.focus = v.anchorTrack;
    else if (g.libMode == LibraryMode::Artists && v.focus >= 0 && v.focus < (int)v.artists.size())
        position.focus = Tracks(v.albums[v.artists[v.focus].albums.front()]).front();
    else if (g.libMode == LibraryMode::Albums && v.focus >= 0 && v.focus < (int)v.albums.size())
        position.focus = Tracks(v.albums[v.focus]).front();
}

// Re-applies a remembered position against the freshly rebuilt groups: selection is reconstructed
// from track ids, since album/artist indices may have shifted.
void LibViewRestorePosition(const PlaylistPosition& position) {
    ClearCanvasSelection();
    v.focus = v.expanded = -1;
    v.anchorTrack = position.focus;
    std::unordered_set<TrackId> selected(position.selected.begin(), position.selected.end());
    const AlbumGroup* expandedGroup = AlbumGroupForTrack(position.expanded);
    const AlbumGroup* focusGroup = AlbumGroupForTrack(position.focus);
    for (int a = 0; a < (int)v.albums.size(); ++a) {
        const auto tracks = Tracks(v.albums[a]);
        auto contains = [&](TrackId id) { return std::find(tracks.begin(), tracks.end(), id) != tracks.end(); };
        if (contains(position.expanded) || (v.albums[a].group && expandedGroup && v.albums[a].group == expandedGroup->id))
            v.expanded = a;
        if (contains(position.focus) || (v.albums[a].group && focusGroup && v.albums[a].group == focusGroup->id))
            v.focus = a;
        if (g.libMode == LibraryMode::Albums &&
            std::all_of(tracks.begin(), tracks.end(), [&](TrackId id) { return selected.count(id); }))
            v.selAlbum[a] = 1;
        else if (g.libMode != LibraryMode::Artists)
            for (TrackId id : tracks)
                if (selected.count(id)) v.selTrack.insert(id);
    }
    if (g.libMode == LibraryMode::Artists) {
        v.focus = -1;
        for (int i = 0; i < (int)v.artists.size(); ++i) {
            bool all = true;
            for (int a : v.artists[i].albums)
                for (TrackId id : Tracks(v.albums[a])) {
                    all = all && selected.count(id) != 0;
                    if (id == position.focus) v.focus = i;
                }
            v.selArtist[i] = all;
        }
    }
    LayoutItems();
    ScrollTo(S(position.canvasScroll));
    g.statsDirty = true;
    InvalidateRect(v.hwnd, nullptr, FALSE);
}

void LibViewPlaySelection() {
    auto sel = LibViewSelection();
    if (!sel.empty()) PlayList(sel, 0);
    else if (!v.albums.empty()) PlayAlbum(0, 0);
}

}  // namespace app
