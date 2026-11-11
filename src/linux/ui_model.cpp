// The Linux window's list model: what the active tab shows, how it is sorted, selected and changed.
#include "alternate_ui.h"
#include "ui_internal.h"

namespace lx {

Playlist& Active() { return g.playlists[g.active]; }

int NowPlayingIndex() {
    for (size_t i = 0; i < g.playlists.size(); ++i)
        if (g.playlists[i].kind == PlaylistKind::NowPlaying) return (int)i;
    return -1;
}

// The title-format fields, with the playing track's position and remaining time.
FieldLookup Lookup(TrackId id, bool withPlayback) {
    return [id, withPlayback](const std::wstring& name, std::wstring& out) {
        if (withPlayback && name == L"playback_time") {
            out = FormatDuration(g.engine.Position());
            return true;
        }
        if (withPlayback && name == L"playback_time_remaining") {
            out = FormatDuration(std::max(0.0, g.engine.Duration() - g.engine.Position()));
            return true;
        }
        return LookupField(id, name, out) || AlternateField(id, name, out) || PackField(id, name, out);
    };
}

const std::string& Cell(TrackId id, int column) {
    if (g_cellsRevision != g_lib.Revision() || g_cells.size() > 20000) {
        g_cells.clear();
        g_cellsRevision = g_lib.Revision();
    }
    uint64_t key = ((uint64_t)id << 32) | (unsigned)column;
    auto it = g_cells.find(key);
    if (it == g_cells.end()) it = g_cells.emplace(key, Utf8(FormatTitle(ColumnAt(column).display, Lookup(id, false)))).first;
    return it->second;
}

// Case-insensitive, with runs of digits compared as numbers ("Track 2" before "Track 10").
int CompareNatural(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (iswdigit(a[i]) && iswdigit(b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && a[i2] == L'0') ++i2;
            while (j2 < b.size() && b[j2] == L'0') ++j2;
            size_t ie = i2, je = j2;
            while (ie < a.size() && iswdigit(a[ie])) ++ie;
            while (je < b.size() && iswdigit(b[je])) ++je;
            if (ie - i2 != je - j2) return ie - i2 < je - j2 ? -1 : 1;
            if (int c = a.compare(i2, ie - i2, b, j2, je - j2)) return c < 0 ? -1 : 1;
            i = ie;
            j = je;
            continue;
        }
        wchar_t x = (wchar_t)towlower(a[i++]), y = (wchar_t)towlower(b[j++]);
        if (x != y) return x < y ? -1 : 1;
    }
    return i < a.size() ? 1 : j < b.size() ? -1 : 0;
}

int CompareText(std::wstring_view a, std::wstring_view b) { return CompareNatural(std::wstring(a), std::wstring(b)); }

// The library's own order: album artist, album, disc, track, then file name.
bool LibraryLess(TrackId x, TrackId y) {
    const Track &a = g_lib[x], &b = g_lib[y];
    auto artist = [](const Track& t) { return g_lib.Str(t.albumArtist ? t.albumArtist : t.artist); };
    if (int r = CompareText(artist(a), artist(b))) return r < 0;
    if (int r = CompareText(g_lib.Str(a.album), g_lib.Str(b.album))) return r < 0;
    if (a.disc != b.disc) return a.disc < b.disc;
    if (a.trackNo != b.trackNo) return a.trackNo < b.trackNo;
    if (int r = CompareText(g_lib.Str(a.dir), g_lib.Str(b.dir))) return r < 0;
    return CompareText(g_lib.Str(a.file), g_lib.Str(b.file)) < 0;
}

std::vector<TrackId> PlaylistTracks(const Playlist& pl) {
    std::vector<TrackId> tracks;
    if (pl.kind == PlaylistKind::Library || pl.kind == PlaylistKind::Inbox) {
        for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
            if (pl.kind == PlaylistKind::Library ? g_lib[id].InLibrary() :
                (g_lib[id].flags & (Track::kInbox | Track::kMissing)) == Track::kInbox) tracks.push_back(id);
        if (pl.sortColumn < 0) std::stable_sort(tracks.begin(), tracks.end(), LibraryLess);
    } else if (pl.kind == PlaylistKind::Auto) {
        AutoPlaylist rules;
        if (ParseAutoPlaylist(pl.rules, rules)) EvaluateAutoPlaylist(rules, tracks);
    } else {
        tracks = pl.items;
    }
    return tracks;
}

