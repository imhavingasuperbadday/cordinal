// The Library tab's column browser (Genre | Album artist | Album by default, as in MusicBee and
// iTunes) and its view buttons (Tracks, Albums & Tracks, Albums, Artists).
//
// Each pane lists the values of its field among the tracks the panes to its left let through,
// with a count, and "All" on top. Multi-value tags ("A; B") count under each value.
#include "app.h"

namespace app {

namespace {

const wchar_t* const kFieldNames[] = {L"Genre", L"Album artist", L"Artist", L"Album", L"Composer", L"Year", L"Label", L"Grouping", L"Folder"};
const wchar_t* const kFieldPlurals[] = {L"genres", L"album artists", L"artists", L"albums", L"composers", L"years", L"labels", L"groupings", L"folders"};
static_assert(std::size(kFieldNames) == (size_t)BrowseField::Count);
static_assert(std::size(kFieldPlurals) == (size_t)BrowseField::Count);
const wchar_t* const kModeNames[] = {L"Tracks", L"Albums && Tracks", L"Albums", L"Artists"};

// Memoised per-tag expansions, keyed by the interned tag handle, so a multi-value split or a year
// extraction is done once and reused across every FilterLibrary pass. Cleared whenever the
// library's tag strings change (handles can then be reused for different text).
std::unordered_map<StringPool::Handle, std::vector<StringPool::Handle>> g_splitCache;
std::unordered_map<StringPool::Handle, StringPool::Handle> g_yearCache;

// "A; B" -> {A, B}; a value without separators needs no cache node or vector allocation.
void SplitTo(StringPool::Handle h, std::vector<StringPool::Handle>& out) {
    if (!h) {
        out.push_back(0);
        return;
    }
    std::string_view s = g_lib.View(h);
    if (s.find(';') == std::string_view::npos) {
        out.push_back(h);
        return;
    }
    auto [it, added] = g_splitCache.emplace(h, std::vector<StringPool::Handle>());
    if (added) {
        for (size_t start = 0; start <= s.size();) {
            size_t sep = s.find(';', start);
            if (sep == std::string_view::npos) sep = s.size();
            std::string_view part = s.substr(start, sep - start);
            while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
            while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
            if (!part.empty() || it->second.empty()) it->second.push_back(g_lib.Intern(part));
            start = sep + 1;
        }
    }
    out = it->second;
}

// "2005-06-01" -> "2005" for the Year pane; a value too short or not starting with a digit is left
// alone rather than reformatted.
StringPool::Handle YearOf(StringPool::Handle date) {
    std::string_view d = g_lib.View(date);
    if (d.size() <= 4 || !isdigit((unsigned char)d[0])) return date;
    auto [it, added] = g_yearCache.emplace(date, 0);
    if (added) it->second = g_lib.Intern(d.substr(0, 4));
    return it->second;
}

}  // namespace

// Fills `out` with every value of `field` for one track: multi-value tags give several handles,
// an empty tag gives handle 0 (drawn as "[Empty]"). `out` is reused to avoid per-call allocation.
void BrowserValues(TrackId id, BrowseField field, std::vector<StringPool::Handle>& out) {
    const Track& t = g_lib[id];
    out.clear();
    switch (field) {
        case BrowseField::Genre: SplitTo(t.genre, out); break;
        case BrowseField::AlbumArtist: SplitTo(t.albumArtist ? t.albumArtist : t.artist, out); break;
        case BrowseField::Artist: SplitTo(t.artist, out); break;
        case BrowseField::Album: out.push_back(t.album); break;
        case BrowseField::Composer: SplitTo(t.composer, out); break;
        case BrowseField::Year: out.push_back(YearOf(t.date)); break;
        case BrowseField::Label: out.push_back(t.label); break;
        case BrowseField::Grouping: out.push_back(t.grouping); break;
        // The full folder handle distinguishes namesakes and treats semicolons as path text.
        case BrowseField::Folder: out.push_back(t.dir); break;
        default: break;
    }
    if (out.empty()) out.push_back(0);
}

// Narrows `items` by every pane's selection, recomputing each pane's values from what the
// panes before it let through, and refreshes the pane lists.
void FilterLibrary(std::vector<TrackId>& items) {
    if (!g.panes[0].list) return;
    std::vector<StringPool::Handle> vals, keys;
    for (BrowserPane& pane : g.panes) {
        // Count each distinct value with one sorted flat list instead of a hash node per value.
        keys.clear();
        for (TrackId id : items) {
            BrowserValues(id, pane.field, vals);
            keys.insert(keys.end(), vals.begin(), vals.end());
        }
        SortRecords(keys.begin(), keys.end());
        std::vector<std::pair<StringPool::Handle, uint32_t>> counts;
        for (size_t i = 0; i < keys.size();) {
            size_t j = i + 1;
            while (j < keys.size() && keys[j] == keys[i]) ++j;
            counts.emplace_back(keys[i], (uint32_t)(j - i));
            i = j;
        }
        std::vector<StringPool::Handle>().swap(keys);
        auto countOf = [&](StringPool::Handle h) -> const uint32_t* {
            auto it = std::lower_bound(counts.begin(), counts.end(), h,
                                       [](const auto& e, StringPool::Handle v) { return e.first < v; });
            return it != counts.end() && it->first == h ? &it->second : nullptr;
        };
        std::vector<StringPool::Handle> handles;
        handles.reserve(counts.size());
        for (auto& [h, n] : counts) handles.push_back(h);
        StringRanks rank(handles);
        SortRecords(handles.begin(), handles.end(), [&](auto a, auto b) { return rank(a) < rank(b); });
        pane.values = std::move(handles);
        pane.counts.clear();
        pane.counts.reserve(counts.size());
        for (auto h : pane.values) pane.counts.push_back(*countOf(h));
        // A selected value that no longer exists upstream drops out of the selection.
        std::erase_if(pane.selected, [&](StringPool::Handle h) { return !countOf(h); });
        if (!pane.selected.empty()) {
            std::unordered_set<StringPool::Handle> sel(pane.selected.begin(), pane.selected.end());
            std::erase_if(items, [&](TrackId id) {
                BrowserValues(id, pane.field, vals);
                return std::none_of(vals.begin(), vals.end(), [&](auto v) { return sel.count(v) != 0; });
            });
        }
        // Rows: 0 = All, then values; selection mirrors pane.selected.
        g.browserUpdating = true;
        ListView_SetItemCountEx(pane.list, (int)pane.values.size() + 1, LVSICF_NOSCROLL);
        ListView_SetItemState(pane.list, -1, 0, LVIS_SELECTED);
        if (pane.selected.empty()) {
            ListView_SetItemState(pane.list, 0, LVIS_SELECTED, LVIS_SELECTED);
        } else {
            std::unordered_set<StringPool::Handle> sel(pane.selected.begin(), pane.selected.end());
            for (int i = 0; i < (int)pane.values.size(); ++i)
                if (sel.count(pane.values[i])) ListView_SetItemState(pane.list, i + 1, LVIS_SELECTED, LVIS_SELECTED);
        }
        InvalidateRect(pane.list, nullptr, FALSE);
        g.browserUpdating = false;
    }
}

// The user changed a pane's selection: read it back and rebuild the library view.
void OnBrowserSelection() {
    for (BrowserPane& pane : g.panes) {
        pane.selected.clear();
        if (ListView_GetItemState(pane.list, 0, LVIS_SELECTED)) continue;  // "All"
        for (int row = 0; (row = ListView_GetNextItem(pane.list, row, LVNI_SELECTED)) > 0;)
            if (row - 1 < (int)pane.values.size()) pane.selected.push_back(pane.values[row - 1]);
    }
    Playlist& lib = g.playlists[0];
    lib.builtRevision = 0;
    if (g.active == 0) {
        ClearSelection();
        RebuildView();
    } else {
        RebuildViewItems(lib);
    }
}

void ResetBrowser() {
    for (BrowserPane& pane : g.panes) pane.selected.clear();
    OnBrowserSelection();
}

void SelectBrowserValue(BrowseField field, StringPool::Handle value) {
    for (BrowserPane& pane : g.panes) {
        pane.selected.clear();
        if (pane.field == field) pane.selected.push_back(value);
    }
    g.playlists[0].builtRevision = 0;
    if (g.active == 0) RebuildView();
}

std::wstring BrowserRowText(const BrowserPane& pane, int row, int col) {
    if (row == 0)
        return col == 0 ? L"All (" + std::to_wstring(pane.values.size()) + L" " + kFieldPlurals[(int)pane.field] + L")" : L"";
    if (row - 1 >= (int)pane.values.size()) return {};
    if (col == 1) return std::to_wstring(pane.counts[row - 1]);
    auto h = pane.values[row - 1];
    return h ? g_lib.Str(h) : L"[Empty]";
}

LRESULT OnBrowserNotify(int index, NMHDR* hdr) {
    BrowserPane& pane = g.panes[index];
    switch (hdr->code) {
        case LVN_GETDISPINFOW: {
            auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
            if (di->item.mask & LVIF_TEXT) {
                std::wstring text = BrowserRowText(pane, di->item.iItem, di->item.iSubItem);
                wcsncpy_s(di->item.pszText, di->item.cchTextMax, text.c_str(), _TRUNCATE);
            }
            return 0;
        }
        case LVN_ODFINDITEMW: {
            // Type to jump: the first value starting with the typed text.
            auto* fi = reinterpret_cast<NMLVFINDITEMW*>(hdr);
            if (!(fi->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !fi->lvfi.psz) return -1;
            size_t n = wcslen(fi->lvfi.psz);
            for (int i = 0; i < (int)pane.values.size(); ++i) {
                std::wstring v = g_lib.Str(pane.values[i]);
                if (v.size() >= n && CompareStringOrdinal(v.c_str(), (int)n, fi->lvfi.psz, (int)n, TRUE) == CSTR_EQUAL)
                    return i + 1;
            }
            return -1;
        }
        case LVN_ITEMCHANGED:
        case LVN_ODSTATECHANGED:
            // Coalesce a burst of changes (shift-select, select all) into one rebuild.
            if (!g.browserUpdating) PostMessageW(g.hwnd, WM_APP_BROWSE, 0, 0);
            return 0;
        case NM_CUSTOMDRAW: {
            auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT || cd->nmcd.dwDrawStage == (CDDS_SUBITEM | CDDS_ITEMPREPAINT)) {
                int row = (int)cd->nmcd.dwItemSpec;
                bool selected = ListView_GetItemState(pane.list, row, LVIS_SELECTED) != 0;
                cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS | CDIS_HOT);
                SelectObject(cd->nmcd.hdc, row == 0 ? g.fontBold : g.font);
                cd->clrTextBk = selected ? Ref(P().select) : WindowBg();
                cd->clrText = selected ? Ref(P().selectText) : cd->iSubItem == 1 ? SubColor() : TextColor();
                if (pane.field == BrowseField::Folder && row > 0 && cd->iSubItem == 0 &&
                    cd->nmcd.dwDrawStage == (CDDS_SUBITEM | CDDS_ITEMPREPAINT)) {
                    // Keep the folder's end visible when a long common prefix fills the pane.
                    RECT r{};
                    ListView_GetSubItemRect(pane.list, row, 0, LVIR_BOUNDS, &r);
                    r.right = r.left + ListView_GetColumnWidth(pane.list, 0);
                    FillSolid(cd->nmcd.hdc, r, cd->clrTextBk);
                    r.left += S(6);
                    r.right -= S(6);
                    std::wstring text = BrowserRowText(pane, row, 0);
                    SetBkMode(cd->nmcd.hdc, TRANSPARENT);
                    SetTextColor(cd->nmcd.hdc, cd->clrText);
                    DrawTextW(cd->nmcd.hdc, text.c_str(), (int)text.size(), &r,
                              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_PATH_ELLIPSIS);
                    return CDRF_SKIPDEFAULT;
                }
                return cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT ? CDRF_NEWFONT | CDRF_NOTIFYSUBITEMDRAW : CDRF_NEWFONT;
            }
            return CDRF_DODEFAULT;
        }
        case NM_DBLCLK: {
            // Double-click a value: play everything under it.
            if (g.active == 0 && !Active().items.empty()) PlayTrack(0, 0, true);
            return 0;
        }
    }
    return 0;
}

