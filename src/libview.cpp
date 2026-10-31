// The Library tab's visual views, drawn with Direct2D:
//   Albums          a cover grid in letter sections; clicking an album opens it in place, with
//                   its tracks next to a large cover (MusicBee's album view)
//   Albums & Tracks every album's cover (sized with the grid's covers) beside its heading and tracks
//   Artists         one round tile per album artist; double-click shows their albums
//
// The views show what the Library tab's list would show (browser and search applied), grouped.
// Covers come from ThumbLoader and are kept as Direct2D bitmaps in a small LRU cache.
#include "album_editions.h"
#include "app.h"
#include "thumbs.h"

#include <d2d1.h>
#include <dwrite.h>

#include <bitset>
#include <list>

namespace app {

namespace {

constexpr UINT WM_APP_THUMB = WM_APP + 20;
const wchar_t kLibViewClass[] = L"CordinalLibraryView";

// One album in the view: its album artist and album name, the tracks grouped under it, and the
// lazily-derived cover identity. `letter` is the section heading it lands under in Albums.
struct Album {
    StringPool::Handle artist = 0, album = 0, date = 0, genre = 0;
    std::vector<TrackId> tracks;  // the shown edition's tracks (or the ordinary album)
    double seconds = 0;
    mutable uint64_t coverKey = 0;  // ThumbLoader::Key of the first track, computed on first draw
    wchar_t letter = L'#';
    // An explicit album group, and the edition currently shown. 0 means an ordinary album, which
    // keeps the old behavior and costs nothing.
    uint32_t group = 0, edition = 0;
    int editions = 0;
    TrackId artTrack = kNone;
    std::wstring canonicalTitle;
};

// An album artist tile: their albums (indices into v.albums) and total track count.
struct Artist {
    StringPool::Handle name = 0;
    std::vector<int> albums;
    uint32_t tracks = 0;
};

enum class ItemKind { Section, Tile, Panel, AlbumGroup, ArtistTile };

struct Item {
    ItemKind kind;
    D2D1_RECT_F rc;  // content coordinates (scrolling subtracts v.scroll)
    int index;       // album, artist or section letter
};

// A cached Direct2D cover bitmap plus its node in the LRU list, so eviction is O(1).
struct Bitmap {
    ComPtr<ID2D1Bitmap> bmp;
    std::list<uint64_t>::iterator lru;
    size_t bytes = 0;
    ULONGLONG missAt = 0;  // when "no art" was cached; a new cover.jpg should not stay hidden
};

// All view state, touched only on the UI thread: covers are decoded by ThumbLoader workers but
// wrapped in bitmaps and hit-tested here. Item rects are content pixels, not client pixels.
struct LibView {
    HWND hwnd = nullptr;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextFormat> fTitle, fText, fSmall, fNum, fHeading, fSection, fBig, fStars, fGlyph;
    ComPtr<IDWriteInlineObject> ellipsis;

    std::vector<Album> albums;
    std::vector<Artist> artists;
    std::vector<Item> items;
    std::vector<int> albumItem;  // album -> its tile / group item
    float contentHeight = 0;
    int scroll = 0, width = 0, height = 0;
    int tile = 160;  // cover size at 96 DPI (Ctrl+wheel), in the grid and, a quarter larger, beside tracks

    int expanded = -1;              // Albums: the open album
    std::unordered_map<uint32_t, uint32_t> chosenEdition;  // group -> edition chosen this session
    std::vector<char> selAlbum, selArtist;
    std::unordered_set<TrackId> selTrack;
    int focus = -1;                 // album, artist or (Albums & Tracks) track row index
    TrackId anchorTrack = kNone;

    // Hover: item index, and the part under the mouse.
    int hoverItem = -1, hoverRow = -1;
    bool hoverPlay = false, hoverClose = false;
    bool tracking = false;

    ThumbLoader loader;
    bool loaderStarted = false;
    std::unordered_map<uint64_t, Bitmap> bitmaps;  // cover identity ^ size -> bitmap
    std::list<uint64_t> lru;                       // bitmap keys, most recently used first
    size_t bitmapBytes = 0;

    bool busyDrawn = false;     // the last paint drew a busy spinner
    ULONGLONG busyProbe = 0;    // when a repaint last looked for newly busy tracks
} v;

void CreateFormats();

bool EnsureGraphics() {
    if (!v.d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, v.d2d.GetAddressOf()))) return false;
    if (!v.dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown**>(v.dw.GetAddressOf())))) return false;
    if (!v.fText) CreateFormats();
    if (!v.loaderStarted) {
        PWSTR local = nullptr;
        std::wstring cache = g.dataDir + L"\\thumbs";
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) cache = std::wstring(local) + L"\\cordinal\\thumbs";
        CoTaskMemFree(local);
        v.loader.Start(v.hwnd, WM_APP_THUMB, cache, ReducedMemoryMode() ? 1 : 2, ReducedMemoryMode());
        v.loaderStarted = true;
    }
    return v.fText && v.fGlyph;
}

// ---------------------------------------------------------------------------
// Geometry (all in pixels; the render target uses 96 DPI so 1 DIP = 1 pixel)

float F(int px) { return (float)S(px); }
float RowH() { return F(26); }
float Margin() { return F(18); }
float TileArt() { return F(v.tile); }
float TileH() { return TileArt() + F(44); }
// Albums & Tracks: the cover follows the grid's size, a quarter larger, so Ctrl+wheel sizes both
// views; the heading (title and year, the details, a rule) sits over the album's track rows.
float GroupArt() { return F(v.tile + v.tile / 4); }
float GroupHead() { return F(56); }
float GroupX(const D2D1_RECT_F& rc) { return rc.left + GroupArt() + F(20); }
D2D1_RECT_F GroupRow(const D2D1_RECT_F& rc, int t) {
    float y = rc.top + GroupHead() + t * RowH();
    return {GroupX(rc) - F(6), y, rc.right, y + RowH()};
}

D2D1_COLOR_F Col(Rgb c, float a = 1) {
    return D2D1::ColorF(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f, a);
}
D2D1_COLOR_F Mix(Rgb a, Rgb b, float t) {
    auto ca = Col(a), cb = Col(b);
    return D2D1::ColorF(ca.r + (cb.r - ca.r) * t, ca.g + (cb.g - ca.g) * t, ca.b + (cb.b - ca.b) * t);
}