void SortPlaylistTracks(std::vector<TrackId>& tracks, const Playlist& pl) {
    if (pl.sortColumn <= 0 || pl.sortColumn >= ColumnTotal()) return;
    std::unordered_map<TrackId, std::wstring> keys;
    for (TrackId id : tracks) {
        if (!g_lib.Contains(id)) return;
        keys.emplace(id, FormatTitle(ColumnAt(pl.sortColumn).sort, Lookup(id, false)));
    }
    std::stable_sort(tracks.begin(), tracks.end(), [&](TrackId a, TrackId b) {
        int r = CompareNatural(keys[a], keys[b]);
        return pl.sortAscending ? r < 0 : r > 0;
    });
}

bool SortKeepsAlbums(int column) {  // #, Artist, Album, Year, Album artist, Disc keep an album together
    return column <= 0 || column == 1 || column == 3 || column == 4 || column == 5 || column == 10 || column == 13;
}

// The album groups and display lines for g.rows.
void BuildLines() {
    g_groups.clear();
    g_lines.clear();
    g_lineOfRow.assign(g.rows.size(), 0);
    bool grouping = g.settings.groupAlbums && !g.playlists.empty() && SortKeepsAlbums(Active().sortColumn);
    int n = (int)g.rows.size();
    for (int p = 0; p < n;) {
        const Track& first = g_lib[g.rows[p]];
        int q = p + 1;
        if (grouping && first.album)
            while (q < n && SameAlbum(first, g_lib[g.rows[q]])) ++q;
        if (q - p >= 2) {
            Group gr;
            gr.first = p;
            gr.count = q - p;
            gr.album = first.album;
            gr.artist = first.albumArtist;
            gr.date = first.date;
            bool oneArtist = true;
            for (int k = p; k < q; ++k) {
                const Track& t = g_lib[g.rows[k]];
                gr.seconds += t.Duration();
                oneArtist &= t.artist == first.artist;
                if (!gr.date) gr.date = t.date;
            }
            if (!gr.artist) gr.artist = oneArtist ? first.artist : 0;
            int k = (int)g_groups.size();
            g_groups.push_back(gr);
            g_lines.push_back({-1, k, 0});
            g_lines.push_back({-1, k, 1});
        }
        for (int k = p; k < q; ++k) {
            g_lineOfRow[k] = (int)g_lines.size();
            g_lines.push_back({k, q - p >= 2 ? (int)g_groups.size() - 1 : -1, 0});
        }
        p = q;
    }
}

// What a grouped track's cell leaves to its header: the album, and the artist and year that match.
bool GroupHidesCell(const Line& line, int column) {
    if (line.group < 0) return false;
    const Group& gr = g_groups[line.group];
    const Track& t = g_lib[g.rows[line.row]];
    switch (column) {
        case 4: case 10: return true;
        case 3: return gr.artist && t.artist == gr.artist;
        case 5: return t.date == gr.date;
        default: return false;
    }
}

// How wide a track's left-aligned text in the index'th shown column (`width` wide) may run: on across
// the columns after it whose cells the album header already speaks for, so a long title uses the
// blank Artist and Album cells beside it instead of being cut short.
float CellTextWidth(const Line& line, size_t index, float width) {
    const auto& types = ActiveColumnTypes();
    for (size_t k = index + 1; k < types.size() && types[k] > 0 && GroupHidesCell(line, types[k]); ++k)
        width += ColumnWidth(types[k]);
    return width;
}

std::vector<TrackId> SelectedTracks() {
    std::vector<TrackId> ids;
    for (size_t i = 0; i < g.rows.size(); ++i)
        if (g.selected[i]) ids.push_back(g.rows[i]);
    return ids;
}

void SelectOnly(int row) {
    std::fill(g.selected.begin(), g.selected.end(), 0);
    if (row >= 0 && row < (int)g.selected.size()) g.selected[row] = 1;
    g.focus = g.anchor = row;
}

