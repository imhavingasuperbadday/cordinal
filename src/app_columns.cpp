// Track list columns: which are shown, left to right (the header's context menu and drags), their
// widths, the header's own mouse handling, and the rating column's stars (drawn and clicked).
//
// The list view's columns are created in display order, so a subitem is a position on screen. The
// header's native resizing and drag-reordering are not used: column_layout.h decides the widths,
// divider drags, moves and sorts, the same way the Linux port does.
#include "app.h"
#include "column_layout.h"

namespace app {

int ColumnType(int subitem) { return subitem >= 0 && subitem < (int)g.columns.size() ? g.columns[subitem] : -1; }

int SubitemOf(int type) {
    auto it = std::find(g.columns.begin(), g.columns.end(), type);
    return it == g.columns.end() ? -1 : (int)(it - g.columns.begin());
}

static_assert(ui::kAlignLeft == LVCFMT_LEFT && ui::kAlignRight == LVCFMT_RIGHT, "ui_spec.h column alignment");

namespace {

// The foobar2000 theme keeps its own columns: foobar2000's default playlist, cover first.
const std::vector<int> kFoobarColumns{kColCover, 0, kColTrackNo, kColTitleArtist};

// And so does the Apollo theme: Apollo's playlist with every extra column it offered.
const std::vector<int> kApolloColumns{0, kColApolloTitle, 6, kColApolloBitrate, kColApolloRate, kColApolloBits, kColApolloSize,
                                      kColApolloTag};

const wchar_t* Section() { return FoobarMode() ? L"FoobarColumns" : ApolloMode() ? L"ApolloColumns" : L"Columns"; }
const std::vector<int>& DefaultColumns() { return FoobarMode() ? kFoobarColumns : ApolloMode() ? kApolloColumns : kDefaultColumns; }

// The header's text: foobar2000 calls the playing column "Playing"; Apollo numbers its rows under
// "No" and calls its "Artist - title" plain "Title".
const wchar_t* Heading(int type) {
    if (FoobarMode() && type == 0) return L"Playing";
    if (ApolloMode() && type == 0) return L"No";
    if (ApolloMode() && type == kColApolloTitle) return L"Title";
    return ColumnAt(type).name;
}

// Each column type's saved width at 96 DPI; 0 is the column's default. Apollo's Title's is unused.
std::vector<int> g_widths;

int DefaultWidth(int type) {
    if (type == 0 && FoobarMode()) return 50;
    if (type == 0 && ApolloMode()) return 30;
    return ColumnAt(type).width;
}

// The playing indicator is a fixed gutter where it has no heading; the themes that name it let it resize.
bool Fixed(int type) { return type == 0 && !*Heading(0); }

int Saved(int type) { return type >= 0 && type < (int)g_widths.size() ? g_widths[type] : 0; }

std::vector<ui::ColumnSpec> Specs() {
    std::vector<ui::ColumnSpec> specs;
    for (int type : g.columns) {
        int w = Saved(type);
        specs.push_back({type, S(w > 0 ? w : DefaultWidth(type)), S(ui::ColumnLeast(type)), Fixed(type)});
    }
    return specs;
}

std::vector<int> ShownWidths() {
    std::vector<int> widths;
    for (int i = 0; i < (int)g.columns.size(); ++i) widths.push_back(ListView_GetColumnWidth(g.list, i));
    return widths;
}

int ListRoom() {
    RECT rc{};
    GetClientRect(g.list, &rc);
    return rc.right;
}

// Apollo's "Automatically resize playlist columns to fit the window": its Title takes the room the
// others leave. Every other list keeps each column's own width, with the room past the last one empty.
int Stretch() { return ApolloMode() ? SubitemOf(kColApolloTitle) : -1; }

// (Re)creates the list view's columns from g.columns, left to right.
void CreateColumns() {
    if (g_widths.size() < (size_t)ColumnTotal()) g_widths.resize(ColumnTotal(), 0);
    while (Header_GetItemCount(g.header) > 0) ListView_DeleteColumn(g.list, 0);
    auto widths = ui::FitColumns(Specs(), ListRoom(), Stretch());
    for (int i = 0; i < (int)g.columns.size(); ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = const_cast<wchar_t*>(Heading(g.columns[i]));
        col.cx = widths[i];
        col.fmt = ColumnAt(g.columns[i]).align;
        ListView_InsertColumn(g.list, i, &col);
    }
    SetSortArrow(g.sortColumn, g.sortAscending);
    InvalidateRect(g.list, nullptr, FALSE);
}

bool Valid(const std::vector<int>& types) {
    std::vector<int> seen(ColumnTotal(), 0);
    bool ok = !types.empty() && std::find(types.begin(), types.end(), 0) != types.end();
    for (int t : types) ok = ok && t >= 0 && t < ColumnTotal() && !seen[t]++;
    return ok;
}

// The header's mouse: a press on a divider resizes; on a column it clicks (sorts) or, once it moves,
// drags the column to a new place.
struct HeaderPress {
    enum Kind { None, Click, Resize, Move } kind = None;
    int column = -1;         // the pressed column, or the dragged divider's left column
    int x = 0;               // where the press began, from the first column's left edge
    int slot = -1;           // a moving column's landing slot
    std::vector<int> start;  // the widths when the press began
};
HeaderPress g_press;
int g_hot = -1;  // the column under the mouse

// The header's x origin: the list scrolls its header sideways with its rows.
int HeaderOrigin(HWND h) {
    RECT r{};
    return Header_GetItemRect(h, 0, &r) ? r.left : 0;
}

bool OnDivider(HWND h, int x) {
    return ui::DividerAt(Specs(), ShownWidths(), x - HeaderOrigin(h), S(5), Stretch()) >= 0;
}

// Keeps the width a divider drag left, in 96 DPI units: the dragged column's, or beside Apollo's Title
// (whose width is the room left) the next one's.
void Remember(const std::vector<int>& widths, int k) {
    int i = k == Stretch() ? k + 1 : k;
    if (i >= 0 && i < (int)widths.size() && i < (int)g.columns.size() && !Fixed(g.columns[i]))
        g_widths[g.columns[i]] = MulDiv(widths[i], 96, (int)g.dpi);
}

void SetHot(HWND h, int column) {
    if (column == g_hot) return;
    g_hot = column;
    InvalidateRect(h, nullptr, FALSE);
}

}  // namespace

bool ColumnsFollowListWidth() { return Stretch() >= 0; }

// Gives each shown column its width: after a divider drag, a DPI change, or (Apollo) a new list width.
void FitListColumns() {
    if (!g.list || !g.header || g.columns.empty() || Header_GetItemCount(g.header) != (int)g.columns.size()) return;
    static bool fitting = false;  // a width change can show or hide a scrollbar, which resizes the list again
    if (fitting) return;
    fitting = true;
    auto widths = ui::FitColumns(Specs(), ListRoom(), Stretch());
    bool changed = false;
    for (int i = 0; i < (int)widths.size(); ++i)
        if (ListView_GetColumnWidth(g.list, i) != widths[i]) {
            ListView_SetColumnWidth(g.list, i, widths[i]);
            changed = true;
        }
    // A width change moves whatever spans the row (the playing underline, a title running over blank
    // cells, the MIX badge at the right edge), but the list repaints only the strip it uncovered: the
    // stale pieces were the stray red dots under a row.
    if (changed) {
        InvalidateRect(g.list, nullptr, FALSE);
        InvalidateRect(g.header, nullptr, FALSE);
    }
    fitting = false;
}

// Pack columns are saved by name ([Columns] PackColumns lists the name of each type from
// kColumnCount on), since their numbers change as packs come and go. Visible lists the shown
// columns left to right and Widths their widths at 96 DPI (0: the column's default).
void LoadColumns() {
    auto visible = ParseInts(IniRead(Section(), L"Visible", L""));
    auto widths = ParseInts(IniRead(Section(), L"Widths", L""));
    auto order = ParseInts(IniRead(Section(), L"Order", L""));
    if (!FoobarMode() && !ApolloMode() && visible.empty() && widths.size() == 8) {
        visible = {0, 1, 2, 3, 4, 5, 6, 7};  // Waveforge's fixed columns: keep their saved widths and order
    }
    // Before November 16, 2026 Visible and Widths were in the order the columns were created, and Order
    // put them left to right (the list's own drag reordering). Read such a profile left to right.
    std::vector<int> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    bool permutation = sorted.size() == visible.size();
    for (int i = 0; permutation && i < (int)sorted.size(); ++i) permutation = sorted[i] == i;
    if (permutation) {
        std::vector<int> v, w;
        for (int sub : order) {
            v.push_back(visible[sub]);
            if (sub < (int)widths.size()) w.push_back(widths[sub]);
        }
        visible = std::move(v);
        if (w.size() == visible.size()) widths = std::move(w);
    } else if (FoobarMode() && order.empty() && visible == std::vector<int>{0, kColCover, kColTrackNo, kColTitleArtist}) {
        visible = kFoobarColumns;  // the foobar2000 default before then, shown with the cover first
    }
    std::vector<std::wstring> names;
    std::wstring list = IniRead(Section(), L"PackColumns", L"");
    for (size_t at = 0; at <= list.size();) {
        size_t bar = std::min(list.find(L'|', at), list.size());
        names.push_back(list.substr(at, bar - at));
        at = bar + 1;
    }
    std::vector<int> mapped, mappedWidths;
    for (int i = 0; i < (int)visible.size(); ++i) {
        int t = visible[i], k = t - kColumnCount;
        if (k >= 0) t = k < (int)names.size() ? PackColumnType(names[k]) : -1;  // -1: its pack has gone
        if (t < 0) continue;
        mapped.push_back(t);
        mappedWidths.push_back(i < (int)widths.size() ? std::max(widths[i], 0) : 0);
    }
    g_widths.assign(ColumnTotal(), 0);
    if (Valid(mapped)) {
        g.columns = mapped;
        for (size_t i = 0; i < mapped.size(); ++i) g_widths[mapped[i]] = mappedWidths[i];
    } else {
        g.columns = DefaultColumns();
    }
}

void SetupColumns() { CreateColumns(); }

void SaveColumns() {
    std::vector<int> widths, order;
    for (int i = 0; i < (int)g.columns.size(); ++i) {
        widths.push_back(Saved(g.columns[i]));
        order.push_back(i);  // left to right already; written so an older build reads the same layout
    }
    IniWrite(Section(), L"Visible", JoinInts(g.columns));
    IniWrite(Section(), L"Widths", JoinInts(widths));
    IniWrite(Section(), L"Order", JoinInts(order));
    std::wstring names;
    for (int t = kColumnCount; t < ColumnTotal(); ++t) names += (t > kColumnCount ? L"|" : L"") + PackColumnName(t);
    IniWrite(Section(), L"PackColumns", names);
}

// Adds or removes one optional column, keeping the others' widths and places; an added column goes
// last. Persisted at once; driven by the list header's context menu.
void ToggleColumn(int type) {
    if (type <= 0 || type >= ColumnTotal()) return;
    if (SubitemOf(type) >= 0) {
        std::erase(g.columns, type);
        if (g.sortColumn == type) g.sortColumn = -1;
    } else {
        g.columns.push_back(type);
    }
    CreateColumns();
    SaveColumns();
}

// Moves the column shown at `from` to land before `slot` (0..count), as a header drag does. Nothing
// moves the fixed playing gutter or lands before it.
void MoveColumnTo(int from, int slot) {
    if (from < 0 || from >= (int)g.columns.size() || Fixed(g.columns[from])) return;
    slot = std::max(slot, ui::DropSlot(Specs(), ShownWidths(), -1));
    if (!ui::MoveColumn(g.columns, from, slot)) return;
    CreateColumns();
    SaveColumns();
}

// Sets a column's saved width at 96 DPI (0: its default) and refits the list.
void SetColumnWidth96(int type, int width) {
    if (type < 0 || type >= ColumnTotal()) return;
    if (g_widths.size() < (size_t)ColumnTotal()) g_widths.resize(ColumnTotal(), 0);
    g_widths[type] = std::max(width, 0);
    FitListColumns();
}

// Shows exactly `types`, left to right, at their default widths (a pack's column set).
void ShowColumns(const std::vector<int>& types) {
    g.columns = types;
    if (g.sortColumn >= 0 && SubitemOf(g.sortColumn) < 0) g.sortColumn = -1;
    for (int t : types)
        if (t >= 0 && t < (int)g_widths.size()) g_widths[t] = 0;
    CreateColumns();
    SaveColumns();
}

void ResetColumns() {
    g.columns = DefaultColumns();
    g_widths.assign(ColumnTotal(), 0);
    CreateColumns();
    SaveColumns();
}

// The track list header's mouse, in place of the native header's. True when it handled `msg`.
bool ColumnHeaderInput(HWND h, UINT msg, LPARAM lp, LRESULT& result) {
    result = 0;
    // Only mouse messages carry a point; measuring the header sends it messages of its own.
    const bool mouse = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP;
    const int x = mouse ? GET_X_LPARAM(lp) - HeaderOrigin(h) : 0;
    if (mouse && g_widths.size() < (size_t)ColumnTotal()) g_widths.resize(ColumnTotal(), 0);  // a pack added columns
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            auto specs = Specs();
            auto widths = ShownWidths();
            int divider = ui::DividerAt(specs, widths, x, S(5), Stretch());
            if (divider >= 0 && msg == WM_LBUTTONDBLCLK) {
                // A double-click on a divider gives its column its own width back.
                g_widths[g.columns[divider == Stretch() ? divider + 1 : divider]] = 0;
                FitListColumns();
                SaveColumns();
                return true;
            }
            g_press = {};
            g_press.x = x;
            g_press.start = widths;
            if (divider >= 0) {
                g_press.kind = HeaderPress::Resize;
                g_press.column = divider;
            } else if (int column = ui::ColumnAtX(widths, x); column >= 0) {
                g_press.kind = HeaderPress::Click;
                g_press.column = column;
            } else {
                return true;
            }
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
            return true;
        }
        case WM_MOUSEMOVE: {
            if (g_press.kind == HeaderPress::None) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                SetHot(h, OnDivider(h, GET_X_LPARAM(lp)) ? -1 : ui::ColumnAtX(ShownWidths(), x));
                return true;
            }
            if (g_press.kind == HeaderPress::Resize) {
                Remember(ui::DragDivider(Specs(), g_press.start, g_press.column, x - g_press.x, Stretch()), g_press.column);
                FitListColumns();
                return true;
            }
            if (g_press.kind == HeaderPress::Click) {
                if (std::abs(x - g_press.x) < GetSystemMetrics(SM_CXDRAG) || Fixed(g.columns[g_press.column])) return true;
                g_press.kind = HeaderPress::Move;
            }
            int slot = ui::DropSlot(Specs(), ShownWidths(), x);
            if (slot != g_press.slot) {
                g_press.slot = slot;
                InvalidateRect(h, nullptr, FALSE);
            }
            return true;
        }
        case WM_LBUTTONUP: {
            HeaderPress press = std::move(g_press);
            g_press = {};
            if (GetCapture() == h) ReleaseCapture();
            InvalidateRect(h, nullptr, FALSE);
            if (press.kind == HeaderPress::Click) {
                SortByColumn(press.column);
            } else if (press.kind == HeaderPress::Move) {
                MoveColumnTo(press.column, press.slot);
            } else if (press.kind == HeaderPress::Resize) {
                SaveColumns();
                if (FoobarMode()) RefreshViewKeepingSelection();  // the cover column's width sets the groups' heights
            }
            return true;
        }
        case WM_CAPTURECHANGED:
            if (g_press.kind != HeaderPress::None) {
                if (g_press.kind == HeaderPress::Resize) SaveColumns();
                g_press = {};
                InvalidateRect(h, nullptr, FALSE);
            }
            return true;
        case WM_MOUSELEAVE:
            SetHot(h, -1);
            return true;
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            bool resize = g_press.kind == HeaderPress::Resize || (g_press.kind == HeaderPress::None && OnDivider(h, pt.x));
            SetCursor(LoadCursorW(nullptr, resize ? IDC_SIZEWE : IDC_ARROW));
            result = TRUE;
            return true;
        }
    }
    return false;
}

