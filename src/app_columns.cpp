// Track list columns: which are shown and in what order (the header's context menu), their
// saved widths, and the rating column's stars (drawn and clicked).
#include "app.h"

namespace app {

int ColumnType(int subitem) { return subitem >= 0 && subitem < (int)g.columns.size() ? g.columns[subitem] : -1; }

int SubitemOf(int type) {
    auto it = std::find(g.columns.begin(), g.columns.end(), type);
    return it == g.columns.end() ? -1 : (int)(it - g.columns.begin());
}

static_assert(ui::kAlignLeft == LVCFMT_LEFT && ui::kAlignRight == LVCFMT_RIGHT, "ui_spec.h column alignment");

namespace {

// The foobar2000 theme keeps its own columns: foobar2000's default playlist.
const std::vector<int> kFoobarColumns{0, kColCover, kColTrackNo, kColTitleArtist};
const std::vector<int> kFoobarOrder{kColCover, 0, kColTrackNo, kColTitleArtist};

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

// Which saved positions LoadColumns kept, so SetupColumns can drop the saved widths and order of a
// pack column whose pack is no longer on (-1: none dropped).
std::vector<int> g_keptPositions;

}  // namespace

// Pack columns are saved by name ([Columns] PackColumns lists the name of each type from
// kColumnCount on), since their numbers change as packs come and go.
void LoadColumns() {
    auto visible = ParseInts(IniRead(Section(), L"Visible", L""));
    if (!FoobarMode() && !ApolloMode() && visible.empty() && ParseInts(IniRead(L"Columns", L"Widths", L"")).size() == 8) {
        visible = {0, 1, 2, 3, 4, 5, 6, 7};  // Waveforge's fixed columns: keep their saved widths and order
    }
    std::vector<std::wstring> names;
    std::wstring list = IniRead(Section(), L"PackColumns", L"");
    for (size_t at = 0; at <= list.size();) {
        size_t bar = std::min(list.find(L'|', at), list.size());
        names.push_back(list.substr(at, bar - at));
        at = bar + 1;
    }
    std::vector<int> mapped;
    g_keptPositions.clear();
    for (int i = 0; i < (int)visible.size(); ++i) {
        int t = visible[i], k = t - kColumnCount;
        if (k >= 0) t = k < (int)names.size() ? PackColumnType(names[k]) : -1;
        if (t < 0) continue;
        mapped.push_back(t);
        g_keptPositions.push_back(i);
    }
    if (mapped.size() == visible.size()) g_keptPositions.clear();
    std::vector<int> seen(ColumnTotal(), 0);
    bool valid = !mapped.empty() && mapped[0] == 0;
    for (int t : mapped) valid = valid && t >= 0 && t < ColumnTotal() && !seen[t]++;
    g.columns = valid ? mapped : DefaultColumns();
    if (!valid) g_keptPositions.clear();
}

namespace {

// (Re)creates the list view's columns from g.columns. Widths are per column type at 96 DPI;
// `order` lists column types left to right.
void CreateColumns(const std::vector<int>& widthByType, const std::vector<int>& orderTypes) {
    while (Header_GetItemCount(g.header) > 0) ListView_DeleteColumn(g.list, 0);
    for (int i = 0; i < (int)g.columns.size(); ++i) {
        const Column& c = ColumnAt(g.columns[i]);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = const_cast<wchar_t*>(Heading(g.columns[i]));
        int w = widthByType[g.columns[i]];
        col.cx = S(w > 0 ? w : FoobarMode() && g.columns[i] == 0 ? 50 : ApolloMode() && g.columns[i] == 0 ? 30 : c.width);
        col.fmt = c.align;
        ListView_InsertColumn(g.list, i, &col);
    }
    std::vector<int> order;
    for (int t : orderTypes)
        if (int sub = SubitemOf(t); sub >= 0) order.push_back(sub);
    if (order.size() == g.columns.size()) ListView_SetColumnOrderArray(g.list, (int)order.size(), order.data());
    SetSortArrow(g.sortColumn, g.sortAscending);
}

void CurrentLayout(std::vector<int>& widthByType, std::vector<int>& orderTypes) {
    widthByType.assign(ColumnTotal(), 0);
    for (int i = 0; i < (int)g.columns.size(); ++i)
        widthByType[g.columns[i]] = MulDiv(ListView_GetColumnWidth(g.list, i), 96, (int)g.dpi);
    std::vector<int> order(g.columns.size());
    ListView_GetColumnOrderArray(g.list, (int)order.size(), order.data());
    orderTypes.clear();
    for (int sub : order) orderTypes.push_back(ColumnType(sub));
}

}  // namespace

void SetupColumns() {
    auto widths = ParseInts(IniRead(Section(), L"Widths", L""));
    auto order = ParseInts(IniRead(Section(), L"Order", L""));
    if (!g_keptPositions.empty()) {  // a pack column left: keep the rest's widths, and their order
        std::vector<int> keptWidths, keptOrder;
        for (int i : g_keptPositions)
            if (i < (int)widths.size()) keptWidths.push_back(widths[i]);
        for (int sub : order)
            if (auto it = std::find(g_keptPositions.begin(), g_keptPositions.end(), sub); it != g_keptPositions.end())
                keptOrder.push_back((int)(it - g_keptPositions.begin()));
        widths = keptWidths;
        order = keptOrder;
        g_keptPositions.clear();
    }
    std::vector<int> widthByType(ColumnTotal(), 0), orderTypes;
    if (widths.size() == g.columns.size())
        for (size_t i = 0; i < widths.size(); ++i) widthByType[g.columns[i]] = std::max(widths[i], 0);
    if (order.size() == g.columns.size())
        for (int sub : order) orderTypes.push_back(ColumnType(sub));
    else if (FoobarMode() && g.columns == kFoobarColumns)
        orderTypes = kFoobarOrder;
    CreateColumns(widthByType, orderTypes);
}

void SaveColumns() {
    std::vector<int> widths, order(g.columns.size());
    for (int i = 0; i < (int)g.columns.size(); ++i) widths.push_back(MulDiv(ListView_GetColumnWidth(g.list, i), 96, (int)g.dpi));
    ListView_GetColumnOrderArray(g.list, (int)order.size(), order.data());
    IniWrite(Section(), L"Visible", JoinInts(g.columns));
    IniWrite(Section(), L"Widths", JoinInts(widths));
    IniWrite(Section(), L"Order", JoinInts(order));
    std::wstring names;
    for (int t = kColumnCount; t < ColumnTotal(); ++t) names += (t > kColumnCount ? L"|" : L"") + PackColumnName(t);
    IniWrite(Section(), L"PackColumns", names);
}

// Adds or removes one optional column, preserving the widths and order of the others. Persisted
// immediately by SaveColumns; driven by the list header's context menu.
void ToggleColumn(int type) {
    if (type <= 0 || type >= ColumnTotal()) return;
    std::vector<int> widthByType, orderTypes;
    CurrentLayout(widthByType, orderTypes);
    if (SubitemOf(type) >= 0) {
        std::erase(g.columns, type);
        std::erase(orderTypes, type);
        if (g.sortColumn == type) g.sortColumn = -1;
    } else {
        g.columns.push_back(type);
        orderTypes.push_back(type);
    }
    CreateColumns(widthByType, orderTypes);
    InvalidateRect(g.list, nullptr, FALSE);
    SaveColumns();
}

// Shows exactly `types`, left to right, at their default widths (a pack's column set).
void ShowColumns(const std::vector<int>& types) {
    g.columns = types;
    if (g.sortColumn >= 0 && SubitemOf(g.sortColumn) < 0) g.sortColumn = -1;
    CreateColumns(std::vector<int>(ColumnTotal(), 0), types);
    InvalidateRect(g.list, nullptr, FALSE);
    SaveColumns();
}

void ResetColumns() {
    g.columns = DefaultColumns();
    CreateColumns(std::vector<int>(ColumnTotal(), 0), FoobarMode() ? kFoobarOrder : std::vector<int>{});
    InvalidateRect(g.list, nullptr, FALSE);
    SaveColumns();
}

void ShowColumnMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    for (int t = 1; t < ColumnTotal(); ++t) {
        if (t == kColumnCount) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);  // the packs' columns follow
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
// color on a selected row), empty ones faint, a half star as a clipped filled star.
void DrawStars(HDC dc, RECT r, uint8_t rating, bool selected) {
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