void SelectRange(int from, int to) {
    std::fill(g.selected.begin(), g.selected.end(), 0);
    if (from > to) std::swap(from, to);
    for (int i = std::max(0, from); i <= to && i < (int)g.selected.size(); ++i) g.selected[i] = 1;
}

void MoveFocus(int row, bool extend) {
    if (g.rows.empty()) return;
    row = std::clamp(row, 0, (int)g.rows.size() - 1);
    if (extend) {
        if (g.anchor < 0) g.anchor = std::max(g.focus, 0);
        SelectRange(g.anchor, row);
        g.focus = row;
    } else {
        SelectOnly(row);
    }
    EnsureVisible(row);
}

void AcceptInbox(const std::vector<TrackId>& ids) {
    std::vector<TrackId> accepted;
    for (TrackId id : ids) {
        if (!g_lib.Contains(id) || !(g_lib[id].flags & Track::kInbox)) continue;
        g_lib[id].flags &= ~Track::kInbox;
        accepted.push_back(id);
    }
    if (accepted.empty()) return;
    AnnouncePoints(AwardLibraryAdds(accepted));
    g_lib.Touch();
    g.libraryDirty = true;
    UpdateInboxTab();
    RebuildRows();
    ShowMessage(L"Added " + std::to_wstring(accepted.size()) +
                (accepted.size() == 1 ? L" track" : L" tracks") + L" to the library.");
}

void SetRating(const std::vector<TrackId>& ids, uint8_t rating) {
    if (ids.empty()) return;
    for (TrackId id : ids) if (g_lib.Contains(id)) g_lib[id].rating = rating;
    PlaySfx(rating ? Sfx::Rate : Sfx::Unrate, rating / 10);
    g_lib.Touch();
    g.libraryDirty = true;
    RebuildRows();
}

void ExcludeFromLibrary(const std::vector<TrackId>& ids) {
    if (ids.empty()) return;
    g_exclude.ids = ids;
    g_exclude.open = true;
    g.redraw = true;
}

void ConfirmExclude(bool confirm) {
    g_exclude.open = false;
    auto ids = std::move(g_exclude.ids);
    if (!confirm) return;
    for (TrackId id : ids) if (g_lib.Contains(id) && (g_lib[id].flags & (Track::kInLibrary | Track::kInbox)))
        g_lib[id].flags = (uint16_t)((g_lib[id].flags | Track::kExcluded) &
                                    ~(Track::kInLibrary | Track::kInbox | Track::kSteamDuplicate));
    g_lib.Touch();
    g.libraryDirty = true;
    UpdateInboxTab();
    RebuildRows();
    if (g_recent.open) RefreshRecentAdditions();
    ShowMessage(L"Removed from the library. The files were kept.");
}

size_t RemoveOccurrences(const std::vector<char>& remove, int index, const wchar_t* what) {
    if (index < 0) index = g.active;
    if (index < 0 || index >= (int)g.playlists.size() || !g.playlists[index].Editable()) return 0;
    auto& playlist = g.playlists[index];
    auto& items = playlist.items;
    if (remove.size() != items.size()) return 0;
    std::vector<size_t> mapping(items.size(), SIZE_MAX);
    std::vector<TrackId> kept;
    kept.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        if (remove[i]) continue;
        mapping[i] = kept.size();
        kept.push_back(items[i]);
    }
    size_t removed = items.size() - kept.size();
    if (!removed) return 0;
    items.swap(kept);
    RecordPlaylistEdit(playlist, std::move(kept), what);
    if (playlist.kind == PlaylistKind::NowPlaying) RemapNowPlaying(mapping);
    g.libraryDirty = true;
    if (index == g.active) {
        g.rows.clear(); g.rowItems.clear(); g.selected.clear(); g.focus = g.anchor = -1;
        RebuildRows();
    }
    return removed;
}

void RemoveFromPlaylist(bool all, bool keepSelected) {
    if (!Active().Editable()) return;
    std::vector<char> remove(Active().items.size(), (all || keepSelected) ? 1 : 0);
    bool anySelected = false;
    for (size_t i = 0; i < g.selected.size() && i < g.rowItems.size(); ++i)
        if (g.selected[i] && g.rowItems[i] < remove.size()) {
            anySelected = true;
            if (!all) remove[g.rowItems[i]] = keepSelected ? 0 : 1;
        }
    if (keepSelected && !anySelected) return;
    RemoveOccurrences(remove, -1, all ? L"clear" : keepSelected ? L"keep only selected" : L"remove");
}

