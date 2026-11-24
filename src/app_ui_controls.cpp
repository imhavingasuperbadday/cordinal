// The stock controls cordinal subclasses itself: search box keys, the flat list header and the
// owner-drawn toolbar buttons with their hot state.
#include "app_ui_internal.h"
#include "search_query.h"

namespace app {

namespace {

struct SuggestPopup {
    HWND hwnd = nullptr;
    std::vector<SearchSuggestion> items;
    int selected = -1;
    int hot = -1;
    int rowHeight = 0;
} g_suggest;

constexpr wchar_t kSuggestClass[] = L"CordinalSearchSuggest";
bool g_suggestClassRegistered = false;

void AcceptSuggestion(int index) {
    if (index < 0 || index >= (int)g_suggest.items.size()) return;
    std::wstring next = ApplySearchSuggestion(WindowText(g.search), g_suggest.items[index].insert);
    SetWindowTextW(g.search, next.c_str());
    SendMessageW(g.search, EM_SETSEL, (WPARAM)next.size(), (LPARAM)next.size());
    SetFocus(g.search);
}

LRESULT CALLBACK SuggestProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            FillSolid(dc, rc, Ref(P().surface2));
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ old = SelectObject(dc, g.font);
            const int rowH = g_suggest.rowHeight > 0 ? g_suggest.rowHeight : S(24);
            for (int i = 0; i < (int)g_suggest.items.size(); ++i) {
                RECT row{0, i * rowH, rc.right, (i + 1) * rowH};
                bool sel = i == g_suggest.selected;
                bool hot = i == g_suggest.hot;
                if (sel) FillSolid(dc, row, Ref(P().select));
                else if (hot) FillSolid(dc, row, Ref(P().hover));
                RECT lr{row.left + S(8), row.top, row.right - S(8), row.bottom};
                SetTextColor(dc, Ref(P().text));
                DrawTextW(dc, g_suggest.items[i].label.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                if (!g_suggest.items[i].hint.empty()) {
                    SetTextColor(dc, Ref(P().sub));
                    DrawTextW(dc, g_suggest.items[i].hint.c_str(), -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                }
            }
            SelectObject(dc, old);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int rowH = g_suggest.rowHeight > 0 ? g_suggest.rowHeight : S(24);
            int idx = GET_Y_LPARAM(lp) / rowH;
            if (idx < 0 || idx >= (int)g_suggest.items.size()) idx = -1;
            if (idx != g_suggest.hot) {
                g_suggest.hot = idx;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_suggest.hot != -1) { g_suggest.hot = -1; InvalidateRect(h, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONDOWN: {
            int rowH = g_suggest.rowHeight > 0 ? g_suggest.rowHeight : S(24);
            int idx = GET_Y_LPARAM(lp) / rowH;
            AcceptSuggestion(idx);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void HideSearchSuggest() {
    g_suggest.items.clear();
    g_suggest.selected = g_suggest.hot = -1;
    if (g_suggest.hwnd) ShowWindow(g_suggest.hwnd, SW_HIDE);
}

void UpdateSearchSuggest() {
    if (!g.search || GetFocus() != g.search || RadioActive() || g.closing) {
        HideSearchSuggest();
        return;
    }
    g_suggest.items = SearchSuggestions(g_lib, WindowText(g.search), 8);
    if (g_suggest.items.empty()) {
        HideSearchSuggest();
        return;
    }
    if (!g_suggestClassRegistered) {
        RegisterAppClass(kSuggestClass, SuggestProc, 0, false);
        g_suggestClassRegistered = true;
    }
    if (!g_suggest.hwnd) {
        g_suggest.hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kSuggestClass, L"",
                                         WS_POPUP | WS_BORDER, 0, 0, 0, 0, g.hwnd, nullptr, g.inst, nullptr);
    }
    g_suggest.selected = -1;
    g_suggest.hot = -1;
    g_suggest.rowHeight = S(24);
    RECT sr;
    GetWindowRect(g.search, &sr);
    int w = std::max<int>((int)(sr.right - sr.left), S(260));
    int h = (int)g_suggest.items.size() * g_suggest.rowHeight + 2;
    SetWindowPos(g_suggest.hwnd, HWND_TOPMOST, sr.left, sr.bottom + 1, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_suggest.hwnd, nullptr, FALSE);
}

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
            if (g_suggest.hwnd && IsWindowVisible(g_suggest.hwnd)) {
                HideSearchSuggest();
                return 0;
            }
            SetWindowTextW(h, L"");
            FlushSearch();
            SetFocus(CanvasActive() ? g.libView : g.list);
            return 0;
        }
        if (wp == VK_TAB && g_suggest.hwnd && IsWindowVisible(g_suggest.hwnd) && !g_suggest.items.empty()) {
            AcceptSuggestion(g_suggest.selected >= 0 ? g_suggest.selected : 0);
            return 0;
        }
        if (wp == VK_RETURN) {
            if (g_suggest.hwnd && IsWindowVisible(g_suggest.hwnd) && g_suggest.selected >= 0) {
                AcceptSuggestion(g_suggest.selected);
                return 0;
            }
            HideSearchSuggest();
            FlushSearch();
            if (GetKeyState(VK_CONTROL) >= 0) {
                if (!g.view.empty()) PlayRow(0);
                else OnlineSearchActivateDefault();
            }
            return 0;
        }
        if (wp == VK_UP && g_suggest.hwnd && IsWindowVisible(g_suggest.hwnd) && !g_suggest.items.empty()) {
            g_suggest.selected = g_suggest.selected <= 0 ? (int)g_suggest.items.size() - 1 : g_suggest.selected - 1;
            InvalidateRect(g_suggest.hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == VK_DOWN) {
            if (g_suggest.hwnd && IsWindowVisible(g_suggest.hwnd) && !g_suggest.items.empty()) {
                g_suggest.selected = (g_suggest.selected + 1) % (int)g_suggest.items.size();
                InvalidateRect(g_suggest.hwnd, nullptr, FALSE);
                return 0;
            }
            FlushSearch();
            if (g.view.empty()) {
                OnlineSearchFocus();
                return 0;
            }
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
    } else if (msg == WM_CHAR && (wp == L'\r' || wp == 27 || wp == 1 ||
                                  (wp == L'\t' && g_suggest.hwnd && IsWindowVisible(g_suggest.hwnd)))) {
        return 0;
    } else if (msg == WM_KILLFOCUS) {
        if ((HWND)wp != g_suggest.hwnd) HideSearchSuggest();
    } else if (msg == WM_NCDESTROY) {
        HideSearchSuggest();
        RemoveWindowSubclass(h, SearchSubclass, 0);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Flat list header in both themes: the list's own background so header and rows read as one
// surface, quiet labels, short faint dividers (enough to find a column edge) and the sort arrow. The
// track list's header takes its own mouse (app_columns.cpp ColumnHeaderInput): the native one's
// resizing and drag-reordering never run, so they cannot leave a column of no width or lose a move.
LRESULT CALLBACK HeaderSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (h == g.header) {
        LRESULT result = 0;
        if (ColumnHeaderInput(h, msg, lp, result)) return result;
    }
    if (msg == WM_RBUTTONUP) {
        // A list view keeps its header's right-click to itself, so no WM_CONTEXTMENU ever reached the
        // window and the column menu could not open. Name the header to the window directly.
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(h, &pt);
        SendMessageW(GetAncestor(h, GA_ROOT), WM_CONTEXTMENU, (WPARAM)h, MAKELPARAM(pt.x, pt.y));
        return 0;
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
            // The column under the mouse, pressed or being moved, when it sorts or moves at all.
            if (h == g.header && i == ColumnHeaderHot() && ColumnHeaderLive(i) && !foobar)
                FillSolid(dc, {r.left, r.top, r.right, r.bottom - 1}, Ref(P().hover));
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
                // The stock DC_BRUSH and DC_PEN need no object per paint: the same solid 1 px arrow,
                // without a brush and pen created and deleted for every header repaint.
                SetDCBrushColor(dc, Ref(P().text));
                SetDCPenColor(dc, Ref(P().text));
                HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
                HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
                Polygon(dc, tri, 3);
                SelectObject(dc, ob);
                SelectObject(dc, op);
            }
        }
        FillSolid(dc, {rc.left, rc.bottom - 1, rc.right, rc.bottom}, Ref(P().border));
        // Where a column being dragged would land.
        if (int drop = h == g.header ? ColumnHeaderDropX() : -1; drop >= 0)
            FillSolid(dc, {std::max(0, drop - S(1)), rc.top, drop + S(1), rc.bottom}, Ref(P().accent));
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
        case IDC_SAVE_SEARCH: glyph = L"\xE710"; break;  // Add (+)
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
