// Two panels for the spotlight and big mode (app_big.cpp):
//   Now playing  the title, the artist line and what plays next, large and centered, over a soft
//                backdrop blended from the cover's colors (the cover itself isn't shown). Click
//                the title to find the track in the list, or to continue a stopped one.
//   Up next      the queue editor: what you queued, Choose by taste's picks, then the rest of the
//                playlist in the order it will play (the shuffle pass too). Queued tracks can be
//                selected, dragged into a new order, removed and played; tracks dragged from the
//                list land in the queue. It is also the normal window's side panel.
// Both are child windows created once; PanelsTick() refreshes them from up here, and the whole
// module keeps its state in the single file-scope `p`.
#include "app.h"

namespace app {

namespace {

const wchar_t kNowClass[] = L"CordinalNowPlaying";
const wchar_t kNextClass[] = L"CordinalUpNext";

// Up next's rows: your queued tracks, the taste picks after them, what the playlist plays then, and
// the labels in between.
enum RowKind : uint8_t { kYours, kPick, kThen, kLabelPicks, kLabelThen };

struct Row {
    TrackId id;
    int pl, index;  // queued rows: the queue position (pl -1); kThen: the playlist and position in it
    RowKind kind;
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
    std::vector<int> tops;
    UINT topsDpi = 0;
    std::deque<TrackId> seen;  // the queue the rows and selection were built for
    std::vector<char> sel;     // per queue position
    int scroll = 0, hot = -1, hotLink = 0, focus = -1, anchor = -1;
    RECT clearLink{}, repickLink{};
    // A press on a queued row: a drag past the system's threshold moves the selection to dropSlot.
    bool press = false, dragging = false;
    POINT pressAt{};
    int deferSelect = -1, dropSlot = -1;
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

// What plays after the current track: the queue first (yours, then the taste picks), then the
// playlist in playback order. In shuffle the remaining shuffle pass is listed (the queue is drawn as
// a stack, so it reads backwards here); while taste picks, the picks are what follows. `labels` adds
// the editor's section labels; `limit` bounds the work for the one-line Now playing case.
std::vector<Row> Upcoming(size_t limit, bool labels) {
    std::vector<Row> rows;
    auto add = [&](Row row) {
        if (rows.size() >= limit) return false;
        rows.push_back(row);
        return true;
    };
    size_t yours = g.queue.size() - std::min(g.queuePicks, g.queue.size());
    for (size_t i = 0; i < g.queue.size(); ++i) {
        if (labels && i == yours) add({kNone, -1, -1, kLabelPicks});
        if (!add({g.queue[i], -1, (int)i, i < yours ? kYours : kPick})) return rows;
    }
    if (g.queuePicks) return rows;
    auto it = g.tokens.find(g.playingToken);
    if (g.playing == kNone || it == g.tokens.end()) return rows;
    int pl = it->second.pl;
    if (pl < 0 || pl >= (int)g.playlists.size()) return rows;
    const auto& items = g.playlists[pl].items;
    int n = (int)items.size(), cur = TokenIndex(g.playingToken, pl);
    bool labelled = !labels;
    auto then = [&](int k) {
        if (!labelled) {
            labelled = true;
            add({kNone, pl, -1, kLabelThen});
        }
        return add({items[k], pl, k, kThen});
    };
    if (g.radio) {
        if (it->second.radioNext < items.size()) then((int)it->second.radioNext);
        return rows;
    }
    if (g.shuffle) {
        if (g.shufflePl != pl) return rows;  // the next pass isn't drawn yet
        for (auto r = g.shuffleQueue.rbegin(); r != g.shuffleQueue.rend(); ++r)
            if (*r < items.size() && (int)*r != cur && !then((int)*r)) break;
        return rows;
    }
    for (int i = 1; i < n; ++i) {
        int k = cur + i;
        if (k >= n) {
            if (g.repeat == Repeat::Off) break;
            k -= n;
        }
        if (!then(k)) break;
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
    auto next = Upcoming(1, false);
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
// Up next: the queue editor

int HeaderHeight() { return S(40); }
int RowHeight(const Row& row) { return row.kind >= kLabelPicks ? S(26) : S(44); }
bool QueueRow(const Row& row) { return row.kind == kYours || row.kind == kPick; }

// Each row's top in the scrolled list (one more entry for the end), rebuilt when the rows or the
// DPI change.
const std::vector<int>& Tops() {
    if (p.tops.size() != p.rows.size() + 1 || p.topsDpi != g.dpi) {
        p.tops.assign(1, 0);
        for (const Row& row : p.rows) p.tops.push_back(p.tops.back() + RowHeight(row));
        p.topsDpi = g.dpi;
    }
    return p.tops;
}

int ListHeight() {
    RECT rc;
    GetClientRect(p.next, &rc);
    return std::max(0, (int)rc.bottom - HeaderHeight());
}

int RowAt(int y) {
    if (y < HeaderHeight()) return -1;
    const auto& tops = Tops();
    int at = y - HeaderHeight() + p.scroll;
    auto it = std::upper_bound(tops.begin(), tops.end(), at);
    int i = (int)(it - tops.begin()) - 1;
    return i >= 0 && i < (int)p.rows.size() ? i : -1;
}

// The row showing queue position `index`, or -1.
int RowOfQueued(int index) {
    for (int i = 0; i < (int)p.rows.size(); ++i)
        if (QueueRow(p.rows[i]) && p.rows[i].index == index) return i;
    return -1;
}

void ClampScroll() { p.scroll = std::clamp(p.scroll, 0, std::max(0, Tops().back() - ListHeight())); }

void EnsureVisible(int row) {
    if (row < 0) return;
    const auto& tops = Tops();
    if (tops[row] < p.scroll) p.scroll = tops[row];
    else if (tops[row + 1] > p.scroll + ListHeight()) p.scroll = tops[row + 1] - ListHeight();
    ClampScroll();
}

size_t Yours() { return g.queue.size() - std::min(g.queuePicks, g.queue.size()); }

std::vector<int> SelectedQueued() {
    std::vector<int> out;
    for (int i = 0; i < (int)p.sel.size() && i < (int)g.queue.size(); ++i)
        if (p.sel[i]) out.push_back(i);
    return out;
}

void SelectOnly(int index) {
    p.sel.assign(g.queue.size(), 0);
    if (index >= 0 && index < (int)p.sel.size()) p.sel[index] = 1;
    p.focus = p.anchor = index;
}

// Rebuilds the rows from the queue and the playlist, keeping the selection on the same entries:
// the queue's head leaving (it started playing) shifts it, anything else keeps it by position.
void Refresh(bool repaint = true) {
    auto rows = Upcoming(200, true);
    size_t n = g.queue.size(), seen = p.seen.size();
    bool queueChanged = g.queue != p.seen;
    if (n < seen && std::equal(g.queue.begin(), g.queue.end(), p.seen.begin() + (seen - n))) {
        int k = (int)(seen - n);
        p.sel.erase(p.sel.begin(), p.sel.begin() + std::min<size_t>(k, p.sel.size()));
        p.focus = std::max(-1, p.focus - k);
        p.anchor = std::max(-1, p.anchor - k);
    }
    p.sel.resize(n, 0);
    if (p.focus >= (int)n) p.focus = (int)n - 1;
    if (p.anchor >= (int)n) p.anchor = p.focus;
    p.seen = g.queue;
    bool same = rows.size() == p.rows.size() &&
                std::equal(rows.begin(), rows.end(), p.rows.begin(), [](const Row& a, const Row& b) {
                    return a.id == b.id && a.pl == b.pl && a.index == b.index && a.kind == b.kind;
                });
    if (!same) {
        p.rows = std::move(rows);
        p.tops.clear();
        ClampScroll();
    }
    if (repaint || queueChanged || !same) InvalidateRect(p.next, nullptr, FALSE);
}

// Where a drag over the list at `y` would put tracks: the queue position they would land before.
// Your own tracks (and drops from the track list) never land below the picks.
int SlotAt(int y, bool amongYours) {
    int slot = (int)g.queue.size();
    const auto& tops = Tops();
    int at = y - HeaderHeight() + p.scroll;
    for (int i = 0; i < (int)p.rows.size(); ++i)
        if (QueueRow(p.rows[i]) && at < (tops[i] + tops[i + 1]) / 2) {
            slot = p.rows[i].index;
            break;
        }
    return amongYours ? std::min(slot, (int)Yours()) : slot;
}

// The insertion line's y for `slot`: under the entry before it, or above the first.
int SlotY(int slot) {
    const auto& tops = Tops();
    int row = slot > 0 ? RowOfQueued(slot - 1) : RowOfQueued(0);
    int y = row < 0 ? 0 : slot > 0 ? tops[row + 1] : tops[row];
    return HeaderHeight() + y - p.scroll;
}

void ScrollNearEdge(int y) {
    int edge = S(18);
    if (y < HeaderHeight() + edge) p.scroll -= S(10);
    else if (y > HeaderHeight() + ListHeight() - edge) p.scroll += S(10);
    ClampScroll();
}

// The header's links: Clear (with anything queued) and New picks (while taste is picking).
void LayoutLinks(HDC dc, RECT rc) {
    p.clearLink = p.repickLink = {};
    HGDIOBJ old = SelectObject(dc, g.font);
    auto width = [&](const wchar_t* s) {
        SIZE size{};
        GetTextExtentPoint32W(dc, s, (int)wcslen(s), &size);
        return (int)size.cx;
    };
    int x = rc.right - S(12), h = HeaderHeight();
    if (!g.queue.empty()) {
        int w = width(L"Clear");
        p.clearLink = {x - w, 0, x, h};
        x -= w + S(14);
    }
    if (g.queuePicks && TastePicking()) {
        int w = width(L"New picks");
        p.repickLink = {x - w, 0, x, h};
    }
    SelectObject(dc, old);
}

int LinkAt(POINT pt) {
    if (PtInRect(&p.clearLink, pt)) return 1;
    if (PtInRect(&p.repickLink, pt)) return 2;
    return 0;
}

void FollowLink(int link) {
    if (link == 1) ClearQueue();
    else if (link == 2) RepickTaste();
    else return;
    p.sel.clear();
    p.focus = p.anchor = -1;
    Refresh();
}

// Paints the header (Up next, the queue's length and the links), then the visible rows: your tracks numbered
// in the accent, the taste picks under their own label, then what the playlist plays after them.
void PaintNext(HDC dc, RECT rc) {
    FillSolid(dc, rc, WindowBg());
    SetBkMode(dc, TRANSPARENT);
    int pad = S(12), hh = HeaderHeight();
    // Header.
    LayoutLinks(dc, rc);
    double total = 0;
    for (TrackId id : g.queue) total += g_lib[id].Duration();
    SIZE titleSize{};
    HGDIOBJ old = SelectObject(dc, g.fontBold);
    GetTextExtentPoint32W(dc, L"Up next", 7, &titleSize);
    SelectObject(dc, old);
    Line(dc, L"Up next", {pad, 0, pad + titleSize.cx, hh}, g.fontBold, TextColor(), DT_LEFT);
    std::wstring sub = g.queue.empty() ? L"nothing queued"
                                       : std::to_wstring(g.queue.size()) + L" queued, " + FormatDuration(total);
    int linksLeft = p.repickLink.left ? p.repickLink.left : p.clearLink.left ? p.clearLink.left : rc.right;
    Line(dc, sub, {pad + titleSize.cx + S(8), 0, linksLeft - S(8), hh}, g.font, Ref(P().faint), DT_LEFT);
    for (int link : {1, 2}) {
        const RECT& r = link == 1 ? p.clearLink : p.repickLink;
        if (r.right <= r.left) continue;
        Line(dc, link == 1 ? L"Clear" : L"New picks", r, g.font, p.hotLink == link ? TextColor() : Ref(P().accent), DT_LEFT);
    }
    FillSolid(dc, {0, hh - 1, rc.right, hh}, Ref(P().border));

    // The paint surface is shared between controls, so the clip goes back afterwards.
    int saved = SaveDC(dc);
    IntersectClipRect(dc, 0, hh, rc.right, rc.bottom);
    if (p.rows.empty()) {
        const wchar_t* text = g.playing == kNone ? L"Play something, or drag tracks here to queue them."
                                                 : L"Nothing comes next. Drag tracks here to queue them.";
        RECT r{pad, hh, rc.right - pad, rc.bottom};
        Line(dc, text, r, g.font, Ref(P().faint), DT_CENTER);
    }
    const auto& tops = Tops();
    bool focused = GetFocus() == p.next;
    int numW = S(30), timeW = S(52);
    for (int i = 0; i < (int)p.rows.size(); ++i) {
        int y = hh + tops[i] - p.scroll, rh = tops[i + 1] - tops[i];
        if (y + rh <= hh) continue;
        if (y >= rc.bottom) break;
        const Row& row = p.rows[i];
        RECT r{0, y, rc.right, y + rh};
        if (row.kind == kLabelPicks || row.kind == kLabelThen) {
            std::wstring label = row.kind == kLabelPicks ? L"Picked by taste" : L"Then from " + g.playlists[row.pl].name;
            Line(dc, label, {pad, y, rc.right - pad, y + rh}, g.font, Ref(P().faint), DT_LEFT);
            continue;
        }
        bool queued = QueueRow(row);
        bool selected = queued && row.index < (int)p.sel.size() && p.sel[row.index];
        bool dragged = selected && p.dragging;
        COLORREF text = TextColor(), sub2 = SubColor();
        if (selected) {
            FillSolid(dc, r, Ref(P().select));
            text = sub2 = Ref(P().selectText);
        } else if (i == p.hot && !p.dragging) {
            FillSolid(dc, r, Ref(P().hover));
        }
        if (focused && queued && row.index == p.focus && !(SendMessageW(p.next, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
            FillSolid(dc, {0, y, S(3), y + rh}, Ref(P().accent));
        if (!selected) FillSolid(dc, {pad, r.bottom - 1, rc.right - pad, r.bottom}, Ref(P().border));
        // Your tracks wear their place in line in the accent; picks and the playlist's are quieter.
        std::wstring number = queued ? std::to_wstring(row.index + 1) : L"";
        COLORREF numColor = selected ? text : row.kind == kYours ? Ref(P().accent) : Ref(P().faint);
        Line(dc, number, {pad, y, pad + numW, y + rh}, row.kind == kYours ? g.fontBold : g.font, numColor, DT_LEFT);
        std::wstring artist, album;
        LookupField(row.id, L"artist", artist);
        LookupField(row.id, L"album", album);
        if (!album.empty()) artist += (artist.empty() ? L"" : L"  \x2022  ") + album;
        int x = pad + numW, right = rc.right - pad - timeW;
        if (dragged) text = sub2 = Ref(P().faint);
        Line(dc, FormatTitle(g.fmtTitle, Lookup(row.id, false)), {x, y + S(4), right, y + S(23)},
             row.kind == kThen ? g.font : g.fontBold, row.kind == kThen && !selected ? SubColor() : text, DT_LEFT);
        Line(dc, artist, {x, y + S(22), right, y + S(40)}, g.font, row.kind == kThen && !selected ? Ref(P().faint) : sub2, DT_LEFT);
        Line(dc, FormatDuration(g_lib[row.id].Duration()), {right, y, rc.right - pad, y + rh}, g.font, sub2, DT_RIGHT);
    }
    // Where a drag would land.
    if (p.dropSlot >= 0) {
        int y = SlotY(p.dropSlot);
        FillSolid(dc, {pad, y - S(1), rc.right - pad, y + S(1)}, Ref(P().accent));
        FillSolid(dc, {pad, y - S(4), pad + S(2), y + S(4)}, Ref(P().accent));
    }
    RestoreDC(dc, saved);
}

// Moves the selection to land before queue position `to`, keeping it selected.
void MoveSelection(int to, bool keep = false) {
    auto picked = SelectedQueued();
    if (picked.empty()) return;
    int first = MoveInQueue(picked, to, keep);
    if (first < 0) return;
    p.seen = g.queue;
    p.sel.assign(g.queue.size(), 0);
    for (int i = first; i < first + (int)picked.size() && i < (int)p.sel.size(); ++i) p.sel[i] = 1;
    p.focus = p.anchor = first;
    Refresh();
    EnsureVisible(RowOfQueued(first));
}

void RemoveSelection() {
    auto picked = SelectedQueued();
    if (picked.empty()) return;
    RemoveFromQueue(picked);
    p.seen = g.queue;
    SelectOnly(g.queue.empty() ? -1 : std::min(picked.front(), (int)g.queue.size() - 1));
    Refresh();
    EnsureVisible(RowOfQueued(p.focus));
}

// Plays a row: a queued track now (it moves to the head of the queue), a playlist track in place.
void PlayRow(const Row& row) {
    if (QueueRow(row)) PlayQueued(row.index);
    else if (row.kind == kThen) PlayTrack(row.pl, row.index, true);
    p.sel.clear();
    p.focus = p.anchor = -1;
    Refresh();
}

void PlayRowAt(int i) {
    if (i >= 0 && i < (int)p.rows.size()) PlayRow(p.rows[i]);
}

// The right-click menu: for queued tracks the moves, Keep for picks, Remove; for the playlist's
// tracks Play, Play next and Add to queue; and the whole queue's Clear and New picks.
void ShowNextMenu(POINT screen, int row) {
    enum { kPlay = 1, kTop, kBottom, kKeep, kRemove, kPlayNext, kAddToQueue, kRepick, kClear };
    HMENU m = CreatePopupMenu();
    // The menu pumps messages, and a tick may rebuild the rows meanwhile: work from a copy. Queued
    // tracks are acted on through the selection, which follows the queue.
    Row target = row >= 0 && row < (int)p.rows.size() ? p.rows[row] : Row{kNone, -1, -1, kLabelThen};
    bool queued = QueueRow(target), then = target.kind == kThen;
    if (queued) {
        if (!p.sel[target.index]) SelectOnly(target.index);
        p.focus = target.index;
        InvalidateRect(p.next, nullptr, FALSE);
        auto picked = SelectedQueued();
        bool anyPick = std::any_of(picked.begin(), picked.end(), [](int i) { return (size_t)i >= Yours(); });
        UINT single = picked.size() == 1 ? 0 : MF_GRAYED;
        AppendMenuW(m, MF_STRING | single, kPlay, L"&Play now\tEnter");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kTop, L"Move to the &top");
        AppendMenuW(m, MF_STRING, kBottom, L"Move to the &bottom");
        if (anyPick) AppendMenuW(m, MF_STRING, kKeep, L"&Keep (play before the picks)");
        AppendMenuW(m, MF_STRING, kRemove, L"&Remove from the queue\tDel");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    } else if (then) {
        AppendMenuW(m, MF_STRING, kPlay, L"&Play");
        AppendMenuW(m, MF_STRING, kPlayNext, L"Play ne&xt");
        AppendMenuW(m, MF_STRING, kAddToQueue, L"Add to &queue");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    if (g.queuePicks && TastePicking()) AppendMenuW(m, MF_STRING, kRepick, L"&New picks");
    AppendMenuW(m, MF_STRING | (g.queue.empty() ? MF_GRAYED : 0), kClear, L"&Clear the queue");
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, p.next, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case kPlay:
            if (then) PlayRow(target);
            else if (auto picked = SelectedQueued(); picked.size() == 1) PlayRowAt(RowOfQueued(picked.front()));
            break;
        case kTop: MoveSelection(0); break;
        case kBottom: MoveSelection((int)g.queue.size()); break;
        case kKeep: MoveSelection((int)Yours(), true); break;
        case kRemove: RemoveSelection(); break;
        case kPlayNext: QueueTracks({target.id}, true); Refresh(); break;
        case kAddToQueue: QueueTracks({target.id}, false); Refresh(); break;
        case kRepick: FollowLink(2); break;
        case kClear: FollowLink(1); break;
    }
}

// Arrow keys move the focus (Shift extends the selection), Alt or Ctrl with them moves the selected
// tracks, Home and End jump, Delete removes, Enter plays, Ctrl+A selects the whole queue.
bool NextKey(WPARAM key) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    int n = (int)g.queue.size();
    if (n == 0) return false;
    p.sel.resize(n, 0);
    if ((key == VK_UP || key == VK_DOWN) && (alt || ctrl)) {
        auto picked = SelectedQueued();
        if (picked.empty()) return true;
        if (key == VK_UP && picked.front() > 0) MoveSelection(picked.front() - 1);
        else if (key == VK_DOWN && picked.back() < n - 1) MoveSelection(picked.front() + 2);
        return true;
    }
    int to = -1;
    if (key == VK_UP) to = std::max(0, p.focus - 1);
    else if (key == VK_DOWN) to = std::min(n - 1, p.focus + 1);
    else if (key == VK_HOME) to = 0;
    else if (key == VK_END) to = n - 1;
    if (to >= 0) {
        if (shift && p.anchor >= 0) {
            p.sel.assign(n, 0);
            for (int i = std::min(p.anchor, to); i <= std::max(p.anchor, to); ++i) p.sel[i] = 1;
            p.focus = to;
        } else {
            SelectOnly(to);
        }
        EnsureVisible(RowOfQueued(to));
        InvalidateRect(p.next, nullptr, FALSE);
        return true;
    }
    if (key == VK_DELETE || key == VK_BACK) {
        RemoveSelection();
        return true;
    }
    if (key == VK_RETURN && p.focus >= 0) {
        PlayRowAt(RowOfQueued(p.focus));
        return true;
    }
    if (key == 'A' && ctrl) {
        p.sel.assign(n, 1);
        InvalidateRect(p.next, nullptr, FALSE);
        return true;
    }
    return false;
}

void EndPress() {
    p.press = p.dragging = false;
    p.dropSlot = -1;
    p.deferSelect = -1;
    InvalidateRect(p.next, nullptr, FALSE);
}

// The Up next window: the editor's mouse and keys. A press selects (Ctrl toggles, Shift extends),
// a drag moves the selected tracks, a double-click plays, the wheel scrolls.
LRESULT CALLBACK NextProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    // Input acts on the queue as it is now, not as the last timer tick saw it.
    if (!p.press && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ||
                     msg == WM_CONTEXTMENU))
        Refresh(false);
    switch (msg) {
        case WM_PAINT: DoubleBufferedPaint(h, PaintNext); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            ClampScroll();
            return 0;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            p.scroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(44) * 3 / WHEEL_DELTA;
            ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(h);
            if (int link = LinkAt(pt)) {
                FollowLink(link);
                return 0;
            }
            p.sel.resize(g.queue.size(), 0);
            int r = RowAt(pt.y);
            bool ctrl = (wp & MK_CONTROL) != 0, shift = (wp & MK_SHIFT) != 0;
            if (r < 0 || !QueueRow(p.rows[r])) {
                if (!ctrl) SelectOnly(-1);
                p.hot = r;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            int index = p.rows[r].index;
            p.deferSelect = -1;
            if (shift && p.anchor >= 0) {
                if (!ctrl) p.sel.assign(g.queue.size(), 0);
                for (int i = std::min(p.anchor, index); i <= std::max(p.anchor, index); ++i) p.sel[i] = 1;
            } else if (ctrl) {
                p.sel[index] = !p.sel[index];
                p.anchor = index;
            } else if (p.sel[index]) {
                p.deferSelect = index;  // a drag takes the whole selection; a click only this one
                p.anchor = index;
            } else {
                SelectOnly(index);
            }
            p.focus = index;
            p.press = true;
            p.pressAt = pt;
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (p.press) {
                if (!p.dragging && (std::abs(pt.x - p.pressAt.x) >= GetSystemMetrics(SM_CXDRAG) ||
                                    std::abs(pt.y - p.pressAt.y) >= GetSystemMetrics(SM_CYDRAG)))
                    p.dragging = !SelectedQueued().empty();
                if (p.dragging) {
                    ScrollNearEdge(pt.y);
                    auto picked = SelectedQueued();
                    bool allPicks = !picked.empty() && (size_t)picked.front() >= Yours();
                    p.dropSlot = SlotAt(pt.y, !allPicks);
                    InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            int hot = RowAt(pt.y), link = LinkAt(pt);
            if (hot != p.hot || link != p.hotLink) {
                p.hot = hot;
                p.hotLink = link;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!p.press) return 0;
            bool dragging = p.dragging;
            int slot = p.dropSlot, defer = p.deferSelect;
            p.press = false;  // ReleaseCapture's WM_CAPTURECHANGED must not undo the drop
            ReleaseCapture();
            EndPress();
            if (dragging && slot >= 0) MoveSelection(slot);
            else if (!dragging && defer >= 0) SelectOnly(defer);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (p.press) EndPress();
            return 0;
        case WM_MOUSELEAVE:
            p.hot = -1;
            p.hotLink = 0;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT at;
                GetCursorPos(&at);
                ScreenToClient(h, &at);
                SetCursor(LoadCursorW(nullptr, p.dragging ? IDC_SIZENS : LinkAt(at) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONDBLCLK:
            PlayRowAt(RowAt(pt.y));
            return 0;
        case WM_CONTEXTMENU: {
            int row = -1;
            if (pt.x == -1 && pt.y == -1) {
                row = RowOfQueued(p.focus);
                RECT rc;
                GetClientRect(h, &rc);
                POINT at{S(24), row >= 0 ? HeaderHeight() + Tops()[row + 1] - p.scroll : HeaderHeight()};
                ClientToScreen(h, &at);
                pt = at;
            } else {
                POINT at = pt;
                ScreenToClient(h, &at);
                row = RowAt(at.y);
            }
            ShowNextMenu(pt, row);
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_ESCAPE && p.press) {
                ReleaseCapture();
                return 0;
            }
            if ((msg == WM_KEYDOWN || wp == VK_UP || wp == VK_DOWN) && NextKey(wp)) return 0;
            break;
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
        auto next = Upcoming(1, false);
        std::wstring shown = WindowText(g.npTitle) + L'\n' + WindowText(g.npSub) + L'\n' +
                             std::to_wstring(next.empty() ? kNone : next[0].id) + L'\n' +
                             std::to_wstring(g.playing != kNone ? g.playing : g.resumeTrack) + (g.dark ? L"d" : L"l");
        if (shown != p.shown) {
            p.shown = shown;
            InvalidateRect(p.now, nullptr, FALSE);
        }
    }
    if (IsWindowVisible(p.next) && !p.press) Refresh(false);
}

// A track drag from the list (app_playlists.cpp) over the queue: shows where the tracks would land
// and says whether `screen` is over it. Off it (or with `screen` null) the mark goes.
bool QueueDragOver(const POINT* screen) {
    int slot = -1;
    if (screen && IsWindowVisible(p.next)) {
        RECT rc;
        GetWindowRect(p.next, &rc);
        if (PtInRect(&rc, *screen)) {
            POINT at = *screen;
            ScreenToClient(p.next, &at);
            ScrollNearEdge(at.y);
            slot = SlotAt(at.y, true);
        }
    }
    if (slot != p.dropSlot) {
        p.dropSlot = slot;
        InvalidateRect(p.next, nullptr, FALSE);
    }
    return slot >= 0;
}

// Drops `ids` into the queue where QueueDragOver last showed; false when the drag ended elsewhere.
bool QueueDrop(const std::vector<TrackId>& ids) {
    int slot = p.dropSlot;
    QueueDragOver(nullptr);
    if (slot < 0) return false;
    InsertIntoQueue(ids, slot);
    Refresh();
    return true;
}

bool QueueShown() { return p.next && IsWindowVisible(p.next); }

// View > Up next: the normal window's side panel, or Big mode's Up next panel. Opening it puts the
// keyboard there, so Ctrl+Shift+Q then the arrow keys and Delete edit the queue straight away.
void ToggleQueue() {
    if (g.size == ViewSize::Spotlight) {  // the spotlight always shows it
        FocusQueue();
        return;
    }
    bool show = !QueueShown();
    if (g.size == ViewSize::Big) {
        TogglePanel(kPanelUpNext);
    } else {
        if (g.size == ViewSize::Compact) SetViewSize(ViewSize::Normal);
        g.layout.upNext = show;
        ApplyMainLayout();
    }
    if (show) FocusQueue();
    else if (GetFocus() == p.next || !GetFocus()) SetFocus(g.list);
}

// Moves the keyboard focus to the queue editor, wherever it is shown.
void FocusQueue() {
    if (!IsWindowVisible(p.next)) return;
    Refresh();
    if (p.focus < 0 && !g.queue.empty()) SelectOnly(0);
    SetFocus(p.next);
}

}  // namespace app