void RemoveDuplicateTracks() {
    if (!Active().Editable()) return;
    std::unordered_set<TrackId> seen;
    std::vector<char> remove(Active().items.size(), 0);
    for (size_t i = 0; i < remove.size(); ++i) remove[i] = !seen.insert(Active().items[i]).second;
    size_t count = RemoveOccurrences(remove, -1, L"remove duplicates");
    ShowMessage(L"Removed " + std::to_wstring(count) + L" duplicate(s).");
}

bool CanStepPlaylistEdit(bool redo, std::wstring* description) {
    const auto& stack = redo ? Active().history.redo : Active().history.undo;
    if (!Active().Editable() || stack.empty()) return false;
    if (description) *description = (redo ? L"Redo " : L"Undo ") + stack.back().what;
    return true;
}

// Puts the active playlist back as it was before (or after) its last change; Now Playing's
// position follows its track into the restored list.
void StepPlaylistEdit(bool redo) {
    std::wstring description;
    if (!CanStepPlaylistEdit(redo, &description)) return;
    Playlist& pl = Active();
    std::vector<uint32_t> mapping;
    if (!(redo ? RedoPlaylistEdit(pl, mapping) : UndoPlaylistEdit(pl, mapping))) return;
    if (pl.kind == PlaylistKind::NowPlaying) {
        std::vector<size_t> moved(mapping.size());
        for (size_t i = 0; i < mapping.size(); ++i) moved[i] = mapping[i] == UINT32_MAX ? SIZE_MAX : mapping[i];
        RemapNowPlaying(moved);
    }
    g.libraryDirty = true;
    g.rows.clear(); g.rowItems.clear(); g.selected.clear(); g.focus = g.anchor = -1;
    RebuildRows();
    ShowMessage(description + L".");
}

void UndoLatest() {
    uint64_t tagTime = NewestTagEditTime();
    if (CanStepPlaylistEdit(false) && (!tagTime || Active().history.undo.back().when >= tagTime)) StepPlaylistEdit(false);
    else UndoLastTagEdit();
}

void CheckDeadItems() {
    if (!Active().Editable() || g_playlistFileBusy || g_playlistImport) return;
    if (!Active().identity) Active().identity = g_nextPlaylistIdentity++;
    auto request = std::make_unique<PlaylistFile>();
    request->checkDead = true;
    request->target = Active().identity;
    request->snapshot = Active().items;
    std::unordered_set<TrackId> seen;
    for (TrackId id : request->snapshot)
        if (g_lib.Contains(id) && seen.insert(id).second) request->checkedPaths.push_back({id, g_lib.Path(id)});
    OnPlaylistFile(0, (LPARAM)request.release());
}

void FindDuplicateAudio() {
    if (g_playlistFileBusy || g_playlistImport) return;
    auto request = std::make_unique<PlaylistFile>();
    request->findDuplicates = true;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        const Track& t = g_lib[id];
        if (t.audioHash && !(t.flags & Track::kMissing) && (t.flags & (Track::kInLibrary | Track::kInbox)))
            request->hashes.push_back({t.audioHash, id});
    }
    OnPlaylistFile(0, (LPARAM)request.release());
}

void AddSteamSoundtracks() {
    if (g_playlistFileBusy || g_playlistImport) return;
    auto request = std::make_unique<PlaylistFile>();
    request->findSteam = true;
    request->watchedFolders = g.settings.folders;
    request->steamSongs = SnapshotSteamSongs(g_lib);
    OnPlaylistFile(0, (LPARAM)request.release());
}

