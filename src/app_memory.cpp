// Library compaction: rebuilds the track pool to drop tracks and strings no longer referenced, so
// memory used by a big scan or a since-edited library is given back on its own. Compaction rewrites
// every TrackId, so it may only run when nothing else holds an id: no worker, editor or pending tag
// write can retain one. Work that can leave records unreferenced (an import, a cleared or closed
// playlist, tags read again) sets g.compactPending, and the next idle tick that finds nothing busy
// compacts.
#include "app.h"
#include "playlog.h"

namespace app {

bool ReducedMemoryMode() { return g.size == ViewSize::Compact || MiniPlayerWindow(); }

size_t ViewMemoryBudget(size_t normal) { return ReducedMemoryMode() ? normal / 2 : normal; }

// Only disposable UI resources change with window size. Track ids, library data and the audio
// buffers remain live, so playback and editors are unaffected. The next paint recreates what it needs.
void UpdateViewMemory() {
    ClearCellCache();
    g.cellCache.rehash(0);
    ClearBrowserCaches();
    StopGroupArt();
    LibViewReleaseGraphics();
}

namespace {
// True when an open editor window could still hold a TrackId: the tag and organize dialogs own
// tracks while they are open. Tooltips and ownerless windows are ignored.
BOOL CALLBACK HasEditor(HWND window, LPARAM value) {
    if (window == g.hwnd || window == g.eqWnd || GetAncestor(window, GA_ROOTOWNER) != g.hwnd) return TRUE;
    wchar_t name[128]{};
    GetClassNameW(window, name, (int)std::size(name));
    if (wcscmp(name, TOOLTIPS_CLASSW) == 0 || !GetWindow(window, GW_OWNER)) return TRUE;
    if (wcsncmp(name, L"Cordinal", 8) != 0 && wcscmp(name, L"#32770") != 0) return TRUE;
    *reinterpret_cast<bool*>(value) = true;
    return FALSE;
}
}

// UI thread only. Sets g.compactPending first so a later idle pass retries if anything below is
// busy now; in that case it returns without compacting.
void CompactLibrary() {
    g.compactPending = true;
    bool editor = false;
    EnumThreadWindows(GetCurrentThreadId(), HasEditor, (LPARAM)&editor);
    // A conversion carries TrackIds from its plan to its result after the worker has finished. Until
    // it has been applied, compaction must not renumber them: the result would then name a different
    // track, renaming the wrong library record when the converted file lands.
    if (g.closing || editor || g.scanner.Busy() || ScanInProgress() || g.rgRunning ||
        g.importRunning || TagWriterRetainsTracks() || OrganizeBusy() || ConvertBusy() || !g.tagPending.empty() ||
        !HelperToolRunning().empty())
        return;
    {
        std::lock_guard<std::mutex> lock(g.tagMx);
        if (g.tagBatchInFlight || !g.tagBatch.empty() || !g.tagQueue.empty()) return;
    }
    {
        std::lock_guard<std::mutex> lock(g.importMx);
        if (!g.importQueue.empty() || g.importOutstanding) return;
    }
    CapturePlaylistPosition();
    // Mark every id that must survive: everything referenced by a playlist, the history/queue, the
    // play log and points, or held by the player. Unmarked ids are dropped by Compact below.
    std::vector<bool> retained(g_lib.Count());
    auto mark = [&](TrackId id) { if (g_lib.Contains(id)) retained[id] = true; };
    for (TrackId id = 0; id < g_lib.Count(); ++id) {
        const auto& t = g_lib[id];
        // Exclusions, ratings, collection points and listening stats are intentional library data.
        if ((t.flags & (Track::kInLibrary | Track::kInbox | Track::kMissing | Track::kExcluded | Track::kCollected)) ||
            t.rating || t.playCount || t.skipCount || t.firstPlayed || t.lastPlayed) mark(id);
    }
    for (const auto& pl : g.playlists) if (pl.Editable()) for (TrackId id : pl.items) mark(id);
    for (const auto& closed : g.closedPlaylists) for (TrackId id : closed.playlist.items) mark(id);
    for (const auto& [token, info] : g.tokens) { mark(info.id); mark(info.anchor); }
    for (TrackId id : g.history) mark(id);
    for (TrackId id : g.queue) mark(id);
    for (const auto& event : PlayLog()) mark(event.id);
    for (const auto& event : PointsRecent()) mark(event.id);
    for (TrackId id : {g.playing, g.resumeTrack, g.statTrack, g.artFor}) mark(id);
    std::vector<TrackId> live;
    for (TrackId id = 0; id < retained.size(); ++id) if (retained[id]) live.push_back(id);
    // Browser selections refer to pooled strings; capture text before rebuilding the pool.
    std::vector<std::wstring> browser[3];
    for (int i = 0; i < 3; ++i) for (auto handle : g.panes[i].selected) browser[i].push_back(g_lib.Str(handle));
    // Compact returns old-id -> new-id; kNone marks a dropped track. Every store that holds ids
    // must be remapped through it (or cleared) or it would point at the wrong track.
    InvalidateWebLibrary();
    auto mapping = g_lib.Compact(live);
    auto remap = [&](TrackId& id) { id = id < mapping.size() ? mapping[id] : kNone; };
    auto playlist = [&](Playlist& pl) {
        for (auto& id : pl.items) remap(id);
        std::erase(pl.items, kNone);
        if (!pl.Editable()) {
            for (auto& id : pl.position.selected) remap(id);
            remap(pl.position.focus);
            remap(pl.position.top);
            remap(pl.position.expanded);
        }
        pl.items.shrink_to_fit();
        pl.position.selected.shrink_to_fit();
        pl.builtRevision = 0;
    };
    for (auto& pl : g.playlists) playlist(pl);
    for (auto& closed : g.closedPlaylists) playlist(closed.playlist);
    for (auto& [token, info] : g.tokens) { remap(info.id); remap(info.anchor); }
    for (auto& id : g.history) remap(id);
    for (auto& id : g.queue) remap(id);
    for (TrackId* id : {&g.playing, &g.resumeTrack, &g.statTrack, &g.artFor}) remap(*id);
    ForgetAlbumDynamics(true);  // Compact remapped the measurements; discard only the derived cache
    ForgetApolloProbes();       // its per-id file probes cannot follow the new numbering
    LyricsLibraryChanged();     // and the lyrics panel's loaded lines belong to the old ids
    RemapPlayLog(mapping);
    RemapPoints(mapping);
    ClearBrowserCaches();
    for (int i = 0; i < 3; ++i) {
        g.panes[i].values.clear();
        g.panes[i].selected.clear();
        for (const auto& text : browser[i]) g.panes[i].selected.push_back(g_lib.InternW(text));
    }
    ClearCellCache();
    g.cellCache.rehash(0);  // drop buckets left over from the larger library
    RestorePlaylistPosition();
    LibViewRebuild();
    g.view.shrink_to_fit();
    g.compactPending = false;
    g.dirty = true;
}

}  // namespace app
