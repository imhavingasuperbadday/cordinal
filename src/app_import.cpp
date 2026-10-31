// Manual imports (Add to Library / Add to Playlist / drag-drop): a worker thread expands the
// inputs into files and the UI appends them in bounded batches. The destination playlist is
// remembered by identity, so a batch still lands correctly if the user switches tabs while the
// scan runs, and only a couple of batches are ever in flight so the message queue can't flood.
#include "app.h"

namespace app {

// Blocks until fewer than two batches are in flight, so a huge folder can't queue thousands of
// heap batches at once. Ownership passes to the message on success; on failure the slot is given
// back so the caller's unique_ptr still frees the batch.
bool PostImport(HWND hwnd, std::unique_ptr<ImportBatch> batch) {
    {
        std::unique_lock<std::mutex> lock(g.importMx);
        g.importCv.wait(lock, [] { return g.importQuit || g.importOutstanding < 2; });
        if (g.importQuit) return false;
        ++g.importOutstanding;
    }
    if (PostMessageW(hwnd, WM_APP_IMPORT, 0, (LPARAM)batch.get())) { batch.release(); return true; }
    std::lock_guard<std::mutex> lock(g.importMx);
    --g.importOutstanding;
    return false;
}

// Worker thread. Handles one request at a time: expands each input (directories are walked),
// posts 256-path batches, then a final batch carrying the totals and any cancel flag.
void ImportWorker(HWND hwnd) {
    for (;;) {
        ImportRequest request;
        {
            std::unique_lock<std::mutex> lock(g.importMx);
            g.importCv.wait(lock, [] { return g.importQuit || !g.importQueue.empty(); });
            if (g.importQuit) break;
            request = std::move(g.importQueue.front());
            g.importQueue.pop_front();
            // Set under the lock; only this worker and the UI touch these, and the UI reads them
            // when it needs to know what's running.
            g.importCancel = false;
            g.importRunning = true;
            g.importTarget = request.playlistIdentity;
        }
        size_t added = 0, errors = 0;
        bool first = request.playFirst;
        for (const auto& input : request.inputs) {
            if (g.importCancel) break;
            std::vector<std::wstring> files;
            if (!CollectAudioFiles(input, files, &g.importCancel)) ++errors;
            for (size_t start = 0; start < files.size() && !g.importCancel; start += 256) {
                auto batch = std::make_unique<ImportBatch>();
                batch->playlistIdentity = request.playlistIdentity;
                batch->playFirst = first;
                size_t end = std::min(start + 256, files.size());
                batch->paths.assign(std::make_move_iterator(files.begin() + start), std::make_move_iterator(files.begin() + end));
                added += batch->paths.size();
                first = false;
                if (!PostImport(hwnd, std::move(batch))) return;
            }
        }
        // Closing batch: marks the run done and reports the totals, whether or not it was cancelled.
        auto final = std::make_unique<ImportBatch>();
        final->playlistIdentity = request.playlistIdentity;
        final->final = true;
        final->cancelled = g.importCancel;
        final->added = added;
        final->errors = errors;
        for (const auto& input : request.inputs) {
            const DWORD attributes = GetFileAttributesW(input.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
                final->folders.push_back(input);
        }
        if (!PostImport(hwnd, std::move(final))) return;
        g.importRunning = false;
        g.importTarget = 0;
    }
}

// UI entry point. Flushes the search so the new files aren't hidden by a stale filter, targets the
// active playlist (switching to the library if it isn't editable), and queues one request for the
// worker. Snapshots the playlist identity so results find it again after a tab change.
void AddPaths(const std::vector<std::wstring>& dropped, bool playFirst) {
    if (dropped.empty() || g.closing) return;
    // A pack among them is installed rather than searched for music (app_packs.cpp).
    const std::vector<std::wstring> inputs = TakePacks(dropped);
    if (inputs.empty()) return;
    FlushSearch();
    EnsurePlaylistIdentities();
    if (!Active().Editable()) ActivatePlaylist(0);
    if (!g.importWorker.joinable()) g.importWorker = std::thread(ImportWorker, g.hwnd);
    {
        std::lock_guard<std::mutex> lock(g.importMx);
        g.importQueue.push_back({Active().identity, inputs, playFirst});
    }
    g.importCv.notify_one();
    ShowMessage(L"Finding audio files...");
}

// UI thread. Adds already-resolved files to one playlist. A library playlist marks each track in
// the library (clearing inbox/excluded/missing) and awards points for tracks that newly joined it;
// any other playlist just appends. Tracks whose tags aren't loaded yet are queued for the reader.
void AppendFiles(int playlist, const std::vector<std::wstring>& files, bool playFirst) {
    if (files.size() > (size_t)INT_MAX - g.playlists[playlist].items.size()) { ShowMessage(L"The playlist is full."); return; }
    bool toLibrary = g.playlists[playlist].kind == PlaylistKind::Library;
    auto& items = g.playlists[playlist].items;
    int first = (int)items.size();
    std::vector<TrackId> unloaded, joined;
    size_t addedCount = 0;
    TrackId firstAdded = kNone;
    for (auto& f : files) {
        TrackId id = g_lib.Add(f);
        if (id == kNone) continue;
        ++addedCount;
        Track& t = g_lib[id];
        if (toLibrary) {
            t.flags = (uint16_t)((t.flags | Track::kInLibrary) & ~(Track::kExcluded | Track::kMissing | Track::kInbox));
            if (t.InLibrary() && !(t.flags & Track::kCollected)) joined.push_back(id);
            g_lib.Touch();
        } else {
            items.push_back(id);
        }
        if (firstAdded == kNone) firstAdded = id;
        if (!t.Loaded()) unloaded.push_back(id);
    }
    QueueTags(unloaded);
    if (playlist == g.active) RebuildView(true);
    RefreshTabs();
    RefreshNext();
    g.dirty = true;
    ShowMessage(L"Added " + std::to_wstring(addedCount) + (addedCount == 1 ? L" track" : L" tracks") +
                (toLibrary ? L" to the library." : L"."));
    AnnouncePoints(AwardLibraryAdds(joined));
    if (!playFirst) return;
    if (toLibrary && playlist != g.active) RebuildViewItems(g.playlists[playlist]);
    if (!toLibrary) PlayTrack(playlist, first, true);
    else if (int k = IndexIn(playlist, firstAdded); k >= 0) PlayTrack(playlist, k, true);
}

// UI thread, from WM_APP_IMPORT. Frees the batch, releases a backpressure slot, and files it under
// the playlist whose identity matches. A playlist that was closed or replaced in the meantime is
// dropped, but the final batch still shows the summary.
void OnImportResult(LPARAM lp) {
    std::unique_ptr<ImportBatch> batch(reinterpret_cast<ImportBatch*>(lp));
    if (!batch) return;
    {
        std::lock_guard<std::mutex> lock(g.importMx);
        if (g.importOutstanding) --g.importOutstanding;
    }
    g.importCv.notify_one();
    auto target = std::find_if(g.playlists.begin(), g.playlists.end(), [&](const Playlist& pl) {
        return pl.identity == batch->playlistIdentity;
    });
    if (target != g.playlists.end() && !batch->paths.empty())
        AppendFiles((int)(target - g.playlists.begin()), batch->paths, batch->playFirst && !g.closing);
    if (batch->final) {
        g.compactPending = true;
        std::wstring text = batch->cancelled ? L"Import cancelled. " : L"Import finished. ";
        text += std::to_wstring(batch->added) + L" track(s) found.";
        if (batch->errors) text += L" " + std::to_wstring(batch->errors) + L" input(s) could not be read.";
        const bool toLibrary = target != g.playlists.end() && target->kind == PlaylistKind::Library;
        if (toLibrary && !batch->cancelled && batch->added && NoteUnwatchedFolders(batch->folders))
            text += L" Music added to the folder later won't appear by itself: Library \x25B8 Watch imported "
                    L"folders keeps the library up to date with it.";
        ShowMessage(text);
    }
}

static bool Watched(const std::wstring& folder) {
    return std::any_of(g.libraryFolders.begin(), g.libraryFolders.end(), [&](const std::wstring& l) {
        return PathIsUnder(folder, l) || _wcsicmp(folder.c_str(), l.c_str()) == 0;
    });
}

// Importing copies what a folder holds now; only a library folder is watched for what arrives
// later. Remembers the imported folders no library folder covers and says whether there were any.
bool NoteUnwatchedFolders(const std::vector<std::wstring>& folders) {
    bool any = false;
    for (std::wstring f : folders) {
        while (f.size() > 3 && f.back() == L'\\') f.pop_back();
        if (Watched(f)) continue;
        any = true;
        if (std::none_of(g.importedFolders.begin(), g.importedFolders.end(),
                         [&](const std::wstring& known) { return _wcsicmp(known.c_str(), f.c_str()) == 0; }))
            g.importedFolders.push_back(std::move(f));
    }
    return any;
}

// Library > Watch imported folders: makes the folders imported this session library folders.
void WatchImportedFolders() {
    int added = 0;
    for (const std::wstring& f : g.importedFolders) {
        if (Watched(f)) continue;
        std::erase_if(g.libraryFolders, [&](const std::wstring& l) { return PathIsUnder(l, f); });
        g.libraryFolders.push_back(f);
        ++added;
    }
    g.importedFolders.clear();
    if (!added) return;
    SaveLibrarySettings();
    StartScanner();
    ShowMessage(L"Watching " + std::to_wstring(added) + (added == 1 ? L" folder" : L" folders") +
                L" for new music. Library \x25B8 Library folders lists them.");
}

// Shutdown: stops both workers, then drains any import/art messages still queued (handling them,
// so their heap payloads are freed) instead of leaking the batches they own.
void StopBackgroundJobs() {
    g.closing = true;
    {
        std::lock_guard<std::mutex> lock(g.importMx);
        g.importQuit = true;
        g.importCancel = true;
        g.importQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g.artMx);
        g.artQuit = true;
    }
    g.importCv.notify_all();
    g.artCv.notify_all();
    if (g.importWorker.joinable()) g.importWorker.join();
    if (g.artWorker.joinable()) g.artWorker.join();
    g.importRunning = false;
    g.importTarget = 0;
    MSG pending;
    while (PeekMessageW(&pending, g.hwnd, WM_APP_IMPORT, WM_APP_IMPORT, PM_REMOVE)) OnImportResult(pending.lParam);
    while (PeekMessageW(&pending, g.hwnd, WM_APP_ART, WM_APP_ART, PM_REMOVE)) OnArtResult(pending.lParam);
}

}  // namespace app
