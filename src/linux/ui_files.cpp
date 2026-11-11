// The Linux window's files: folder and playlist pickers, playlist editing and the background imports.
#include "ui_internal.h"

namespace lx {

void FilesChosen(void* data, const char* const* list, int) {
    auto* request = static_cast<PlaylistFile*>(data);
    if (list && !*list) { delete request; return; }
    if (!list) request->failed = true;
    else for (; *list; ++list) request->inputs.push_back(compat::Widen(*list));
    if (!PostMessageW(nullptr, WM_APP_PLAYLIST_FILE, 0, (LPARAM)request)) delete request;
}

void FolderDialogCallback(void*, const char* const* list, int) {
    auto* folders = new std::vector<std::wstring>();
    if (list)
        for (; *list; ++list) folders->push_back(compat::Widen(*list));
    if (!PostMessageW(nullptr, WM_APP + 30, 0, (LPARAM)folders)) delete folders;
}

void ChooseFolder() {
    std::string music = MusicFolder();
    SDL_ShowOpenFolderDialog(FolderDialogCallback, nullptr, g.window, music.c_str(), true);
}

std::unique_ptr<PlaylistFile> NewPathImport() {
    if (g_playlistFileBusy || g_playlistImport) { ShowMessage(L"A playlist file is already being processed."); return {}; }
    int target = g.active;
    if (!Active().Editable()) {
        auto it = std::find_if(g.playlists.begin(), g.playlists.end(), [](const Playlist& p) { return p.kind == PlaylistKind::Library; });
        if (it == g.playlists.end()) return {};
        target = (int)(it - g.playlists.begin());
        SetActiveTab(target);
    }
    if (!g.playlists[target].identity) g.playlists[target].identity = g_nextPlaylistIdentity++;
    auto request = std::make_unique<PlaylistFile>();
    request->addToExisting = true;
    request->target = g.playlists[target].identity;
    return request;
}

void ChooseImportFolder() {
    auto request = NewPathImport();
    if (!request) return;
    std::string music = MusicFolder();
    SDL_ShowOpenFolderDialog(FilesChosen, request.release(), g.window, music.c_str(), true);
}

void ChooseFiles() {
    auto request = NewPathImport();
    if (!request) return;
    static const SDL_DialogFileFilter filter{"Audio files", "flac;mp3;m4a;mp4;aac;ogg;oga;opus;wav;aiff;aif;wv;ape;mka;mpc;cord;cue;m3u;m3u8;pls"};
    std::string music = MusicFolder();
    SDL_ShowOpenFileDialog(FilesChosen, request.release(), g.window, &filter, 1, music.c_str(), true);
}

void PlaylistFileChosen(void* data, const char* const* list, int) {
    auto* request = static_cast<PlaylistFile*>(data);
    if (list && !*list) { delete request; return; }
    if (!list) request->failed = true;
    else request->path = compat::Widen(*list);
    if (!PostMessageW(nullptr, WM_APP_PLAYLIST_FILE, 0, (LPARAM)request)) delete request;
}

void ChoosePlaylistFile(bool save) {
    if (g_playlistFileBusy || g_playlistImport) { ShowMessage(L"A playlist file is already being processed."); return; }
    auto request = std::make_unique<PlaylistFile>();
    request->save = save;
    if (save) {
        Playlist snapshot = Active();
        snapshot.items = PlaylistTracks(snapshot);
        SortPlaylistTracks(snapshot.items, snapshot);
        if (!CaptureM3U(snapshot, request->entries)) { ShowMessage(L"The playlist contains an unknown track."); return; }
    }
    static const SDL_DialogFileFilter filters[] = {{"Playlists", "m3u;m3u8;pls"}, {"PLS playlist", "pls"}};
    std::string location = MusicFolder();
    if (save) {
        location += "/playlist.m3u8";  // a name ending in .pls saves a PLS instead
        SDL_ShowSaveFileDialog(PlaylistFileChosen, request.release(), g.window, filters, 2, location.c_str());
    } else SDL_ShowOpenFileDialog(PlaylistFileChosen, request.release(), g.window, filters, 1, location.c_str(), false);
}

void EndRename(bool commit) {
    if (g_renameIndex < 0) return;
    if (commit && g_renameIndex < (int)g.playlists.size() && g_renameText.find_first_not_of(L" \t") != std::wstring::npos) {
        std::wstring name = CleanPlaylistName(g_renameText);
        bool duplicate = false;
        for (int i = 0; i < (int)g.playlists.size(); ++i)
            if (i != g_renameIndex && CompareStringOrdinal(g.playlists[i].name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL)
                duplicate = true;
        g.playlists[g_renameIndex].name = duplicate ? UniquePlaylistName(g.playlists, name) : name;
        g.libraryDirty = true;
    }
    g_renameIndex = -1;
    g_renameText.clear();
    SDL_StopTextInput(g.window);
    g.redraw = true;
}

void BeginRename(int index) {
    if (index < 0 || index >= (int)g.playlists.size()) return;
    if (g.playlists[index].kind == PlaylistKind::Library || g.playlists[index].kind == PlaylistKind::Inbox) return;
    EndRename(true);
    g.searchFocused = false;
    g_renameIndex = index;
    g_renameText = g.playlists[index].name;
    g_renameSelected = true;
    g_renameCaret = g_renameText.size();
    SDL_StartTextInput(g.window);
    g.redraw = true;
}

void NewPlaylist() {
    EndRename(true);
    g.playlists.push_back(Playlist{UniquePlaylistName(g.playlists, L"New Playlist"), {}, PlaylistKind::Static});
    g.libraryDirty = true;
    SetActiveTab((int)g.playlists.size() - 1);
    BeginRename(g.active);
}

void DuplicatePlaylist() {
    EndRename(true);
    Playlist copy = Active();
    copy.items = PlaylistTracks(copy);
    copy.name = UniquePlaylistName(g.playlists, copy.name + L" copy");
    copy.kind = PlaylistKind::Static;
    copy.rules.clear();
    copy.position = {};
    copy.identity = 0;
    int index = g.active + 1;
    g.playlists.insert(g.playlists.begin() + index, std::move(copy));
    g.libraryDirty = true;
    SetActiveTab(index);
}

void ClosePlaylist(int index) {
    if (index < 0 || index >= (int)g.playlists.size()) return;
    PlaylistKind kind = g.playlists[index].kind;
    if (kind != PlaylistKind::Static && kind != PlaylistKind::Auto && kind != PlaylistKind::NowPlaying) return;
    if (kind == PlaylistKind::NowPlaying && g.playing != kNone) {
        ShowMessage(L"Stop playback before closing Now Playing.");
        return;
    }
    EndRename(true);
    if (kind == PlaylistKind::NowPlaying) {
        g.queuePos = -1;
        g.shuffleOrder.clear();
        g.shufflePos = -1;
    }
    g_closedPlaylists.push_back({std::move(g.playlists[index]), index});
    size_t retained = 0;
    for (const auto& closed : g_closedPlaylists) retained += closed.playlist.items.size();
    while (g_closedPlaylists.size() > 8 || (retained > 200000 && g_closedPlaylists.size() > 1)) {
        retained -= g_closedPlaylists.front().playlist.items.size();
        g_closedPlaylists.pop_front();
    }
    g.playlists.erase(g.playlists.begin() + index);
    int active = g.active > index ? g.active - 1 : std::min(g.active, (int)g.playlists.size() - 1);
    g.libraryDirty = true;
    SetActiveTab(active);
    ShowMessage(L"Playlist closed. Ctrl+Shift+T reopens it.");
}

void ReopenPlaylist() {
    if (g_closedPlaylists.empty()) return;
    EndRename(true);
    ClosedPlaylist closed = std::move(g_closedPlaylists.back());
    g_closedPlaylists.pop_back();
    if (closed.playlist.kind == PlaylistKind::NowPlaying && NowPlayingIndex() >= 0) {
        // Now Playing was made again since it closed: the old tracks go after the new ones.
        const int np = NowPlayingIndex();
        auto& items = g.playlists[np].items;
        items.insert(items.end(), closed.playlist.items.begin(), closed.playlist.items.end());
        g.libraryDirty = true;
        SetActiveTab(np);
        ShowMessage(L"Now Playing reopened; its earlier tracks are after the new ones.");
        return;
    }
    int index = std::clamp(closed.index, 1, (int)g.playlists.size());
    closed.playlist.name = UniquePlaylistName(g.playlists, closed.playlist.name);
    g.playlists.insert(g.playlists.begin() + index, std::move(closed.playlist));
    g.libraryDirty = true;
    SetActiveTab(index);
}

// One tab over: past the shared auto-playlist tab means past all of its playlists, and one of those
// stays where it is, since that tab keeps its place.
void ReorderPlaylist(int direction) {
    EndRename(true);
    const auto slots = CurrentTabSlots();
    int tab = TabOfPlaylist(g.playlists, slots, g.active), next = tab + direction;
    if (IsAutoGroupTab(g.playlists, slots, tab) || next < 0 || next >= (int)slots.size()) return;
    int target = slots[next];
    if (IsAutoGroupTab(g.playlists, slots, next))
        for (int i = 0; i < (int)g.playlists.size(); ++i)
            if (g.playlists[i].kind == PlaylistKind::Auto && (direction > 0 || target > i)) target = i;
    if (g.active <= 0 || target <= 0 || !MovePlaylist(g.playlists, g.active, target)) return;
    g.active = target;
    g.libraryDirty = true;
    g.redraw = true;
}

void ToggleMute() {
    if (g_muteVolume > 0) {
        SetVolume(g_muteVolume);
        g_muteVolume = 0;
    } else if (g.settings.volume > 0) {
        g_muteVolume = g.settings.volume;
        SetVolume(0);
    }
}

// Files dropped on the window, once expanded: into the active playlist, or Now Playing from another tab.
void AddDropped(const std::vector<std::wstring>& files) {
    std::vector<TrackId> ids;
    for (auto& f : files) ids.push_back(g_lib.Add(f));
    std::vector<TrackId> unread;
    for (TrackId id : ids)
        if (!g_lib[id].Loaded()) unread.push_back(id);
    QueueTags(unread);
    if (Active().kind == PlaylistKind::Static) AddToPlaylist(g.active, ids);
    else InsertIntoNowPlaying(ids, false);
}

void MaybeOfferSteamSoundtracks() {
    if (g.settings.steamOffered || g_steamOfferWorker.joinable()) return;
    auto request = std::make_unique<PlaylistFile>();
    request->watchedFolders = g.settings.folders;
    g_steamOfferCancel = false;
    g_steamOfferWorker = std::thread([request = std::move(request)]() mutable {
        request->paths = NewSteamFolders(request->watchedFolders, g_steamOfferCancel);
        request->cancelled = g_steamOfferCancel;
        auto* result = request.release();
        if (!PostMessageW(nullptr, WM_APP_PLAYLIST_FILE, 2, (LPARAM)result)) delete result;
    });
}

void OnPlaylistFile(WPARAM kind, LPARAM value) {
    std::unique_ptr<PlaylistFile> request((PlaylistFile*)value);
    if (g_playlistFileStopping) return;
    if (kind == 2) {
        if (g_steamOfferWorker.joinable()) g_steamOfferWorker.join();
        if (!request->cancelled && !g.settings.steamOffered && request->watchedFolders == g.settings.folders &&
            !request->paths.empty()) {
            g.settings.steamOffered = true;
            SaveSettings();
            ShowMessage(L"Found Steam soundtracks: Library \x25B8 Add Steam soundtracks adds them to your library.");
        }
        return;
    }
    if (!kind) {
        if (request->failed) { ShowMessage(L"Couldn't open the playlist file dialog."); return; }
        if (g_playlistFileBusy || g_playlistImport) { ShowMessage(L"A playlist file is already being processed."); return; }
        if (g_playlistFileWorker.joinable()) g_playlistFileWorker.join();
        g_playlistFileBusy = true;
        g_checkingDead = request->checkDead;
        g_checkingDuplicates = request->findDuplicates;
        g_checkingSteam = request->findSteam;
        g_playlistFileCancel = false;
        ShowMessage(request->checkDead ? L"Checking for missing files..." : request->findDuplicates ? L"Finding duplicate audio..." :
                    request->findSteam ? L"Looking for Steam soundtracks..." :
                    request->addToExisting ? L"Finding audio files..." :
                    request->save ? L"Saving playlist..." : L"Opening playlist...");
        g_playlistFileWorker = std::thread([request = std::move(request)]() mutable {
            if (request->findSteam) {
                request->steam.folders = FindSteamMusicFolders(&g_playlistFileCancel);
                request->ok = PrepareSteamImport(request->steam, std::move(request->steamSongs), &g_playlistFileCancel);
                request->paths = NewSteamFolders(request->watchedFolders, g_playlistFileCancel, &request->steam.folders);
                request->cancelled = g_playlistFileCancel;
            } else if (request->findDuplicates) {
                request->duplicateGroups = GroupDuplicateAudio(std::move(request->hashes));
                request->cancelled = g_playlistFileCancel;
                request->ok = true;
            } else if (request->checkDead) {
                for (const auto& [id, path] : request->checkedPaths) {
                    if (g_playlistFileCancel) { request->cancelled = true; break; }
                    if (GetFileAttributesW(ContainerOf(AudioFileOf(path)).c_str()) == INVALID_FILE_ATTRIBUTES) {
                        DWORD error = GetLastError();
                        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) request->missing.push_back({id, path});
                    }
                }
                if (g_playlistFileCancel) request->cancelled = true;
                request->ok = true;
            } else if (request->addToExisting) {
                for (const auto& path : request->inputs) {
                    if (g_playlistFileCancel) { request->cancelled = true; break; }
                    if (!CollectAudioFiles(path, request->paths, &g_playlistFileCancel)) ++request->failedInputs;
                }
                request->ok = !request->paths.empty() || !request->failedInputs;
            } else request->ok = request->save ? ExportM3U(request->path, request->entries) : ImportM3U(request->path, request->paths);
            auto* result = request.release();
            if (!PostMessageW(nullptr, WM_APP_PLAYLIST_FILE, 1, (LPARAM)result)) delete result;
        });
        return;
    }
    if (g_playlistFileWorker.joinable()) g_playlistFileWorker.join();
    g_playlistFileBusy = false;
    g_checkingDead = false;
    g_checkingDuplicates = false;
    g_checkingSteam = false;
    if ((request->checkDead || request->findDuplicates || request->findSteam) && g_playlistFileCancel) request->cancelled = true;
    if (request->cancelled) { ShowMessage(L"Search or import cancelled."); return; }
    if (!request->ok) {
        ShowMessage(request->findSteam ? L"Couldn't search for Steam soundtracks." :
                    request->addToExisting ? L"Couldn't read the selected files or folders." :
                    request->save ? L"Couldn't save the playlist. The original file was kept." : L"Couldn't open the playlist file.");
        return;
    }
    if (request->save) { ShowMessage(L"Playlist saved."); return; }
    if (request->findSteam) {
        if (request->watchedFolders != g.settings.folders) {
            ShowMessage(L"Library folders changed while searching for Steam soundtracks. Try again.");
            return;
        }
        int formats = 2;
        if (request->steam.multipleFormats) {
            SDL_MessageBoxButtonData buttons[] = {{0, 1, "Keep each format"},
                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 2, "Keep one copy"},
                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"}};
            SDL_MessageBoxData dialog{SDL_MESSAGEBOX_INFORMATION, g.window, "Add Steam soundtracks",
                "Steam soundtracks include more than one audio format.\n\n"
                "Keep a copy of each song in every format, or keep one copy?\n"
                "Existing Library copies are kept first. Cancel leaves the Library unchanged.",
                3, buttons, nullptr};
            formats = 0;
            SDL_ShowMessageBox(&dialog, &formats);
            if (!formats) return;
        }
        const size_t skipped = SkipSteamDuplicates(g_lib, request->steam, formats == 1);
        g.libraryDirty = true;
        g.redraw = true;
        UpdateInboxTab();
        g.settings.steamOffered = true;
        size_t count = request->paths.size();
        if (!count) {
            SaveSettings();
            StartScanner();
            ShowMessage(skipped ? L"Skipped " + std::to_wstring(skipped) + L" duplicate Steam tracks."
                                : L"No new Steam soundtrack folders found.");
            return;
        }
        g.settings.folders.insert(g.settings.folders.end(), request->paths.begin(), request->paths.end());
        SaveSettings();
        StartScanner();
        ShowMessage(L"Added " + std::to_wstring(count) + (count == 1 ? L" Steam soundtrack folder." : L" Steam soundtrack folders.") +
                    L" Skipped " + std::to_wstring(skipped) + L" duplicate tracks.");
        return;
    }
    if (request->findDuplicates) {
        std::vector<TrackId> tracks;
        for (const auto& group : request->duplicateGroups) {
            uint64_t hash = 0;
            for (TrackId id : group) {
                if (!g_lib.Contains(id) || !g_lib[id].audioHash || (g_lib[id].flags & Track::kMissing) ||
                    !(g_lib[id].flags & (Track::kInLibrary | Track::kInbox)) || (hash && hash != g_lib[id].audioHash)) {
                    ShowMessage(L"The library changed while finding duplicates. Try again.");
                    return;
                }
                hash = g_lib[id].audioHash;
                tracks.push_back(id);
            }
        }
        if (tracks.empty()) { ShowMessage(L"No duplicate audio found. cordinal recognizes identical audio in FLAC files."); return; }
        size_t groups = request->duplicateGroups.size();
        g.playlists.push_back(Playlist{UniquePlaylistName(g.playlists, L"Duplicate audio"), std::move(tracks), PlaylistKind::Static});
        g.libraryDirty = true;
        SetActiveTab((int)g.playlists.size() - 1);
        ShowMessage(std::to_wstring(groups) + (groups == 1 ? L" song is" : L" songs are") + L" in the library more than once.");
        return;
    }
    if (request->checkDead) {
        auto target = std::find_if(g.playlists.begin(), g.playlists.end(), [&](const Playlist& p) { return p.identity == request->target; });
        bool unchanged = target != g.playlists.end() && target->items == request->snapshot;
        for (const auto& [id, path] : request->checkedPaths)
            if (!g_lib.Contains(id) || g_lib.Path(id) != path) unchanged = false;
        if (!unchanged) { ShowMessage(L"The playlist changed while checking; no tracks were removed. Try again."); return; }
        std::unordered_set<TrackId> missing;
        for (const auto& [id, path] : request->missing) missing.insert(id);
        std::vector<char> remove(target->items.size(), 0);
        for (size_t i = 0; i < remove.size(); ++i) remove[i] = missing.contains(target->items[i]);
        size_t count = RemoveOccurrences(remove, (int)(target - g.playlists.begin()), L"remove dead items");
        ShowMessage(L"Removed " + std::to_wstring(count) + L" dead item(s).");
        return;
    }
    if (request->addToExisting) { g_playlistImport = std::move(request); return; }
    Playlist playlist{UniquePlaylistName(g.playlists, StemOf(request->path)), {}, PlaylistKind::Static};
    playlist.identity = g_nextPlaylistIdentity++;
    request->target = playlist.identity;
    g.playlists.push_back(std::move(playlist));
    g.libraryDirty = true;
    SetActiveTab((int)g.playlists.size() - 1);
    g_playlistImport = std::move(request);
}

bool PlaylistImportPending() { return g_playlistImport != nullptr; }

void TickPlaylistImport() {
    if (!g_playlistImport) return;
    auto& request = *g_playlistImport;
    auto found = std::find_if(g.playlists.begin(), g.playlists.end(), [&](const Playlist& p) { return p.identity == request.target; });
    if (found == g.playlists.end()) {
        g_playlistImport.reset();
        ShowMessage(L"Playlist import stopped because its tab was closed.");
        return;
    }
    if (request.done == 0 && found->kind != PlaylistKind::Library) request.undoBefore = found->items;
    size_t end = std::min(request.paths.size(), request.done + 256);
    std::vector<TrackId> unread;
    std::vector<TrackId> joined;
    bool inboxChanged = false;
    for (; request.done < end; ++request.done) {
        TrackId id = g_lib.Add(request.paths[request.done]);
        if (id == kInvalidTrack) continue;
        if (found->kind == PlaylistKind::Library) {
            Track& t = g_lib[id];
            inboxChanged |= (t.flags & Track::kInbox) != 0;
            t.flags = (uint16_t)((t.flags | Track::kInLibrary) & ~(Track::kExcluded | Track::kMissing | Track::kInbox));
            if (!(t.flags & Track::kCollected)) joined.push_back(id);
            g_lib.Touch();
        } else found->items.push_back(id);
        ++request.added;
        if (!g_lib[id].Loaded()) unread.push_back(id);
    }
    QueueTags(unread);
    AnnouncePoints(AwardLibraryAdds(joined));
    g.libraryDirty = true;
    if (request.done == request.paths.size()) {
        // The whole import is one step to undo, however many batches it took.
        if (found->kind != PlaylistKind::Library) RecordPlaylistEdit(*found, std::move(request.undoBefore), L"add");
        size_t count = request.added, failed = request.failedInputs;
        bool added = request.addToExisting;
        if ((int)(found - g.playlists.begin()) == g.active) RebuildRows();
        g_playlistImport.reset();
        ShowMessage((added ? L"Added " : L"Opened playlist with ") + std::to_wstring(count) +
                    (count == 1 ? L" track." : L" tracks.") +
                    (failed ? L" Couldn't read " + std::to_wstring(failed) + L" input(s)." : L""));
    } else if (request.done % 4096 == 0) {
        ShowMessage(L"Opening playlist: " + std::to_wstring(request.done) + L" of " + std::to_wstring(request.paths.size()) + L" tracks.");
    }
    if (inboxChanged) UpdateInboxTab();
}

void StopPlaylistFiles() {
    g_playlistFileStopping = true;
    g_playlistFileCancel = true;
    g_steamOfferCancel = true;
    if (g_playlistFileWorker.joinable()) g_playlistFileWorker.join();
    if (g_steamOfferWorker.joinable()) g_steamOfferWorker.join();
    g_playlistImport.reset();
}

void OpenLibraryFolders() {
    g_folders.open = true;
    g.redraw = true;
}

void OpenWebServer() {
    g_web.open = true;
    g_web.port = std::to_string(g.settings.webPort);
    g_web.fixed = g.settings.webCode;
    g_web.focus = 1;
    g.redraw = true;
}

}  // namespace lx
