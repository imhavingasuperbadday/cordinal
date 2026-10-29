// Everything the playlist tab strip does itself: hit testing, the close box, click-drag reordering,
// middle-click close, the wheel and keyboard, and owner-draw painting. The tab control is painted
// from the palette in both themes because the common control has no dark mode.
#include "app.h"

namespace app {

namespace {

// Keep the native overflow control's scrolling and button-repeat behavior, but paint it
// from our palette just like the tabs around it.
LRESULT CALLBACK TabScrollSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    static int hover = -1;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        HDC dc = msg == WM_PRINTCLIENT ? (HDC)wp : BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);
        int low = 0, high = 0;
        SendMessageW(h, UDM_GETRANGE32, (WPARAM)&low, (LPARAM)&high);
        int pos = (int)SendMessageW(h, UDM_GETPOS32, 0, 0);
        for (int i = 0; i < 2; ++i) {
            RECT r{ i ? rc.right / 2 : rc.left, rc.top, i ? rc.right : rc.right / 2, rc.bottom };
            bool enabled = IsWindowEnabled(h) && (i ? pos < std::max(low, high) : pos > std::min(low, high));
            bool hot = enabled && hover == i;
            FillSolid(dc, r, Ref(P().border));
            InflateRect(&r, -1, -1);
            FillSolid(dc, r, Ref(hot ? (GetCapture() == h ? P().select : P().hover) : P().surface));
            int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            int d = std::max(2, S(3)), direction = i ? 1 : -1;
            HPEN pen = CreatePen(PS_SOLID, std::max(1, S(1)), Ref(enabled ? (hot ? P().accent : P().text) : P().faint));
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, cx - direction * d / 2, cy - d, nullptr);
            LineTo(dc, cx + direction * d / 2, cy);
            LineTo(dc, cx - direction * d / 2, cy + d);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        if (msg == WM_PAINT) EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        RECT rc{};
        GetClientRect(h, &rc);
        hover = GET_X_LPARAM(lp) < rc.right / 2 ? 0 : 1;
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
        TrackMouseEvent(&track);
        InvalidateRect(h, nullptr, FALSE);
    } else if (msg == WM_MOUSELEAVE) {
        hover = -1;
        InvalidateRect(h, nullptr, FALSE);
    }
    if (msg == WM_NCDESTROY) {
        hover = -1;
        RemoveWindowSubclass(h, TabScrollSubclass, 0);
    }
    LRESULT result = DefSubclassProc(h, msg, wp, lp);
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED || msg == WM_TIMER || msg == WM_ENABLE)
        InvalidateRect(h, nullptr, FALSE);
    return result;
}

// Tab painting and hit testing stop before the overflow buttons, including a small gap.
RECT TabContentRect(HWND tabs) {
    RECT rc{};
    GetClientRect(tabs, &rc);
    if (HWND scroll = FindWindowExW(tabs, nullptr, UPDOWN_CLASSW, nullptr)) {
        DWORD_PTR data = 0;
        if (!GetWindowSubclass(scroll, TabScrollSubclass, 0, &data)) {
            SetWindowSubclass(scroll, TabScrollSubclass, 0, 0);
            InvalidateRect(scroll, nullptr, FALSE);
        }
        if (GetWindowLongPtrW(scroll, GWL_STYLE) & WS_VISIBLE) {
            RECT buttons{};
            GetWindowRect(scroll, &buttons);
            MapWindowPoints(nullptr, tabs, (POINT*)&buttons, 2);
            rc.right = std::clamp(buttons.left - S(2), rc.left, rc.right);
        }
    }
    return rc;
}

}  // namespace

// Flat tab strip drawn from the palette in both themes (the tab control has no dark theme).
// Drag state is shared across messages (the subclass proc keeps no instance data): press records the
// source tab, a mousemove past the system drag threshold flips g_tabDragging, and the drop target
// updates live during the drag.
int g_tabDragFrom = -1, g_tabDragTo = -1, g_tabClosePressed = -1;
bool g_tabDragging = false;
POINT g_tabDragStart{};

int TabAtPoint(HWND tabs, POINT point) {
    RECT content = TabContentRect(tabs);
    if (!PtInRect(&content, point)) return -1;
    TCHITTESTINFO hit{};
    hit.pt = point;
    return TabCtrl_HitTest(tabs, &hit);
}

// The close box for a tab, or an empty rect. Only tabs after the Library (index 0) and outside the
// Inbox get one, confined to the right 22px of the tab.
RECT TabCloseRect(HWND tabs, int index) {
    RECT rect{};
    if (index > 0 && index < (int)g.playlists.size() && g.playlists[index].kind != PlaylistKind::Inbox &&
        TabCtrl_GetItemRect(tabs, index, &rect)) {
        rect.left = std::max(rect.left, rect.right - S(22));
        rect.right -= S(3);
        InflateRect(&rect, 0, -S(3));
        RECT content = TabContentRect(tabs);
        if (rect.left < content.left || rect.right > content.right) SetRectEmpty(&rect);
    }
    return rect;
}

