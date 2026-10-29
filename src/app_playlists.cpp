// Playlist tabs, the playlist list view, filtering, sorting and editing.
//
// g.view is the visible rows: indices into the active playlist's items after the search filter.
// The list is owner-data, so only visible cells are formatted, on demand and cached. View
// playlists (Library/Inbox/Auto) are computed from the library; static ones own their items.
// Album grouping adds two header rows per album, so a list row is not a view position.
#include "app.h"
#include "archive.h"

namespace app {

void ClearSelection() { ListView_SetItemState(g.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED); }

bool MatchesFilter(TrackId id, const std::vector<std::wstring>& words) {
    return MatchesTrack(g_lib, id, words);
}

PlaylistPosition RememberPosition();
void RestorePosition(const PlaylistPosition& position);

// Drops the bounded formatted-cell cache (also called when it fills up).
void ClearCellCache() {
    g.cellCache.clear();
    g.cellCacheBytes = 0;
}

void FlushSearch() {
    if (!g.searchPending) return;
    KillTimer(g.hwnd, kSearchTimer);
    g.searchPending = false;
    RebuildView(true);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// View playlists (Library, Inbox, Auto): computed from the library, rebuilt lazily when the
// library's revision moves on.

StringRanks::StringRanks(std::vector<StringPool::Handle> handles) {
    SortRecords(handles.begin(), handles.end());
    handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
    // One contiguous UTF-16 blob with an offset per value replaces a std::wstring (and its
    // heap block) per value; sorting compares pointers into the blob, so the order is the
    // same: empty values last, otherwise natural, case-insensitive order.
    std::wstring blob;
    std::vector<uint32_t> offsets(handles.size());
    size_t characters = 0;
    for (size_t i = 0; i < handles.size(); ++i) {
        offsets[i] = (uint32_t)characters;
        auto text = g_lib.View(handles[i]);
        characters += text.empty() ? 1 :
            (size_t)MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0) + 1;
    }
    // Size the blob once and convert directly into it, avoiding a temporary string per tag and
    // the old/new allocations that overlapped whenever the blob grew.
    blob.resize(characters, L'\0');
    for (size_t i = 0; i < handles.size(); ++i) {
        auto text = g_lib.View(handles[i]);
        const size_t end = i + 1 < offsets.size() ? offsets[i + 1] : characters;
        if (!text.empty()) MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(),
            blob.data() + offsets[i], (int)(end - offsets[i] - 1));
    }
    std::vector<uint32_t> order(handles.size());
    for (uint32_t i = 0; i < (uint32_t)order.size(); ++i) order[i] = i;
    const wchar_t* base = blob.c_str();
    auto length = [&](uint32_t i) {
        return ((i + 1 < offsets.size() ? offsets[i + 1] : (uint32_t)blob.size()) - offsets[i]) - 1;
    };
    SortRecords(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const wchar_t* sa = base + offsets[a], *sb = base + offsets[b];
        bool emptyA = length(a) == 0, emptyB = length(b) == 0;
        if (emptyA != emptyB) return emptyB;  // empty values sort last
        return StrCmpLogicalW(sa, sb) < 0;
    });
    // Handles are already sorted: scatter the ranks back to those slots instead of sorting
    // another permutation of all the handle/rank pairs.
    rank_.resize(handles.size());
    for (size_t i = 0; i < handles.size(); ++i) rank_[i].first = handles[i];
    for (uint32_t i = 0; i < (uint32_t)order.size(); ++i) rank_[order[i]].second = i;
}

uint32_t StringRanks::operator()(StringPool::Handle h) const {
    auto it = std::lower_bound(rank_.begin(), rank_.end(), h, [](const auto& entry, auto handle) {
        return entry.first < handle;
    });
    return it == rank_.end() || it->first != h ? UINT32_MAX : it->second;
}

// Album artist, year, album, disc, track, file name: the order MusicBee and foobar2000 browse in.
void SortLibraryOrder(std::vector<TrackId>& items) {
    std::vector<StringPool::Handle> strings;
    size_t sorted = 0;
    auto compact = [&] {
        if (strings.size() <= sorted) return;
        SortRecords(strings.begin() + (ptrdiff_t)sorted, strings.end());
        strings.erase(std::unique(strings.begin() + (ptrdiff_t)sorted, strings.end()), strings.end());
        std::inplace_merge(strings.begin(), strings.begin() + (ptrdiff_t)sorted, strings.end());
        strings.erase(std::unique(strings.begin(), strings.end()), strings.end());
        sorted = strings.size();
    };
    for (TrackId id : items) {
        const Track& t = g_lib[id];
        for (auto h : {t.albumArtist ? t.albumArtist : t.artist, t.date, t.album}) strings.push_back(h);
        if (strings.size() - sorted >= 8192) compact();
    }
    compact();
    StringRanks rank(std::move(strings));
    struct Key {
        uint32_t artist, date, album, discTrack;
        TrackId id;
    };
    std::vector<Key> keys;
    keys.reserve(items.size());
    for (TrackId id : items) {
        const Track& t = g_lib[id];
        keys.push_back({rank(t.albumArtist ? t.albumArtist : t.artist), rank(t.date), rank(t.album),
                        ((uint32_t)t.disc << 16) | t.trackNo, id});
    }
    SortRecords(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        return std::tie(a.artist, a.date, a.album, a.discTrack, a.id) <
               std::tie(b.artist, b.date, b.album, b.discTrack, b.id);
    });
    // File names only break ties when artist, date, album, disc and track number are all equal.
    for (size_t i = 0; i < keys.size();) {
        size_t j = i + 1;
        while (j < keys.size() &&
               std::tie(keys[j].artist, keys[j].date, keys[j].album, keys[j].discTrack) ==
                   std::tie(keys[i].artist, keys[i].date, keys[i].album, keys[i].discTrack))
            ++j;
        if (j - i > 1) {
            SortRecords(keys.begin() + (ptrdiff_t)i, keys.begin() + (ptrdiff_t)j, [](const Key& a, const Key& b) {
                std::wstring fa = g_lib.Str(g_lib[a.id].file), fb = g_lib.Str(g_lib[b.id].file);
                if (fa.empty() != fb.empty()) return fb.empty();
                int c = StrCmpLogicalW(fa.c_str(), fb.c_str());
                return c != 0 ? c < 0 : a.id < b.id;
            });
        }
        i = j;
    }
    for (size_t i = 0; i < items.size(); ++i) items[i] = keys[i].id;
}

