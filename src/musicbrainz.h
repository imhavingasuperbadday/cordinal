// Optional MusicBrainz album-edition lookup. A single background worker asks the MusicBrainz web
// service for a release-group id when two local albums share an artist and base title but carry no
// id in their tags. The UI thread applies results; browsing and playback never wait for the network.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace musicbrainz {

// Starts the idle worker and loads its on-disk cache. `enabled` is the saved preference.
void Start(const std::wstring& dataDir, bool enabled);
void Stop();
void SetEnabled(bool enabled);
bool Enabled();

// UI thread: queues conservative local candidates every few seconds and applies finished lookups.
// Returns true when track metadata or album groups changed.
bool Tick();
bool Busy();

// The result shape is also used by tests and diagnostics.
struct LookupResult {
    std::string artist, title, releaseGroup;
    bool found = false;
};

// The conservative part of a search reply, exposed for the self-test: exactly one high-score,
// matching release group yields its id; no match or two plausible groups yields empty.
std::string ParseReleaseGroupSearch(std::string_view json, std::string_view artist, std::string_view title);

}  // namespace musicbrainz