void ShowBrowserFieldMenu(int index, POINT pt) {
    HMENU m = CreatePopupMenu();
    for (int f = 0; f < (int)BrowseField::Count; ++f)
        AppendMenuW(m, MF_STRING | (g.panes[index].field == (BrowseField)f ? MF_CHECKED : 0), 1 + f, kFieldNames[f]);
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd <= 0) return;
    BrowserPane& pane = g.panes[index];
    pane.field = (BrowseField)(cmd - 1);
    pane.selected.clear();
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT;
    col.pszText = const_cast<wchar_t*>(kFieldNames[cmd - 1]);
    ListView_SetColumn(pane.list, 0, &col);
    OnBrowserSelection();
    SaveBrowserSettings();
}

// ---------------------------------------------------------------------------
// Controls

void CreateBrowser(HWND parent) {
    LoadBrowserSettings();
    for (int i = 0; i < 3; ++i) {
        BrowserPane& pane = g.panes[i];
        pane.list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                    WS_CHILD | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP, 0, 0, 0, 0,
                                    parent, (HMENU)(INT_PTR)(IDC_BROWSER0 + i), g.inst, nullptr);
        ListView_SetExtendedListViewStyle(pane.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        SetWindowSubclass(ListView_GetHeader(pane.list), HeaderSubclass, 0, 0);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = const_cast<wchar_t*>(kFieldNames[(int)pane.field]);
        col.cx = S(200);
        ListView_InsertColumn(pane.list, 0, &col);
        col.pszText = const_cast<wchar_t*>(L"");
        col.cx = S(48);
        col.fmt = LVCFMT_RIGHT;
        ListView_InsertColumn(pane.list, 1, &col);
    }
    for (int i = 0; i < kViewButtons; ++i) {
        const wchar_t* text = i < (int)LibraryMode::Count ? kModeNames[i] : L"Browser";
        g.viewButtons[i] = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 0, 0, parent,
                                           (HMENU)(INT_PTR)(IDC_VIEW0 + i), g.inst, nullptr);
        SetWindowSubclass(g.viewButtons[i], ButtonSubclass, 0, 0);
    }
}

