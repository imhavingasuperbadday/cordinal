// The playlist model behind the tab strip: stable identities, the tab text, remembered selection /
// search / scroll per playlist, close / reopen / duplicate / reorder, and the inline tab rename.
// Every stored index into g.playlists (active, playing, shuffle, resume, token lookups) is kept
// consistent here whenever the vector changes.
#include "app.h"

namespace app {

void EnsurePlaylistIdentities() {
    for (auto& pl : g.playlists) if (!pl.identity) pl.identity = g.nextPlaylistIdentity++;
}

// Repaints the tab captions from g.playlists: name, an auto-playlist marker and, when the layout
// asks for it, the item count.
// WM_SETREDRAW brackets the updates so the control does not flicker while the count changes.
void RefreshTabs() {
    EnsurePlaylistIdentities();
    if (!g.tabs) return;
    SendMessageW(g.tabs, WM_SETREDRAW, FALSE, 0);
    while (TabCtrl_GetItemCount(g.tabs) > (int)g.playlists.size()) TabCtrl_DeleteItem(g.tabs, TabCtrl_GetItemCount(g.tabs) - 1);
    for (int i = 0; i < (int)g.playlists.size(); ++i) {
        std::wstring name = (g.playlists[i].kind == PlaylistKind::Auto ? L"\x26A1 " : L"") + g.playlists[i].name;
        if (g.layout.tabCounts) name += L" (" + std::to_wstring(g.playlists[i].items.size()) + L")";
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = name.data();
        if (i < TabCtrl_GetItemCount(g.tabs)) TabCtrl_SetItem(g.tabs, i, &item);
        else TabCtrl_InsertItem(g.tabs, i, &item);
    }
    TabCtrl_SetCurSel(g.tabs, RadioActive() ? -1 : g.active);
    if (!RadioActive()) TabCtrl_SetCurFocus(g.tabs, g.active);
    SendMessageW(g.tabs, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.tabs, nullptr, FALSE);
}

void UpdateSearchCue() {
    if (!g.search || g.playlists.empty()) return;
    if (RadioActive()) { SendMessageW(g.search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Name, call sign or FM/AM number"); return; }
    const Playlist& pl = Active();
    std::wstring cue = (pl.kind == PlaylistKind::Library ? L"Search library" : L"Search " + pl.name) + L" (Ctrl+F)";
    SendMessageW(g.search, EM_SETCUEBANNER, TRUE, (LPARAM)cue.c_str());
}

// Captures the current selection, focus, top row and search text so a playlist switch can restore
// it. Editable playlists remember occurrence indices; views (Library / Inbox / Auto) remember
// stable TrackIds. The album canvas keeps its own scroll / selection state.
PlaylistPosition RememberPosition() {
    PlaylistPosition position;
    position.search = g.filter;
    if (CanvasActive()) {
        position.selected = SelectedTracks();
        LibViewRememberPosition(position);
    } else {
        for (int index : SelectedIndices())
            position.selected.push_back(Active().Editable() ? (uint32_t)index : Active().items[index]);
        auto trackAt = [](int row) {
            int pos = ViewPosNear(row);
            if (pos < 0) return kNone;
            return Active().Editable() ? g.view[pos] : Active().items[g.view[pos]];
        };
        position.focus = trackAt(ListView_GetNextItem(g.list, -1, LVNI_FOCUSED));
        position.top = trackAt(ListView_GetTopIndex(g.list));
    }
    return position;
}

// Reapplies a remembered PlaylistPosition: selection, focus and scroll. Selection keys are stable
// occurrence indices for editable playlists; the top row drops back one header row when it lands on an album's
// first track so the group header stays in view.
void RestorePosition(const PlaylistPosition& position) {
    if (CanvasActive()) {
        LibViewRestorePosition(position);
        return;
    }
    ClearSelection();
    std::unordered_set<TrackId> selected(position.selected.begin(), position.selected.end());
    int top = 0;
    for (int pos = 0; pos < (int)g.view.size(); ++pos) {
        uint32_t key = Active().Editable() ? g.view[pos] : Active().items[g.view[pos]];
        int row = RowOfViewPos(pos);
        UINT state = (selected.count(key) ? LVIS_SELECTED : 0) | (key == position.focus ? LVIS_FOCUSED : 0);
        if (state) ListView_SetItemState(g.list, row, state, state);
        // An album's first track brings its header along.
        if (key == position.top) top = HeaderGroupOfRow(row - 1) >= 0 ? row - 2 : row;
    }
    RECT r{};
    if (!g.view.empty() && ListView_GetItemRect(g.list, 0, &r, LVIR_BOUNDS))
        ListView_Scroll(g.list, 0, (top - ListView_GetTopIndex(g.list)) * (r.bottom - r.top));
}

// Switches the active playlist: persists the outgoing position, restores the incoming one, and
// rebuilds the view and sort arrow. restoringView guards the search box so the programmatic text
// change is not treated as typing.
void ActivatePlaylist(int index) {
    if (index < 0 || index >= (int)g.playlists.size()) return;
    bool wasRadio = RadioActive();
    LeaveRadio();
    EndRename(true);
    FlushSearch();
    bool contentFocused = GetFocus() == g.list || GetFocus() == g.libView;
    if (index != g.active && !wasRadio) Active().position = RememberPosition();
    g.active = index;
    TabCtrl_SetCurSel(g.tabs, index);
    const Playlist& pl = g.playlists[index];
    UpdateSearchCue();
    g.sortColumn = pl.sortColumn;
    g.sortAscending = pl.sortAscending;
    SetSortArrow(g.sortColumn, g.sortAscending);
    g.filter = pl.position.search;
    g.restoringView = true;
    SetTextIfChanged(g.search, g.filter);
    g.restoringView = false;
    ClearSelection();
    RebuildView();
    InvalidateRect(g.tabs, nullptr, TRUE);
    Layout();  // the Library tab has its own browser and view buttons
    RestorePosition(pl.position);
    if (contentFocused) SetFocus(CanvasActive() ? g.libView : g.list);
}

std::wstring UniquePlaylistName(const std::wstring& base) {
    return ::UniquePlaylistName(g.playlists, base);
}

void CapturePlaylistPosition() {
    if (RadioActive()) return;
    FlushSearch();
    if (!g.playlists.empty()) Active().position = RememberPosition();
}

void RestorePlaylistPosition() {
    if (g.playlists.empty()) return;
    g.filter = Active().position.search;
    g.sortColumn = Active().sortColumn;
    g.sortAscending = Active().sortAscending;
    g.restoringView = true;
    SetTextIfChanged(g.search, g.filter);
    g.restoringView = false;
    SetSortArrow(g.sortColumn, g.sortAscending);
    RebuildView();
    RestorePosition(Active().position);
}

void NewPlaylist(const std::wstring& name) {
    EndRename(true);
    g.playlists.push_back(Playlist{UniquePlaylistName(name), {}});
    RefreshTabs();
    ActivatePlaylist((int)g.playlists.size() - 1);
    g.dirty = true;
}

// Inserts a playlist at `at`, keeping every stored playlist index pointing at the same list.
void InsertPlaylist(int at, Playlist pl) {
    EndRename(true);
    if (!pl.identity) pl.identity = g.nextPlaylistIdentity++;
    g.playlists.insert(g.playlists.begin() + at, std::move(pl));
    for (int* v : {&g.active, &g.playingPl, &g.shufflePl, &g.resumePl})
        if (*v >= at) ++*v;
    for (auto& [token, info] : g.tokens)
        if (info.pl >= at) ++info.pl;
    RefreshTabs();
    g.dirty = true;
}

// Rebuilds a view after the library changed, keeping the selection, focus and scroll position.
void RefreshViewKeepingSelection() {
    PlaylistPosition position = RememberPosition();
    RebuildView();
    RestorePosition(position);
}

// Removes a playlist without asking, keeping stored playlist indices pointing at the same lists.
void ErasePlaylist(int index) {
    EndRename(true);
    // Stop any in-flight import for this playlist and drop its queued requests; importMx guards
    // these fields, which are shared with the importer thread.
    {
        std::lock_guard<std::mutex> lock(g.importMx);
        uint64_t identity = g.playlists[index].identity;
        if (g.importTarget == identity) g.importCancel = true;
        std::erase_if(g.importQueue, [&](const ImportRequest& request) { return request.playlistIdentity == identity; });
    }
    g.playlists.erase(g.playlists.begin() + index);
    auto shift = [&](int& v) {
        if (v == index) v = -1;
        else if (v > index) --v;
    };
    shift(g.playingPl);
    shift(g.shufflePl);
    shift(g.resumePl);
    for (auto& [token, info] : g.tokens) shift(info.pl);
    if (g.active > index || g.active >= (int)g.playlists.size()) --g.active;
    RefreshTabs();
    g.dirty = true;
}

// Closes a tab into the reopen stack, which is bounded by count (8) and total items (~200k) so
// closed playlists cannot grow memory without limit. Index 0 (the Library) and the Inbox cannot close.
void RemovePlaylist(int index) {
    if (index <= 0 || index >= (int)g.playlists.size() || g.playlists[index].kind == PlaylistKind::Inbox) return;
    EndRename(true);
    CapturePlaylistPosition();
    g.closedPlaylists.push_back({std::move(g.playlists[index]), index});
    size_t retained = 0;
    for (const auto& closed : g.closedPlaylists) retained += closed.playlist.items.size();
    while (g.closedPlaylists.size() > 8 || (retained > 200000 && g.closedPlaylists.size() > 1)) {
        retained -= g.closedPlaylists.front().playlist.items.size();
        g.closedPlaylists.pop_front();
    }
    // Keep the identity until ErasePlaylist has cancelled its pending imports.
    g.playlists[index].identity = g.closedPlaylists.back().playlist.identity;
    ErasePlaylist(index);
    RestorePlaylistPosition();
    RefreshNext();
    g.compactPending = true;
    ShowMessage(L"Playlist closed. Ctrl+Shift+T reopens it.");
}

// Moves a tab left or right. The Library (index 0) never moves, and every stored playlist index is
// remapped so playback and the queue keep pointing at the same lists.
void ReorderPlaylist(int from, int to) {
    if (from <= 0 || to <= 0) return;  // the Library remains the first tab
    EndRename(true);
    CapturePlaylistPosition();
    if (!MovePlaylist(g.playlists, from, to)) return;
    for (int* index : {&g.active, &g.playingPl, &g.shufflePl, &g.resumePl}) *index = RemapPlaylistIndex(*index, from, to);
    for (auto& [token, info] : g.tokens) info.pl = RemapPlaylistIndex(info.pl, from, to);
    RefreshTabs();
    g.dirty = true;
}

// Copies the active playlist into a new static playlist named "... copy", inserted after it. Auto
// playlists become static copies: their rules cannot be duplicated verbatim, so they are dropped.
void DuplicatePlaylist() {
    EndRename(true);
    CapturePlaylistPosition();
    Playlist copy = Active();
    if (copy.IsView()) {
        std::unordered_map<TrackId, uint32_t> positions;
        positions.reserve(copy.items.size());
        for (uint32_t i = 0; i < copy.items.size(); ++i) positions.emplace(copy.items[i], i);
        auto remap = [&](TrackId& id) {
            auto it = positions.find(id);
            id = it == positions.end() ? kNone : it->second;
        };
        for (auto& id : copy.position.selected) remap(id);
        for (TrackId* id : {&copy.position.focus, &copy.position.top, &copy.position.expanded}) remap(*id);
    }
    copy.name = UniquePlaylistName(copy.name + L" copy");
    copy.kind = PlaylistKind::Static;
    SanitizePlaylistPosition(copy);
    copy.rules.clear();
    copy.identity = g.nextPlaylistIdentity++;
    int target = g.active + 1;
    InsertPlaylist(target, std::move(copy));
    ActivatePlaylist(target);
}

// Reopens the most recently closed playlist at its old index. Its stored identity is kept so any
// token or pending import still referring to it reattaches to the restored tab.
void ReopenPlaylist() {
    if (g.closedPlaylists.empty()) return;
    EndRename(true);
    CapturePlaylistPosition();
    auto closed = std::move(g.closedPlaylists.back());
    g.closedPlaylists.pop_back();
    int target = std::clamp(closed.index, 1, (int)g.playlists.size());
    closed.playlist.name = UniquePlaylistName(closed.playlist.name);
    uint64_t identity = closed.playlist.identity;
    InsertPlaylist(target, std::move(closed.playlist));
    for (auto& [token, info] : g.tokens) if (info.playlistIdentity == identity) info.pl = target;
    if (auto playing = g.tokens.find(g.playingToken); playing != g.tokens.end()) g.playingPl = playing->second.pl;
    ActivatePlaylist(target);
    RefreshNext();
}

// Inline rename:
// Inline rename: an edit box over the tab.
int g_renameIndex = -1;

// Closes the rename edit. On commit it trims surrounding whitespace, de-duplicates the name
// case-insensitively (a unique suffix is appended if needed) and marks the library dirty, then
// rebuilds the tabs.
void EndRename(bool commit) {
    if (!g.rename) return;
    HWND edit = g.rename;
    g.rename = nullptr;
    if (commit && g_renameIndex >= 0 && g_renameIndex < (int)g.playlists.size()) {
        wchar_t buf[129];
        GetWindowTextW(edit, buf, (int)std::size(buf));
        std::wstring name = buf;
        size_t b = name.find_first_not_of(L" \t"), e = name.find_last_not_of(L" \t");
        if (b != std::wstring::npos) {
            std::wstring clean = CleanPlaylistName(name.substr(b, e - b + 1));
            bool duplicate = false;
            for (int i = 0; i < (int)g.playlists.size(); ++i)
                if (i != g_renameIndex && CompareStringOrdinal(g.playlists[i].name.c_str(), -1, clean.c_str(), -1, TRUE) == CSTR_EQUAL)
                    duplicate = true;
            g.playlists[g_renameIndex].name = duplicate ? UniquePlaylistName(clean) : clean;
            g.dirty = true;
        }
    }
    DestroyWindow(edit);
    RefreshTabs();
    UpdateSearchCue();
    SetFocus(g.list);
}

// Keyboard handling for the rename box: swallows the keys so they do not beep, commits on Enter and
// cancels on Escape. Losing focus commits too, by posting the same command the main loop handles.
LRESULT CALLBACK RenameSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        EndRename(wp == VK_RETURN);
        return 0;
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 27 || wp == 1)) return 0;
    if (msg == WM_KILLFOCUS) PostMessageW(g.hwnd, WM_COMMAND, MAKEWPARAM(IDC_RENAME, EN_KILLFOCUS), 0);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, RenameSubclass, 0);
    return DefSubclassProc(h, msg, wp, lp);
}

