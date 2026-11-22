// Discover outside my library on Linux (app_discover.h on Windows): one unowned song, found on
// MusicBrainz and downloaded through the web-save queue for preview, queued like any other track
// and kept only when Ctrl+Alt+K asks.
#pragma once
#include "app.h"

struct WebTrack;

namespace lx {

void DiscoverMaybe();
void DiscoverRegisterPreview(TrackId id, const WebTrack& track, const std::wstring& file, const std::wstring& dir);
bool DiscoverFindPreview(const std::string& site, const std::string& id, TrackId& trackOut);
bool IsDiscoverPreview(TrackId id);
bool DiscoverCanKeep();
void DiscoverKeep();
bool DiscoverKeepTrack(TrackId id, std::wstring* keptOut = nullptr);
void DiscoverTrackStarted();
void DiscoverTick();
void DiscoverShutdown();
void OnDiscoverMessage(WPARAM wp, LPARAM lp);

}  // namespace lx