void LoadBrowserSettings() {
    auto fields = ParseInts(IniRead(L"Library", L"BrowserFields", L"0,1,3"));
    for (int i = 0; i < 3; ++i) {
        BrowserPane& pane = g.panes[i];
        pane.field = i < (int)fields.size() && fields[i] >= 0 && fields[i] < (int)BrowseField::Count ? (BrowseField)fields[i]
                                                                                                         : (BrowseField)i;
    }
    g.libMode = (LibraryMode)std::clamp(IniInt(L"Library", L"View", (int)LibraryMode::Albums), 0, (int)LibraryMode::Count - 1);
    g.browserVisible = IniInt(L"Library", L"Browser", 1) != 0;
    g.defaultLibraryMode = g.libMode;
    g.defaultBrowserVisible = g.browserVisible;
    g.browserHeight = std::clamp(IniInt(L"Library", L"BrowserHeight", 150), 60, 2000);
}

void SaveBrowserSettings() {
    std::vector<int> fields;
    for (auto& pane : g.panes) fields.push_back((int)pane.field);
    IniWrite(L"Library", L"BrowserFields", JoinInts(fields));
    IniWrite(L"Library", L"View", std::to_wstring((int)g.defaultLibraryMode));
    IniWrite(L"Library", L"Browser", g.defaultBrowserVisible ? L"1" : L"0");
    IniWrite(L"Library", L"BrowserHeight", std::to_wstring(g.browserHeight));
}