// Sorts playlist items by a visible column. The key of a row is the column's formatted text
// (numeric columns are compared as numbers instead), so equal keys keep their previous order.
// `mapping`, when given, maps each old position to its new one so stored positions can be remapped.
void SortItems(std::vector<TrackId>& items, int column, bool ascending, std::vector<uint32_t>* mapping) {
    struct Entry {
        uint32_t offset, order;
        TrackId id;
    };
    const bool numeric = column == 1 || column == 6 || column == 13;
    std::vector<wchar_t> chars;
    if (!numeric) chars.reserve(items.size() * 16);
    std::vector<Entry> keyed;
    keyed.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        TrackId id = items[i];
        uint32_t off = (uint32_t)chars.size();
        if (!numeric) {
            std::wstring s = FormatTitle(ColumnAt(column).sort, Lookup(id, false));
            chars.insert(chars.end(), s.c_str(), s.c_str() + s.size() + 1);
        }
        keyed.push_back({off, (uint32_t)i, id});
    }
    SortRecords(keyed.begin(), keyed.end(), [&](const Entry& a, const Entry& b) {
        int c;
        if (numeric) {
            const Track& at = g_lib[a.id];
            const Track& bt = g_lib[b.id];
            uint32_t av = column == 1 ? at.trackNo ? ((uint32_t)at.disc << 16) | at.trackNo : 0 : column == 6 ? at.durationMs : at.disc;
            uint32_t bv = column == 1 ? bt.trackNo ? ((uint32_t)bt.disc << 16) | bt.trackNo : 0 : column == 6 ? bt.durationMs : bt.disc;
            if (!av != !bv) return av != 0;
            c = av < bv ? -1 : av > bv ? 1 : 0;
        } else c = StrCmpLogicalW(chars.data() + a.offset, chars.data() + b.offset);
        if (c != 0) return ascending ? c < 0 : c > 0;
        return a.order < b.order;
    });
    if (mapping) mapping->resize(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        items[i] = keyed[i].id;
        if (mapping) (*mapping)[keyed[i].order] = (uint32_t)i;
    }
}

// Fills a view playlist's items from the library, at most once per library revision (tracked by
// builtRevision). Static playlists keep their own items and are skipped; an auto-playlist is
// evaluated here.
void RebuildViewItems(Playlist& pl) {
    if (!pl.IsView() || pl.builtRevision == g_lib.Revision()) return;
    pl.builtRevision = g_lib.Revision();
    // Keep a manually arranged view in that order as its membership changes. New matches go
    // at the end in the view's normal order; its rules and limits still determine membership.
    bool manual = pl.sortColumn == -2;
    std::vector<TrackId> previous;
    if (manual) previous = std::move(pl.items);
    pl.items.clear();
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        const Track& t = g_lib[id];
        bool in = pl.kind == PlaylistKind::Library ? t.InLibrary()
                  : pl.kind == PlaylistKind::Inbox ? (t.flags & (Track::kInbox | Track::kMissing)) == Track::kInbox
                                                   : false;
        if (in) pl.items.push_back(id);
    }
    bool ordered = false;
    if (pl.kind == PlaylistKind::Auto) {
        // Random picks stay put for the session (a fresh shuffle per start), per playlist.
        static const uint32_t session = GetTickCount();
        AutoPlaylist a;
        if (ParseAutoPlaylist(pl.rules, a)) EvaluateAutoPlaylist(a, pl.items, session ^ (uint32_t)std::hash<std::wstring>()(pl.name));
        ordered = a.ordered;
    }
    if (manual) {
        std::unordered_set<TrackId> remaining(pl.items.begin(), pl.items.end());
        std::vector<TrackId> userOrder, added;
        userOrder.reserve(pl.items.size());
        for (TrackId id : previous)
            if (remaining.erase(id)) userOrder.push_back(id);
        for (TrackId id : pl.items)
            if (remaining.count(id)) added.push_back(id);
        if (!ordered) SortLibraryOrder(added);
        userOrder.insert(userOrder.end(), added.begin(), added.end());
        pl.items = std::move(userOrder);
    } else if (pl.sortColumn > 0) {
        SortItems(pl.items, pl.sortColumn, pl.sortAscending);
    } else if (!ordered) {
        SortLibraryOrder(pl.items);
    }
    if (pl.kind == PlaylistKind::Library) FilterLibrary(pl.items);
    pl.items.shrink_to_fit();
}

// Exactly one Library playlist, always the first tab; at most one Inbox and Now Playing.
void EnsureSpecialPlaylists() {
    auto& pls = g.playlists;
    for (PlaylistKind kind : {PlaylistKind::Library, PlaylistKind::Inbox, PlaylistKind::NowPlaying}) {
        bool seen = false;
        for (size_t i = 0; i < pls.size();)
            if (pls[i].kind == kind && seen) pls.erase(pls.begin() + i);
            else seen |= pls[i++].kind == kind;
    }
    auto lib = std::find_if(pls.begin(), pls.end(), [](const Playlist& p) { return p.kind == PlaylistKind::Library; });
    Playlist library = lib != pls.end() ? std::move(*lib) : Playlist{L"Library", {}, PlaylistKind::Library};
    if (lib != pls.end()) pls.erase(lib);
    pls.insert(pls.begin(), std::move(library));
}

int FindPlaylist(PlaylistKind kind) {
    for (int i = 0; i < (int)g.playlists.size(); ++i)
        if (g.playlists[i].kind == kind) return i;
    return -1;
}

