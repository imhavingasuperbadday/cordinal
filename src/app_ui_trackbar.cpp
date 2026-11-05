// The player's seek and volume bars: their custom drawing, the upright seek column and the
// channel drag that replaces the stock bar's paging.
#include "app_ui_internal.h"

namespace app {

namespace {
bool FlatPlayerBar(HWND bar) {
    return (bar == g.seek || bar == g.volume) && !FoobarMode() && !ApolloMode();
}

// Native trackbars keep their channel near the top even when the waveform makes the control taller.
// Use the same centred track for the fill, handle and mouse position on both platforms.
RECT PlayerBarTrack(HWND bar) {
    RECT r{};
    GetClientRect(bar, &r);
    int inset = std::min<int>(S(ui::kSliderInset), std::max(0L, (r.right - r.left - 1) / 2));
    int h = std::min(S(ui::kSliderTrackHeight), (int)(r.bottom - r.top)), cy = (r.top + r.bottom) / 2;
    return {r.left + inset, cy - h / 2, r.right - inset, cy - h / 2 + h};
}

RECT PlayerBarThumb(HWND bar) {
    RECT track = PlayerBarTrack(bar), client{};
    GetClientRect(bar, &client);
    int lo = (int)SendMessageW(bar, TBM_GETRANGEMIN, 0, 0), hi = (int)SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);
    int pos = (int)SendMessageW(bar, TBM_GETPOS, 0, 0);
    double frac = hi > lo ? std::clamp((double)(pos - lo) / (hi - lo), 0.0, 1.0) : 0;
    int w = std::max(2, S(ui::kSliderThumbWidth)), h = std::min(S(ui::kSliderThumbHeight), (int)client.bottom);
    if (g.themeMode == ThemeMode::Deltarune) {
        RECT native{};
        SendMessageW(bar, TBM_GETTHUMBRECT, 0, (LPARAM)&native);
        w = native.right - native.left;
        h = native.bottom - native.top;
    }
    int cx = track.left + (int)std::lround(frac * (track.right - track.left)), cy = client.bottom / 2;
    return {cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h};
}

// The live waveform covers every pixel of its bar, so one compatible surface and its DC can be
// kept across frames instead of creating and deleting both thirty times a second.
struct BarSurface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    int width = 0, height = 0;

    bool Ready(HDC target, int w, int h) {
        if (dc && width == w && height == h) return true;
        Reset();
        HDC created = CreateCompatibleDC(target);
        if (!created) return false;
        HBITMAP bmp = CreateCompatibleBitmap(target, std::max(1, w), std::max(1, h));
        if (!bmp) {
            DeleteDC(created);
            return false;
        }
        previous = SelectObject(created, bmp);
        dc = created;
        bitmap = bmp;
        width = w;
        height = h;
        return true;
    }

    void Reset() {
        if (dc && previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        dc = nullptr;
        bitmap = nullptr;
        previous = nullptr;
        width = height = 0;
    }
};

BarSurface liveBarSurface;
}  // namespace

// The seek bar's waveform with its section over it; false while there is none to
// draw (the flat channel stands in). `rc` is the whole bar as drawn.
bool DrawSeekWaveform(HDC dc, const RECT& rc, const RECT& channel, double frac) {
    if (FountainShown()) PaintFountain(dc, rc, channel.left, channel.right, frac);
    else if (!DrawWaveform(dc, rc, channel, frac)) return false;
    DrawRegion(dc, rc, channel);  // the section, over the peaks (app_region.cpp)
    return true;
}

// The seek bar of a player docked at a side runs down its column. The common control has no
// vertical bar that keeps the horizontal one's messages, so the bar stays horizontal underneath and
// is painted here through a transposed world transform: the horizontal drawing code then lays
// time out top to bottom. Its channel is the whole height, inset a little at each end.
RECT VerticalChannel(const RECT& client) { return {client.top + S(4), client.left, client.bottom - S(4), client.right}; }

