// The Library tab's visual views, painted with Direct2D (libview_internal.h): the Direct2D and DirectWrite
// resources, the cover bitmaps (decoded by ThumbLoader, kept in a small LRU cache), and every tile, panel,
// heading and track row of Albums, Albums & Tracks and Artists.
#include "libview_internal.h"

namespace app::lv {

bool EnsureGraphics() {
    if (!v.d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, v.d2d.GetAddressOf()))) return false;
    if (!v.dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown**>(v.dw.GetAddressOf())))) return false;
    if (!v.fText) CreateFormats();
    if (!v.loaderStarted) {
        v.loader.Start(v.hwnd, WM_APP_THUMB, LocalCacheDir(L"thumbs"),ReducedMemoryMode() ? 1 : 2, ReducedMemoryMode());
        v.loaderStarted = true;
    }
    return v.fText && v.fGlyph;
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
    if (!a.trackCount) return nullptr;
    TrackId id = a.artTrack != kNone && g_lib.Contains(a.artTrack) ? a.artTrack : Tracks(a)[0];
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
    double hue = bmp ? CoverHue((const uint8_t*)r->image.pixels.data(), r->image.pixels.size(), true) : -1;
    v.bitmaps[key] = {bmp, v.lru.begin(), bytes, bmp ? 0 : GetTickCount64(), hue};  // null caches "no art" briefly
    v.bitmapBytes += bytes;
    TrimCovers();
    if (bmp) InvalidateRect(v.hwnd, nullptr, FALSE);
}

// The hue of an album's cover from whichever of its cached sizes has arrived (`size` first), or -1.
double CachedHue(const Album& a, int size) {
    if (!a.coverKey) return -1;
    for (int s : {size, (int)TileArt()}) {
        auto it = v.bitmaps.find(a.coverKey ^ (uint64_t)s);
        if (it != v.bitmaps.end() && it->second.bmp) return it->second.hue;
    }
    return -1;
}