void ThemeBrowser() {
    for (auto& pane : g.panes) {
        if (!pane.list) continue;
        ApplyControlTheme(pane.list, g.dark);
        ApplyControlTheme(ListView_GetHeader(pane.list), g.dark);
        ListView_SetBkColor(pane.list, WindowBg());
        ListView_SetTextBkColor(pane.list, WindowBg());
        ListView_SetTextColor(pane.list, TextColor());
        SendMessageW(pane.list, WM_SETFONT, (WPARAM)g.font, TRUE);
    }
    for (HWND b : g.viewButtons) SendMessageW(b, WM_SETFONT, (WPARAM)g.font, TRUE);
}

bool LibraryActive() { return g.active == 0 && !g.playlists.empty() && g.playlists[0].kind == PlaylistKind::Library; }
bool CanvasActive() { return g.libView && LibraryActive() && g.libMode != LibraryMode::Tracks && !FoobarMode() && !ApolloMode(); }

void SetLibraryMode(LibraryMode mode) {
    g.libMode = mode;
    for (HWND b : g.viewButtons) InvalidateRect(b, nullptr, FALSE);
    Layout();
    RebuildView();
    SetFocus(CanvasActive() ? g.libView : g.list);
    SaveBrowserSettings();
}

void ToggleBrowser() {
    g.browserVisible = !g.browserVisible;
    if (!g.browserVisible) ResetBrowser();
    Layout();
    InvalidateRect(g.viewButtons[kViewButtons - 1], nullptr, FALSE);
    SaveBrowserSettings();
}

