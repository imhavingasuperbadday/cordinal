// Two panels for the spotlight and big mode (app_big.cpp):
//   Now playing  the title, the artist line and what plays next, large and centered, over a soft
//                backdrop blended from the cover's colors (the cover itself isn't shown). Click
//                the title to find the track in the list, or to continue a stopped one.
//   Up next      the queue, then the rest of the playlist in the order it will play (the shuffle
//                pass too). Double-click a track to play it; the wheel scrolls.
// Both are child windows created once; PanelsTick() refreshes them from up here, and the whole
// module keeps its state in the single file-scope `p`.
#include "app.h"

namespace app {

namespace {

const wchar_t kNowClass[] = L"CordinalNowPlaying";
const wchar_t kNextClass[] = L"CordinalUpNext";

struct Row {
    TrackId id;
    int pl, index;  // where it is in the playlist; -1 for a queued track
};

struct Panels {
    HWND now = nullptr, next = nullptr;
    // Now playing: the cover's colors as a 4 x 4 grid, and the backdrop drawn from them.
    TrackId artFor = kNone;
    COLORREF grid[4][4]{};
    std::vector<uint32_t> backdrop;
    int backdropW = 0, backdropH = 0;
    bool backdropDark = false;
    HFONT fontBig = nullptr, fontMid = nullptr;
    UINT fontDpi = 0;
    RECT title{};
    std::wstring shown;  // what the panel showed, to repaint only on a change
    // Up next.
    std::vector<Row> rows;
    int scroll = 0, hot = -1;
} p;

// Integer per-channel blend: bPercent (0..100) of `b` folded into `a`.
COLORREF Mix(COLORREF a, COLORREF b, int bPercent) {
    auto ch = [&](int s) { return (((a >> s) & 0xFF) * (100 - bPercent) + ((b >> s) & 0xFF) * bPercent) / 100; };
    return RGB(ch(0), ch(8), ch(16));
}

void Line(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF color, UINT align) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

// What plays after the current track: the queue first, then the playlist in playback order. In
// shuffle the remaining shuffle pass is listed (the queue is drawn as a stack, so it reads backwards
// here). `limit` bounds the work for the one-line Now playing case.
std::vector<Row> Upcoming(size_t limit) {
    std::vector<Row> rows;
    for (TrackId id : g.queue) {
        if (rows.size() >= limit) return rows;
        rows.push_back({id, -1, -1});
    }
    auto it = g.tokens.find(g.playingToken);
    if (g.playing == kNone || it == g.tokens.end()) return rows;
    int pl = it->second.pl;
    if (pl < 0 || pl >= (int)g.playlists.size()) return rows;
    const auto& items = g.playlists[pl].items;
    int n = (int)items.size(), cur = TokenIndex(g.playingToken, pl);
    if (g.radio) {
        if (rows.size() < limit && it->second.radioNext < items.size()) {
            int next = (int)it->second.radioNext;
            rows.push_back({items[next], pl, next});
        }
        return rows;
    }
    if (g.shuffle) {
        if (g.shufflePl != pl) return rows;  // the next pass isn't drawn yet
        for (auto r = g.shuffleQueue.rbegin(); r != g.shuffleQueue.rend() && rows.size() < limit; ++r)
            if (*r < items.size() && (int)*r != cur) rows.push_back({items[*r], pl, (int)*r});
        return rows;
    }
    for (int i = 1; i < n && rows.size() < limit; ++i) {
        int k = cur + i;
        if (k >= n) {
            if (g.repeat == Repeat::Off) break;
            k -= n;
        }
        rows.push_back({items[k], pl, k});
    }
    return rows;
}

// ---------------------------------------------------------------------------
// Now playing

// Samples a small (64px) copy of the current or last cover into the 4x4 grid; with no cover a
// diagonal blend of the accent pair stands in. Early-outs when the track hasn't changed.
void LoadCover() {
    TrackId id = g.playing != kNone ? g.playing : g.resumeTrack;
    if (id == p.artFor) return;
    p.artFor = id;
    p.backdropW = 0;
    // The cover, small: only its colors matter. Without one, the accents.
    ArtImage art;
    if (id != kNone) LoadAlbumArt(g_lib.Path(id), 64, art);
    for (int gy = 0; gy < 4; ++gy)
        for (int gx = 0; gx < 4; ++gx) {
            if (art.Empty()) {
                p.grid[gy][gx] = Mix(Ref(P().accent), Ref(P().accent2), (gx + gy) * 100 / 6);
                continue;
            }
            uint64_t sum[3] = {}, n = 0;
            for (int y = art.height * gy / 4; y < art.height * (gy + 1) / 4; ++y)
                for (int x = art.width * gx / 4; x < art.width * (gx + 1) / 4; ++x, ++n)
                    for (int c = 0; c < 3; ++c) sum[c] += (art.pixels[(size_t)y * art.width + x] >> (8 * c)) & 0xFF;
            n = std::max<uint64_t>(n, 1);
            p.grid[gy][gx] = RGB(sum[2] / n, sum[1] / n, sum[0] / n);
        }
}

// Builds the two panel fonts from the system message font at the current DPI; re-created when the
// DPI changes because GDI fonts are not DPI-scaled on their own.
void EnsureFonts() {
    if (p.fontDpi == g.dpi && p.fontBig) return;
    for (HFONT f : {p.fontBig, p.fontMid})
        if (f) DeleteObject(f);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -S(32);
    lf.lfWeight = FW_SEMIBOLD;
    p.fontBig = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(16);
    lf.lfWeight = FW_NORMAL;
    p.fontMid = CreateFontIndirectW(&lf);
    p.fontDpi = g.dpi;
}

// The backdrop: the grid's colors blended smoothly across the panel (a heavy blur of the cover),
// toned toward the surface so the text stays readable, and fading into it at the bottom edge.
// It is smooth enough to be built at a quarter of the panel's size and stretched when painted.
constexpr int kBackdropDiv = 4;
int BackdropSize(int full) { return std::max(1, (full + kBackdropDiv - 1) / kBackdropDiv); }

// Rebuilds the stretched bitmap only when the panel's size, the theme or the grid has changed.
void BuildBackdrop(int panelW, int panelH) {
    if (panelW == p.backdropW && panelH == p.backdropH && g.dark == p.backdropDark && !p.backdrop.empty()) return;
    p.backdropW = panelW;
    p.backdropH = panelH;
    p.backdropDark = g.dark;
    int w = BackdropSize(panelW), h = BackdropSize(panelH);
    p.backdrop.assign((size_t)w * h, 0);
    COLORREF surface = SurfaceColor();
    int tone = g.dark ? 52 : 62;  // how far toward the surface
    for (int y = 0; y < h; ++y) {
        double fy = std::clamp((y + 0.5) / h * 4 - 0.5, 0.0, 3.0);
        int y0 = std::min((int)fy, 2);
        double ty = fy - y0;
        double yFull = (y + 0.5) * panelH / h;
        int fade = std::max(0, 100 - (int)((panelH - yFull) * 100 / std::max(1, S(48))));  // into the surface below
        for (int x = 0; x < w; ++x) {
            double fx = std::clamp((x + 0.5) / w * 4 - 0.5, 0.0, 3.0);
            int x0 = std::min((int)fx, 2);
            double tx = fx - x0;
            uint32_t px = 0;
            for (int c = 0; c < 3; ++c) {
                auto ch = [&](int gy, int gx) { return (double)((p.grid[gy][gx] >> (8 * c)) & 0xFF); };
                double top = ch(y0, x0) * (1 - tx) + ch(y0, x0 + 1) * tx;
                double bottom = ch(y0 + 1, x0) * (1 - tx) + ch(y0 + 1, x0 + 1) * tx;
                double v = top * (1 - ty) + bottom * ty;
                double s = (surface >> (8 * c)) & 0xFF;
                v = v + (s - v) * (tone + (100 - tone) * fade / 100.0) / 100;
                px |= (uint32_t)std::clamp(v, 0.0, 255.0) << (8 * (2 - c));  // COLORREF is BGR; DIBs want RGB
            }
            p.backdrop[(size_t)y * w + x] = px;
        }
    }
}

// Paints the Now playing panel: the cached backdrop stretched to the client, then the title, artist
// line and a one-line "Up next", centered as a block. The title rect is kept in p.title for hit
// testing.
void PaintNow(HDC dc, RECT rc) {
    EnsureFonts();
    LoadCover();
    int w = rc.right, h = rc.bottom, pad = S(24);
    if (w <= 0 || h <= 0) return;
    BuildBackdrop(w, h);
    int bw = BackdropSize(w), bh = BackdropSize(h);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(bi.bmiHeader), bw, -bh, 1, 32};
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, 0, 0, w, h, 0, 0, bw, bh, p.backdrop.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    SetBkMode(dc, TRANSPARENT);