std::vector<std::wstring> NewSteamFolders(const std::vector<std::wstring>& folders, const std::atomic<bool>& cancel,
                                        const std::vector<std::wstring>* discovered) {
    auto roots = discovered ? *discovered : FindSteamMusicFolders(&cancel);
    std::vector<std::wstring> watched, result;
    for (const auto& folder : folders) {
        if (cancel) return {};
        char* resolved = realpath(compat::Narrow(folder.c_str()).c_str(), nullptr);
        watched.push_back(resolved ? compat::Widen(resolved) : folder);
        std::free(resolved);
    }
    for (const auto& root : roots) {
        if (cancel) return {};
        bool covered = std::any_of(watched.begin(), watched.end(), [&](const auto& folder) {
            return root == folder || (!folder.empty() && root.size() > folder.size() &&
                   root.compare(0, folder.size(), folder) == 0 &&
                   (root[folder.size()] == L'/' || folder.back() == L'/'));
        });
        if (!covered) result.push_back(root);
    }
    return result;
}

void RevealPlaying() {
    int np = NowPlayingIndex();
    if (g.playing == kNone || np < 0) return;
    if (g.active != np) SetActiveTab(np);
    int heard = HeardIndex();
    if (heard < 0) return;
    for (size_t i = 0; i < g.rowItems.size(); ++i)
        if (g.rowItems[i] == (size_t)heard) { MoveFocus((int)i, false); break; }
}

void UpdateInboxTab() {
    size_t count = 0;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if ((g_lib[id].flags & (Track::kInbox | Track::kMissing)) == Track::kInbox) ++count;
    auto it = std::find_if(g.playlists.begin(), g.playlists.end(), [](const Playlist& p) { return p.kind == PlaylistKind::Inbox; });
    int index = it == g.playlists.end() ? -1 : (int)(it - g.playlists.begin());
    if (count && index < 0) {
        CloseMenus();
        index = std::min(1, (int)g.playlists.size());
        g.playlists.insert(g.playlists.begin() + index, Playlist{L"Inbox", {}, PlaylistKind::Inbox});
        if (g.active >= index) ++g.active;
        if (g_renameIndex >= index) ++g_renameIndex;
        for (auto& closed : g_closedPlaylists) if (closed.index >= index) ++closed.index;
        g.libraryDirty = true;
    } else if (!count && index >= 0 && index != g.active) {
        CloseMenus();
        g.playlists.erase(g.playlists.begin() + index);
        if (g.active > index) --g.active;
        if (g_renameIndex > index) --g_renameIndex;
        for (auto& closed : g_closedPlaylists) if (closed.index > index) --closed.index;
        g.libraryDirty = true;
        index = -1;
    }
    if (index >= 0) g.playlists[index].name = count ? L"Inbox (" + std::to_wstring(count) + L")" : L"Inbox";
    g.redraw = true;
}

void InvalidateCellCache() {
    g_cells.clear();
    g_cellsRevision = g_lib.Revision();
}

// The playing track, when Now Playing lists only its album (as app_playlists.cpp on Windows): Now
// Playing is open, the track has an album tag, no search is typed and Preferences leaves it on.
// kNone otherwise, and the whole list shows. Linux always plays from Now Playing.
TrackId NowPlayingAlbumTrack() {
    if (!g.settings.npAlbumOnly || AlternateMode() || InternetRadio().active || g.playing == kNone ||
        g.playing >= g_lib.Count() || g.playlists.empty())
        return kNone;
    if (Active().kind != PlaylistKind::NowPlaying || !g.search.empty()) return kNone;
    return g_lib[g.playing].album ? g.playing : kNone;
}

TrackId g_npAlbumTrack = kNone;  // the track whose album the rows were narrowed to

void FollowNowPlayingAlbum() {
    TrackId want = NowPlayingAlbumTrack(), shown = g_npAlbumTrack;
    bool same = want == shown ||
        (want != kNone && shown != kNone && shown < g_lib.Count() && SameAlbum(g_lib[want], g_lib[shown]));
    if (!same) RebuildRows();
    else g_npAlbumTrack = want;
}