// What the header paints over its columns: the column hovered, pressed or being moved.
int ColumnHeaderHot() {
    if (g_press.kind == HeaderPress::Click || g_press.kind == HeaderPress::Move) return g_press.column;
    return g_press.kind == HeaderPress::None ? g_hot : -1;
}

// Where a moving column would land, in header coordinates, or -1.
int ColumnHeaderDropX() {
    if (g_press.kind != HeaderPress::Move || g_press.slot < 0) return -1;
    int x = HeaderOrigin(g.header);
    auto widths = ShownWidths();
    for (int i = 0; i < g_press.slot && i < (int)widths.size(); ++i) x += widths[i];
    return x;
}

// Whether a header column does anything when clicked or dragged, so hovering it lights it.
bool ColumnHeaderLive(int subitem) {
    int type = ColumnType(subitem);
    return type >= 0 && (ui::SortsBy(type) || !Fixed(type));
}

// Whether the header's menu lists a column. The foobar2000 and Apollo themes' own columns appear only
// under their theme (Linux leaves them out entirely), unless one is shown, so it can still be hidden.
bool ColumnOffered(int type) {
    if (type <= 0 || type >= ColumnTotal()) return false;
    if (SubitemOf(type) >= 0) return true;
    if (type >= kColCover && type <= kColTitleArtist) return FoobarMode();
    if (type >= kColApolloTitle && type <= kColApolloTag) return ApolloMode();
    return true;
}

void ShowColumnMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    for (int t = 1; t < ColumnTotal(); ++t) {
        if (t == kColumnCount) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);  // the packs' columns follow
        if (!ColumnOffered(t)) continue;
        AppendMenuW(m, MF_STRING | (SubitemOf(t) >= 0 ? MF_CHECKED : 0), IDM_COLUMN_BASE + t, ColumnAt(t).name);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendPackColumnSets(m);
    AppendMenuW(m, MF_STRING, IDM_COLUMNS_RESET, L"&Reset columns");
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
}

// ---------------------------------------------------------------------------
// Rating stars

int StarWidth(HDC dc) {
    HGDIOBJ old = SelectObject(dc, g.fontSymbol);
    SIZE sz{};
    const wchar_t* mark = ThemeInfo(g.themeMode).ratingMark;
    GetTextExtentPoint32W(dc, mark, (int)wcslen(mark), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

// Five stars from the left of `r`: filled ones in the accent color (or the selection text
// color on a selected row), empty ones faint, a half star as a clipped filled star. An unrated
// track's row shows nothing until it is hovered or selected (ui::ShowEmptyStars).
void DrawStars(HDC dc, RECT r, uint8_t rating, bool selected, bool hover) {
    if (!ui::ShowEmptyStars(rating, hover, selected)) return;
    int w = StarWidth(dc);
    HGDIOBJ old = SelectObject(dc, g.fontSymbol);
    SetBkMode(dc, TRANSPARENT);
    COLORREF on = selected ? Ref(P().selectText) : Ref(P().accent), off = selected ? Ref(P().sub) : Ref(P().faint);
    int x = r.left + S(4);
    const wchar_t* mark = ThemeInfo(g.themeMode).ratingMark;  // a pack theme may draw hearts
    for (int i = 0; i < 5; ++i, x += w) {
        RECT s{x, r.top, x + w, r.bottom};
        int fill = std::clamp((int)rating - i * 20, 0, 20);  // 0, 10 (half) or 20
        if (fill < 20) {
            SetTextColor(dc, off);
            DrawTextW(dc, mark, -1, &s, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        if (fill) {
            SetTextColor(dc, on);
            // Logical-coordinate clipping: the list's double-buffered DC has a shifted origin.
            int saved = SaveDC(dc);
            IntersectClipRect(dc, s.left, s.top, fill < 20 ? s.left + w / 2 : s.right, s.bottom);
            DrawTextW(dc, mark, -1, &s, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            RestoreDC(dc, saved);
        }
    }
    SelectObject(dc, old);
}

// Rating for a click `x` pixels into a stars cell: half-star steps; clicking the current
// rating clears it.
uint8_t RatingAt(int x, uint8_t current) {
    HDC dc = GetDC(g.list);
    int w = StarWidth(dc);
    ReleaseDC(g.list, dc);
    double stars = (x - S(4)) / (double)std::max(w, 1);
    if (stars < -0.2 || stars > 5.4) return current;
    int halves = std::clamp((int)std::ceil(stars * 2), 1, 10);
    uint8_t rating = (uint8_t)(halves * 10);
    return rating == current ? 0 : rating;
}

// Applies a rating (0..100, ten per half star) to every selected track and marks the library dirty.
void SetRating(const std::vector<TrackId>& ids, uint8_t rating) {
    if (!ids.empty()) PlaySfx(rating ? Sfx::Rate : Sfx::Unrate, rating / 10);
    for (TrackId id : ids) g_lib[id].rating = rating;
    g_lib.Touch();
    g.dirty = true;
    InvalidateRect(g.list, nullptr, FALSE);
    LibViewInvalidate();
    if (g.playing != kNone) UpdateNowPlaying();
}

// A favorite is a heart the user chose, not a rating: pressing Favorite on a selection that is
// entirely favorited clears them, otherwise it hearts them all, so one key both ways.
void ToggleFavorites(const std::vector<TrackId>& ids) {
    if (ids.empty()) return;
    const bool all = std::all_of(ids.begin(), ids.end(), [](TrackId id) { return g_lib.Favorite(id); });
    for (TrackId id : ids) g_lib.SetFavorite(id, !all);
    PlaySfx(all ? Sfx::Unrate : Sfx::Sparkle, 0);
    g_lib.Touch();
    g.dirty = true;
    InvalidateRect(g.list, nullptr, FALSE);
    LibViewInvalidate();
    if (g.playing != kNone) UpdateNowPlaying();
}

HMENU RatingMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_RATE_BASE + 0, L"&No rating");
    for (int halves = 2; halves <= 10; halves += 2)
        AppendMenuW(m, MF_STRING, IDM_RATE_BASE + halves, (std::to_wstring(halves / 2) + L"  " + RatingStars((uint8_t)(halves * 10))).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_WRITE_STATISTICS, L"&Write ratings and play counts to files");
    return m;
}

}  // namespace app