std::wstring AlbumTitle(const Album& a) {
    if (!a.canonicalTitle.empty()) return a.canonicalTitle;
    if (a.album) return g_lib.Str(a.album);
    std::wstring t;
    return a.tracks.size() == 1 && LookupField(a.tracks[0], L"title", t) ? t : L"Unknown album";
}
std::wstring ArtistName(StringPool::Handle h) { return h ? g_lib.Str(h) : L"Unknown artist"; }

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

// Track rows two columns wide when an open album has many tracks and the panel is wide.
int PanelColumns(const Album& a, float width) {
    float listW = width - Margin() * 2 - F(40) - F(280);
    return a.tracks.size() > 10 && listW > F(760) ? 2 : 1;
}

float PanelHeight(const Album& a, float width) {
    int cols = PanelColumns(a, width);
    int rows = (int)(a.tracks.size() + cols - 1) / cols;
    return std::max(F(280) + F(40), F(84) + rows * RowH() + F(28));
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
        }
        Album& a = v.albums[it->second];
        a.tracks.push_back(id);
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
    if (g.libMode == LibraryMode::Artists) {
        // "Toby Fox; Geoffrey Day" is two artists: the album shows under each of them.
        std::unordered_map<StringPool::Handle, uint32_t> byArtist;
        std::vector<StringPool::Handle> names;
        for (int i = 0; i < (int)v.albums.size(); ++i) {
            BrowserValues(v.albums[i].tracks[0], BrowseField::AlbumArtist, names);
            for (StringPool::Handle name : names) {
                auto [it, added] = byArtist.try_emplace(name, (uint32_t)v.artists.size());
                if (added) v.artists.push_back(Artist{name, {}, 0});
                v.artists[it->second].albums.push_back(i);
                v.artists[it->second].tracks += (uint32_t)v.albums[i].tracks.size();
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
    // so far apart that a row stops reading as one.
    gap = F(16);
    cols = std::max(1, (int)((avail + gap) / (itemW + gap)));
    gap = cols > 1 ? (avail - cols * itemW) / (cols - 1) : 0;
    gap = std::min(gap, F(32));
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
            float h = std::max(GroupHead() + v.albums[i].tracks.size() * RowH(), GroupArt()) + F(8);
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

// ---------------------------------------------------------------------------
// Covers

size_t CoverBudget() {
    // One viewport plus two large covers for partially visible rows/the expanded album.
    // DPI changes scale this naturally; memory depends on visible pixels, not scroll history.
    size_t edge = (size_t)S(300);
    return ViewMemoryBudget(std::max<size_t>(512 * 1024,
        (size_t)std::max(0, v.width) * std::max(0, v.height) * 4 + edge * edge * 8));
}

// Evicts least-recently-used covers until both the byte budget and the hard entry count are met.
void TrimCovers() {
    while (!v.lru.empty() && (v.bitmapBytes > CoverBudget() || v.bitmaps.size() > ViewMemoryBudget(512))) {
        auto it = v.bitmaps.find(v.lru.back());
        v.bitmapBytes -= it->second.bytes;
        v.bitmaps.erase(it);
        v.lru.pop_back();
    }
}

// The cached bitmap for an album's cover at `size`, or nullptr after queueing a decode with the
// loader. The cache key mixes in `size`, so one cover can be held at several DPI-scaled sizes.
ID2D1Bitmap* Cover(const Album& a, int size) {
    if (a.tracks.empty()) return nullptr;
    TrackId id = a.artTrack != kNone && g_lib.Contains(a.artTrack) ? a.artTrack : a.tracks[0];
    if (!a.coverKey) {
        a.coverKey = ThumbLoader::Key(g_lib.Path(id), g_lib[id].fileTime);
        if (!a.coverKey) a.coverKey = 1;
    }
    uint64_t key = a.coverKey ^ (uint64_t)size;
    auto it = v.bitmaps.find(key);
    if (it != v.bitmaps.end()) {
        // A null entry means "no art" for now, not forever: the album may gain a cover while
        // cordinal runs, and its audio file's time doesn't change when that happens.
        if (it->second.bmp || GetTickCount64() - it->second.missAt < 15000) {
            v.lru.splice(v.lru.begin(), v.lru, it->second.lru);
            return it->second.bmp.Get();
        }
        v.lru.erase(it->second.lru);
        v.bitmaps.erase(it);
    }
    v.loader.Request(a.coverKey, g_lib.Path(id), size);
    return nullptr;
}

// UI thread, from WM_APP_THUMB. Wraps decoded pixels in a Direct2D bitmap and inserts it into the
// LRU; an empty image is stored as a null bitmap, so "this album has no art" is remembered for as
// long as the key stays cached.
void OnThumb(ThumbResult* r) {
    std::unique_ptr<ThumbResult> owned(r);
    uint64_t key = r->key ^ (uint64_t)r->size;
    ComPtr<ID2D1Bitmap> bmp;
    size_t bytes = r->image.pixels.size() * sizeof(uint32_t);
    if (!r->image.Empty()) {
        if (!v.rt || bytes > CoverBudget()) return;
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(v.rt->CreateBitmap(D2D1::SizeU(r->image.width, r->image.height), r->image.pixels.data(), r->image.width * 4,
                                  props, &bmp)))
            return;
    }
    auto old = v.bitmaps.find(key);
    if (old != v.bitmaps.end()) {
        v.bitmapBytes -= old->second.bytes;
        v.lru.erase(old->second.lru);
    }
    v.lru.push_front(key);
    v.bitmaps[key] = {bmp, v.lru.begin(), bytes, bmp ? 0 : GetTickCount64()};  // null caches "no art" briefly
    v.bitmapBytes += bytes;
    TrimCovers();
    if (bmp) InvalidateRect(v.hwnd, nullptr, FALSE);
}

// Drops every cached bitmap and any queued requests: call when the render target is lost, the cover
// size or DPI changes, or the view is hidden.
void DropBitmaps() {
    v.bitmaps.clear();
    v.lru.clear();
    v.bitmapBytes = 0;
    v.loader.DropPending();
}

// ---------------------------------------------------------------------------
// Drawing

void Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
    v.brush->SetColor(c);
    v.rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, v.brush.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

void Fill(D2D1_RECT_F r, D2D1_COLOR_F c) {
    v.brush->SetColor(c);
    v.rt->FillRectangle(r, v.brush.Get());
}

// A cover filling `r` (cropped to square), or the album's own tile with its initial (CoverTileFor),
// with a faint hairline just inside its edge so a cover as dark or as light as the page keeps it.
void DrawCover(const Album& a, D2D1_RECT_F r, const std::wstring& title) {
    int size = (int)(r.right - r.left);
    if (ID2D1Bitmap* bmp = Cover(a, size)) {
        D2D1_SIZE_F s = bmp->GetSize();
        float side = std::min(s.width, s.height);
        D2D1_RECT_F src{(s.width - side) / 2, (s.height - side) / 2, (s.width + side) / 2, (s.height + side) / 2};
        v.rt->DrawBitmap(bmp, r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
    } else {
        CoverTile tile = CoverTileFor(title);
        Fill(r, Col(tile.tile));
        std::wstring initial = CoverInitial(title);
        ComPtr<IDWriteTextLayout> layout;
        float side = r.right - r.left;
        v.dw->CreateTextLayout(initial.c_str(), (UINT32)initial.size(), v.fBig.Get(), side, side, &layout);
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TEXT_RANGE all{0, (UINT32)initial.size()};
        layout->SetFontSize(side * 0.40f, all);
        layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
        v.brush->SetColor(Col(tile.ink));
        v.rt->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), v.brush.Get());
    }
    v.brush->SetColor(Col(P().dark ? 0xFFFFFF : 0x000000, 0.10f));
    v.rt->DrawRectangle({r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, v.brush.Get(), 1.0f);
}

void DrawStarsD2D(D2D1_RECT_F r, uint8_t rating, bool selected) {
    float w = F(12);
    for (int i = 0; i < 5; ++i) {
        D2D1_RECT_F s{r.left + i * w, r.top, r.left + (i + 1) * w + F(2), r.bottom};
        int fill = std::clamp((int)rating - i * 20, 0, 20);
        if (fill < 20) Text(ThemeInfo(g.themeMode).ratingMark, v.fStars.Get(), s, Col(selected ? P().sub : P().faint));
        if (fill) {
            v.rt->PushAxisAlignedClip({s.left, s.top, fill < 20 ? s.left + w / 2 : s.right, s.bottom}, D2D1_ANTIALIAS_MODE_ALIASED);
            Text(ThemeInfo(g.themeMode).ratingMark, v.fStars.Get(), s, Col(selected ? P().selectText : P().accent));
            v.rt->PopAxisAlignedClip();
        }
    }
}

bool IsPlaying(TrackId id) { return id == g.playing && g.playing != kNone; }

// The busy spinner (see app_badges.cpp): a ring of small square dots, the brightest stepping
// round every 90 ms and the rest fading out behind it.
void DrawSpinnerD2D(D2D1_POINT_2F c, Rgb color) {
    constexpr int kDots = 8;
    const int phase = (int)(GetTickCount64() / 90 % kDots);
    const float radius = F(5), dot = std::max(1.0f, F(3) / 2);
    for (int i = 0; i < kDots; ++i) {
        const int age = (phase - i + kDots) % kDots;
        const float angle = i * 6.2831853f / kDots - 1.5707963f;
        const float x = c.x + radius * std::cos(angle), y = c.y + radius * std::sin(angle);
        Fill({x - dot, y - dot, x + dot, y + dot}, Col(color, 1.0f - age * 0.11f));
    }
    v.busyDrawn = true;
}

// One track row: number, title (and the artist when it differs from the album's), stars, length.
void DrawTrackRow(const Album& a, int t, D2D1_RECT_F r, bool hover) {
    TrackId id = a.tracks[t];
    const Track& tr = g_lib[id];
    bool sel = v.selTrack.count(id) != 0, playing = IsPlaying(id);
    if (sel) Fill(r, Col(P().select));
    else if (hover) Fill(r, Col(P().hover));
    Rgb text = sel ? P().selectText : playing ? P().playing : P().text;
    Rgb sub = sel ? P().selectText : P().sub;
    std::wstring num = playing ? (g.engine.Paused() ? L"\x275A\x275A" : L"\x25B6") : tr.trackNo ? std::to_wstring(tr.trackNo) : L"";
    // A track cordinal is working on shows the spinner where its number goes.
    if (!playing && TrackBusy(id)) DrawSpinnerD2D({r.left + F(15), (r.top + r.bottom) / 2}, sel ? P().selectText : P().accent);
    else Text(num, playing ? v.fGlyph.Get() : v.fNum.Get(), {r.left + F(4), r.top, r.left + F(26), r.bottom}, Col(playing && !sel ? P().playing : sub));
    std::wstring title;
    LookupField(id, L"title", title);
    float right = r.right - F(6);
    std::wstring len = FormatDuration(tr.Duration());
    Text(len, v.fNum.Get(), {right - F(48), r.top, right, r.bottom}, Col(sub));
    right -= F(56);
    DrawStarsD2D({right - F(64), r.top, right, r.bottom}, tr.rating, sel);
    right -= F(72);
    StringPool::Handle artist = tr.artist;
    if (artist && artist != a.artist) {
        float aw = std::min(F(220), (right - r.left) * 0.4f);
        Text(g_lib.Str(artist), v.fSmall.Get(), {right - aw, r.top, right, r.bottom}, Col(sub));
        right -= aw + F(8);
    }
    Text(title, playing ? v.fTitle.Get() : v.fText.Get(), {r.left + F(34), r.top, right, r.bottom}, Col(text));
}

std::wstring AlbumYear(const Album& a) { return a.date ? g_lib.Str(a.date).substr(0, 4) : L""; }

// Artist, year, genre and length; Albums & Tracks leaves the year out, as its heading shows it.
std::wstring AlbumSubtitle(const Album& a, bool year = true) {
    std::wstring s = ArtistName(a.artist);
    if (year && a.date) s += L"  \x2022  " + AlbumYear(a);
    if (a.genre) s += L"  \x2022  " + g_lib.Str(a.genre);
    s += L"  \x2022  " + std::to_wstring(a.tracks.size()) + (a.tracks.size() == 1 ? L" track, " : L" tracks, ") +
         FormatTotalDuration(a.seconds);
    if (a.editions > 1) s += L"  \x2022  " + std::to_wstring(a.editions) + L" editions";
    return s;
}

// The panel's track rows (one or two columns) inside `area`.
D2D1_RECT_F PanelRow(const Album& a, D2D1_RECT_F area, int t) {
    int cols = PanelColumns(a, (float)v.width);
    int rows = (int)(a.tracks.size() + cols - 1) / cols;
    int c = t / rows, r = t % rows;
    float colW = (area.right - area.left - (cols - 1) * F(24)) / cols;
    float x = area.left + c * (colW + F(24));
    return {x, area.top + r * RowH(), x + colW, area.top + (r + 1) * RowH()};
}

D2D1_RECT_F PanelArea(const Item& it) {
    return {Margin() + F(28), it.rc.top + F(84), it.rc.right - Margin() - F(28) - F(280), it.rc.bottom - F(20)};
}
// The square play button in the corner of a tile's cover.
D2D1_RECT_F PlayButton(const Item& it) {
    float d = F(34), right = it.rc.right - F(8), bottom = it.rc.top + TileArt() - F(8);
    return {right - d, bottom - d, right, bottom};
}
D2D1_RECT_F PanelClose(const Item& it) { return {it.rc.left + Margin(), it.rc.top + F(14), it.rc.left + Margin() + F(22), it.rc.top + F(36)}; }
bool HasEditionSwitch(const Album& a) { return a.group && a.editions > 1; }
// The edition chip sits at the right end of the subtitle row, before the large cover.
D2D1_RECT_F PanelEditionRect(const Item& it) {
    float right = it.rc.right - Margin() - F(28) - F(280);
    return {right - F(210), it.rc.top + F(48), right, it.rc.top + F(72)};
}

void DrawItem(const Item& it, int index) {
    bool hover = index == v.hoverItem;
    switch (it.kind) {
        case ItemKind::Section: {
            std::wstring letter(1, (wchar_t)it.index);
            Text(letter, v.fSection.Get(), {it.rc.left, it.rc.top, it.rc.right, it.rc.bottom - F(6)}, Col(P().text));
            Fill({it.rc.left + F(34), it.rc.bottom - F(18), it.rc.right, it.rc.bottom - F(17)}, Col(P().border));
            break;
        }
        case ItemKind::Tile: {
            const Album& a = v.albums[it.index];
            bool sel = v.selAlbum[it.index], open = it.index == v.expanded;
            // Selection is a quiet fill around the cover and its labels; the open album also gets a
            // rule under its labels, over its panel.
            D2D1_RECT_F bg{it.rc.left - F(6), it.rc.top - F(6), it.rc.right + F(6), it.rc.bottom + F(2)};
            if (sel || open) Fill(bg, Col(sel ? P().select : P().hover));
            else if (hover) Fill(bg, Col(P().hover));
            if (open) Fill({bg.left, bg.bottom - F(2), bg.right, bg.bottom}, Col(P().border));
            D2D1_RECT_F art{it.rc.left, it.rc.top, it.rc.right, it.rc.top + TileArt()};
            std::wstring title = AlbumTitle(a);
            DrawCover(a, art, title);
            bool playing = std::any_of(a.tracks.begin(), a.tracks.end(), IsPlaying);
            if (hover || playing) {
                // A square play button in the cover's corner (shows the playing state too).
                D2D1_RECT_F button = PlayButton(it);
                Fill(button, Col(hover && v.hoverPlay ? P().accent2 : P().accent));
                std::wstring glyph = playing && !g.engine.Paused() ? L"\x266B" : L"\x25B6";
                Text(glyph, v.fGlyph.Get(), button, D2D1::ColorF(1, 1, 1));
            }
            // What's playing shows without the mouse too: its title in the playing colour.
            Rgb tc = sel ? P().selectText : playing ? P().playing : P().text, sc = sel ? P().selectText : P().sub;
            Text(title, v.fTitle.Get(), {it.rc.left, art.bottom + F(5), it.rc.right, art.bottom + F(23)}, Col(tc));
            std::wstring sub = ArtistName(a.artist);
            if (a.editions > 1) sub += L"  \x2022  " + std::to_wstring(a.editions) + L" editions";
            Text(sub, v.fSmall.Get(), {it.rc.left, art.bottom + F(22), it.rc.right, art.bottom + F(40)}, Col(sc));
            break;
        }
        case ItemKind::Panel: {
            const Album& a = v.albums[it.index];
            Fill(it.rc, Mix(P().surface2, P().accent, P().dark ? 0.16f : 0.10f));
            // A notch pointing at the open tile.
            int tileItem = v.albumItem[it.index];
            if (tileItem >= 0) {
                float cx = (v.items[tileItem].rc.left + v.items[tileItem].rc.right) / 2;
                ComPtr<ID2D1PathGeometry> tri;
                v.d2d->CreatePathGeometry(&tri);
                ComPtr<ID2D1GeometrySink> sink;
                tri->Open(&sink);
                sink->BeginFigure(D2D1::Point2F(cx - F(10), it.rc.top), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(cx, it.rc.top - F(10)));
                sink->AddLine(D2D1::Point2F(cx + F(10), it.rc.top));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                v.brush->SetColor(Mix(P().surface2, P().accent, P().dark ? 0.16f : 0.10f));
                v.rt->FillGeometry(tri.Get(), v.brush.Get());
            }
            D2D1_RECT_F close = PanelClose(it);
            if (v.hoverClose && hover) Fill(close, Col(P().hover));
            Text(L"\x2715", v.fText.Get(), {close.left + F(5), close.top + F(2), close.right, close.bottom}, Col(P().sub));
            float x = Margin() + F(28), right = it.rc.right - Margin() - F(28) - F(280);
            Text(AlbumTitle(a) + L"  \x25B8", v.fSection.Get(), {x, it.rc.top + F(14), right, it.rc.top + F(50)}, Col(P().text));
            D2D1_RECT_F subtitle{x, it.rc.top + F(50), right, it.rc.top + F(72)};
            if (HasEditionSwitch(a)) {
                D2D1_RECT_F chip = PanelEditionRect(it);
                subtitle.right = chip.left - F(10);
                if (hover && v.hoverRow == -100) Fill(chip, Col(P().hover));
                else Fill(chip, Col(P().surface2));
                v.brush->SetColor(Col(P().border));
                v.rt->DrawRectangle(chip, v.brush.Get(), 1.0f);
                const AlbumGroup* group = FindAlbumGroupConst(a.group);
                const AlbumEdition* edition = group ? FindAlbumEdition(*group, a.edition) : nullptr;
                std::wstring label = edition ? EditionLabel(*edition) : L"Edition";
                int position = 1;
                if (group) for (size_t i = 0; i < group->editions.size(); ++i)
                    if (group->editions[i].id == a.edition) position = (int)i + 1;
                label = std::to_wstring(position) + L"/" + std::to_wstring(a.editions) + L"  " + label + L"  \x25BE";
                Text(label, v.fSmall.Get(), {chip.left + F(8), chip.top, chip.right - F(6), chip.bottom}, Col(P().text));
            }
            Text(AlbumSubtitle(a), v.fText.Get(), subtitle, Col(P().sub));
            D2D1_RECT_F area = PanelArea(it);
            for (int t = 0; t < (int)a.tracks.size(); ++t)
                DrawTrackRow(a, t, PanelRow(a, area, t), hover && v.hoverRow == t);
            float side = std::min(F(280), it.rc.bottom - it.rc.top - F(40));
            D2D1_RECT_F art{it.rc.right - Margin() - side, it.rc.top + F(20), it.rc.right - Margin(), it.rc.top + F(20) + side};
            DrawCover(a, art, AlbumTitle(a));
            break;
        }
        case ItemKind::AlbumGroup: {
            const Album& a = v.albums[it.index];
            D2D1_RECT_F art{it.rc.left, it.rc.top + F(4), it.rc.left + GroupArt(), it.rc.top + F(4) + GroupArt()};
            DrawCover(a, art, AlbumTitle(a));
            // The heading: the title, and the year on the right over the rows' lengths, then the
            // details and a rule that ties the heading to its tracks.
            float x = GroupX(it.rc), right = it.rc.right - F(6);
            std::wstring year = AlbumYear(a);
            if (!year.empty()) {
                Text(year, v.fNum.Get(), {right - F(60), it.rc.top, right, it.rc.top + F(26)}, Col(P().sub));
                right -= F(68);
            }
            Text(AlbumTitle(a), v.fHeading.Get(), {x, it.rc.top, right, it.rc.top + F(26)}, Col(P().text));
            Text(AlbumSubtitle(a, false), v.fSmall.Get(), {x, it.rc.top + F(26), it.rc.right - F(6), it.rc.top + F(44)}, Col(P().sub));
            Fill({x - F(6), it.rc.top + GroupHead() - F(6), it.rc.right, it.rc.top + GroupHead() - F(5)}, Col(P().border));
            for (int t = 0; t < (int)a.tracks.size(); ++t) DrawTrackRow(a, t, GroupRow(it.rc, t), hover && v.hoverRow == t);
            break;
        }
        case ItemKind::ArtistTile: {
            const Artist& ar = v.artists[it.index];
            bool sel = v.selArtist[it.index];
            float side = it.rc.right - it.rc.left;
            if (sel || hover)
                Fill({it.rc.left - F(6), it.rc.top - F(6), it.rc.right + F(6), it.rc.bottom}, Col(sel ? P().select : P().hover));
            D2D1_RECT_F art{it.rc.left, it.rc.top, it.rc.right, it.rc.top + side};
            DrawCover(v.albums[ar.albums[0]], art, ArtistName(ar.name));
            Rgb tc = sel ? P().selectText : P().text, sc = sel ? P().selectText : P().sub;
            ComPtr<IDWriteTextLayout> layout;
            std::wstring name = ArtistName(ar.name);
            Text(name, v.fTitle.Get(), {it.rc.left, art.bottom + F(8), it.rc.right, art.bottom + F(26)}, Col(tc));
            std::wstring info = std::to_wstring(ar.albums.size()) + (ar.albums.size() == 1 ? L" album" : L" albums");
            Text(info, v.fSmall.Get(), {it.rc.left, art.bottom + F(26), it.rc.right, art.bottom + F(44)}, Col(sc));
            break;
        }
    }
}

void Paint() {
    if (!IsWindowVisible(v.hwnd)) return;
    if (!EnsureGraphics()) return;
    if (!v.rt) {
        RECT rc;
        GetClientRect(v.hwnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        // The small viewport does not need a hardware device and its driver-side allocations.
        if (ReducedMemoryMode()) props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        props.dpiX = props.dpiY = 96.0f;  // work in pixels; S() does the scaling
        if (FAILED(v.d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(v.hwnd, D2D1::SizeU(rc.right, rc.bottom)),
                                                 &v.rt)))
            return;
        v.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        v.rt->CreateSolidColorBrush(D2D1::ColorF(0), &v.brush);
    }
    v.rt->BeginDraw();
    v.busyDrawn = false;
    v.rt->Clear(Col(P().bg));
    v.rt->SetTransform(D2D1::Matrix3x2F::Translation(0, (float)-v.scroll));
    float top = (float)v.scroll, bottom = top + v.height;
    if (v.items.empty()) {
        std::wstring msg = EmptyViewText();
        Text(msg, v.fText.Get(), {Margin(), F(40), (float)v.width - Margin(), F(120)}, Col(P().sub));
    }
    for (int i = 0; i < (int)v.items.size(); ++i) {
        const Item& it = v.items[i];
        if (it.rc.bottom + F(12) < top || it.rc.top - F(12) > bottom) continue;
        DrawItem(it, i);
    }
    if (v.rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        v.rt.Reset();
        v.brush.Reset();
        DropBitmaps();
        InvalidateRect(v.hwnd, nullptr, FALSE);
    }
}

void CreateFormats() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi);
    float base = (float)std::abs(ncm.lfMessageFont.lfHeight);
    const wchar_t* face = ncm.lfMessageFont.lfFaceName;
    // A theme's system face reaches the album and artist views too; a built-in pixel font is GDI-only
    // (AddFontMemResourceEx), so those themes keep the system face here.
    const Theme& theme = ThemeInfo(g.themeMode);
    if (theme.font && !theme.fontResource) {
        face = theme.font;
        if (theme.fontPx) base = (float)S(theme.fontPx);
    }
    auto make = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* family, DWRITE_FONT_WEIGHT weight, float size, bool trim = true) {
        f.Reset();
        v.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (trim) {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            if (!v.ellipsis) v.dw->CreateEllipsisTrimmingSign(f.Get(), &v.ellipsis);
            f->SetTrimming(&t, v.ellipsis.Get());
        }
    };
    v.ellipsis.Reset();
    make(v.fText, face, DWRITE_FONT_WEIGHT_NORMAL, base);
    make(v.fTitle, face, DWRITE_FONT_WEIGHT_SEMI_BOLD, base);
    make(v.fSmall, face, DWRITE_FONT_WEIGHT_NORMAL, base * 0.92f);
    make(v.fNum, face, DWRITE_FONT_WEIGHT_NORMAL, base * 0.92f);
    v.fNum->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);  // track numbers, lengths and years line up
    make(v.fHeading, face, DWRITE_FONT_WEIGHT_SEMI_BOLD, base * 1.2f);
    make(v.fSection, face, DWRITE_FONT_WEIGHT_SEMI_LIGHT, base * 1.75f);
    make(v.fBig, face, DWRITE_FONT_WEIGHT_LIGHT, base * 3, false);
    make(v.fStars, L"Segoe UI Symbol", DWRITE_FONT_WEIGHT_NORMAL, base * 0.95f, false);
    make(v.fGlyph, L"Segoe UI Symbol", DWRITE_FONT_WEIGHT_NORMAL, base * 1.05f, false);
    v.fGlyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    v.fText->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);  // also used for the empty-library message
}

