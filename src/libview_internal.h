// What the Library tab's visual views share (libview.h is their public face): the albums and artists
// they show, the items laid out for them, the Direct2D resources and cover cache, and each part's entry
// points. libview.cpp builds the data and holds the public calls, libview_draw.cpp paints, and
// libview_input.cpp handles the mouse, keyboard and scrolling. Private to those files.
#pragma once
#include "album_editions.h"
#include "app.h"
#include "soft_canvas.h"
#include "thumbs.h"

#include <d2d1.h>
#include <dwrite.h>

#include <list>
#include <span>

namespace app::lv {

constexpr UINT WM_APP_THUMB = WM_APP + 20;
const wchar_t kLibViewClass[] = L"CordinalLibraryView";
constexpr UINT_PTR kScrollTimer = 0x5CA1;  // Smooth scrolling's frame timer on the library view
// Frees the canvas's window-sized surface once no frame has been drawn for this long. A maximized
// 1920 x 1080 window holds about 8 MB there, a 4K one over 30 MB, and an idle view needs none of it.
constexpr UINT_PTR kSurfaceTimer = 0x5CA2;
constexpr UINT kSurfaceIdleMs = 3000;

// One album in the view: its album artist and album name, the tracks grouped under it, and the
// lazily-derived cover identity. `letter` is the section heading it lands under in Albums.
struct Album {
    StringPool::Handle artist = 0, album = 0, date = 0, genre = 0;
    uint32_t trackBegin = 0, trackCount = 0;  // a slice of v.albumTracks
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
    double hue = -1;       // the cover's hue (CoverHue), which an open album's panel is washed with
};

// All view state, touched only on the UI thread: covers are decoded by ThumbLoader workers but
// wrapped in bitmaps and hit-tested here. Item rects are content pixels, not client pixels.
struct LibView {
    HWND hwnd = nullptr;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<ID2D1RenderTarget> rt;      // what Paint draws on: the canvas's context, or the GPU's window target
    SoftCanvas soft;                   // the usual target, drawn in software on this thread
    ComPtr<ID2D1HwndRenderTarget> gpu; // with "Draw with the graphics card"
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteTextFormat> fTitle, fText, fSmall, fNum, fHeading, fSection, fBig, fStars, fGlyph;
    ComPtr<IDWriteInlineObject> ellipsis;

    std::vector<Album> albums;
    std::vector<TrackId> albumTracks;  // every album's tracks, in album order
    std::vector<Artist> artists;
    std::vector<Item> items;
    std::vector<int> albumItem;  // album -> its tile / group item
    float contentHeight = 0;
    int scroll = 0, width = 0, height = 0;
    int scrollGoal = 0;             // Smooth scrolling: where the wheel or paging is taking the view
    bool scrollAnimating = false;   // the scroll timer is running
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
};
extern LibView v;

inline std::span<const TrackId> Tracks(const Album& a) {
    if (!a.trackCount) return {};
    return {v.albumTracks.data() + a.trackBegin, (size_t)a.trackCount};
}

// Geometry (all in pixels; the render target uses 96 DPI so 1 DIP = 1 pixel)
inline float F(int px) { return (float)S(px); }
inline float RowH() { return F(26); }
inline float Margin() { return F(18); }
inline float TileArt() { return F(v.tile); }
inline float TileH() { return TileArt() + F(44); }
// The narrowest a column of track rows gets beside a cover; a narrow window (the compact one)
// shrinks the cover rather than squeezing the rows under it.
inline float MinRowsW() { return F(220); }
// Albums & Tracks: the cover follows the grid's size, a quarter larger, so Ctrl+wheel sizes both
// views; the heading (title and year, the details, a rule) sits over the album's track rows.
// `width` is the album item's.
inline float GroupArt(float width) { return std::clamp(width - F(20) - MinRowsW(), F(48), F(v.tile + v.tile / 4)); }
inline float GroupHead() { return F(56); }
inline float GroupX(const D2D1_RECT_F& rc) { return rc.left + GroupArt(rc.right - rc.left) + F(20); }
inline D2D1_RECT_F GroupRow(const D2D1_RECT_F& rc, int t) {
    float y = rc.top + GroupHead() + t * RowH();
    return {GroupX(rc) - F(6), y, rc.right, y + RowH()};
}

inline D2D1_COLOR_F Col(Rgb c, float a = 1) {
    return D2D1::ColorF(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f, a);
}
inline D2D1_COLOR_F Mix(Rgb a, Rgb b, float t) {
    auto ca = Col(a), cb = Col(b);
    return D2D1::ColorF(ca.r + (cb.r - ca.r) * t, ca.g + (cb.g - ca.g) * t, ca.b + (cb.b - ca.b) * t);
}

void AssignPool(std::wstring& out, StringPool::Handle h);
void AppendPool(std::wstring& out, StringPool::Handle h);
const std::wstring& AlbumTitle(const Album& a);
const std::wstring& ArtistName(StringPool::Handle h);
wchar_t SectionLetter(StringPool::Handle artist);
float PanelCover(float width);
float PanelRight(float width);
int PanelColumns(const Album& a, float width);
float PanelHeight(const Album& a, float width);
void BuildGroups();
void GridMetrics(float itemW, int& cols, float& gap);
void LayoutItems();
bool EnsureGraphics();
size_t CoverBudget();
void TrimCovers();
ID2D1Bitmap* Cover(const Album& a, int size);
void OnThumb(ThumbResult* r);
double CachedHue(const Album& a, int size);
Rgb PanelSurface(const Album& a, float width);
void DropBitmaps();
void Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c);
void Fill(D2D1_RECT_F r, D2D1_COLOR_F c);
void DrawCover(const Album& a, D2D1_RECT_F r, const std::wstring& title);
void DrawStarsD2D(D2D1_RECT_F r, uint8_t rating, bool selected, bool hover);
bool IsPlaying(TrackId id);
void DrawSpinnerD2D(D2D1_POINT_2F c, Rgb color);
bool RowStars(const D2D1_RECT_F& r);
void DrawTrackRow(const Album& a, int t, D2D1_RECT_F r, bool hover);
const std::wstring& AlbumYear(const Album& a);
const std::wstring& AlbumSubtitle(const Album& a, bool year = true);
D2D1_RECT_F PanelRow(const Album& a, D2D1_RECT_F area, int t);
D2D1_RECT_F PanelArea(const Item& it);
D2D1_RECT_F PlayButton(const Item& it);
D2D1_RECT_F PanelClose(const Item& it);
bool HasEditionSwitch(const Album& a);
D2D1_RECT_F PanelEditionRect(const Item& it);
bool RowOnScreen(D2D1_RECT_F row);
void DrawItem(const Item& it, int index);
void ReleaseTarget();
void Paint(HDC dc);
void CreateFormats();
void ClearCanvasSelection();
int MaxScroll();
void PlaceScroll(int y);
void StopSmoothScroll();
void ScrollTo(int y);
void SmoothScrollTo(int y);
void StepSmoothScroll();
void EnsureVisible(const D2D1_RECT_F& r);
std::vector<std::pair<int, int>> AllRows();
std::vector<TrackId> ViewTracks();
void PlayAlbum(int album, int track);
void ShowEditionPicker(int album, POINT pt);
void ActivateRow(int album, int track);
void ShowArtist(int artist);
void OnClick(int x, int y, bool dbl, WPARAM keys);
void MoveFocus(int dx, int dy);
void OnKey(WPARAM key);
void UpdateHover(int x, int y);
LRESULT CALLBACK LibViewProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

}  // namespace app::lv