    // Title, artist line and what's next, centered as a block.
    int blockH = S(44) + S(28) + S(30), y = std::max(pad / 2, (h - blockH) / 2);
    p.title = {pad, y, w - pad, y + S(44)};
    Line(dc, WindowText(g.npTitle), p.title, p.fontBig, TextColor(), DT_CENTER);
    Line(dc, WindowText(g.npSub), {pad, y + S(44), w - pad, y + S(72)}, p.fontMid, SubColor(), DT_CENTER);
    auto next = Upcoming(1);
    if (!next.empty()) {
        std::wstring s = L"Up next  \x00B7  " + FormatTitle(g.fmtTitle, Lookup(next[0].id, false));
        std::wstring artist;
        if (LookupField(next[0].id, L"artist", artist) && !artist.empty()) s += L"  \x2014  " + artist;
        Line(dc, s, {pad, y + S(76), w - pad, y + S(100)}, g.font, SubColor(), DT_CENTER);
    }
}

// The Now playing window: paints the backdrop and text. Clicking the title resumes a stopped track,
// or (with nothing to resume) reveals the playing track in the list.
LRESULT CALLBACK NowProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: DoubleBufferedPaint(h, PaintNow); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONUP:
            if (PtInRect(&p.title, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) && !Resume()) RevealPlayingTrack();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(h, &pt);
                SetCursor(LoadCursorW(nullptr, PtInRect(&p.title, pt) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Up next

int RowHeight() { return S(46); }

int RowAt(int y) {
    int i = (y + p.scroll) / RowHeight();
    return y >= 0 && i < (int)p.rows.size() ? i : -1;
}

void ClampScroll() {
    RECT rc;
    GetClientRect(p.next, &rc);
    p.scroll = std::clamp(p.scroll, 0, std::max(0, (int)p.rows.size() * RowHeight() - (int)rc.bottom));
}

// Paints the Up next list: one row per entry, showing a "Q" for queued tracks and their place in
// line otherwise, then title, artist / album and duration. Only the visible rows are drawn.
void PaintNext(HDC dc, RECT rc) {
    FillSolid(dc, rc, WindowBg());
    SetBkMode(dc, TRANSPARENT);
    if (p.rows.empty()) {
        Line(dc, g.playing == kNone ? L"Play something to see what comes next." : L"Nothing comes next.", rc, g.font,
             Ref(P().faint), DT_CENTER);
        return;
    }
    int rh = RowHeight(), pad = S(12), numW = S(34), timeW = S(56);
    int first = p.scroll / rh;
    for (int i = first; i < (int)p.rows.size(); ++i) {
        int y = i * rh - p.scroll;
        if (y >= rc.bottom) break;
        const Row& row = p.rows[i];
        RECT r{0, y, rc.right, y + rh};
        if (i == p.hot) FillSolid(dc, r, Ref(P().hover));
        FillSolid(dc, {pad, r.bottom - 1, rc.right - pad, r.bottom}, Ref(P().border));
        // Queued tracks wear an accent "Q"; the rest their place in line.
        bool queued = row.pl < 0;
        Line(dc, queued ? L"Q" : std::to_wstring(i + 1), {pad, y, pad + numW, y + rh}, queued ? g.fontBold : g.font,
             queued ? Ref(P().accent) : Ref(P().faint), DT_LEFT);
        std::wstring artist, album;
        LookupField(row.id, L"artist", artist);
        LookupField(row.id, L"album", album);
        if (!album.empty()) artist += (artist.empty() ? L"" : L"  \x2022  ") + album;
        int x = pad + numW, right = rc.right - pad - timeW;
        Line(dc, FormatTitle(g.fmtTitle, Lookup(row.id, false)), {x, y + S(5), right, y + S(25)}, g.fontBold, TextColor(), DT_LEFT);
        Line(dc, artist, {x, y + S(24), right, y + S(42)}, g.font, SubColor(), DT_LEFT);
        Line(dc, FormatDuration(g_lib[row.id].Duration()), {right, y, rc.right - pad, y + rh}, g.font, SubColor(), DT_RIGHT);
    }
}

// Plays a row: a queued track is moved to the head of the queue and started, a playlist track is
// played in place (making its playlist current).
void PlayRowAt(int i) {
    if (i < 0 || i >= (int)p.rows.size()) return;
    Row row = p.rows[i];
    if (row.pl < 0) {
        // A queued track: to the head of the queue, then on to it.
        if (i < (int)g.queue.size()) g.queue.erase(g.queue.begin() + i);
        g.queue.push_front(row.id);
        Next();
    } else {
        PlayTrack(row.pl, row.index, true);
    }
}

// The Up next window: paints the list and handles the wheel, hover highlighting and double-click to
// play. Rows are recomputed by PanelsTick(); this proc only scrolls and picks.
LRESULT CALLBACK NextProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: DoubleBufferedPaint(h, PaintNext); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            ClampScroll();
            return 0;
        case WM_MOUSEWHEEL:
            p.scroll -= GET_WHEEL_DELTA_WPARAM(wp) * RowHeight() * 3 / WHEEL_DELTA;
            ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            int hot = RowAt(GET_Y_LPARAM(lp));
            if (hot != p.hot) {
                p.hot = hot;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            p.hot = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDBLCLK:
            PlayRowAt(RowAt(GET_Y_LPARAM(lp)));
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void CreatePanels(HWND parent) {
    RegisterAppClass(kNowClass, NowProc, CS_HREDRAW | CS_VREDRAW, false);
    RegisterAppClass(kNextClass, NextProc, CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW, false);
    p.now = CreateWindowExW(0, kNowClass, L"Now playing", WS_CHILD, 0, 0, 0, 0, parent, nullptr, g.inst, nullptr);
    p.next = CreateWindowExW(0, kNextClass, L"Up next", WS_CHILD, 0, 0, 0, 0, parent, nullptr, g.inst, nullptr);
}

HWND NowPlayingPanel() { return p.now; }
HWND UpNextPanel() { return p.next; }

// Periodic refresh (driven by the app timer). Each panel redraws only when its visible content
// string / row list actually differs, so an idle player stays cheap.
void PanelsTick() {
    if (IsWindowVisible(p.now)) {
        auto next = Upcoming(1);
        std::wstring shown = WindowText(g.npTitle) + L'\n' + WindowText(g.npSub) + L'\n' +
                             std::to_wstring(next.empty() ? kNone : next[0].id) + L'\n' +
                             std::to_wstring(g.playing != kNone ? g.playing : g.resumeTrack) + (g.dark ? L"d" : L"l");
        if (shown != p.shown) {
            p.shown = shown;
            InvalidateRect(p.now, nullptr, FALSE);
        }
    }
    if (IsWindowVisible(p.next)) {
        auto rows = Upcoming(200);
        bool same = rows.size() == p.rows.size() &&
                    std::equal(rows.begin(), rows.end(), p.rows.begin(), [](const Row& a, const Row& b) {
                        return a.id == b.id && a.pl == b.pl && a.index == b.index;
                    });
        if (!same) {
            p.rows = std::move(rows);
            ClampScroll();
            InvalidateRect(p.next, nullptr, FALSE);
        }
    }
}

}  // namespace app