// ---------------------------------------------------------------------------
// Hit testing and selection

struct Hit {
    int item = -1, row = -1;
    bool play = false, close = false, stars = false, editions = false;
    float starsX = 0;
};

// Maps a client point to an item and, inside album rows, to a track row and whether the click hit
// the star rating widget (whose geometry mirrors DrawTrackRow).
Hit HitTest(int x, int y) {
    Hit h;
    float cx = (float)x, cy = (float)(y + v.scroll);
    for (int i = 0; i < (int)v.items.size(); ++i) {
        const Item& it = v.items[i];
        D2D1_RECT_F r = it.rc;
        if (it.kind == ItemKind::Tile || it.kind == ItemKind::ArtistTile) r = {r.left - F(6), r.top - F(6), r.right + F(6), r.bottom};
        if (cx < r.left || cx >= r.right || cy < r.top || cy >= r.bottom) continue;
        h.item = i;
        auto rowAt = [&](D2D1_RECT_F row, int t) {
            if (cx >= row.left && cx < row.right && cy >= row.top && cy < row.bottom) {
                h.row = t;
                // Stars sit 56..120 px from the row's right edge (see DrawTrackRow).
                float starsRight = row.right - F(6) - F(56);
                if (cx >= starsRight - F(64) && cx < starsRight + F(4)) {
                    h.stars = true;
                    h.starsX = cx - (starsRight - F(64));
                }
            }
        };
        if (it.kind == ItemKind::Tile) {
            D2D1_RECT_F b = PlayButton(it);
            h.play = cx >= b.left && cx < b.right && cy >= b.top && cy < b.bottom;
        } else if (it.kind == ItemKind::Panel) {
            D2D1_RECT_F c = PanelClose(it);
            h.close = cx >= c.left && cx < c.right && cy >= c.top && cy < c.bottom;
            const Album& a = v.albums[it.index];
            if (HasEditionSwitch(a)) {
                D2D1_RECT_F chip = PanelEditionRect(it);
                h.editions = cx >= chip.left && cx < chip.right && cy >= chip.top && cy < chip.bottom;
            }
            D2D1_RECT_F area = PanelArea(it);
            for (int t = 0; t < (int)a.tracks.size() && h.row < 0; ++t) rowAt(PanelRow(a, area, t), t);
        } else if (it.kind == ItemKind::AlbumGroup) {
            const Album& a = v.albums[it.index];
            for (int t = 0; t < (int)a.tracks.size() && h.row < 0; ++t) rowAt(GroupRow(it.rc, t), t);
        }
        return h;
    }
    return h;
}

