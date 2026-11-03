// The stock controls cordinal subclasses itself: search box keys, the flat list header and the
// owner-drawn toolbar buttons with their hot state.
#include "app_ui_internal.h"

namespace app {

// Search box key handling: Ctrl+A selects all, Esc clears, Enter applies the query
// (and plays the first match unless Ctrl), Down drops focus into the list. The
// WM_CHAR clauses swallow the control chars produced by keys already handled in
// WM_KEYDOWN so they neither beep nor insert text.
LRESULT CALLBACK SearchSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (RadioActive() && wp == VK_RETURN) { RadioSearch(); return 0; }
        if (RadioActive() && wp == VK_DOWN) { RadioFocus(); return 0; }
        if (RadioActive() && wp == VK_ESCAPE) { SetWindowTextW(h, L""); RadioFocus(); return 0; }
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            SetWindowTextW(h, L"");
            FlushSearch();
            SetFocus(CanvasActive() ? g.libView : g.list);
            return 0;
        }
        if (wp == VK_RETURN) {
            FlushSearch();
            if (GetKeyState(VK_CONTROL) >= 0) PlayRow(0);
            return 0;
        }
        if (wp == VK_DOWN) {
            FlushSearch();
            if (g.view.empty()) return 0;
            if (CanvasActive()) {
                LibViewFocusFirst();
            } else {
                SetFocus(g.list);
                ClearSelection();
                int row = RowOfViewPos(0);
                ListView_SetItemState(g.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(g.list, 0, FALSE);
                ListView_EnsureVisible(g.list, row, FALSE);
            }
            return 0;
        }
    } else if (msg == WM_CHAR && (wp == L'\r' || wp == 27 || wp == 1)) {
        return 0;
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, SearchSubclass, 0);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Flat list header in both themes: the list's own background so header and rows read as one
// surface, quiet labels, short faint dividers (enough to find a column edge) and the sort arrow.
LRESULT CALLBACK HeaderSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (h == g.header && (msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK)) {
        // Let the header finish resizing/reordering before capturing the resulting columns.
        LRESULT result = DefSubclassProc(h, msg, wp, lp);
        SaveColumns();
        if (FoobarMode()) RefreshViewKeepingSelection();  // the cover column's width sets the groups' heights
        return result;
    }
    if (msg == WM_PAINT || msg == WM_ERASEBKGND) {
        if (msg == WM_ERASEBKGND) return 1;
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (ApolloMode()) {
            ApolloPaintHeader(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        RECT rc;
        GetClientRect(h, &rc);
        bool foobar = FoobarMode();  // foobar2000's header: near black, light labels, full dividers
        FillSolid(dc, rc, foobar ? Ref(kFoobarHeaderBg) : WindowBg());
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, g.font);
        COLORREF divider = foobar ? Ref(P().border) : RGB((GetRValue(WindowBg()) + GetRValue(Ref(P().border))) / 2,
                               (GetGValue(WindowBg()) + GetGValue(Ref(P().border))) / 2,
                               (GetBValue(WindowBg()) + GetBValue(Ref(P().border))) / 2);
        int n = Header_GetItemCount(h);
        for (int i = 0; i < n; ++i) {
            RECT r;
            if (!Header_GetItemRect(h, i, &r)) continue;
            HDITEMW item{};
            wchar_t text[128] = {};
            item.mask = HDI_TEXT | HDI_FORMAT;
            item.pszText = text;
            item.cchTextMax = (int)std::size(text);
            Header_GetItem(h, i, &item);
            // Column divider.
            FillSolid(dc, {r.right - 1, r.top + (foobar ? 0 : S(6)), r.right, r.bottom - (foobar ? 0 : S(6))}, divider);
            UINT fmt = item.fmt & (HDF_LEFT | HDF_CENTER | HDF_RIGHT);
            UINT dt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
            dt |= fmt == HDF_CENTER ? DT_CENTER : fmt == HDF_RIGHT ? DT_RIGHT : DT_LEFT;
            RECT tr = r;
            tr.left += S(5);
            tr.right -= S(5);
            bool sorted = (item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) != 0;
            if (sorted) tr.right -= S(14);
            SetTextColor(dc, foobar ? Ref(kFoobarChromeText) : sorted ? TextColor() : SubColor());
            DrawTextW(dc, text, -1, &tr, dt);
            if (sorted) {
                bool up = (item.fmt & HDF_SORTUP) != 0;
                int cx = r.right - S(12), cy = (r.top + r.bottom) / 2;
                POINT tri[3];
                if (up) {
                    tri[0] = {cx - S(4), cy + S(2)};
                    tri[1] = {cx + S(4), cy + S(2)};
                    tri[2] = {cx, cy - S(3)};
                } else {
                    tri[0] = {cx - S(4), cy - S(2)};
                    tri[1] = {cx + S(4), cy - S(2)};
                    tri[2] = {cx, cy + S(3)};
                }
                HBRUSH ab = CreateSolidBrush(Ref(P().text));
                HPEN ap = CreatePen(PS_SOLID, 1, Ref(P().text));
                HGDIOBJ ob = SelectObject(dc, ab), op = SelectObject(dc, ap);
                Polygon(dc, tri, 3);
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(ab);
                DeleteObject(ap);
            }
        }
        FillSolid(dc, {rc.left, rc.bottom - 1, rc.right, rc.bottom}, Ref(P().border));
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, HeaderSubclass, 0);
    return DefSubclassProc(h, msg, wp, lp);
}