void PaintPlayerBar(HWND bar, HDC printDC = nullptr) {
    PAINTSTRUCT ps{};
    HDC dc = printDC ? printDC : BeginPaint(bar, &ps);
    RECT client;
    GetClientRect(bar, &client);
    int lo = (int)SendMessageW(bar, TBM_GETRANGEMIN, 0, 0), hi = (int)SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);
    int pos = (int)SendMessageW(bar, TBM_GETPOS, 0, 0);
    double frac = hi > lo ? (double)(pos - lo) / (hi - lo) : 0;
    // Drawn off screen and copied once: the Dark Fountain, the live waveform and a glowing whole
    // song repaint 30 times a second. Those paint every pixel of their bar, and so does every
    // upright bar (its fallback fills the whole column), so that surface is kept instead of created
    // each frame; the plain horizontal bar draws only its channel and thumb.
    const bool reusable = !printDC && bar == g.seek && (FountainShown() || LiveWaveformShown() || SeekVertical()) &&
                          liveBarSurface.Ready(dc, (int)client.right, (int)client.bottom);
    HDC mem = reusable ? liveBarSurface.dc : CreateCompatibleDC(dc);
    HBITMAP bmp = reusable ? nullptr : CreateCompatibleBitmap(dc, std::max(1, (int)client.right), std::max(1, (int)client.bottom));
    HGDIOBJ oldBmp = reusable ? nullptr : SelectObject(mem, bmp);
    if (bar == g.volume || (!SeekVertical() && !FountainShown())) {
        // Paint the whole control so the native trackbar cannot leave chrome behind: its own
        // background fill kept the palette brush of the theme it started in (black after a switch
        // from dark to light).
        NMCUSTOMDRAW draw{};
        draw.hdr.hwndFrom = bar;
        draw.hdc = mem;
        draw.dwDrawStage = CDDS_ITEMPREPAINT;
        draw.dwItemSpec = TBCD_CHANNEL;
        SendMessageW(bar, TBM_GETCHANNELRECT, 0, (LPARAM)&draw.rc);
        DrawTrackbar(&draw);
        draw.dwItemSpec = TBCD_THUMB;
        SendMessageW(bar, TBM_GETTHUMBRECT, 0, (LPARAM)&draw.rc);
        if (FlatPlayerBar(bar)) draw.rc = PlayerBarThumb(bar);
        POINT cursor;
        GetCursorPos(&cursor);
        ScreenToClient(bar, &cursor);
        draw.uItemState = GetCapture() == bar ? CDIS_SELECTED : PtInRect(&draw.rc, cursor) ? CDIS_HOT : 0;
        DrawTrackbar(&draw);
        if (g.showFocusBoxes && GetFocus() == bar && !(SendMessageW(bar, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
            DrawFocusRect(mem, &client);
    } else if (SeekVertical()) {
        RECT ch = VerticalChannel(client);
        bool fountain = FountainShown();
        if (fountain) PaintFountain(mem, client, ch.left, ch.right, frac);
        int oldMode = SetGraphicsMode(mem, GM_ADVANCED);
        XFORM transpose{0, 1, 1, 0, 0, 0};  // x' = y, y' = x
        SetWorldTransform(mem, &transpose);
        RECT rc{client.top, client.left, client.bottom, client.right};
        if (fountain) {
            DrawRegion(mem, rc, ch);
        } else if (!DrawSeekWaveform(mem, rc, ch, frac)) {
            FillSolid(mem, rc, PlayerColor());
            // The same regular bar turned upright for a side dock; its endpoints match BarFracAt.
            int cy = rc.bottom / 2, h = S(ui::kSliderTrackHeight);
            RECT track{ch.left, cy - h / 2, ch.right, cy - h / 2 + h};
            int x = track.left + (int)std::lround(frac * (track.right - track.left));
            bool enabled = IsWindowEnabled(bar) != 0;
            FillSolid(mem, track, Ref(P().border));
            FillSolid(mem, {track.left, track.top, x, track.bottom}, enabled ? Ref(P().accent) : Ref(P().faint));
            int w = std::max(2, S(ui::kSliderThumbWidth)), th = S(ui::kSliderThumbHeight);
            FillSolid(mem, {x - w / 2, cy - th / 2, x - w / 2 + w, cy - th / 2 + th},
                      !enabled ? Ref(P().faint) : GetCapture() == bar ? Ref(P().accent) : TextColor());
        }
        ModifyWorldTransform(mem, nullptr, MWT_IDENTITY);
        SetGraphicsMode(mem, oldMode);
    } else {
        RECT channel{}, thumb{};
        SendMessageW(bar, TBM_GETCHANNELRECT, 0, (LPARAM)&channel);
        SendMessageW(bar, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
        int halfThumb = (thumb.right - thumb.left) / 2;
        channel.left += halfThumb;
        channel.right -= halfThumb;
        if (FlatPlayerBar(bar)) channel = PlayerBarTrack(bar);
        DrawSeekWaveform(mem, client, channel, frac);
    }
    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    if (!reusable) {
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
    if (!printDC) EndPaint(bar, &ps);
}

// Where along `h` the point falls, 0..1: the thumb-centre mapping the stock bar uses, or for the
// vertical seek bar its channel top to bottom. DragBarTo seeks by it and the section menu marks
// by it (RegionTimeAt), so both land on the same spot.
double BarFracAt(HWND h, POINT pt) {
    if (FoobarMode() && (h == g.seek || h == g.volume)) {
        RECT track = FoobarSliderTrack(h);
        return std::clamp((double)(pt.x - track.left) / std::max(1L, track.right - track.left), 0.0, 1.0);
    }
    if (h == g.seek && SeekVertical()) {
        RECT client;
        GetClientRect(h, &client);
        RECT ch = VerticalChannel(client);
        return std::clamp((double)(pt.y - ch.left) / std::max(1, (int)(ch.right - ch.left)), 0.0, 1.0);
    }
    if (FlatPlayerBar(h)) {
        RECT track = PlayerBarTrack(h);
        return std::clamp((double)(pt.x - track.left) / std::max(1L, track.right - track.left), 0.0, 1.0);
    }
    RECT thumb{}, channel{};
    SendMessageW(h, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
    SendMessageW(h, TBM_GETCHANNELRECT, 0, (LPARAM)&channel);
    int thumbW = thumb.right - thumb.left;
    int width = std::max(1, (int)(channel.right - channel.left) - thumbW);
    return std::clamp(((double)pt.x - channel.left - thumbW / 2) / width, 0.0, 1.0);
}

// Flat trackbar for both themes: square channel with an accent fill, classic foobar feel.
// Position/range logic is untouched so seek/volume length behavior is identical.
LRESULT DrawTrackbar(NMCUSTOMDRAW* cd) {
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    HDC dc = cd->hdc;
    HWND bar = cd->hdr.hwndFrom;
    if (bar == g.seek && LiveWaveformShown() && !FountainShown()) {
        if (cd->dwItemSpec == TBCD_CHANNEL) {
            RECT all{}; GetClientRect(bar, &all);
            int lo = (int)SendMessageW(bar, TBM_GETRANGEMIN, 0, 0), hi = (int)SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);
            int pos = (int)SendMessageW(bar, TBM_GETPOS, 0, 0);
            RECT channel = FlatPlayerBar(bar) ? PlayerBarTrack(bar) : cd->rc;
            DrawWaveform(dc, all, channel, hi > lo ? (double)(pos - lo) / (hi - lo) : 0);
        }
        return CDRF_SKIPDEFAULT;
    }
    if (ApolloMode() && (bar == g.seek || bar == g.volume)) return ApolloDrawTrackbar(cd);
    if (FoobarMode() && (bar == g.seek || bar == g.volume)) {
        // foobar2000's sliders: a sunken groove and a small blue thumb (none on a stopped seek bar).
        if (cd->dwItemSpec == TBCD_CHANNEL) {
            RECT all;
            GetClientRect(bar, &all);
            FillSolid(dc, all, SurfaceColor());
            RECT groove = FoobarSliderTrack(bar);
            FillSolid(dc, groove, WindowBg());
            FillSolid(dc, {groove.left, groove.top, groove.right, groove.top + 1}, Ref(P().border));
            FillSolid(dc, {groove.left, groove.top, groove.left + 1, groove.bottom}, Ref(P().border));
            return CDRF_SKIPDEFAULT;
        }
        if (cd->dwItemSpec == TBCD_THUMB) {
            if (!IsWindowEnabled(bar)) return CDRF_SKIPDEFAULT;
            // The native thumb rectangle can lag TBM_SETPOS(FALSE) because this control paints
            // itself. Read the position directly, using the same geometry as clicks and drags.
            RECT track = FoobarSliderTrack(bar);
            int lo = (int)SendMessageW(bar, TBM_GETRANGEMIN, 0, 0), hi = (int)SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);
            int pos = (int)SendMessageW(bar, TBM_GETPOS, 0, 0);
            double frac = hi > lo ? std::clamp((double)(pos - lo) / (hi - lo), 0.0, 1.0) : 0;
            int w = S(ui::kFoobarThumb), h = S(ui::kFoobarThumbHeight);
            int cx = track.left + (int)std::lround(frac * (track.right - track.left)), cy = (track.top + track.bottom) / 2;
            FillSolid(dc, {cx - w / 2, cy - h / 2, cx - w / 2 + w, cy - h / 2 + h}, Ref(P().accent));
            return CDRF_SKIPDEFAULT;
        }
        return cd->dwItemSpec == TBCD_TICS ? CDRF_SKIPDEFAULT : CDRF_DODEFAULT;
    }
    if (cd->dwItemSpec == TBCD_CHANNEL) {
        RECT r = FlatPlayerBar(bar) ? PlayerBarTrack(bar) : cd->rc;
        bool vert = (GetWindowLongW(bar, GWL_STYLE) & TBS_VERT) != 0;
        int lo = (int)SendMessageW(bar, TBM_GETRANGEMIN, 0, 0);
        int hi = (int)SendMessageW(bar, TBM_GETRANGEMAX, 0, 0);
        int pos = (int)SendMessageW(bar, TBM_GETPOS, 0, 0);
        double frac = hi > lo ? (double)(pos - lo) / (hi - lo) : 0;
        RECT all;
        GetClientRect(bar, &all);
        if (bar == g.seek && DrawSeekWaveform(dc, all, r, frac)) return CDRF_SKIPDEFAULT;
        // Paint the bar's own background: the channel item is the only thing repainted over an
        // invalidated bar, so a track whose waveform has gone (or isn't known yet) would
        // otherwise leave the previous track's columns standing outside the slim channel.
        FillSolid(dc, all, ControlBg(bar));
        COLORREF fill = Ref(P().accent);
        if (!IsWindowEnabled(bar)) fill = Ref(P().faint);
        // Slim flat channel, centered.
        RECT t = r;
        if (vert) {
            int w = std::min<long>(S(4), t.right - t.left);
            int cx = (t.left + t.right) / 2;
            t.left = cx - w / 2;
            t.right = t.left + w;
        } else {
            int h = std::min<long>(S(4), t.bottom - t.top);
            int cy = (t.top + t.bottom) / 2;
            t.top = cy - h / 2;
            t.bottom = t.top + h;
        }
        FillSolid(dc, t, Ref(P().border));  // the empty track, no outline
        // Filled portion: left->thumb (horizontal) or bottom->thumb (vertical).
        // EQ sliders fill from zero dB. The preamp has extra attenuation for headphone profiles;
        // native vertical trackbars put position zero at the top.
        RECT f = t;
        bool bipolar = vert && lo == 0 && (hi == 48 || hi == 168);
        if (vert) {
            if (bipolar) {
                double zero = hi == 168 ? 48.0 / 168 : 0.5;
                int mid = t.top + (int)((t.bottom - t.top) * zero);
                int y = t.top + (int)((t.bottom - t.top) * frac);
                if (y < mid) f = {t.left, y, t.right, mid};
                else f = {t.left, mid, t.right, y};
            } else {
                int y = t.top + (int)((t.bottom - t.top) * (1.0 - frac));
                f.top = y;
            }
        } else {
            f.right = f.left + (int)((f.right - f.left) * frac);
        }
        if (f.right > f.left && f.bottom > f.top) FillSolid(dc, f, fill);
        if (bar == g.seek && !vert && IsWindowEnabled(bar)) {
            // A long track's channel is cut at round times (ui::SeekNotchSeconds), so "the start
            // of the third hour" is somewhere the eye and the mouse can find.
            double dur = CurrentDuration();
            int step = ui::SeekNotchSeconds(dur), width = t.right - t.left;
            if (step > 0 && width * step / dur >= S(16))
                for (int s = step; s < dur; s += step) {
                    int x = t.left + (int)(width * (s / dur));
                    FillSolid(dc, {x, t.top, x + std::max(1, S(1)), t.bottom}, ControlBg(bar));
                }
        }
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_THUMB) {
        // Over a waveform the split between its two shades is the playhead. Regular seek bars keep
        // their handle in the large players too; those players' volume bars still use just the fill.
        if (bar == g.seek && (WaveformShown() || FountainShown())) return CDRF_SKIPDEFAULT;
        if (g.size >= ViewSize::Spotlight && ((bar == g.seek && g.waveform) || bar == g.volume)) return CDRF_SKIPDEFAULT;
        bool enabled = IsWindowEnabled(bar);
        bool hot = (cd->uItemState & CDIS_HOT) != 0;
        bool pressed = (cd->uItemState & CDIS_SELECTED) != 0;
        // A slim upright bar centred in the stock thumb's rect: the text color at rest, the accent
        // while hovered or held. DELTARUNE keeps its square, yellow-edged thumb.
        RECT r = cd->rc;
        if (FlatPlayerBar(bar)) r = PlayerBarThumb(bar);
        if (g.themeMode == ThemeMode::Deltarune) {
            COLORREF face = !enabled ? Ref(P().surface) : pressed ? Ref(P().accent) : hot ? Ref(P().hover) : Ref(P().surface);
            COLORREF edge = !enabled ? Ref(P().faint) : bar == g.volume ? Ref(P().accent) : Ref(P().border);
            SetDCBrushColor(dc, face);
            SetDCPenColor(dc, edge);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
            Rectangle(dc, r.left, r.top, r.right + 1, r.bottom + 1);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            return CDRF_SKIPDEFAULT;
        }
        int w = std::max(2, S(ui::kSliderThumbWidth)), hgt = std::min<int>(S(ui::kSliderThumbHeight), r.bottom - r.top);
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        FillSolid(dc, {cx - w / 2, cy - hgt / 2, cx - w / 2 + w, cy - hgt / 2 + hgt},
                  !enabled ? Ref(P().faint) : pressed || hot ? Ref(P().accent) : TextColor());
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_TICS) {
        // No ticks; the flat channel reads cleaner.
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_DODEFAULT;
}

static HWND s_barDrag = nullptr;  // the bar whose channel (not thumb) the mouse is down on

// Moves `h` to the position under `pt` (BarFracAt) and tells the parent, which previews the time
// while a drag is in progress.
void DragBarTo(HWND h, POINT pt) {
    int lo = (int)SendMessageW(h, TBM_GETRANGEMIN, 0, 0), hi = (int)SendMessageW(h, TBM_GETRANGEMAX, 0, 0);
    int pos = lo + (int)std::lround(BarFracAt(h, pt) * (hi - lo));
    SendMessageW(h, TBM_SETPOS, TRUE, pos);
    // The stock bar only repaints the strip the thumb left; over a waveform it isn't drawn at
    // all and the split is the playhead, so repaint the whole bar (without the background
    // erase, which blinks the waveform away).
    InvalidateRect(h, nullptr, FALSE);
    SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, pos), (LPARAM)h);
}

LRESULT CALLBACK TrackbarSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_LBUTTONDOWN) {
        // The stock bar pages on a channel click, so the click never landed where it was aimed,
        // and its own capture then moved the bar a second time: two seeks, the second one wrong,
        // and a pressed state repainting over the waveform. A channel press is therefore driven
        // here from press to release. Custom player handles use the same mapping, since their
        // centred drawing no longer follows the native thumb's hit rectangle.
        // The vertical seek bar has no thumb of its own: every press is a channel press.
        RECT thumb{};
        bool vertical = h == g.seek && SeekVertical();
        if (!vertical) SendMessageW(h, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (vertical || FlatPlayerBar(h) || (FoobarMode() && (h == g.seek || h == g.volume)) || !PtInRect(&thumb, pt)) {
            s_barDrag = h;
            SetFocus(h);
            SetCapture(h);
            DragBarTo(h, pt);
            return 0;
        }
    } else if (msg == WM_MOUSEMOVE && s_barDrag == h) {
        DragBarTo(h, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    } else if (msg == WM_TIMER && h == g.seek && (FountainStep(wp) || WaveformStep(wp))) {
        InvalidateRect(h, nullptr, FALSE);  // the next live waveform or fountain frame
        return 0;
    } else if ((msg == WM_PAINT || msg == WM_ERASEBKGND || msg == WM_PRINTCLIENT) && (h == g.seek || h == g.volume)) {
        if (msg != WM_ERASEBKGND) PaintPlayerBar(h, msg == WM_PRINTCLIENT ? (HDC)wp : nullptr);
        return msg == WM_ERASEBKGND;  // the paint fills the whole bar
    } else if ((msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) && s_barDrag == h) {
        HWND parent = GetParent(h);
        if (msg == WM_LBUTTONUP) DragBarTo(h, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});  // the release decides the position
        int pos = (int)SendMessageW(h, TBM_GETPOS, 0, 0);
        s_barDrag = nullptr;  // before ReleaseCapture: its WM_CAPTURECHANGED must not fire again
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        // MainScroll seeks on TB_ENDTRACK alone, so a lost capture still has to end the drag, or
        // g.seeking would stay set and the bar would stop following playback.
        SendMessageW(parent, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, pos), (LPARAM)h);
        return 0;
    } else if (msg == WM_NCDESTROY) {
        if (s_barDrag == h) s_barDrag = nullptr;
        if (h == g.seek) liveBarSurface.Reset();
        RemoveWindowSubclass(h, TrackbarSubclass, 0);
    }
    LRESULT result = DefSubclassProc(h, msg, wp, lp);
    // Native invalidation follows its old thumb rectangle; repaint the centred drawing as a whole.
    if (FlatPlayerBar(h) && (msg == TBM_SETPOS || msg == WM_KEYDOWN || msg == WM_MOUSEWHEEL ||
                             msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE))
        InvalidateRect(h, nullptr, FALSE);
    return result;
}

}  // namespace app