void ClearCanvasSelection() {
    std::fill(v.selAlbum.begin(), v.selAlbum.end(), (char)0);
    std::fill(v.selArtist.begin(), v.selArtist.end(), (char)0);
    v.selTrack.clear();
}

void ScrollTo(int y) {
    int maxScroll = std::max(0, (int)v.contentHeight - v.height);
    y = std::clamp(y, 0, maxScroll);
    if (y == v.scroll) return;
    v.scroll = y;
    SetScrollPos(v.hwnd, SB_VERT, v.scroll, TRUE);
    InvalidateRect(v.hwnd, nullptr, FALSE);
}

void EnsureVisible(const D2D1_RECT_F& r) {
    if (r.top - F(10) < v.scroll) ScrollTo((int)(r.top - F(10)));
    else if (r.bottom + F(10) > v.scroll + v.height) ScrollTo((int)(r.bottom + F(10) - v.height));
}

// Rows of Albums & Tracks in order: (album, track) pairs, for keyboard navigation.
std::vector<std::pair<int, int>> AllRows() {
    std::vector<std::pair<int, int>> rows;
    for (int a = 0; a < (int)v.albums.size(); ++a)
        for (int t = 0; t < (int)v.albums[a].tracks.size(); ++t) rows.emplace_back(a, t);
    return rows;
}