// Edits the name in place, preselecting it. Library and Inbox tabs cannot be renamed.
void BeginRename(int index) {
    if (index < 0 || index >= (int)g.playlists.size()) return;
    PlaylistKind kind = g.playlists[index].kind;
    if (kind == PlaylistKind::Library || kind == PlaylistKind::Inbox) return;
    EndRename(true);
    RECT r{};
    if (!TabCtrl_GetItemRect(g.tabs, index, &r)) return;
    g_renameIndex = index;
    MapWindowPoints(g.tabs, g.hwnd, reinterpret_cast<POINT*>(&r), 2);
    r.left += S(6);
    r.right -= S(index > 0 ? 24 : 6);
    r.bottom -= S(3);  // leave the tab's accent underline visible
    int width = std::max<int>(1, r.right - r.left);
    g.rename = CreateWindowExW(0, WC_EDITW, g.playlists[index].name.c_str(),
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_CENTER, r.left, r.top, width, r.bottom - r.top, g.hwnd,
                               (HMENU)(INT_PTR)IDC_RENAME, g.inst, nullptr);
    SendMessageW(g.rename, EM_SETLIMITTEXT, 128, 0);
    SendMessageW(g.rename, WM_SETFONT, (WPARAM)g.font, TRUE);
    SetWindowPos(g.rename, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowSubclass(g.rename, RenameSubclass, 0, 0);
    SendMessageW(g.rename, EM_SETSEL, 0, -1);
    SetFocus(g.rename);
}


}  // namespace app