// The open album's panel colour: its cover's hue over the theme's surface.
Rgb PanelSurface(const Album& a, float width) {
    return AlbumPanelSurface(P(), CachedHue(a, (int)PanelCover(width)));
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
    // An empty or inverted box would lay the text out leftward, over whatever is beside it.
    if (r.right <= r.left || r.bottom <= r.top) return;
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

void DrawStarsD2D(D2D1_RECT_F r, uint8_t rating, bool selected, bool hover) {
    if (!ui::ShowEmptyStars(rating, hover, selected)) return;
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

// A row shows its rating stars only when the title keeps a useful width beside them.
bool RowStars(const D2D1_RECT_F& r) { return r.right - r.left >= F(34 + 120 + 72 + 56 + 6); }

// One track row: number, title (and the artist when it differs from the album's), stars, length.
void DrawTrackRow(const Album& a, int t, D2D1_RECT_F r, bool hover) {
    TrackId id = Tracks(a)[t];
    const Track& tr = g_lib[id];
    bool sel = v.selTrack.count(id) != 0, playing = IsPlaying(id);
    if (sel) Fill(r, Col(P().select));
    else if (hover) Fill(r, Col(P().hover));
    Rgb text = sel ? P().selectText : playing ? P().playing : P().text;
    Rgb sub = sel ? P().selectText : P().sub;
    // One buffer each: a scroll paints the same row shapes again, and these stay on the UI thread.
    static std::wstring num, title, len, who;
    if (playing) num = g.engine.Paused() ? L"\x275A\x275A" : L"\x25B6";
    else if (tr.trackNo) num = std::to_wstring(tr.trackNo);
    else num.clear();
    // A track cordinal is working on shows the spinner where its number goes.
    if (!playing && TrackBusy(id)) DrawSpinnerD2D({r.left + F(15), (r.top + r.bottom) / 2}, sel ? P().selectText : P().accent);
    else Text(num, playing ? v.fGlyph.Get() : v.fNum.Get(), {r.left + F(4), r.top, r.left + F(26), r.bottom}, Col(playing && !sel ? P().playing : sub));
    if (tr.title) AssignPool(title, tr.title);
    else title = StemOf(g_lib.Str(tr.file));
    float right = r.right - F(6);
    len = FormatDuration(tr.Duration());
    Text(len, v.fNum.Get(), {right - F(48), r.top, right, r.bottom}, Col(sub));
    right -= F(56);
    if (RowStars(r)) {
        DrawStarsD2D({right - F(64), r.top, right, r.bottom}, tr.rating, sel, hover);
        right -= F(72);
    }
    StringPool::Handle artist = tr.artist;
    if (artist && artist != a.artist) {
        float aw = std::min(F(220), std::max(0.f, right - r.left - F(34)) * 0.4f);
        AssignPool(who, artist);
        Text(who, v.fSmall.Get(), {right - aw, r.top, right, r.bottom}, Col(sub));
        right -= aw + F(8);
    }
    Text(title, playing ? v.fTitle.Get() : v.fText.Get(), {r.left + F(34), r.top, right, r.bottom}, Col(text));
}

const std::wstring& AlbumYear(const Album& a) {
    static std::wstring year;
    if (!a.date) {
        year.clear();
        return year;
    }
    AssignPool(year, a.date);
    if (year.size() > 4) year.resize(4);
    return year;
}

// Artist, year, genre and length; Albums & Tracks leaves the year out, as its heading shows it.
const std::wstring& AlbumSubtitle(const Album& a, bool year) {
    static std::wstring s;
    s = ArtistName(a.artist);
    if (year && a.date) {
        s += L"  \x2022  ";
        s += AlbumYear(a);
    }
    if (a.genre) {
        s += L"  \x2022  ";
        AppendPool(s, a.genre);
    }
    s += L"  \x2022  ";
    s += std::to_wstring(a.trackCount);
    s += a.trackCount == 1 ? L" track, " : L" tracks, ";
    s += FormatTotalDuration(a.seconds);
    if (a.editions > 1) {
        s += L"  \x2022  ";
        s += std::to_wstring(a.editions);
        s += L" editions";
    }
    return s;
}

// The panel's track rows (one or two columns) inside `area`.
D2D1_RECT_F PanelRow(const Album& a, D2D1_RECT_F area, int t) {
    int cols = PanelColumns(a, (float)v.width);
    int rows = (int)(a.trackCount + cols - 1) / cols;
    int c = t / rows, r = t % rows;
    float colW = (area.right - area.left - (cols - 1) * F(24)) / cols;
    float x = area.left + c * (colW + F(24));
    return {x, area.top + r * RowH(), x + colW, area.top + (r + 1) * RowH()};
}

D2D1_RECT_F PanelArea(const Item& it) {
    return {Margin() + F(28), it.rc.top + F(84), PanelRight(it.rc.right), it.rc.bottom - F(20)};
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
    float right = PanelRight(it.rc.right);
    return {std::max(right - F(210), Margin() + F(28)), it.rc.top + F(48), right, it.rc.top + F(72)};
}

// Row rects are in content space, same as v.scroll. A row that only clips the window still draws.
bool RowOnScreen(D2D1_RECT_F row) {
    const float top = (float)v.scroll - 1.0f;
    const float bottom = (float)v.scroll + (float)v.height + 1.0f;
    return row.bottom >= top && row.top <= bottom;
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
            // Selection is a quiet fill around the cover and its labels; the open album stands on its
            // panel's colour, so the tile and the panel under it read as one piece.
            D2D1_RECT_F bg{it.rc.left - F(6), it.rc.top - F(6), it.rc.right + F(6), it.rc.bottom + F(2)};
            if (sel) Fill(bg, Col(P().select));
            else if (open) Fill(bg, Col(PanelSurface(a, (float)v.width)));
            else if (hover) Fill(bg, Col(P().hover));
            D2D1_RECT_F art{it.rc.left, it.rc.top, it.rc.right, it.rc.top + TileArt()};
            const std::wstring& title = AlbumTitle(a);
            DrawCover(a, art, title);
            bool playing = std::any_of(Tracks(a).begin(), Tracks(a).end(), IsPlaying);
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
            static std::wstring sub;
            sub = ArtistName(a.artist);
            if (a.editions > 1) {
                sub += L"  \x2022  ";
                sub += std::to_wstring(a.editions);
                sub += L" editions";
            }
            Text(sub, v.fSmall.Get(), {it.rc.left, art.bottom + F(22), it.rc.right, art.bottom + F(40)}, Col(sc));
            break;
        }
        case ItemKind::Panel: {
            const Album& a = v.albums[it.index];
            // The cover's colour at the top, settling halfway into the page by the bottom.
            Rgb surface = PanelSurface(a, it.rc.right);
            ComPtr<ID2D1GradientStopCollection> stops;
            D2D1_GRADIENT_STOP stop[2] = {{0, Col(surface)}, {1, Mix(surface, P().bg, 0.5f)}};
            ComPtr<ID2D1LinearGradientBrush> fade;
            if (SUCCEEDED(v.rt->CreateGradientStopCollection(stop, 2, &stops)) &&
                SUCCEEDED(v.rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, it.rc.top), D2D1::Point2F(0, it.rc.bottom)),
                    stops.Get(), &fade)))
                v.rt->FillRectangle(it.rc, fade.Get());
            else Fill(it.rc, Col(surface));
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
                v.brush->SetColor(Col(surface));
                v.rt->FillGeometry(tri.Get(), v.brush.Get());
            }
            D2D1_RECT_F close = PanelClose(it);
            if (v.hoverClose && hover) Fill(close, Col(P().hover));
            Text(L"\x2715", v.fText.Get(), {close.left + F(5), close.top + F(2), close.right, close.bottom}, Col(P().sub));
            float x = Margin() + F(28), right = PanelRight(it.rc.right);
            static std::wstring heading;
            heading = AlbumTitle(a);
            heading += L"  \x25B8";
            Text(heading, v.fSection.Get(), {x, it.rc.top + F(14), right, it.rc.top + F(50)}, Col(P().text));
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
            for (int t = 0; t < (int)a.trackCount; ++t) {
                D2D1_RECT_F row = PanelRow(a, area, t);
                if (RowOnScreen(row)) DrawTrackRow(a, t, row, hover && v.hoverRow == t);
            }
            float side = std::min(PanelCover(it.rc.right), it.rc.bottom - it.rc.top - F(40));
            D2D1_RECT_F art{it.rc.right - Margin() - side, it.rc.top + F(20), it.rc.right - Margin(), it.rc.top + F(20) + side};
            if (side > 0) DrawCover(a, art, AlbumTitle(a));
            break;
        }
        case ItemKind::AlbumGroup: {
            const Album& a = v.albums[it.index];
            float side = GroupArt(it.rc.right - it.rc.left);
            D2D1_RECT_F art{it.rc.left, it.rc.top + F(4), it.rc.left + side, it.rc.top + F(4) + side};
            DrawCover(a, art, AlbumTitle(a));
            // The heading: the title, and the year on the right over the rows' lengths, then the
            // details and a rule that ties the heading to its tracks.
            float x = GroupX(it.rc), right = it.rc.right - F(6);
            const std::wstring& year = AlbumYear(a);
            if (!year.empty()) {
                Text(year, v.fNum.Get(), {right - F(60), it.rc.top, right, it.rc.top + F(26)}, Col(P().sub));
                right -= F(68);
            }
            Text(AlbumTitle(a), v.fHeading.Get(), {x, it.rc.top, right, it.rc.top + F(26)}, Col(P().text));
            Text(AlbumSubtitle(a, false), v.fSmall.Get(), {x, it.rc.top + F(26), it.rc.right - F(6), it.rc.top + F(44)}, Col(P().sub));
            Fill({x - F(6), it.rc.top + GroupHead() - F(6), it.rc.right, it.rc.top + GroupHead() - F(5)}, Col(P().border));
            for (int t = 0; t < (int)a.trackCount; ++t) {
                D2D1_RECT_F row = GroupRow(it.rc, t);
                if (RowOnScreen(row)) DrawTrackRow(a, t, row, hover && v.hoverRow == t);
            }
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
            Text(ArtistName(ar.name), v.fTitle.Get(), {it.rc.left, art.bottom + F(8), it.rc.right, art.bottom + F(26)}, Col(tc));
            static std::wstring info;
            info = std::to_wstring(ar.albums.size());
            info += ar.albums.size() == 1 ? L" album" : L" albums";
            Text(info, v.fSmall.Get(), {it.rc.left, art.bottom + F(26), it.rc.right, art.bottom + F(44)}, Col(sc));
            break;
        }
    }
}