std::vector<TrackId> ViewTracks() {
    std::vector<TrackId> all;
    for (auto& a : v.albums) all.insert(all.end(), a.tracks.begin(), a.tracks.end());
    return all;
}

void PlayAlbum(int album, int track) {
    if (album < 0 || album >= (int)v.albums.size()) return;
    PlayList(v.albums[album].tracks, track);
}

void ShowEditionPicker(int album, POINT pt) {
    if (album < 0 || album >= (int)v.albums.size() || !v.albums[album].group) return;
    const Album& shown = v.albums[album];
    const AlbumGroup* group = FindAlbumGroupConst(shown.group);
    if (!group) return;
    HMENU menu = CreatePopupMenu();
    int current = 0;
    for (size_t i = 0; i < group->editions.size(); ++i) {
        const AlbumEdition& edition = group->editions[i];
        std::wstring label = EditionLabel(edition) + L"  (" + std::to_wstring(edition.tracks.size()) +
                             (edition.tracks.size() == 1 ? L" track)" : L" tracks)");
        AppendMenuW(menu, MF_STRING, 100 + (int)i, label.c_str());
        if (edition.id == shown.edition) current = 100 + (int)i;
    }
    if (current) CheckMenuRadioItem(menu, current, current, current, MF_BYCOMMAND);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 200, L"Set as &preferred edition");
    AppendMenuW(menu, MF_STRING, 201, L"Album &editions...");
    AppendMenuW(menu, MF_STRING, 202, L"&Compare editions...");
    int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(menu);
    if (command >= 100 && command < 200) {
        LibViewChooseEdition(shown.group, group->editions[(size_t)(command - 100)].id);
    } else if (command == 200) {
        if (SetPreferredEdition(shown.group, shown.edition)) {
            LibViewChooseEdition(shown.group, shown.edition);
            AlbumGroupChangedInApp();
        }
    } else if (command == 201) {
        ShowAlbumEditionsDialog(shown.group);
    } else if (command == 202) {
        ShowEditionComparisonDialog(shown.group);
    }
}