// Rebuilds the visible rows for the active playlist and hands them to the list. Redraw is
// suppressed so the view doesn't flash; when preserveSelection is set, the selection, focus and
// scroll are remembered and restored around the rebuild.
void RebuildView(bool preserveSelection) {
    PlaylistPosition position;
    if (preserveSelection) position = RememberPosition();
    SendMessageW(g.list, WM_SETREDRAW, FALSE, 0);
    ClearSelection();
    ClearCellCache();
    g.view.clear();
    RebuildViewItems(Active());
    g.viewRevision = g_lib.Revision();
    g.viewBuilt = GetTickCount64();
    auto& items = Active().items;
    auto words = FilterWords(g.filter);
    TrackMatcher matcher(g_lib, words);
    // Dense ids need one byte each, rather than a hash node and buckets per song. For a
    // small playlist in a huge library, direct matching avoids allocating for unseen tracks.
    std::vector<uint8_t> matches;
    if (!words.empty() && items.size() >= g_lib.Count() / 8) matches.resize(g_lib.Count());
    if (words.empty()) g.view.reserve(items.size());
    for (uint32_t i = 0; i < (uint32_t)items.size(); ++i) {
        bool match = true;
        if (!words.empty()) {
            TrackId id = items[i];
            if (id >= g_lib.Count()) match = false;
            else if (matches.empty()) match = matcher.Matches(id);
            else {
                uint8_t& cached = matches[id];
                if (!cached) cached = matcher.Matches(id) ? 2 : 1;
                match = cached == 2;
            }
        }
        if (match) g.view.push_back(i);
    }
    BuildRows();
    RefreshTabs();
    ListView_SetItemCountEx(g.list, RowCount(), LVSICF_NOSCROLL);
    SendMessageW(g.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.list, nullptr, FALSE);
    if (CanvasActive()) LibViewRebuild();
    if (preserveSelection) RestorePosition(position);
    if (g.playing == kNone && g.npSub) UpdateNowPlaying();  // the hint depends on what's shown
    g.statsDirty = true;
}

// Selected indices into the active playlist's items (not list rows).
std::vector<int> SelectedIndices() {
    std::vector<int> result;
    for (int row = -1; (row = ListView_GetNextItem(g.list, row, LVNI_SELECTED)) >= 0;)
        if (int pos = ViewPosOfRow(row); pos >= 0) result.push_back((int)g.view[pos]);
    return result;
}

// The selected tracks; the canvas library view has its own selection model.
std::vector<TrackId> SelectedTracks() {
    if (CanvasActive()) return LibViewSelection();
    std::vector<TrackId> r;
    for (int i : SelectedIndices()) r.push_back(Active().items[i]);
    return r;
}

int IndexIn(int pl, TrackId id) {
    if (pl < 0 || pl >= (int)g.playlists.size() || id == kNone) return -1;
    auto& items = g.playlists[pl].items;
    auto it = std::find(items.begin(), items.end(), id);
    return it == items.end() ? -1 : (int)(it - items.begin());
}

void SetSortArrow(int type, bool ascending) {
    int column = SubitemOf(type);
    for (int i = 0; i < Header_GetItemCount(g.header); ++i) {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        Header_GetItem(g.header, i, &item);
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == column) item.fmt |= ascending ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(g.header, i, &item);
    }
}

void SortByColumn(int subitem) {
    int column = ColumnType(subitem);
    if (column <= 0) return;
    g.sortAscending = g.sortColumn == column ? !g.sortAscending : true;
    g.sortColumn = column;
    Playlist& pl = Active();
    pl.sortColumn = column;
    pl.sortAscending = g.sortAscending;
    if (pl.IsView()) {
        // A view keeps its sort and re-applies it whenever it is rebuilt.
        pl.builtRevision = 0;
    } else {
        CapturePlaylistPosition();
        std::vector<uint32_t> mapping;
        SortItems(pl.items, column, g.sortAscending, &mapping);
        RemapOccurrences(g.active, mapping);
        auto remap = [&](uint32_t& i) { i = i < mapping.size() ? mapping[i] : UINT32_MAX; };
        for (auto& i : pl.position.selected) remap(i);
        remap(pl.position.focus);
        remap(pl.position.top);
    }
    g.dirty = true;
    SetSortArrow(column, g.sortAscending);
    RestorePlaylistPosition();
    RefreshNext();
}

// ---------------------------------------------------------------------------
// Playlists