// The views are one segmented control (the current one filled); the browser toggle is a button
// of its own, filled while the browser shows. Neither has the tabs' accent underline.
void DrawViewButton(const DRAWITEMSTRUCT* di) {
    int i = (int)di->CtlID - IDC_VIEW0;
    bool toggle = i == kViewButtons - 1;
    bool on = !toggle ? (int)g.libMode == i : g.browserVisible;
    bool hot = di->hwndItem == g_hotButton, pressed = (di->itemState & ODS_SELECTED) != 0;
    RECT r = di->rcItem;
    // A step below the playlist tabs: the current view (or the shown browser) is a square raised
    // box, so it never reads as another tab; hover and press just brighten the label.
    FillSolid(di->hDC, r, SurfaceColor());
    if (on) {
        FillSolid(di->hDC, r, Ref(P().border));
        FillSolid(di->hDC, {r.left + S(1), r.top + S(1), r.right - S(1), r.bottom - S(1)}, Ref(P().surface2));
    }
    wchar_t text[64] = {};
    GetWindowTextW(di->hwndItem, text, (int)std::size(text));
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, on || hot || pressed ? TextColor() : SubColor());
    HGDIOBJ old = SelectObject(di->hDC, g.font);
    DrawTextW(di->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(di->hDC, old);
    if (g.showFocusBoxes && (di->itemState & ODS_FOCUS)) {
        InflateRect(&r, -S(2), -S(2));
        DrawFocusRect(di->hDC, &r);
    }
}

// Width a view button needs for its label.
int ViewButtonWidth(int i) {
    HDC dc = GetDC(g.hwnd);
    HGDIOBJ old = SelectObject(dc, g.font);
    wchar_t text[64] = {};
    GetWindowTextW(g.viewButtons[i], text, (int)std::size(text));
    RECT r{};
    DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_SINGLELINE);
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);
    return r.right - r.left + S(16);
}

// Must follow any change to the library's tag strings: a cached handle can otherwise refer to text
// that has since been replaced.
void ClearBrowserCaches() {
    decltype(g_splitCache)().swap(g_splitCache);
    decltype(g_yearCache)().swap(g_yearCache);
}

}  // namespace app