void ActivateRow(int album, int track) {
    if (g.libMode == LibraryMode::AlbumTracks) {
        // A track row in Albums & Tracks plays the whole view from there, like the track list.
        auto all = ViewTracks();
        int index = 0;
        for (int a = 0; a < album; ++a) index += (int)v.albums[a].tracks.size();
        PlayList(all, index + track);
    } else {
        PlayAlbum(album, track);
    }
}

// Double-click an artist tile: reveal the browser, point its album-artist pane at this artist, and
// switch to Albums. Falls back to the second pane if no pane currently shows album artist.
void ShowArtist(int artist) {
    if (artist < 0 || artist >= (int)v.artists.size()) return;
    StringPool::Handle name = v.artists[artist].name;
    if (!g.browserVisible) {
        g.browserVisible = true;
        Layout();
    }
    // Show the artist through the browser pane listing album artists (or the second pane).
    int pane = 1;
    for (int i = 0; i < 3; ++i)
        if (g.panes[i].field == BrowseField::AlbumArtist) pane = i;
    g.panes[pane].field = BrowseField::AlbumArtist;
    SelectBrowserValue(BrowseField::AlbumArtist, name ? Split1(name) : 0);
    SetLibraryMode(LibraryMode::Albums);
}

void OnClick(int x, int y, bool dbl, WPARAM keys) {
    SetFocus(v.hwnd);
    Hit h = HitTest(x, y);
    bool ctrl = keys & MK_CONTROL, shift = keys & MK_SHIFT;
    if (h.item < 0) {
        if (!ctrl && !shift) ClearCanvasSelection();
        InvalidateRect(v.hwnd, nullptr, FALSE);
        g.statsDirty = true;
        return;
    }
    const Item& it = v.items[h.item];
    switch (it.kind) {
        case ItemKind::Section: break;
        case ItemKind::Tile: {
            int a = it.index;
            if (h.play) {
                PlayAlbum(a, 0);
                break;
            }
            if (dbl) {
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
                const float tileTop = it.rc.top;
                LayoutItems();
                if (v.expanded >= 0)
                    for (auto& p : v.items)
                        if (p.kind == ItemKind::Panel) EnsureVisible({p.rc.left, tileTop, p.rc.right, p.rc.bottom});
            }
            break;
        }
        case ItemKind::Panel:
        case ItemKind::AlbumGroup: {
            const Album& a = v.albums[it.index];
            if (h.close) {
                v.expanded = -1;
                LayoutItems();
                break;
            }
            if (h.editions) {
                POINT pt{x, y};
                ClientToScreen(v.hwnd, &pt);
                ShowEditionPicker(it.index, pt);
                break;
            }
            if (h.row < 0) {
                if (it.kind == ItemKind::AlbumGroup && !dbl) {  // the header/cover selects the album
                    if (!ctrl) ClearCanvasSelection();
                    for (TrackId id : a.tracks) v.selTrack.insert(id);
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
                double stars = h.starsX / F(12);
                int halves = std::clamp((int)std::ceil(stars * 2), 1, 10);
                uint8_t rating = (uint8_t)(halves * 10);
                SetRating({id}, rating == g_lib[id].rating ? 0 : rating);
                break;
            }
            if (shift && v.anchorTrack != kNone) {
                auto all = ViewTracks();
                auto p1 = std::find(all.begin(), all.end(), v.anchorTrack), p2 = std::find(all.begin(), all.end(), id);
                if (p1 != all.end() && p2 != all.end()) {
                    if (!ctrl) ClearCanvasSelection();
                    if (p1 > p2) std::swap(p1, p2);
                    for (auto p = p1; p <= p2; ++p) v.selTrack.insert(*p);
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
    g.statsDirty = true;
    InvalidateRect(v.hwnd, nullptr, FALSE);
}

void MoveFocus(int dx, int dy) {
    if (g.libMode == LibraryMode::AlbumTracks) {
        auto rows = AllRows();
        if (rows.empty()) return;
        int cur = -1;
        for (int i = 0; i < (int)rows.size(); ++i)
            if (v.albums[rows[i].first].tracks[rows[i].second] == v.anchorTrack) cur = i;
        int next = std::clamp(cur < 0 ? 0 : cur + dy, 0, (int)rows.size() - 1);
        auto [a, t] = rows[next];
        ClearCanvasSelection();
        v.anchorTrack = v.albums[a].tracks[t];
        v.selTrack.insert(v.anchorTrack);
        const Item& it = v.items[v.albumItem[a]];
        D2D1_RECT_F row = GroupRow(it.rc, t);
        EnsureVisible({it.rc.left, t == 0 ? it.rc.top : row.top, it.rc.right, row.bottom});
        InvalidateRect(v.hwnd, nullptr, FALSE);
        g.statsDirty = true;
        return;
    }
    bool artists = g.libMode == LibraryMode::Artists;
    int count = artists ? (int)v.artists.size() : (int)v.albums.size();
    if (!count) return;
    int cols;
    float gap;
    GridMetrics(artists ? F(140) : TileArt(), cols, gap);
    int cur = v.focus < 0 ? 0 : v.focus;
    int next = std::clamp(v.focus < 0 ? 0 : cur + dx + dy * cols, 0, count - 1);
    ClearCanvasSelection();
    (artists ? v.selArtist : v.selAlbum)[next] = 1;
    v.focus = next;
    for (auto& it : v.items)
        if ((it.kind == (artists ? ItemKind::ArtistTile : ItemKind::Tile)) && it.index == next) EnsureVisible(it.rc);
    g.statsDirty = true;
    InvalidateRect(v.hwnd, nullptr, FALSE);
}

void OnKey(WPARAM key) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    auto cycleEdition = [&](int direction) {
        if (v.expanded < 0 || v.expanded >= (int)v.albums.size()) return false;
        Album& album = v.albums[v.expanded];
        const AlbumGroup* group = album.group ? FindAlbumGroupConst(album.group) : nullptr;
        if (!group || group->editions.size() < 2) return false;
        int current = 0;
        for (size_t i = 0; i < group->editions.size(); ++i)
            if (group->editions[i].id == album.edition) current = (int)i;
        int next = (current + direction + (int)group->editions.size()) % (int)group->editions.size();
        LibViewChooseEdition(group->id, group->editions[(size_t)next].id);
        return true;
    };
    switch (key) {
        case VK_OEM_4:
            if (cycleEdition(-1)) return;
            break;
        case VK_OEM_6:
            if (cycleEdition(1)) return;
            break;
        case VK_LEFT: MoveFocus(-1, 0); break;
        case VK_RIGHT: MoveFocus(1, 0); break;
        case VK_UP: MoveFocus(0, -1); break;
        case VK_DOWN: MoveFocus(0, 1); break;
        case VK_HOME: ScrollTo(0); break;
        case VK_END: ScrollTo((int)v.contentHeight); break;
        case VK_PRIOR: ScrollTo(v.scroll - v.height + (int)F(40)); break;
        case VK_NEXT: ScrollTo(v.scroll + v.height - (int)F(40)); break;
        case VK_ESCAPE:
            if (v.expanded >= 0) {
                v.expanded = -1;
                LayoutItems();
                InvalidateRect(v.hwnd, nullptr, FALSE);
            }
            break;
        case VK_RETURN:
            if (g.libMode == LibraryMode::Artists && v.focus >= 0) ShowArtist(v.focus);
            else if (g.libMode == LibraryMode::Albums && v.focus >= 0 && v.selTrack.empty()) PlayAlbum(v.focus, 0);
            else PlayFocusedOrFirst();
            break;
        case VK_SPACE: TogglePause(); break;
        case VK_DELETE: ExcludeFromLibrary(SelectedTracks()); break;
        case 'A':
            if (ctrl) LibViewSelectAll();
            break;
    }
}

void UpdateHover(int x, int y) {
    Hit h = HitTest(x, y);
    if (h.editions) h.row = -100;  // the edition chip's own hover state
    if (h.item != v.hoverItem || h.row != v.hoverRow || h.play != v.hoverPlay || h.close != v.hoverClose) {
        v.hoverItem = h.item;
        v.hoverRow = h.row;
        v.hoverPlay = h.play;
        v.hoverClose = h.close;
        InvalidateRect(v.hwnd, nullptr, FALSE);
    }
    SetCursor(LoadCursorW(nullptr, h.play || h.close || h.stars ? IDC_HAND : IDC_ARROW));
}

LRESULT CALLBACK LibViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            v.width = LOWORD(lp);
            v.height = HIWORD(lp);
            if (v.rt) v.rt->Resize(D2D1::SizeU(v.width, v.height));
            LayoutItems();
            TrimCovers();
            return 0;
        }
        case WM_SHOWWINDOW:
            if (!wp) {
                LibViewReleaseGraphics();
                std::vector<Album>().swap(v.albums);
                std::vector<Artist>().swap(v.artists);
                std::vector<Item>().swap(v.items);
                std::vector<int>().swap(v.albumItem);
                std::vector<char>().swap(v.selAlbum);
                std::vector<char>().swap(v.selArtist);
                decltype(v.selTrack)().swap(v.selTrack);
            } else if (v.albums.empty() && !g.view.empty() && CanvasActive()) {
                LibViewRebuild();
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            Paint();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof(si), SIF_ALL};
            GetScrollInfo(h, SB_VERT, &si);
            int y = v.scroll;
            switch (LOWORD(wp)) {
                case SB_LINEUP: y -= (int)F(40); break;
                case SB_LINEDOWN: y += (int)F(40); break;
                case SB_PAGEUP: y -= v.height; break;
                case SB_PAGEDOWN: y += v.height; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: y = si.nTrackPos; break;
                case SB_TOP: y = 0; break;
                case SB_BOTTOM: y = (int)v.contentHeight; break;
            }
            ScrollTo(y);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
                // Ctrl+wheel: bigger or smaller covers.
                v.tile = std::clamp(v.tile + (delta > 0 ? 20 : -20), 80, 300);
                IniWrite(L"Library", L"CoverSize", std::to_wstring(v.tile));
                DropBitmaps();
                LayoutItems();
                InvalidateRect(h, nullptr, FALSE);
            } else {
                ScrollTo(v.scroll - MulDiv(delta, (int)F(120), WHEEL_DELTA));
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (!v.tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                v.tracking = TrackMouseEvent(&tme);
            }
            UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            v.tracking = false;
            v.hoverItem = v.hoverRow = -1;
            v.hoverPlay = v.hoverClose = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;  // set in WM_MOUSEMOVE
            break;
        case WM_LBUTTONDOWN:
            OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false, wp);
            return 0;
        case WM_LBUTTONDBLCLK:
            OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true, wp);
            return 0;
        case WM_RBUTTONDOWN: {
            // Right-clicking outside the selection selects what's under the mouse first.
            Hit hit = HitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (hit.item >= 0) {
                const Item& it = v.items[hit.item];
                bool selected = (it.kind == ItemKind::Tile && v.selAlbum[it.index]) ||
                                (it.kind == ItemKind::ArtistTile && v.selArtist[it.index]) ||
                                (hit.row >= 0 && v.selTrack.count(v.albums[it.index].tracks[hit.row]));
                if (!selected && it.kind != ItemKind::Section) OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false, 0);
            }
            return 0;
        }
        case WM_CONTEXTMENU:
            return SendMessageW(g.hwnd, msg, wp, lp);
        case WM_KEYDOWN:
            OnKey(wp);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_APP_THUMB:
            // The loader posts one message while results are waiting; drain every ready bitmap.
            while (auto result = v.loader.TakeResult()) OnThumb(result.release());
            return 0;
        case WM_DESTROY:
            LibViewReleaseGraphics();
            v.hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------------------

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
    v.tile = std::clamp(IniInt(L"Library", L"CoverSize", 160), 80, 300);
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