// Removes the selection from the active list. keepSelected is "Keep only selected tracks", so the
// unselected rows go instead. On the Library tab removing means excluding from the library rather
// than editing a list.
void RemoveTracks(bool keepSelected) {
    if (Active().kind == PlaylistKind::Library && !keepSelected) {
        ExcludeFromLibrary(SelectedTracks());
        return;
    }
    auto selected = SelectedIndices();
    if (selected.empty()) return;
    if (!Active().Editable()) return;
    auto& items = Active().items;
    std::vector<char> sel(items.size(), 0);
    for (int i : selected) sel[i] = 1;
    std::vector<TrackId> kept;
    kept.reserve(keepSelected ? selected.size() : items.size() - selected.size());
    std::vector<uint32_t> mapping(items.size(), UINT32_MAX);
    for (size_t i = 0; i < items.size(); ++i)
        if ((sel[i] != 0) == keepSelected) { mapping[i] = (uint32_t)kept.size(); kept.push_back(items[i]); }
    RemapOccurrences(g.active, mapping);
    items.swap(kept);
    g.compactPending = true;
    int focus = ViewPosNear(ListView_GetNextItem(g.list, -1, LVNI_FOCUSED));
    ClearSelection();
    RebuildView();
    if (!keepSelected && !g.view.empty()) {
        int row = RowOfViewPos(std::clamp(focus, 0, (int)g.view.size() - 1));
        ListView_SetItemState(g.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    RefreshNext();
    g.dirty = true;
}

void RemoveDuplicates() {
    if (!Active().Editable()) return;
    auto& items = Active().items;
    std::unordered_set<TrackId> seen;  // the library already maps equal paths to one id
    size_t before = items.size();
    std::vector<uint32_t> mapping(before, UINT32_MAX);
    size_t write = 0;
    for (size_t i = 0; i < before; ++i)
        if (seen.insert(items[i]).second) { mapping[i] = (uint32_t)write; items[write++] = items[i]; }
    items.resize(write);
    RemapOccurrences(g.active, mapping);
    g.compactPending = true;
    ClearSelection();
    RebuildView();
    RefreshNext();
    g.dirty = true;
    ShowMessage(L"Removed " + std::to_wstring(before - items.size()) + L" duplicate(s).");
}

// Drops items whose file no longer exists. Only "file not found"/"path not found" counts: a
// permission or share error must not lose tracks that are still on disk.
void RemoveDeadItems() {
    if (!Active().Editable()) return;
    auto& items = Active().items;
    size_t before = items.size();
    std::vector<uint32_t> mapping(before, UINT32_MAX);
    std::unordered_map<TrackId, bool> missing;
    size_t write = 0;
    for (size_t i = 0; i < before; ++i) {
        TrackId id = items[i];
        auto [it, inserted] = missing.try_emplace(id, false);
        if (inserted && GetFileAttributesW(ContainerOf(AudioFileOf(g_lib.Path(id))).c_str()) == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            it->second = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if (!it->second) { mapping[i] = (uint32_t)write; items[write++] = id; }
    }
    items.resize(write);
    RemapOccurrences(g.active, mapping);
    g.compactPending = true;
    ClearSelection();
    RebuildView();
    RefreshNext();
    g.dirty = true;
    ShowMessage(L"Removed " + std::to_wstring(before - items.size()) + L" dead item(s).");
}

// Takes tracks out of the library without touching the files. They stay out on later scans.
void ExcludeFromLibrary(const std::vector<TrackId>& ids) {
    if (ids.empty()) return;
    std::wstring q = L"Remove " + std::to_wstring(ids.size()) + (ids.size() == 1 ? L" track" : L" tracks") +
                     L" from the library?\n\nThe files stay on disk and won't be added back by folder scans.";
    if (MessageBoxW(g.hwnd, q.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    for (TrackId id : ids) g_lib[id].flags = (uint16_t)((g_lib[id].flags | Track::kExcluded) & ~(Track::kInLibrary | Track::kInbox));
    g_lib.Touch();
    g.dirty = true;
    ClearSelection();
    RebuildView();
}

void ShowListContextMenu(POINT pt) {
    if (pt.x == -1 && pt.y == -1) {
        int row = ListView_GetNextItem(g.list, -1, LVNI_FOCUSED);
        RECT r{};
        if (row >= 0 && ListView_GetItemRect(g.list, row, &r, LVIR_LABEL)) pt = {r.left, r.bottom};
        else pt = {0, 0};
        ClientToScreen(g.list, &pt);
    }
    if (pt.x == -1 && pt.y == -1 && CanvasActive()) GetCursorPos(&pt);
    UINT en = !SelectedTracks().empty() ? MF_ENABLED : MF_GRAYED;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | en, IDM_PLAY_SEL, L"&Play");
    SetMenuDefaultItem(m, IDM_PLAY_SEL, FALSE);
    AppendMenuW(m, MF_STRING | en, IDM_QUEUE_NEXT, L"Play ne&xt\tCtrl+Enter");
    AppendMenuW(m, MF_STRING | en, IDM_QUEUE_ADD, L"Add to &queue\tCtrl+Q");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    PlaylistKind kind = Active().kind;
    if (kind == PlaylistKind::Inbox) {
        AppendMenuW(m, MF_STRING | en, IDM_INBOX_ACCEPT, L"&Add to library");
        AppendMenuW(m, MF_STRING | en, IDM_EXCLUDE, L"&Remove from library...");
    } else if (kind == PlaylistKind::Library) {
        AppendMenuW(m, MF_STRING | en, IDM_EXCLUDE, L"&Remove from library...\tDel");
    } else if (Active().Editable()) {
        AppendMenuW(m, MF_STRING | en, IDM_REMOVE, L"&Remove\tDel");
        AppendMenuW(m, MF_STRING | en, IDM_CROP, L"&Keep only selected tracks");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP | en, (UINT_PTR)RatingMenu(), L"Ra&ting");
    AppendMenuW(m, MF_STRING | en | (g.rgRunning && !g.rgAuto ? MF_GRAYED : 0), IDM_RG_SCAN_SEL, L"Scan Replay&Gain");
    AppendMenuW(m, MF_STRING | en | (g.rgRunning && !g.rgAuto ? MF_GRAYED : 0), IDM_RG_WRITE_TAGS, L"Write ReplayGain t&ags");
    AppendMenuW(m, MF_STRING | en | (g.rgRunning && !g.rgAuto ? MF_GRAYED : 0), IDM_RG_CLEAR, L"&Clear ReplayGain");
    AppendMenuW(m, MF_STRING | en, IDM_SHOW_IN_EXPLORER, L"Show in &Explorer");
    AppendMenuW(m, MF_STRING | en, IDM_ORGANIZE, L"&Organize files...");
    AppendMenuW(m, MF_STRING | (en == MF_ENABLED && !ConvertBusy() ? MF_ENABLED : MF_GRAYED), IDM_CONVERT, L"Con&vert to .cord...");
    AppendMenuW(m, MF_POPUP | (en == MF_ENABLED && !ConvertBusy() ? MF_ENABLED : MF_GRAYED), (UINT_PTR)CordExportMenu(), L"&Export .cord");
    AppendMenuW(m, MF_STRING | (en == MF_ENABLED && !CopyBusy() ? MF_ENABLED : MF_GRAYED), IDM_COPY_TO, L"Cop&y to folder or device...");
    AppendMenuW(m, MF_STRING | en, IDM_PROPERTIES, L"Edit &tags...\tAlt+Enter");
    AppendHelperTools(m, en == MF_ENABLED);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_ADD_FILES, L"&Add files...");
    AppendMenuW(m, MF_STRING, IDM_ADD_FOLDER, L"Import &folder once...");
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
}

void ShowTabContextMenu(POINT pt) {
    TCHITTESTINFO hit{};
    hit.pt = pt;
    ScreenToClient(g.tabs, &hit.pt);
    int index = TabCtrl_HitTest(g.tabs, &hit);
    if (index >= 0 && index != g.active) ActivatePlaylist(index);
    HMENU m = CreatePopupMenu();
    UINT en = index >= 0 && g.playlists[index].kind != PlaylistKind::Library &&
              g.playlists[index].kind != PlaylistKind::Inbox ? MF_ENABLED : MF_GRAYED;
    AppendMenuW(m, MF_STRING, IDM_NEW_PL, L"&New playlist");
    AppendMenuW(m, MF_STRING, IDM_NEW_AUTO, L"New &auto-playlist...");
    if (index >= 0 && g.playlists[index].kind == PlaylistKind::Auto) AppendMenuW(m, MF_STRING, IDM_EDIT_AUTO, L"&Edit rules...");
    AppendMenuW(m, MF_STRING | en, IDM_RENAME_PL, L"&Rename");
    AppendMenuW(m, MF_STRING | en, IDM_REMOVE_PL, L"&Close\tCtrl+W");
    AppendMenuW(m, MF_STRING, IDM_DUPLICATE_PL, L"Duplicate\tCtrl+Shift+D");
    AppendMenuW(m, MF_STRING | (g.closedPlaylists.empty() ? MF_GRAYED : MF_ENABLED), IDM_REOPEN_PL, L"Reopen closed\tCtrl+Shift+T");
    AppendMenuW(m, MF_STRING | (index > 1 ? MF_ENABLED : MF_GRAYED), IDM_MOVE_PL_LEFT, L"Move left");
    AppendMenuW(m, MF_STRING | (index > 0 && index + 1 < (int)g.playlists.size() ? MF_ENABLED : MF_GRAYED), IDM_MOVE_PL_RIGHT, L"Move right");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXPORT_M3U, L"&Save as...");
    AppendMenuW(m, MF_STRING | (index >= 0 && !CopyBusy() ? MF_ENABLED : MF_GRAYED), IDM_COPY_PL, L"Cop&y to folder or device...");
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
}

namespace {

// A track row's cell text: the playing / queue / problem mark in column 0, otherwise the column's
// format, left out where the album header already says it. Formatted on demand and cached.
std::wstring CellText(int row, int col) {
    int pos = ViewPosOfRow(row);
    if (pos < 0 || ColumnType(col) < 0) return L"";
    TrackId id = Active().items[g.view[pos]];
    std::wstring text;
    if (col == 0 && ApolloMode()) {
        text = std::to_wstring(g.view[pos] + 1) + L".";  // Apollo numbers its playlist rows
    } else if (col == 0) {
        if (id == g.playing && g.active == g.playingPl && (!Active().Editable() || (int)g.view[pos] == TokenIndex(g.playingToken, g.playingPl))) text = g.engine.Paused() ? L"❙❙" : ThemeInfo(g.themeMode).playingMark;
        else if (int q = QueuePosition(id)) text = L"[" + std::to_wstring(q) + L"]";
        else if (g_lib[id].Failed() || (g_lib[id].flags & Track::kTagFailed)) text = L"⚠";
    } else if (!GroupHidesCell(pos, ColumnType(col), id)) {
        static uint32_t revision = 0;
        if (revision != g_lib.Revision()) { ClearCellCache(); revision = g_lib.Revision(); }
        uint64_t key = (uint64_t)id * kMaxColumnTypes + ColumnType(col);
        auto cached = g.cellCache.find(key);
        if (cached == g.cellCache.end()) {
            text = FormatTitle(ColumnAt(ColumnType(col)).display, Lookup(id, false));
            if (text.size() > 4096) text.resize(4096);
            size_t bytes = text.size() * sizeof(wchar_t);
            if (g.cellCache.size() >= ViewMemoryBudget(2048) ||
                g.cellCacheBytes + bytes > ViewMemoryBudget(2u << 20)) ClearCellCache();
            g.cellCacheBytes += bytes;
            cached = g.cellCache.emplace(key, text).first;
        }
        text = cached->second;
    }
    return text;
}

// Paints a whole track row: its background (selection, hover or the list's own), then every
// column. The visual style would otherwise paint its own blue hover and a line at each column
// edge; the list keeps the style for its scroll bars.
void DrawTrackRow(NMLVCUSTOMDRAW* cd, int row, TrackId id, bool playing) {
    HDC dc = cd->nmcd.hdc;
    const RECT& rc = cd->nmcd.rc;
    bool selected = ListView_GetItemState(g.list, row, LVIS_SELECTED) != 0;
    bool apollo = ApolloMode();
    bool hot = !selected && !apollo && ListView_GetHotItem(g.list) == row;
    // Apollo's list is a Windows 2000 one: no hover, the selection the face color while the list
    // isn't focused, and the playing row only in bold.
    bool faded = apollo && selected && GetFocus() != g.list;
    COLORREF bg = faded ? SurfaceColor() : selected ? Ref(P().select) : hot ? Ref(P().hover) : WindowBg();
    COLORREF text = faded ? TextColor() : selected ? Ref(P().selectText) : playing && !apollo ? Ref(P().playing)
                    : g_lib[id].Failed() ? Ref(P().faint) : TextColor();
    if (FoobarMode() && playing) {
        bg = Ref(P().playing);
        text = Ref(TextOn(P().playing));
    }
    HFONT font = playing && !FoobarMode() ? g.fontBold : g.font;
    FillSolid(dc, rc, bg);
    SetBkMode(dc, TRANSPARENT);
    int columns = Header_GetItemCount(g.header);
    for (int sub = 0; sub < columns; ++sub) {
        RECT r{};
        if (!ListView_GetSubItemRect(g.list, row, sub, sub == 0 ? LVIR_LABEL : LVIR_BOUNDS, &r)) continue;
        if (r.right <= rc.left || r.left >= rc.right) continue;
        int type = ColumnType(sub);
        if (HasBadge(type, id)) {
            DrawBadgeCell(dc, row, sub, id, text, bg, font, selected);
            continue;
        }
        if (type == kColRating) {
            DrawStars(dc, r, g_lib[id].rating, selected);
            continue;
        }
        std::wstring s = CellText(row, sub);
        if (apollo && type == kColApolloTitle) {
            ApolloDrawTrackIcon(dc, r);
            r.left += S(18);
        }
        if (s.empty()) continue;
        int align = type >= 0 ? ColumnAt(type).align & LVCFMT_JUSTIFYMASK : LVCFMT_LEFT;
        if (apollo && type == 0) align = LVCFMT_RIGHT;
        RECT tr{r.left + S(6), r.top, r.right - S(6), r.bottom};
        HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, text);
        DrawTextW(dc, s.c_str(), (int)s.size(), &tr,
                  (align == LVCFMT_RIGHT ? DT_RIGHT : align == LVCFMT_CENTER ? DT_CENTER : DT_LEFT) | DT_VCENTER | DT_SINGLELINE |
                      DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, old);
    }
    if (g.showFocusBoxes && (cd->nmcd.uItemState & CDIS_FOCUS) && GetFocus() == g.list &&
        !(SendMessageW(g.list, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        SetTextColor(dc, text);
        SetBkColor(dc, bg);
        DrawFocusRect(dc, &rc);
    }
}

// Dragging rows to reorder a playlist. A press on an album header (or any selected
// track) starts a drag; the insertion mark shows where the block would land, and releasing moves
// the underlying items. View playlists keep this order as their computed membership changes.
struct TrackDrag {
    bool active = false;
    int markRow = -1;
    bool markAfter = false;
    std::vector<int> sources;  // item positions in the active playlist, ascending
};

TrackDrag g_trackDrag;

// Some list-view styles do not send LVN_BEGINDRAG until the parent creates a drag image, so a
// plain press-and-move also starts the drag from the list's own mouse messages.
struct PendingDrag {
    bool on = false;
    int row = -1;
    POINT at{};
};
PendingDrag g_pendingDrag;

struct DropSpot {
    int item = 0;     // insert before this item position
    int markRow = -1; // row carrying the insertion mark
    bool after = false;
};

DropSpot DropSpotAt(POINT pt) {
    DropSpot spot;
    int rows = RowCount();
    auto atEnd = [&] {
        spot.item = g.view.empty() ? 0 : (int)g.view.back() + 1;
        spot.markRow = rows - 1;
        spot.after = true;
    };
    LVHITTESTINFO hit{};
    hit.pt = pt;
    ListView_SubItemHitTest(g.list, &hit);
    if (hit.iItem < 0 || hit.iItem >= rows) {
        atEnd();
        return spot;
    }
    if (int group = HeaderGroupOfRow(hit.iItem); group >= 0) {
        spot.item = (int)g.view[g.groups[group].firstView];
        spot.markRow = hit.iItem;
        return spot;
    }
    int pos = ViewPosOfRow(hit.iItem);
    if (pos < 0) pos = ViewPosNear(hit.iItem);  // a foobar2000 pad row beside the cover
    if (pos < 0 || pos >= (int)g.view.size()) {
        atEnd();
        return spot;
    }
    RECT r{};
    ListView_GetItemRect(g.list, hit.iItem, &r, LVIR_BOUNDS);
    spot.after = pt.y > (r.top + r.bottom) / 2;
    int next = spot.after ? pos + 1 : pos;
    spot.item = next < (int)g.view.size() ? (int)g.view[next] : (g.view.empty() ? 0 : (int)g.view.back() + 1);
    spot.markRow = hit.iItem;
    return spot;
}

// Moves the dragged item positions to `target` (an item position before which they land).
void MoveTrackBlockImpl(const std::vector<int>& sources, int target) {
    if (sources.empty()) return;
    int first = sources.front(), last = sources.back();
    if (last - first + 1 == (int)sources.size() && target >= first && target <= last + 1) return;
    auto& items = Active().items;
    int insert = target;
    for (int s : sources) insert -= s < target ? 1 : 0;
    insert = std::clamp(insert, 0, (int)items.size() - (int)sources.size());
    std::vector<TrackId> moved;
    moved.reserve(sources.size());
    for (int s : sources) moved.push_back(items[s]);
    std::vector<TrackId> kept;
    kept.reserve(items.size());
    std::vector<uint32_t> mapping(items.size(), UINT32_MAX);
    size_t next = 0;
    for (int i = 0; i < (int)items.size(); ++i) {
        if (next < sources.size() && sources[next] == i) {
            ++next;
            continue;
        }
        mapping[i] = (uint32_t)kept.size();
        kept.push_back(items[i]);
    }
    kept.insert(kept.begin() + insert, moved.begin(), moved.end());
    // Kept entries at or after the insertion point shift right by the moved block. The mapping
    // is old slot -> final slot, so adjust those before assigning the moved slots themselves.
    for (auto& slot : mapping)
        if (slot != UINT32_MAX && slot >= (uint32_t)insert) slot += (uint32_t)moved.size();
    for (size_t j = 0; j < moved.size(); ++j) mapping[sources[j]] = (uint32_t)(insert + j);
    if (Active().IsView()) {
        // The view keeps the user's order until a column sort replaces it.
        Active().sortColumn = g.sortColumn = -2;
        Active().sortAscending = g.sortAscending = true;
        SetSortArrow(g.sortColumn, g.sortAscending);
    }
    items.swap(kept);
    RemapOccurrences(g.active, mapping);
    ClearSelection();
    RebuildView();
    g.dirty = true;
    // Select what moved, by occurrence so duplicate tracks keep their identity.
    int firstRow = -1;
    for (int pos = 0; pos < (int)g.view.size(); ++pos) {
        int index = (int)g.view[pos];
        if (index < insert || index >= insert + (int)moved.size()) continue;
        int row = RowOfViewPos(pos);
        ListView_SetItemState(g.list, row, LVIS_SELECTED, LVIS_SELECTED);
        if (firstRow < 0) firstRow = row;
    }
    if (firstRow >= 0) {
        ListView_SetItemState(g.list, firstRow, LVIS_FOCUSED, LVIS_FOCUSED);
        ListView_EnsureVisible(g.list, firstRow, FALSE);
    }
    RefreshNext();
}

void UpdateTrackDrag(POINT pt) {
    DropSpot spot = DropSpotAt(pt);
    if (spot.markRow == g_trackDrag.markRow && spot.after == g_trackDrag.markAfter) return;
    g_trackDrag.markRow = spot.markRow;
    g_trackDrag.markAfter = spot.after;
    LVINSERTMARK mark{sizeof(mark), (DWORD)(spot.after ? LVIM_AFTER : 0), spot.markRow, 0};
    ListView_SetInsertMark(g.list, &mark);
}

void EndTrackDrag(bool drop, POINT pt) {
    if (!g_trackDrag.active) return;
    g_trackDrag.active = false;
    LVINSERTMARK mark{sizeof(mark), 0, -1, 0};
    ListView_SetInsertMark(g.list, &mark);
    if (GetCapture() == g.list) ReleaseCapture();
    if (drop) MoveTrackBlockImpl(g_trackDrag.sources, DropSpotAt(pt).item);
    g_trackDrag.sources.clear();
    g_trackDrag.markRow = -1;
}

void BeginTrackDrag(int row, POINT pt) {
    if (g_trackDrag.active) return;
    if (row < 0 || row >= RowCount()) return;
    g_pendingDrag.on = false;
    LVHITTESTINFO hit{};
    hit.pt = pt;
    ListView_SubItemHitTest(g.list, &hit);
    if (ColumnType(hit.iSubItem) == kColRating) return;  // the star column keeps its own drag
    g_trackDrag = TrackDrag{};
    if (int group = HeaderGroupOfRow(row); group >= 0) {
        SelectGroup(group, false);
        const TrackGroup& gr = g.groups[group];
        for (uint32_t k = gr.firstView; k < gr.firstView + gr.count; ++k) g_trackDrag.sources.push_back((int)g.view[k]);
    } else {
        if (!ListView_GetItemState(g.list, row, LVIS_SELECTED)) {
            ClearSelection();
            ListView_SetItemState(g.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        for (int r = -1; (r = ListView_GetNextItem(g.list, r, LVNI_SELECTED)) >= 0;) {
            int pos = ViewPosOfRow(r);
            if (pos >= 0) g_trackDrag.sources.push_back((int)g.view[pos]);
        }
    }
    std::sort(g_trackDrag.sources.begin(), g_trackDrag.sources.end());
    g_trackDrag.sources.erase(std::unique(g_trackDrag.sources.begin(), g_trackDrag.sources.end()),
                              g_trackDrag.sources.end());
    if (g_trackDrag.sources.empty()) return;
    g_trackDrag.active = true;
    SetCapture(g.list);
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    UpdateTrackDrag(pt);
}

}  // namespace

// The drag/drop handler above and the self-test both need the same occurrence remapping.
void MoveTrackBlock(const std::vector<int>& sources, int target) { MoveTrackBlockImpl(sources, target); }

// Mouse tracking for the track drag begun by LVN_BEGINDRAG. The list would otherwise keep the
// messages (it captures the mouse itself), so the drag lives in this subclass.
LRESULT CALLBACK ListSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (g_trackDrag.active) {
        if (msg == WM_MOUSEMOVE) {
            UpdateTrackDrag({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        }
        if (msg == WM_LBUTTONUP) {
            EndTrackDrag(true, {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        }
        if (msg == WM_CAPTURECHANGED) {
            EndTrackDrag(false, {});
            return 0;
        }
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
            EndTrackDrag(false, {});
            return 0;
        }
        if (msg == WM_SETCURSOR) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
    }
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (msg == WM_LBUTTONDOWN && !g_trackDrag.active) {
        LVHITTESTINFO hit{};
        hit.pt = pt;
        ListView_SubItemHitTest(h, &hit);
        g_pendingDrag = {true, hit.iItem, pt};
    } else if (msg == WM_MOUSEMOVE && g_pendingDrag.on && (wp & MK_LBUTTON)) {
        int dx = std::abs(pt.x - g_pendingDrag.at.x), dy = std::abs(pt.y - g_pendingDrag.at.y);
        if (dx >= GetSystemMetrics(SM_CXDRAG) || dy >= GetSystemMetrics(SM_CYDRAG)) {
            BeginTrackDrag(g_pendingDrag.row, g_pendingDrag.at);
            if (g_trackDrag.active) UpdateTrackDrag(pt);
        }
    } else if (msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) {
        g_pendingDrag.on = false;
    }
    if (msg == WM_NCDESTROY) {
        EndTrackDrag(false, {});
        RemoveWindowSubclass(h, ListSubclass, 0);
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Owner-data list notifications. Handlers are addressed by list row, so each maps through the
// view (and the group headers) first; cells are formatted on demand and cached.
LRESULT OnListNotify(NMHDR* hdr) {
    switch (hdr->code) {
        case LVN_GETEMPTYMARKUP: {
            auto* empty = reinterpret_cast<NMLVEMPTYMARKUP*>(hdr);
            empty->dwFlags = EMF_CENTERED;
            wcsncpy_s(empty->szMarkup, EmptyViewText().c_str(), _TRUNCATE);
            return TRUE;
        }
        case LVN_GETDISPINFOW: {
            auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
            if (!(di->item.mask & LVIF_TEXT) || !di->item.pszText || di->item.cchTextMax <= 0) break;
            std::wstring text = CellText(di->item.iItem, di->item.iSubItem);
            wcsncpy_s(di->item.pszText, di->item.cchTextMax, text.c_str(), _TRUNCATE);
            break;
        }
        case LVN_ODFINDITEMW: {
            auto* find = reinterpret_cast<NMLVFINDITEMW*>(hdr);
            if (!(find->lvfi.flags & (LVFI_STRING | LVFI_PARTIAL)) || !find->lvfi.psz) return -1;
            int rows = RowCount(), start = std::clamp(find->iStart, 0, rows);
            for (int step = 0; step < rows; ++step) {
                int row = (start + step) % rows;
                TrackId id = RowTrack(row);
                std::wstring title;
                if (id != kNone) LookupField(id, L"title", title);
                if (!title.empty() && FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_STARTSWITH | LINGUISTIC_IGNORECASE,
                    title.c_str(), (int)title.size(), find->lvfi.psz, -1, nullptr, nullptr, nullptr, 0) >= 0) return row;
                if (!(find->lvfi.flags & LVFI_WRAP) && start + step + 1 >= rows) break;
            }
            return -1;
        }
        case NM_CUSTOMDRAW: {
            auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | (FoobarMode() ? CDRF_NOTIFYPOSTPAINT : 0);
            if (cd->nmcd.dwDrawStage == CDDS_POSTPAINT) {
                // foobar2000's playlist is plain black past its last row (the dark list theme would
                // draw its column lines there).
                RECT client, last{};
                GetClientRect(g.list, &client);
                int n = ListView_GetItemCount(g.list);
                if (n > 0 && ListView_GetItemRect(g.list, n - 1, &last, LVIR_BOUNDS)) client.top = std::max(client.top, last.bottom);
                else if (HWND header = ListView_GetHeader(g.list)) {
                    RECT hr;
                    GetWindowRect(header, &hr);
                    client.top = hr.bottom - hr.top;
                }
                if (client.bottom > client.top) FillSolid(cd->nmcd.hdc, client, WindowBg());
                return CDRF_DODEFAULT;
            }
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                // Every row is painted here, in palette colors in both themes: album headers by
                // DrawGroupRow, tracks by DrawTrackRow (uniform rows, no zebra).
                int row = (int)cd->nmcd.dwItemSpec;
                if (HeaderGroupOfRow(row) >= 0) {
                    DrawGroupRow(cd->nmcd.hdc, row, cd->nmcd.rc);
                    return CDRF_SKIPDEFAULT;
                }
                TrackId id = RowTrack(row);
                if (id == kNone) {
                    if (!FoobarMode() || GroupOfRow(row) < 0) return CDRF_DODEFAULT;
                    FillSolid(cd->nmcd.hdc, cd->nmcd.rc, WindowBg());  // a pad row beside the cover
                    DrawFoobarCover(cd->nmcd.hdc, row, cd->nmcd.rc);
                    return CDRF_SKIPDEFAULT;
                }
                // Apollo has one playlist, so its playing track is bold wherever it is listed.
                bool playing = id == g.playing && (ApolloMode() || g.active == g.playingPl) &&
                    (ApolloMode() || !Active().Editable() || (int)g.view[ViewPosOfRow(row)] == TokenIndex(g.playingToken, g.playingPl));
                DrawTrackRow(cd, row, id, playing);
                if (FoobarMode()) DrawFoobarCover(cd->nmcd.hdc, row, cd->nmcd.rc);
                return CDRF_SKIPDEFAULT;
            }
            return CDRF_DODEFAULT;
        }
        case NM_CLICK: {
            auto* ia = reinterpret_cast<NMITEMACTIVATE*>(hdr);
            LVHITTESTINFO hit{};
            hit.pt = ia->ptAction;
            ListView_SubItemHitTest(g.list, &hit);
            if (int group = HeaderGroupOfRow(hit.iItem); group >= 0) {
                SelectGroup(group, (ia->uKeyFlags & LVKF_CONTROL) != 0);  // a header selects its album
                break;
            }
            if (RowTrack(hit.iItem) != kNone && ColumnType(hit.iSubItem) == kColRating &&
                !(ia->uKeyFlags & (LVKF_CONTROL | LVKF_SHIFT))) {
                RECT r{};
                ListView_GetSubItemRect(g.list, hit.iItem, hit.iSubItem, LVIR_BOUNDS, &r);
                TrackId id = RowTrack(hit.iItem);
                SetRating({id}, RatingAt(ia->ptAction.x - r.left, g_lib[id].rating));
            }
            break;
        }
        case LVN_BEGINDRAG: {
            auto* lv = reinterpret_cast<NMLISTVIEW*>(hdr);
            BeginTrackDrag(lv->iItem, lv->ptAction);
            break;
        }
        case NM_DBLCLK: {
            auto* ia = reinterpret_cast<NMITEMACTIVATE*>(hdr);
            // Resolve the actual cell even when the native notification has no item index.
            LVHITTESTINFO hit{};
            hit.pt = ia->ptAction;
            if (ListView_SubItemHitTest(g.list, &hit) >= 0) PlayRow(hit.iItem);
            break;
        }
        case NM_RETURN:
            PlayFocusedOrFirst();
            break;
        case LVN_KEYDOWN: {
            auto* kd = reinterpret_cast<NMLVKEYDOWN*>(hdr);
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (kd->wVKey == VK_DELETE) RemoveTracks(false);
            else if (kd->wVKey == VK_SPACE && !ctrl) TogglePause();
            else if (kd->wVKey == 'A' && ctrl) ListView_SetItemState(g.list, -1, LVIS_SELECTED, LVIS_SELECTED);
            break;
        }
        case LVN_COLUMNCLICK:
            SortByColumn(reinterpret_cast<NMLISTVIEW*>(hdr)->iSubItem);
            break;
        case LVN_ITEMCHANGED:
        case LVN_ODSTATECHANGED:
            g.statsDirty = true;
            break;
    }
    return 0;
}

}  // namespace app