void RebuildRows() {
    if (g.playlists.empty()) return;
    g.active = std::clamp(g.active, 0, (int)g.playlists.size() - 1);
    Playlist& pl = Active();
    std::vector<TrackId> source = PlaylistTracks(pl);
    if (pl.kind == PlaylistKind::Library && !AlternateMode()) FilterLibraryTracks(source);  // the column browser, before the search
    if (pl.kind == PlaylistKind::Library) FilterAlternateLibraryTracks(source);
    g_sourceCount = source.size();
    // Use the Windows search matcher, including containing-folder paths and quoted phrases.
    const auto words = FilterWords(AlternateMode() ? std::wstring{} : g.search);
    TrackMatcher matcher(g_lib, words);
    std::vector<size_t> occurrences;
    occurrences.reserve(source.size());
    g_npAlbumTrack = NowPlayingAlbumTrack();
    const Track* album = g_npAlbumTrack != kNone ? &g_lib[g_npAlbumTrack] : nullptr;
    for (size_t i = 0; i < source.size(); ++i)
        if (source[i] < g_lib.Count() && (!album || SameAlbum(g_lib[source[i]], *album)) && matcher.Matches(source[i]))
            occurrences.push_back(i);
    // A column's sort: its sort format compared naturally (ui_spec.h, as on Windows).
    if (pl.sortColumn > 0 && pl.sortColumn < ColumnTotal()) {
        const wchar_t* format = ColumnAt(pl.sortColumn).sort;
        std::unordered_map<TrackId, std::wstring> keys;
        for (size_t i : occurrences) keys.emplace(source[i], FormatTitle(format, Lookup(source[i], false)));
        bool asc = pl.sortAscending;
        std::stable_sort(occurrences.begin(), occurrences.end(), [&](size_t a, size_t b) {
            int r = CompareNatural(keys[source[a]], keys[source[b]]);
            return asc ? r < 0 : r > 0;
        });
    }
    // Editable playlists distinguish duplicate occurrences; library views use track IDs.
    std::vector<size_t> sel;
    bool editable = pl.Editable() && g.rowItems.size() == g.rows.size();
    if (g.selected.size() == g.rows.size())
        for (size_t i = 0; i < g.rows.size(); ++i)
            if (g.selected[i]) sel.push_back(editable ? g.rowItems[i] : g.rows[i]);
    size_t focusKey = g.focus >= 0 && g.focus < (int)g.rows.size() ?
                      (editable ? g.rowItems[g.focus] : g.rows[g.focus]) : SIZE_MAX;
    std::sort(sel.begin(), sel.end());
    g.rows.clear();
    g.rows.reserve(occurrences.size());
    for (size_t i : occurrences) g.rows.push_back(source[i]);
    g.rowItems.swap(occurrences);
    g.selected.assign(g.rows.size(), 0);
    g.focus = -1;
    for (size_t i = 0; i < g.rows.size(); ++i) {
        size_t key = pl.Editable() ? g.rowItems[i] : g.rows[i];
        if (std::binary_search(sel.begin(), sel.end(), key)) g.selected[i] = 1;
        if (key == focusKey && g.focus < 0) g.focus = (int)i;
    }
    if (g.anchor >= (int)g.rows.size()) g.anchor = -1;
    BuildLines();
    CanvasRebuild();
    g.rowsRevision = g_lib.Revision();
    g_lastRebuild = SDL_GetTicks();
    ClampScroll();
    g.redraw = true;
}

void SetActiveTab(int tab) {
    if (tab < 0 || tab >= (int)g.playlists.size()) return;
    LeaveRadio();
    bool leavingInbox = !g.playlists.empty() && Active().kind == PlaylistKind::Inbox && tab != g.active;
    if (g_drag == Drag::Tracks) SDL_CaptureMouse(false);
    if (g_drag == Drag::Tracks) g_drag = Drag::None;
    g_trackDrag = {};
    EndRename(true);
    g.active = tab;
    if (Active().kind == PlaylistKind::Auto) {  // the shared auto-playlist tab now shows this one
        if (!Active().identity) Active().identity = g_nextPlaylistIdentity++;
        g_autoTabShown = Active().identity;
    }
    if (leavingInbox) UpdateInboxTab();
    g.search.clear();
    g.selected.clear();
    g.rows.clear();
    g.rowItems.clear();
    g.focus = g.anchor = -1;
    g.scroll = 0;
    RebuildRows();
    if (Active().kind == PlaylistKind::NowPlaying) {
        int at = g.playing != kNone ? HeardIndex() : g.queuePos;
        if (at >= 0)
            for (size_t i = 0; i < g.rowItems.size(); ++i)
                if (g.rowItems[i] == (size_t)at) { EnsureVisible((int)i); break; }
    }
}

}  // namespace lx