void LibViewReleaseGraphics() {
    v.loader.Stop();
    v.loaderStarted = false;
    DropBitmaps();
    v.bitmaps.rehash(0);
    v.brush.Reset(); v.rt.Reset(); v.ellipsis.Reset();
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
        for (TrackId id : v.albums[a].tracks)
            if (whole || v.selTrack.count(id)) r.push_back(id);
    }
    for (int i = 0; i < (int)v.artists.size(); ++i)
        if (v.selArtist[i])
            for (int a : v.artists[i].albums) r.insert(r.end(), v.albums[a].tracks.begin(), v.albums[a].tracks.end());
    return r;
}

std::vector<AlbumSelection> LibViewSelectedAlbums() {
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
        position.expanded = v.albums[v.expanded].tracks.front();
    if (g.libMode == LibraryMode::AlbumTracks) position.focus = v.anchorTrack;
    else if (g.libMode == LibraryMode::Artists && v.focus >= 0 && v.focus < (int)v.artists.size())
        position.focus = v.albums[v.artists[v.focus].albums.front()].tracks.front();
    else if (g.libMode == LibraryMode::Albums && v.focus >= 0 && v.focus < (int)v.albums.size())
        position.focus = v.albums[v.focus].tracks.front();
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
        const auto& tracks = v.albums[a].tracks;
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
                for (TrackId id : v.albums[a].tracks) {
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
