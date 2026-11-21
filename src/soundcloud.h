// SoundCloud links: a track, a set, someone's likes or an artist's tracks page,
// resolved through api-v2.soundcloud.com with the client id the site's own pages use.
// A track saves only when it has a progressive (plain HTTP) stream; one the site hands
// out only as HLS is reported as hlsOnly instead of saved broken.
#pragma once
#include "webfetch.h"
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace soundcloud {

enum class Kind { None, Track, Playlist, Likes, ArtistTracks };
// soundcloud.com, m.soundcloud.com and on.soundcloud.com short links' targets.
Kind Classify(std::string_view url);

// Pure parsers over api-v2 JSON, fixture-tested and never on the network. ParseTrack
// fills one track - its progressive stream when it has one, unavailable + why when it
// does not (not streamable, blocked, snippet-only). ParseTrackList takes the shapes a
// set, a likes page and an artist's tracks page each answer with. ParseSetTitle reads
// the set's own name.
bool ParseTrack(std::string_view json, WebTrack& out);
size_t ParseTrackList(std::string_view json, std::vector<WebTrack>& out);
bool ParseSetTitle(std::string_view json, std::string& title);

// Reads the whole list a link holds, on the caller's worker thread, one request at a
// time with `cancel` polled around each. `listTitle` is the set's own name, "" for a
// single. Stream URLs come back ready to fetch (the client id is already attached); a
// track whose stream is HLS carries `hls` with an audio/mp4 entry whose init and
// segments join into a real .m4a. A bare artist profile is refused with an error that
// says to add /tracks to it, the same caution soundcli makes you give deliberately.
// `error` is a full sentence.
bool Resolve(const std::string& url, const std::atomic<bool>& cancel,
             std::vector<WebTrack>& tracks, std::string& listTitle, std::string& error,
             const std::function<void(const std::string&)>& notice = {});

// Re-reads one track's stream, for the queue's single retry when a download fails:
// SoundCloud's stream entries are signed and expire in minutes.
bool RefreshTrack(WebTrack& track, const std::atomic<bool>& cancel, std::string& error);

// Saves a track whose stream is HLS (the `hls` flag in webfetch.h): follows the entry's
// signed redirect, then writes the playlist's init segment and every media segment in
// order into one .m4a - a lossless copy of the site's own audio. One request at a time,
// `cancel` polled around each, `progress` 0-100 by segment.
bool DownloadHlsTrack(const WebTrack& track, const std::atomic<bool>& cancel,
                      const std::wstring& file, const std::function<void(int)>& progress,
                      std::string& error);

}  // namespace soundcloud
