// The foobar2000 theme's window, in foobar2000's default UI arrangement:
//   - the toolbar: stop, play, pause, previous, next and random, a visualization box, the volume,
//     the seek bar and a second visualization box, drawn as foobar2000 draws them
//   - the Album List panel on the left ("by folder structure", "by artist/album", ... with a
//     filter), with Selection Properties as its second tab
//   - the playlist tabs and the playlist on the right: grouped as "Artist - [Year] Album" with the
//     cover in its own column beside the tracks, "Track no" and "Title / track artist"
//   - the status bar: "Playback stopped." or the playing file's format and time
// It applies only in the normal window size; the other sizes keep Cordinal's own arrangement.
#include "app.h"

#include <d2d1.h>

namespace app {

bool FoobarMode() { return g.themeMode == ThemeMode::Foobar && g.size == ViewSize::Normal && !RadioActive(); }

namespace {

constexpr wchar_t kPanelClass[] = L"CordinalFoobarPanel";
constexpr UINT_PTR kFilterTimer = 1, kVisTimer = 2;
const wchar_t* const kViews[] = {L"by folder structure", L"by artist/album", L"by album", L"by genre"};
const wchar_t* const kTabs[] = {L"Album List", L"Selection Properties"};

// One Album List node: a folder (or artist, album, genre) with its children, or a track.
struct Node {
    std::wstring name;
    std::vector<uint32_t> children;
    TrackId track = kNone;
    uint32_t count = 0;  // tracks under it
    bool inserted = false;  // its children are in the tree view
};

struct Panel {
    HWND wnd = nullptr, tree = nullptr, view = nullptr, filter = nullptr, props = nullptr;
    int tab = 0;
    std::vector<Node> nodes;  // [0] is the root
    uint32_t libRevision = UINT32_MAX;
    ULONGLONG builtAt = 0;
    TrackId propsFor = kNone;
    bool propsShown = false;
    RECT tabRects[2]{};
    HFONT headerFont = nullptr;
    int fontDpi = 0;
    RECT vis[2]{};  // the toolbar's two visualization boxes
    AudioVisuals visuals;
    bool visualTimer = false;
    ULONGLONG visualAt = 0;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<ID2D1DCRenderTarget> scopeTarget;
    HDC scopeDC = nullptr;
    HBITMAP scopeBitmap = nullptr;
    HGDIOBJ scopeOldBitmap = nullptr;
    SIZE scopeSize{};
    bool on = false;
    int rowH = 0;
} fb;

void ReleaseScopeSurface() {
    fb.scopeTarget.Reset();
    if (fb.scopeDC) {
        SelectObject(fb.scopeDC, fb.scopeOldBitmap);
        DeleteObject(fb.scopeBitmap);
        DeleteDC(fb.scopeDC);
    }
    fb.scopeDC = nullptr;
    fb.scopeBitmap = nullptr;
    fb.scopeSize = {};
}

void WakeVisuals() {
    bool moving = g.playing != kNone ? !g.engine.Paused() : fb.visuals.time >= 0 || fb.visuals.Moving();
    if (!fb.visualTimer && fb.on && fb.wnd && moving) {
        fb.visualAt = 0;
        fb.visualTimer = SetTimer(fb.wnd, kVisTimer, ui::kFoobarFrameMs, nullptr) != 0;
    }
}

COLORREF Chrome() { return Ref(kFoobarChromeText); }

bool Natural(const std::wstring& a, const std::wstring& b) {
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(), (int)a.size(),
                           b.c_str(), (int)b.size(), nullptr, nullptr, 0) == CSTR_LESS_THAN;
}

std::wstring StrOr(StringPool::Handle h, const wchar_t* fallback) {
    std::wstring s = h ? g_lib.Str(h) : L"";
    return s.empty() ? fallback : s;
}

// The node path a track takes in the current view, its leaf name last.
std::vector<std::wstring> NodePath(TrackId id, int view, const std::vector<std::wstring>& roots) {
    const Track& t = g_lib[id];
    std::wstring title = t.title ? g_lib.Str(t.title) : StemOf(g_lib.Str(t.file));
    std::wstring numbered = t.trackNo ? (t.trackNo < 10 ? L"0" : L"") + std::to_wstring(t.trackNo) + L". " + title : title;
    StringPool::Handle artist = t.albumArtist ? t.albumArtist : t.artist;
    switch (view) {
        case 1: return {StrOr(artist, L"?"), StrOr(t.album, L"?"), numbered};
        case 2: return {StrOr(t.album, L"?"), numbered};
        case 3: return {StrOr(t.genre, L"?"), StrOr(artist, L"?"), StrOr(t.album, L"?"), numbered};
        default: return FolderTreePath(g_lib.Path(id), roots);
    }
}

std::vector<TrackId> LibraryTracks() {
    std::vector<TrackId> ids;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if (g_lib[id].InLibrary()) ids.push_back(id);
    return ids;
}

void CollectTracks(uint32_t node, std::vector<TrackId>& out) {
    const Node& n = fb.nodes[node];
    if (n.track != kNone) out.push_back(n.track);
    for (uint32_t c : n.children) CollectTracks(c, out);
}

void InsertChildren(HTREEITEM parent, uint32_t node) {
    Node& n = fb.nodes[node];
    if (n.inserted) return;
    n.inserted = true;
    for (uint32_t c : n.children) {
        const Node& child = fb.nodes[c];
        std::wstring label = child.track == kNone ? child.name + L" (" + std::to_wstring(child.count) + L")" : child.name;
        if (parent == TVI_ROOT && child.track == kNone && child.name.find_first_of(L"\\/") != std::wstring::npos) {
            std::wstring name = FileNameOf(child.name);
            if (!name.empty()) label = name + L" (" + std::to_wstring(child.count) + L")  [" + child.name + L"]";
        }
        TVINSERTSTRUCTW ins{};
        ins.hParent = parent;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
        ins.item.pszText = label.data();
        ins.item.lParam = (LPARAM)c;
        ins.item.cChildren = child.children.empty() ? 0 : 1;
        TreeView_InsertItem(fb.tree, &ins);
    }
}

// Rebuilds the Album List from the library, the view and the filter. Children go into the tree
// view as their parents are expanded, so a large library opens at once.
void BuildTree() {
    if (!fb.tree) return;
    fb.libRevision = g_lib.Revision();
    fb.builtAt = GetTickCount64();
    int view = std::max(0, (int)SendMessageW(fb.view, CB_GETCURSEL, 0, 0));
    wchar_t text[512] = {};
    GetWindowTextW(fb.filter, text, (int)std::size(text));
    auto words = FilterWords(text);
    fb.nodes.assign(1, Node{});
    std::vector<std::unordered_map<std::wstring, uint32_t>> index(1);
    auto roots = g.libraryFolders;
    std::wstring music = MusicFolder();
    if (!music.empty() && std::none_of(roots.begin(), roots.end(), [&](const auto& r) {
            return PathIsUnder(music, r) || CompareStringOrdinal(music.c_str(), -1, r.c_str(), -1, TRUE) == CSTR_EQUAL;
        })) roots.push_back(std::move(music));
    for (TrackId id : LibraryTracks()) {
        if (!words.empty() && !MatchesFilter(id, words)) continue;
        auto path = NodePath(id, view, roots);
        uint32_t at = 0;
        ++fb.nodes[0].count;
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            auto [it, added] = index[at].try_emplace(path[i], (uint32_t)fb.nodes.size());
            if (added) {
                fb.nodes[at].children.push_back(it->second);
                fb.nodes.push_back(Node{path[i]});
                index.emplace_back();
            }
            at = it->second;
            ++fb.nodes[at].count;
        }
        uint32_t leaf = (uint32_t)fb.nodes.size();
        fb.nodes[at].children.push_back(leaf);
        fb.nodes.push_back(Node{path.back(), {}, id, 1});
        index.emplace_back();
    }
    // Folders before files, each in natural order (foobar2000's own).
    for (Node& n : fb.nodes)
        std::stable_sort(n.children.begin(), n.children.end(), [](uint32_t a, uint32_t b) {
            const Node &x = fb.nodes[a], &y = fb.nodes[b];
            bool fx = x.track == kNone, fy = y.track == kNone;
            return fx != fy ? fx : Natural(x.name, y.name);
        });
    SendMessageW(fb.tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(fb.tree);
    InsertChildren(TVI_ROOT, 0);
    // Open small roots so an imported album shows its folders or files immediately. Large roots
    // stay lazy; their expand buttons and the Right key always work.
    if (fb.nodes[0].children.size() <= 8) {
        HTREEITEM item = TreeView_GetRoot(fb.tree);
        for (uint32_t node : fb.nodes[0].children) {
            if (fb.nodes[node].children.size() <= 256) TreeView_Expand(fb.tree, item, TVE_EXPAND);
            item = TreeView_GetNextSibling(fb.tree, item);
        }
    }
    SendMessageW(fb.tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(fb.tree, nullptr, TRUE);
}

std::vector<TrackId> TreeSelection() {
    std::vector<TrackId> out;
    HTREEITEM item = TreeView_GetSelection(fb.tree);
    if (!item) return out;
    TVITEMW tv{};
    tv.mask = TVIF_PARAM;
    tv.hItem = item;
    if (TreeView_GetItem(fb.tree, &tv) && (size_t)tv.lParam < fb.nodes.size()) CollectTracks((uint32_t)tv.lParam, out);
    return out;
}

// Selection Properties: foobar2000's Metadata, Location and General sections for the focused track.
void FillProperties(TrackId id) {
    fb.propsFor = id;
    SendMessageW(fb.props, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(fb.props);
    if (id != kNone) {
        static const struct { const wchar_t *label, *field; } rows[] = {
            {L"Metadata", nullptr},         {L"Artist name", L"artist"},     {L"Track title", L"title"},
            {L"Album title", L"album"},     {L"Date", L"date"},              {L"Genre", L"genre"},
            {L"Composer", L"composer"},     {L"Track number", L"tracknumber"}, {L"Total tracks", L"totaltracks"},
            {L"Disc number", L"discnumber"}, {L"Total discs", L"totaldiscs"}, {L"Comment", L"comment"},
            {L"Location", nullptr},         {L"File name", L"filename_ext"}, {L"Folder name", L"directory"},
            {L"File path", L"path"},        {L"General", nullptr},           {L"Duration", L"length"},
            {L"Sample rate", L"samplerate"}, {L"Channels", L"channels"},     {L"Bitrate", L"bitrate"},
            {L"Codec", L"codec"},           {L"Play count", L"play_count"},  {L"Last played", L"last_played"},
        };
        int i = 0;
        for (const auto& r : rows) {
            std::wstring value;
            if (r.field) {
                if (!wcscmp(r.field, L"directory")) value = DirectoryOf(g_lib.Path(id));
                else if (!LookupField(id, r.field, value)) continue;
                if (!wcscmp(r.field, L"samplerate")) value += L" Hz";
                if (!wcscmp(r.field, L"bitrate")) value += L" kbps";
            }
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = const_cast<wchar_t*>(r.label);
            ListView_InsertItem(fb.props, &item);
            ListView_SetItemText(fb.props, i, 1, value.data());
            ++i;
        }
    }
    SendMessageW(fb.props, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(fb.props, nullptr, TRUE);
}

TrackId FocusedTrack() {
    int row = ListView_GetNextItem(g.list, -1, LVNI_FOCUSED);
    TrackId id = row >= 0 ? RowTrack(row) : kNone;
    return id != kNone ? id : g.playing;
}

void ArrangePanel() {
    if (!fb.wnd) return;
    RECT rc;
    GetClientRect(fb.wnd, &rc);
    int W = rc.right, H = rc.bottom, tabH = S(21), foot = S(46);
    HDC dc = GetDC(fb.wnd);
    HGDIOBJ old = SelectObject(dc, g.font);
    int x = S(2);
    for (int i = 0; i < 2; ++i) {
        SIZE ext{};
        GetTextExtentPoint32W(dc, kTabs[i], (int)wcslen(kTabs[i]), &ext);
        fb.tabRects[i] = {x, S(2), x + ext.cx + S(10), S(2) + tabH};
        x = fb.tabRects[i].right;
    }
    SelectObject(dc, old);
    ReleaseDC(fb.wnd, dc);
    bool list = fb.tab == 0;
    int top = S(2) + tabH + S(1), half = (W - S(12)) / 2;
    SetWindowPos(fb.tree, nullptr, 0, top, W, H - top - foot, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(fb.props, nullptr, 0, top, W, H - top, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(fb.view, nullptr, S(6), H - S(26), half, S(200), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(fb.filter, nullptr, S(6) + half + S(6), H - S(25), W - S(18) - half, S(20), SWP_NOZORDER | SWP_NOACTIVATE);
    for (HWND h : {fb.tree, fb.view, fb.filter}) ShowWindow(h, list ? SW_SHOWNA : SW_HIDE);
    ShowWindow(fb.props, list ? SW_HIDE : SW_SHOWNA);
    InvalidateRect(fb.wnd, nullptr, TRUE);
}

void PaintPanel(HDC dc, RECT rc) {
    FillSolid(dc, rc, SurfaceColor());
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, g.font);
    // The tabs: framed, the open one joined to the panel under it.
    for (int i = 0; i < 2; ++i) {
        RECT t = fb.tabRects[i];
        bool open = i == fb.tab;
        if (!open) t.top += S(2);
        FillSolid(dc, t, open ? Ref(P().border) : SurfaceColor());
        FillSolid(dc, {t.left, t.top, t.right, t.top + 1}, Ref(kFoobarFrame));
        FillSolid(dc, {t.left, t.top, t.left + 1, t.bottom}, Ref(kFoobarFrame));
        FillSolid(dc, {t.right - 1, t.top, t.right, t.bottom}, Ref(kFoobarFrame));
        SetTextColor(dc, open ? Chrome() : Ref(0xA0A0A0));
        DrawTextW(dc, kTabs[i], -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (fb.tab == 0) {
        int H = rc.bottom, half = (rc.right - S(12)) / 2;
        RECT view{S(6), H - S(44), S(6) + half, H - S(28)}, filter{S(12) + half, H - S(44), rc.right - S(6), H - S(28)};
        SetTextColor(dc, Ref(0xC0C0C0));
        DrawTextW(dc, L"View", -1, &view, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        DrawTextW(dc, L"Filter", -1, &filter, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, old);
}

LRESULT TreeNotify(NMHDR* hdr) {
    switch (hdr->code) {
        case TVN_ITEMEXPANDINGW: {
            auto* nm = reinterpret_cast<NMTREEVIEWW*>(hdr);
            if (nm->action == TVE_EXPAND && (size_t)nm->itemNew.lParam < fb.nodes.size())
                InsertChildren(nm->itemNew.hItem, (uint32_t)nm->itemNew.lParam);
            return FALSE;
        }
        case NM_DBLCLK: {
            TVITEMW item{};
            item.mask = TVIF_PARAM;
            item.hItem = TreeView_GetSelection(fb.tree);
            if (item.hItem && TreeView_GetItem(fb.tree, &item) && (size_t)item.lParam < fb.nodes.size() &&
                fb.nodes[item.lParam].track == kNone) {
                TreeView_Expand(fb.tree, item.hItem, TVE_TOGGLE);
                return TRUE;
            }
            auto ids = TreeSelection();
            if (!ids.empty()) PlayList(ids, 0);
            return TRUE;
        }
        case TVN_KEYDOWN:
            if (reinterpret_cast<NMTVKEYDOWN*>(hdr)->wVKey == VK_RETURN) {
                auto ids = TreeSelection();
                if (!ids.empty()) PlayList(ids, 0);
                return TRUE;
            }
            return FALSE;
        case NM_RCLICK: {
            POINT pt;
            GetCursorPos(&pt);
            TVHITTESTINFO hit{};
            hit.pt = pt;
            ScreenToClient(fb.tree, &hit.pt);
            if (HTREEITEM item = TreeView_HitTest(fb.tree, &hit)) TreeView_SelectItem(fb.tree, item);
            auto ids = TreeSelection();
            HMENU m = CreatePopupMenu();
            if (!ids.empty()) {
                AppendMenuW(m, MF_STRING, 1, L"&Play");
                AppendMenuW(m, MF_STRING, 2, L"Play &next");
                AppendMenuW(m, MF_STRING, 3, L"Add to playback &queue");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            }
            AppendMenuW(m, MF_STRING, 4, L"Library &folders...");
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, fb.wnd, nullptr);
            DestroyMenu(m);
            if (cmd == 1) PlayList(ids, 0);
            else if (cmd == 2 || cmd == 3) QueueTracks(ids, cmd == 2);
            else if (cmd == 4) ShowLibraryFolders();
            return TRUE;
        }
        case TVN_GETINFOTIPW: {
            auto* tip = reinterpret_cast<NMTVGETINFOTIPW*>(hdr);
            if ((size_t)tip->lParam < fb.nodes.size())
                wcsncpy_s(tip->pszText, tip->cchTextMax, fb.nodes[tip->lParam].name.c_str(), _TRUNCATE);
            return 0;
        }
        case NM_CUSTOMDRAW: {
            // The selection in the palette's colors rather than the dark theme's gray.
            auto* cd = reinterpret_cast<NMTVCUSTOMDRAW*>(hdr);
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                bool selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
                cd->clrText = selected ? Ref(P().selectText) : TextColor();
                cd->clrTextBk = selected ? Ref(P().select) : WindowBg();
            }
            return CDRF_DODEFAULT;
        }
    }
    return 0;
}

LRESULT CALLBACK PanelProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: ArrangePanel(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            PaintPanel(dc, rc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            for (int i = 0; i < 2; ++i)
                if (PtInRect(&fb.tabRects[i], pt) && fb.tab != i) {
                    fb.tab = i;
                    if (i == 1) FillProperties(FocusedTrack());
                    ArrangePanel();
                }
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return FieldControlColor((HDC)wp);
        case WM_COMMAND:
            if (LOWORD(wp) == IDC_FB_VIEW && HIWORD(wp) == CBN_SELCHANGE) {
                IniWrite(L"Foobar", L"AlbumListView", std::to_wstring(SendMessageW(fb.view, CB_GETCURSEL, 0, 0)));
                BuildTree();
            }
            if (LOWORD(wp) == IDC_FB_FILTER && HIWORD(wp) == EN_CHANGE) SetTimer(h, kFilterTimer, 250, nullptr);
            return 0;
        case WM_TIMER:
            if (wp == kFilterTimer) {
                KillTimer(h, kFilterTimer);
                BuildTree();
            } else if (wp == kVisTimer) {
                if (g.playing != kNone && g.engine.Paused()) {
                    KillTimer(h, kVisTimer);
                    fb.visualTimer = false;
                    return 0;  // freeze the same frame as the paused playback clock
                }
                ULONGLONG now = GetTickCount64();
                double elapsed = fb.visualAt ? (now - fb.visualAt) / 1000.0 : 0.016;
                fb.visualAt = now;
                bool active = fb.on && g.playing != kNone && !g.engine.Paused();
                unsigned rate = g.engine.Rate();
                size_t frames = fb.visuals.Prepare(rate);
                if (active) active = g.engine.VisualSamples(fb.visuals.wave.data(), frames, fb.visuals.time) == frames;
                bool moving = fb.visuals.Update(active, rate, elapsed);
                for (const RECT& r : fb.vis) InvalidateRect(g.hwnd, &r, FALSE);
                if (!moving) { KillTimer(h, kVisTimer); fb.visualTimer = false; }
            }
            return 0;
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr->hwndFrom == fb.tree) return TreeNotify(hdr);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void CreatePanel() {
    if (fb.wnd) return;
    RegisterAppClass(kPanelClass, PanelProc, 0, false);
    fb.wnd = CreateWindowExW(0, kPanelClass, L"Album List", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, g.hwnd, nullptr, g.inst, nullptr);
    fb.tree = CreateWindowExW(0, WC_TREEVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT |
                              TVS_INFOTIP | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT | TVS_NOHSCROLL,
                              0, 0, 0, 0, fb.wnd, (HMENU)(INT_PTR)IDC_FB_TREE, g.inst, nullptr);
    TreeView_SetExtendedStyle(fb.tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    fb.view = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0, 0, 0, 0, fb.wnd,
                              (HMENU)(INT_PTR)IDC_FB_VIEW, g.inst, nullptr);
    for (const wchar_t* v : kViews) SendMessageW(fb.view, CB_ADDSTRING, 0, (LPARAM)v);
    SendMessageW(fb.view, CB_SETCURSEL, std::clamp(IniInt(L"Foobar", L"AlbumListView", 0), 0, (int)std::size(kViews) - 1), 0);
    fb.filter = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0,
                                fb.wnd, (HMENU)(INT_PTR)IDC_FB_FILTER, g.inst, nullptr);
    fb.props = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_NOSORTHEADER | LVS_SINGLESEL, 0, 0, 0, 0,
                               fb.wnd, (HMENU)(INT_PTR)IDC_FB_PROPS, g.inst, nullptr);
    ListView_SetExtendedListViewStyle(fb.props, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const wchar_t* heads[] = {L"Name", L"Value"};
    const int widths[] = {110, 170};
    for (int i = 0; i < 2; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(heads[i]);
        col.cx = S(widths[i]);
        ListView_InsertColumn(fb.props, i, &col);
    }
    FoobarTheme();
}

}  // namespace

void FoobarFoldersChanged() {
    fb.libRevision = UINT32_MAX;
    if (fb.on) BuildTree();
}

void FoobarTheme() {
    if (!fb.wnd) return;
    for (HWND h : {fb.tree, fb.view, fb.filter, fb.props}) SendMessageW(h, WM_SETFONT, (WPARAM)g.font, FALSE);
    ApplyControlTheme(fb.tree, true);
    ApplyControlTheme(fb.props, true);
    ApplyControlTheme(ListView_GetHeader(fb.props), true);
    SetWindowTheme(fb.view, L"DarkMode_CFD", nullptr);
    SetWindowTheme(fb.filter, L"DarkMode_CFD", nullptr);
    TreeView_SetBkColor(fb.tree, WindowBg());
    TreeView_SetTextColor(fb.tree, TextColor());
    TreeView_SetItemHeight(fb.tree, S(18));
    ListView_SetBkColor(fb.props, WindowBg());
    ListView_SetTextBkColor(fb.props, WindowBg());
    ListView_SetTextColor(fb.props, TextColor());
    ArrangePanel();
}

void FoobarSync() {
    bool on = FoobarMode() && g.hwnd && g.list;
    if (on) CreatePanel();
    if (fb.wnd) ShowWindow(fb.wnd, on ? SW_SHOWNA : SW_HIDE);
    for (HWND b : g.fbButtons) ShowWindow(b, on ? SW_SHOWNA : SW_HIDE);
    if (on == fb.on) return;
    fb.on = on;
    g.engine.SetScope(false);
    if (on) WakeVisuals();
    else {
        if (fb.wnd) KillTimer(fb.wnd, kVisTimer);
        fb.visualTimer = false;
        fb.visuals = {};
        ReleaseScopeSurface();
    }
    if (on) {
        FoobarTheme();
        if (fb.libRevision != g_lib.Revision()) BuildTree();
    }
    // Each arrangement keeps its own columns ([Columns] / [FoobarColumns]), and grouping and the
    // header rows differ, so the list is rebuilt.
    LoadColumns();
    SetupColumns();
    RefreshViewKeepingSelection();
}

void LayoutFoobar(HDWP& dwp, RECT r) {
    auto place = [&](HWND h, int x, int y, int w, int hgt) { Place(dwp, h, x, y, w, hgt); };
    auto show = [](HWND h, bool on) { ShowWindow(h, on ? SW_SHOWNA : SW_HIDE); };
    for (HWND h : {g.npTitle, g.npSub, g.elapsed, g.time, g.speaker, g.pointsChip, g.search, g.buttons[4],
                   g.buttons[5], GetDlgItem(g.hwnd, IDC_LYRICS), UpNextPanel()})
        show(h, false);
    for (HWND b : g.viewButtons) show(b, false);
    for (auto& pane : g.panes) show(pane.list, false);
    for (HWND h : {g.buttons[0], g.buttons[1], g.buttons[2], g.buttons[3], g.seek, g.volume}) show(h, true);
    g.gripRect = g.splitRect = g.artRect = {};
    // The toolbar: stop, play, pause, previous, next, random; then a visualization box, the volume,
    // the seek bar and another visualization box.
    int W = r.right - r.left, panelW = std::min(S(ui::kFoobarPanel), W / 2);
    int y = r.top + S(3), tb = S(ui::kFoobarButton);
    int x = r.left + (W >= S(900) ? panelW - S(2) : S(4));
    HWND order[] = {g.fbButtons[0], g.buttons[1], g.fbButtons[1], g.buttons[0], g.buttons[2], g.buttons[3]};
    for (HWND b : order) {
        place(b, x, y, tb, tb);
        x += tb;
    }
    int visW = S(ui::kFoobarSpectrum), right = r.right - S(2);
    x += S(10);
    fb.vis[0] = {x, y, x + visW, y + tb};
    x += visW + S(10);
    place(g.volume, x, y, S(ui::kFoobarVolume), tb);
    x += S(ui::kFoobarVolume) + S(14);
    int seekW = std::clamp(right - x - S(14) - S(ui::kFoobarScopeMin), S(60), S(ui::kFoobarSeek));
    place(g.seek, x, y, seekW, tb);
    fb.vis[1] = {x + seekW + S(14), y, right, y + tb};
    // Under it: the Album List on the left, the playlist tabs and the playlist on the right.
    int top = y + tb + S(5);
    if (fb.wnd) place(fb.wnd, r.left, top, panelW, r.bottom - top);
    int x0 = r.left + panelW + S(5), w = r.right - x0, tabH = S(22);
    show(g.tabs, true);
    place(g.tabs, x0, top, w, tabH);
    bool canvas = CanvasActive();
    if (g.libView) show(g.libView, canvas);
    show(g.list, !canvas);
    if (g.libView) place(g.libView, x0, top + tabH + S(1), w, r.bottom - top - tabH - S(1));
    place(g.list, x0, top + tabH + S(1), w, r.bottom - top - tabH - S(1));
}

void PaintFoobarVisuals(HDC dc, RECT spectrum, RECT scope, const AudioVisuals& frame) {
    for (const RECT& v : {spectrum, scope}) {
        if (IsRectEmpty(&v)) continue;
        FillSolid(dc, v, WindowBg());
        FillSolid(dc, {v.left, v.top, v.right, v.top + 1}, Ref(P().border));
        FillSolid(dc, {v.left, v.top, v.left + 1, v.bottom}, Ref(P().border));
    }
    InflateRect(&spectrum, -S(2), -S(2));
    InflateRect(&scope, -S(2), -S(2));
    const COLORREF ink = Ref(P().text);
    int sw = spectrum.right - spectrum.left, sh = spectrum.bottom - spectrum.top;
    if (sw > 0 && sh > 0) {
        for (size_t i = 0; i < frame.levels.size(); ++i) {
            int x = spectrum.left + (int)(i * sw / frame.levels.size());
            int end = spectrum.left + (int)((i + 1) * sw / frame.levels.size());
            int h = (int)std::lround(frame.levels[i] * sh);
            if (h) FillSolid(dc, {x, spectrum.bottom - h, std::max(x + 1, end - S(1)), spectrum.bottom}, ink);
            int peak = (int)std::lround(frame.peaks[i] * sh);
            if (peak > h) FillSolid(dc, {x, spectrum.bottom - peak, std::max(x + 1, end - S(1)), spectrum.bottom - peak + S(1)},
                                   Ref((P().text >> 1) & 0x7F7F7F));
        }
    }
    int w = scope.right - scope.left, h = scope.bottom - scope.top;
    if (w <= 1 || h <= 1) return;
    const auto curve = frame.Curve(w);
    // Foobar uses an antialiased, one-DIP trace in the ordinary text color, at fixed gain.
    // A software DC target shares those Direct2D rules without allocating a GPU surface.
    if (!fb.d2d) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, fb.d2d.GetAddressOf());
    if (fb.scopeSize.cx != w || fb.scopeSize.cy != h) {
        ReleaseScopeSurface();
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* bits = nullptr;
        fb.scopeDC = CreateCompatibleDC(dc);
        fb.scopeBitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (fb.scopeDC && fb.scopeBitmap) {
            fb.scopeOldBitmap = SelectObject(fb.scopeDC, fb.scopeBitmap);
            fb.scopeSize = {w, h};
        } else {
            if (fb.scopeBitmap) DeleteObject(fb.scopeBitmap);
            if (fb.scopeDC) DeleteDC(fb.scopeDC);
            fb.scopeDC = nullptr;
            fb.scopeBitmap = nullptr;
        }
    }
    if (fb.d2d && !fb.scopeTarget) {
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        fb.d2d->CreateDCRenderTarget(&props, fb.scopeTarget.GetAddressOf());
    }
    // Render into our own bitmap. Binding directly to a partial WM_PAINT DC makes Direct2D
    // inherit changing window clips and can lose the trace during GDI/PrintWindow repaints.
    RECT target{0, 0, w, h};
    if (fb.scopeTarget && fb.scopeDC && SUCCEEDED(fb.scopeTarget->BindDC(fb.scopeDC, &target))) {
        ComPtr<ID2D1SolidColorBrush> brush;
        auto color = [](Rgb c) { return D2D1::ColorF(c); };
        if (SUCCEEDED(fb.scopeTarget->CreateSolidColorBrush(color(P().text), brush.GetAddressOf()))) {
            fb.scopeTarget->BeginDraw();
            fb.scopeTarget->Clear(color(P().bg));
            for (int x = 1; x < w; ++x)
                fb.scopeTarget->DrawLine({x - 0.5f, h * (0.5f - curve[x - 1] * 0.5f)},
                                        {x + 0.5f, h * (0.5f - curve[x] * 0.5f)}, brush.Get(), (float)S(1));
            HRESULT result = fb.scopeTarget->EndDraw();
            GdiFlush();
            if (SUCCEEDED(result)) {
                BitBlt(dc, scope.left, scope.top, w, h, fb.scopeDC, 0, 0, SRCCOPY);
                GdiFlush();
                return;
            }
            fb.scopeTarget.Reset();
        }
    }
    std::vector<POINT> line(w);
    for (int x = 0; x < w; ++x) {
        int y = scope.top + (int)std::lround(h * (0.5f - curve[x] * 0.5f));
        line[x] = {scope.left + x, y};
    }
    int saved = SaveDC(dc);
    IntersectClipRect(dc, scope.left, scope.top, scope.right, scope.bottom);
    HGDIOBJ old = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, ink);
    Polyline(dc, line.data(), (int)line.size());
    SelectObject(dc, old);
    RestoreDC(dc, saved);
}

void PaintFoobarChrome(HDC dc) {
    PaintFoobarVisuals(dc, fb.vis[0], fb.vis[1], fb.visuals);
}

RECT FoobarSliderTrack(HWND bar) {
    RECT r{};
    GetClientRect(bar, &r);
    int inset = S(5) + S(ui::kFoobarThumb) / 2, cy = (r.top + r.bottom) / 2;
    return {r.left + inset, cy - S(2), std::max(r.left + inset + 1, r.right - inset), cy + S(2)};
}

void DrawFoobarButton(const DRAWITEMSTRUCT* di) {
    RECT r = di->rcItem;
    bool pressed = (di->itemState & ODS_SELECTED) != 0, hot = di->hwndItem == g_hotButton;
    FillSolid(di->hDC, r, pressed ? Ref(P().border) : SurfaceColor());
    if (pressed || hot) {
        HGDIOBJ brush = SelectObject(di->hDC, GetStockObject(NULL_BRUSH));
        HGDIOBJ pen = SelectObject(di->hDC, GetStockObject(DC_PEN));
        SetDCPenColor(di->hDC, Ref(P().border));
        Rectangle(di->hDC, r.left, r.top, r.right, r.bottom);
        SelectObject(di->hDC, brush);
        SelectObject(di->hDC, pen);
    }
    int id = GetDlgCtrlID(di->hwndItem), cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    COLORREF ink = Ref(id == IDC_SHUFFLE && g.shuffle ? P().accent : TextOn(P().surface));
    auto fill = [&](int x, int y, int w, int h) { FillSolid(di->hDC, {cx + S(x), cy + S(y), cx + S(x + w), cy + S(y + h)}, ink); };
    auto triangle = [&](int x, bool left) {
        POINT p[] = {{cx + S(x), cy - S(5)}, {cx + S(x + (left ? -7 : 7)), cy}, {cx + S(x), cy + S(5)}};
        HGDIOBJ brush = SelectObject(di->hDC, GetStockObject(DC_BRUSH));
        HGDIOBJ pen = SelectObject(di->hDC, GetStockObject(NULL_PEN));
        SetDCBrushColor(di->hDC, ink);
        Polygon(di->hDC, p, 3);
        SelectObject(di->hDC, brush);
        SelectObject(di->hDC, pen);
    };
    if (id == IDC_STOP) fill(-4, -4, 9, 9);
    else if (id == IDC_PLAY) triangle(-3, false);
    else if (id == IDC_PAUSE) { fill(-4, -5, 3, 10); fill(2, -5, 3, 10); }
    else if (id == IDC_PREV) { fill(-5, -5, 2, 10); triangle(4, true); }
    else if (id == IDC_NEXT) { fill(4, -5, 2, 10); triangle(-4, false); }
    else {
        HGDIOBJ pen = SelectObject(di->hDC, GetStockObject(DC_PEN));
        SetDCPenColor(di->hDC, ink);
        POINT a[] = {{cx - S(5), cy - S(4)}, {cx - S(2), cy - S(4)}, {cx + S(2), cy + S(4)}, {cx + S(5), cy + S(4)}};
        POINT b[] = {{cx - S(5), cy + S(4)}, {cx - S(2), cy + S(4)}, {cx + S(2), cy - S(4)}, {cx + S(5), cy - S(4)}};
        Polyline(di->hDC, a, 4);
        Polyline(di->hDC, b, 4);
        SelectObject(di->hDC, pen);
    }
}

void FoobarTick() {
    if (!fb.on || !fb.wnd) return;
    WakeVisuals();
    if (fb.tab == 1) {
        TrackId id = FocusedTrack();
        if (id != fb.propsFor) FillProperties(id);
    }
    // The library changed (a scan, an import): rebuilt at most every few seconds.
    if (fb.libRevision != g_lib.Revision() && GetTickCount64() - fb.builtAt > 3000) BuildTree();
}

// ---------------------------------------------------------------------------
// The playlist

namespace {

HFONT HeaderFont() {
    if (!fb.headerFont || fb.fontDpi != (int)g.dpi) {
        if (fb.headerFont) DeleteObject(fb.headerFont);
        LOGFONTW lf{};
        GetObjectW(g.font, sizeof(lf), &lf);
        lf.lfHeight = lf.lfHeight * 5 / 4;  // foobar2000's group headers are a size up
        fb.headerFont = CreateFontIndirectW(&lf);
        fb.fontDpi = (int)g.dpi;
    }
    return fb.headerFont;
}

int RowHeight() {
    RECT rc{};
    if (ListView_GetItemCount(g.list) > 0 && ListView_GetItemRect(g.list, 0, &rc, LVIR_BOUNDS) && rc.bottom > rc.top)
        fb.rowH = rc.bottom - rc.top;
    return fb.rowH > 0 ? fb.rowH : S(19);
}

int CoverSize() {
    int sub = SubitemOf(kColCover);
    return sub < 0 ? 0 : std::max(0, ListView_GetColumnWidth(g.list, sub) - S(8));
}

}  // namespace

uint32_t FoobarCoverRows() {
    int size = CoverSize(), h = RowHeight();
    return size <= 0 ? 0 : (uint32_t)((size + S(6) + h - 1) / h);
}

void DrawFoobarHeader(HDC dc, int group, RECT rc) {
    const TrackGroup& gr = g.groups[group];
    FillSolid(dc, rc, WindowBg());
    std::wstring year = gr.date ? g_lib.Str(gr.date).substr(0, 4) : L"";
    std::wstring text = StrOr(gr.artist, L"?") + L" - " + (year.empty() ? L"" : L"[" + year + L"] ") + StrOr(gr.album, L"?");
    RECT client;
    GetClientRect(g.list, &client);
    RECT tr{rc.left + S(4), rc.top, std::min(rc.right, client.right) - S(4), rc.bottom};
    HGDIOBJ old = SelectObject(dc, HeaderFont());
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, TextColor());
    RECT measure = tr;
    DrawTextW(dc, text.c_str(), (int)text.size(), &measure, DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
    DrawTextW(dc, text.c_str(), (int)text.size(), &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    // Foobar's group rule is half the text's intensity on the black list.
    int lx = std::min(measure.right, tr.right) + S(8), ly = (rc.top + rc.bottom) / 2 + S(1);
    if (lx < tr.right) FillSolid(dc, {lx, ly, tr.right, ly + 1}, Ref((P().text >> 1) & 0x7F7F7F));
}

// The slice of a group's cover that falls in `row`: the cover starts at the group's first track
// and runs down its column, so each row paints its own part and partial repaints line up.
void DrawFoobarCover(HDC dc, int row, RECT rc) {
    int k = GroupOfRow(row), sub = SubitemOf(kColCover);
    if (k < 0 || sub < 0) return;
    const TrackGroup& gr = g.groups[k];
    RECT col{};
    if (!ListView_GetSubItemRect(g.list, row, sub, LVIR_BOUNDS, &col)) return;
    col.top = rc.top;
    col.bottom = rc.bottom;
    FillSolid(dc, col, WindowBg());  // the cover's column is never highlighted
    int h = rc.bottom - rc.top, size = CoverSize();
    if (size <= 0) return;
    int first = (int)gr.firstRow + HeaderRows(), top = rc.top - (row - first) * h + S(2);
    RECT cover{col.left + S(4), top, col.left + S(4) + size, top + size};
    RECT clip;
    if (!IntersectRect(&clip, &cover, &col)) return;
    const ArtImage* art = GroupCover(gr, size);
    if (!art || art->Empty()) return;
    int save = SaveDC(dc);
    IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);
    DrawCoverArt(dc, cover, *art);
    RestoreDC(dc, save);
}

}  // namespace app