void ReleaseTarget() {
    v.brush.Reset();
    v.rt.Reset();
    v.gpu.Reset();
    v.soft.Reset();
}

void Paint(HDC dc) {
    if (!IsWindowVisible(v.hwnd)) return;
    if (!EnsureGraphics()) return;
    RECT rc;
    GetClientRect(v.hwnd, &rc);
    if (!v.rt) {
        if (GpuDrawing()) {
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
            props.dpiX = props.dpiY = 96.0f;  // work in pixels; S() does the scaling
            if (FAILED(v.d2d->CreateHwndRenderTarget(
                    props, D2D1::HwndRenderTargetProperties(v.hwnd, D2D1::SizeU(rc.right, rc.bottom)), &v.gpu)))
                return;
            v.rt = v.gpu;
        } else {
            v.rt = v.soft.Context(v.d2d.Get());
            if (!v.rt) return;
        }
        v.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        v.rt->CreateSolidColorBrush(D2D1::ColorF(0), &v.brush);
    }
    if (v.gpu) v.rt->BeginDraw();
    else if (!v.soft.Begin(rc.right, rc.bottom)) return;
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
    if ((v.gpu ? v.rt->EndDraw() : v.soft.End(dc, 0, 0)) == D2DERR_RECREATE_TARGET) {
        ReleaseTarget();
        DropBitmaps();
        InvalidateRect(v.hwnd, nullptr, FALSE);
    }
    if (!v.gpu) SetTimer(v.hwnd, kSurfaceTimer, kSurfaceIdleMs, nullptr);  // each frame restarts the wait
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

}  // namespace app::lv