LRESULT CALLBACK TabsSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    static int s_hover = -1;
    POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (msg == WM_LBUTTONDOWN) {
        int hit = TabAtPoint(h, point);
        auto close = TabCloseRect(h, hit);
        if (hit >= 0 && PtInRect(&close, point)) {
            g_tabClosePressed = hit;
            SetCapture(h);
            InvalidateRect(h, &close, FALSE);
            return 0;
        }
        if (hit >= 0) SendMessageW(g.hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
        g_tabDragFrom = hit > 0 ? hit : -1;
        g_tabDragTo = hit;
        g_tabDragStart = point;
        LRESULT result = DefSubclassProc(h, msg, wp, lp);
        if (hit >= 0) { SetFocus(h); SetCapture(h); }
        return result;
    }
    if (msg == WM_LBUTTONUP) {
        int from = g_tabDragFrom, to = g_tabDragTo, close = g_tabClosePressed;
        bool dragging = g_tabDragging;
        g_tabDragFrom = g_tabDragTo = g_tabClosePressed = -1;
        g_tabDragging = false;
        if (GetCapture() == h) ReleaseCapture();
        if (close >= 0) {
            auto rect = TabCloseRect(h, close);
            if (PtInRect(&rect, point)) RemovePlaylist(close);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (dragging) { ReorderPlaylist(from, to); return 0; }
    }
    if (msg == WM_CAPTURECHANGED || msg == WM_CANCELMODE) {
        g_tabDragFrom = g_tabDragTo = g_tabClosePressed = -1;
        g_tabDragging = false;
        if (msg == WM_CANCELMODE && GetCapture() == h) ReleaseCapture();
        InvalidateRect(h, nullptr, FALSE);
    }
    if (msg == WM_MBUTTONUP) { RemovePlaylist(TabAtPoint(h, point)); return 0; }
    if (msg == WM_LBUTTONDBLCLK) {
        if (GetCapture() == h) ReleaseCapture();
        int hit = TabAtPoint(h, point);
        if (hit >= 0) BeginRename(hit);
        else { NewPlaylist(L"New Playlist"); BeginRename(g.active); }
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        // High-resolution wheels deliver deltas smaller than WHEEL_DELTA: accumulate them and step
        // one playlist per full notch.
        static int wheel = 0;
        wheel += GET_WHEEL_DELTA_WPARAM(wp);
        while (std::abs(wheel) >= WHEEL_DELTA) {
            int direction = wheel > 0 ? -1 : 1;
            ActivatePlaylist((g.active + (int)g.playlists.size() + direction) % (int)g.playlists.size());
            wheel += direction * WHEEL_DELTA;
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && (SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
        SendMessageW(g.hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_F2)) { BeginRename(g.active); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && GetCapture() == h) { ReleaseCapture(); return 0; }
    if (msg == WM_KEYDOWN && (wp == VK_HOME || wp == VK_END)) {
        ActivatePlaylist(wp == VK_HOME ? 0 : (int)g.playlists.size() - 1);
        TabCtrl_SetCurFocus(h, g.active);
        return 0;
    }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(h, nullptr, FALSE);
    if (msg == WM_PAINT || msg == WM_PRINTCLIENT || msg == WM_ERASEBKGND || msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE) {
        if (msg == WM_MOUSEMOVE) {
            if (GetCapture() == h && g_tabDragFrom >= 0 && (wp & MK_LBUTTON)) {
                if (std::abs(point.x - g_tabDragStart.x) >= GetSystemMetricsForDpi(SM_CXDRAG, g.dpi) ||
                    std::abs(point.y - g_tabDragStart.y) >= GetSystemMetricsForDpi(SM_CYDRAG, g.dpi)) g_tabDragging = true;
                int hit = TabAtPoint(h, point);
                if (g_tabDragging && hit >= 0 && hit != g_tabDragTo) {
                    g_tabDragTo = hit;
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            int index = TabAtPoint(h, point);
            if (index != s_hover) {
                s_hover = index;
                InvalidateRect(h, nullptr, FALSE);
            }
            return DefSubclassProc(h, msg, wp, lp);
        }
        if (msg == WM_MOUSELEAVE) {
            if (s_hover != -1) {
                s_hover = -1;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        if (msg == WM_ERASEBKGND) return 1;
        // Owner-draw paint. The same path serves WM_PAINT and WM_PRINTCLIENT (used for the tooltip
        // snapshot); WM_ERASEBKGND is swallowed above so only this fill shows.
        PAINTSTRUCT ps;
        HDC dc = msg == WM_PRINTCLIENT ? (HDC)wp : BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g.surfaceBrush);
        RECT content = TabContentRect(h);
        // A hairline under the tabs (not the overflow arrows) separates the playlists from the views
        // and lists below; the open tab's underline sits on it.
        if (!FoobarMode()) FillSolid(dc, {content.left, rc.bottom - S(1), content.right, rc.bottom}, Ref(P().border));
        int saved = SaveDC(dc);
        IntersectClipRect(dc, content.left, content.top, content.right, content.bottom);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, g.font);
        int sel = RadioActive() ? -1 : TabCtrl_GetCurSel(h);
        for (int i = 0; i < TabCtrl_GetItemCount(h); ++i) {
            RECT r;
            if (!TabCtrl_GetItemRect(h, i, &r) || r.right <= rc.left || r.left >= rc.right) continue;
            // Flat tabs on the strip: no boxes. The selected tab is marked by a short accent
            // underline; hover just brightens the label.
            bool playing = i == g.playingPl && g.playing != kNone;
            bool foobar = FoobarMode();  // foobar2000's tabs: framed boxes, the open one raised
            if (foobar) {
                RECT b = r;
                if (i != sel) b.top += S(2);
                FillSolid(dc, b, i == sel ? Ref(P().border) : i == s_hover ? Ref(P().hover) : SurfaceColor());
                FillSolid(dc, {b.left, b.top, b.right, b.top + 1}, Ref(kFoobarFrame));
                FillSolid(dc, {b.left, b.top, b.left + 1, b.bottom}, Ref(kFoobarFrame));
                FillSolid(dc, {b.right - 1, b.top, b.right, b.bottom}, Ref(kFoobarFrame));
            } else if (i == sel) {
                FillSolid(dc, {r.left + S(10), r.bottom - S(2), r.right - S(10), r.bottom}, Ref(P().accent));
            }
            wchar_t text[256] = {};
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = text;
            item.cchTextMax = (int)std::size(text);
            TabCtrl_GetItem(h, i, &item);
            bool closable = i > 0 && g.playlists[i].kind != PlaylistKind::Inbox;
            RECT tr = r;
            tr.left += S(6);
            tr.right -= S(closable ? 24 : 6);
            // "Name (12)": the count follows the name in the faint color, without the brackets.
            std::wstring name = text, count;
            if (size_t open = name.rfind(L" ("); open != std::wstring::npos && name.size() > open + 3 && name.back() == L')') {
                count = name.substr(open + 2, name.size() - open - 3);
                name.resize(open);
            }
            SIZE nameExt{}, countExt{};
            GetTextExtentPoint32W(dc, name.c_str(), (int)name.size(), &nameExt);
            if (!count.empty()) GetTextExtentPoint32W(dc, count.c_str(), (int)count.size(), &countExt);
            int gap = count.empty() ? 0 : S(6), total = nameExt.cx + gap + countExt.cx;
            int left = std::max<int>(tr.left, (tr.left + tr.right - total) / 2);
            // Playing playlist: the name in the playing color.
            SetTextColor(dc, foobar ? Ref(kFoobarChromeText)
                             : playing ? Ref(P().playing) : i == sel || i == s_hover ? Ref(P().text) : Ref(P().sub));
            RECT nr{left, tr.top, std::min<int>(tr.right, left + nameExt.cx), tr.bottom};
            DrawTextW(dc, name.c_str(), (int)name.size(), &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (!count.empty() && nr.right + gap < tr.right) {
                RECT cr{nr.right + gap, tr.top, tr.right, tr.bottom};
                SetTextColor(dc, Ref(P().faint));
                DrawTextW(dc, count.c_str(), (int)count.size(), &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            // The close box appears where it can be used: on the hovered and the selected tab.
            RECT close = TabCloseRect(h, i);
            if (closable && (i == s_hover || i == sel || i == g_tabClosePressed)) {
                SetTextColor(dc, i == s_hover ? TextColor() : Ref(P().faint));
                if (i == g_tabClosePressed) FillSolid(dc, close, Ref(P().select));
                DrawTextW(dc, L"\x00D7", 1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            if (g.showFocusBoxes && i == sel && GetFocus() == h &&
                !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
                InflateRect(&tr, -S(2), -S(4));
                DrawFocusRect(dc, &tr);
            }
            if (g_tabDragging && i == g_tabDragTo) {
                int x = g_tabDragTo < g_tabDragFrom ? r.left : r.right - S(2);
                FillSolid(dc, {x, r.top, x + S(2), r.bottom}, Ref(P().accent));
            }
        }
        SelectObject(dc, old);
        RestoreDC(dc, saved);
        if (msg != WM_PRINTCLIENT) EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, TabsSubclass, 0);
    return DefSubclassProc(h, msg, wp, lp);
}

}  // namespace app