// Flat toolbar buttons: track hover so the owner draw can show a subtle hot state.
HWND g_hotButton = nullptr;
LRESULT CALLBACK ButtonSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE && g_hotButton != h) {
        HWND old = g_hotButton;
        g_hotButton = h;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        if (old) InvalidateRect(old, nullptr, FALSE);
        InvalidateRect(h, nullptr, FALSE);
    } else if (msg == WM_MOUSELEAVE && g_hotButton == h) {
        g_hotButton = nullptr;
        InvalidateRect(h, nullptr, FALSE);
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, ButtonSubclass, 0);
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        InvalidateRect(h, nullptr, FALSE);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Owner-drawn toolbar button. Glyphs are Segoe MDL2 Assets code points; the icon
// font size follows the button height so compact and big modes stay proportional.
void DrawToolbarButton(const DRAWITEMSTRUCT* di) {
    bool pressed = (di->itemState & ODS_SELECTED) != 0, hot = di->hwndItem == g_hotButton;
    int id = GetDlgCtrlID(di->hwndItem);
    // Shuffle, repeat and resume show that they are on with an accent glyph and a dot under it.
    bool active = (id == IDC_SHUFFLE && g.shuffle) || (id == IDC_REPEAT && g.repeat != Repeat::Off) ||
                  (id == IDC_RESUME && g.resumeOn);
    RECT r = di->rcItem;
    if (FoobarMode()) {
        DrawFoobarButton(di);
        return;
    }
    FillSolid(di->hDC, r, pressed ? Ref(P().select) : hot ? Ref(P().hover) : ControlBg(di->hwndItem));
    if (pressed || hot) {
        HGDIOBJ ob = SelectObject(di->hDC, GetStockObject(NULL_BRUSH));
        HGDIOBJ op = SelectObject(di->hDC, GetStockObject(DC_PEN));
        SetDCPenColor(di->hDC, Ref(P().border));
        Rectangle(di->hDC, r.left, r.top, r.right, r.bottom);
        SelectObject(di->hDC, ob);
        SelectObject(di->hDC, op);
    }
    // Segoe MDL2 Assets code points.
    const wchar_t* glyph = L"";
    switch (id) {
        case IDC_PREV: glyph = L"\xE892"; break;
        case IDC_PLAY: glyph = (g.playing != kNone || RadioPlaying()) && !g.engine.Paused() ? L"\xE769" : L"\xE768"; break;
        case IDC_NEXT: glyph = L"\xE893"; break;
        case IDC_SHUFFLE: glyph = L"\xE8B1"; break;
        case IDC_REPEAT: glyph = g.repeat == Repeat::One ? L"\xE8ED" : L"\xE8EE"; break;
        case IDC_RESUME: glyph = L"\xE81C"; break;  // History
    }
    COLORREF color = (di->itemState & ODS_DISABLED) ? Ref(P().faint) : active ? Ref(P().accent) : TextColor();
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, color);
    RECT gr = r;
    if (active) OffsetRect(&gr, 0, -S(2));
    int height = r.bottom - r.top;  // the glyph grows with the button (compact and big sizes)
    HGDIOBJ old = SelectObject(di->hDC, height >= S(46) ? g.fontIconsHuge : height >= S(34) ? g.fontIconsBig : g.fontIcons);
    DrawTextW(di->hDC, glyph, -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(di->hDC, old);
    if (active) {
        int cx = (r.left + r.right) / 2, cy = r.bottom - S(5), d = std::max(2, S(3));
        RECT dot{cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d};
        FillSolid(di->hDC, dot, color);
    }
    if (g.showFocusBoxes && (di->itemState & ODS_FOCUS)) {
        InflateRect(&r, -S(2), -S(2));
        DrawFocusRect(di->hDC, &r);
    }
}

}  // namespace app
