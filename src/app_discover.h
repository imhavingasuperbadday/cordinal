// Discover outside my library in Keep playing like this: one unowned song, found on
// MusicBrainz and downloaded for preview by the web-save queue. It queues like any other track,
// shows in Now Playing and the queue, and is not written into the library or counted as a play
// until Ctrl+Alt+K keeps it. Turning the preference or radio off stops new looks; a preview file is
// deleted once it plays or is skipped.
#pragma once
#include "library.h"

struct WebTrack;

namespace app {

// Starts a look when radio has been on long enough and nothing is already waiting. Called when a
// local track starts, and when Keep playing like this is turned on.
void DiscoverMaybe();
// Registers a downloaded preview (for Keep playing like this or unified online search) so it
// never counts as a play and is cleaned up when unreferenced, until kept in the library.
void DiscoverRegisterPreview(TrackId id, const WebTrack& track, const std::wstring& file, const std::wstring& dir);
// Adds a downloaded track to the library (either permanently or as a preview registered above).
TrackId DiscoverAddTrack(const std::wstring& file, WebTrack& track, bool permanent, const std::wstring& dir,
                         const std::string& albumArtist = {}, const std::string& date = {});
void DiscoverRemoveTree(const std::wstring& dir);
// Finds an existing unkept preview matching `(site, id)` whose file is still on disk.
bool DiscoverFindPreview(const std::string& site, const std::string& id, TrackId& trackOut);
// True for the previews this session downloaded (playing, decoding or queued) and not yet kept.
bool DiscoverIsPreview(TrackId id);
// True while a preview can still be kept: playing, decoding or in the queue.
bool DiscoverCanKeep();
// Keeps the current preview (or a specific preview `id`): copies its tagged file into Artist\Album,
// makes it a library track and records it as saved. Safe to call when there is nothing to keep.
void DiscoverKeep();
bool DiscoverKeepTrack(TrackId id, std::wstring* keptOut = nullptr);
// Announces A preview that just became audible, then lets DiscoverMaybe decide when the next
// local track starts. Called from OnEngineEvent's TrackStarted.
void DiscoverTrackStarted();
// Deletes preview files nothing references any more. Runs on the UI tick and at shutdown.
void DiscoverTick();
void DiscoverShutdown();
void DiscoverRemap(const std::vector<TrackId>& mapping);
void OnDiscoverMessage(WPARAM wp, LPARAM lp);

}  // namespace app
