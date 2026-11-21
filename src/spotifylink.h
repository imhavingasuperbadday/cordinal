// Spotify links without ever touching Spotify's encrypted stream: the pages say what
// an item is (title, artists, cover, durations, disc and track numbers), read from the
// public embed pages and an album page's initialState blob, and the audio is then
// matched to the same recording on YouTube by the queue. The saved file is a YouTube
// copy of the song, labelled as one - never a decrypt of Spotify's audio.
#pragma once
#include "webfetch.h"
#include "ytaudio.h"
#include <atomic>
#include <string>
#include <string_view>
#include <vector>

namespace spotify {

enum class Kind { None, Track, Album, Playlist };
// open.spotify.com links, with or without the international prefix (open.spotify.com/intl-xx/...).
Kind Classify(std::string_view url);

// Pure parsers, fixture-tested with no network. The embed pages' __NEXT_DATA__ JSON
// holds the item; the album page additionally server-renders an initialState blob with
// the complete track list (ids, durations, disc and track numbers), which Puto reads
// first, so ParseAlbumState is tried before ParseListEmbed for albums.
bool ParseTrackEmbed(std::string_view html, WebTrack& single);
bool ParseListEmbed(std::string_view html, std::vector<WebTrack>& list, std::string& setTitle);
bool ParseAlbumState(std::string_view html, std::vector<WebTrack>& list, std::string& setTitle);

// The YouTube queries for a track, best first: "artist - title", the same with
// decorative suffixes stripped from the title, then the title alone. The queue tries
// each until a match is found, the way Puto retries.
std::vector<std::string> BuildQueries(const WebTrack& track);

// Puto's matching, scored on the title (decoration stripped for close calls), the
// artist, official and Topic uploads preferred, and the duration within 2%. Returns
// `have`'s indices, best first; `why` names the closest duration when nothing scored.
// The queue walks the order until a candidate's own clock, read from its player
// reply, agrees too - so a search row without a duration cannot smuggle a clip in.
std::vector<int> RankMatches(const WebTrack& want, const std::vector<yt::Candidate>& have,
                             std::string& why);

// Fetches an embed page's HTML on the caller's worker thread, cancellable.
bool ReadEmbed(Kind kind, const std::string& id, const std::atomic<bool>& cancel,
               std::string& html, std::string& error);

// Fetches the real album page's HTML, whose server-rendered initialState holds the
// complete numbered track list; the queue tries it before the embed page and falls
// back when it does not answer or carries no list.
bool ReadAlbumPage(const std::string& id, const std::atomic<bool>& cancel,
                   std::string& html, std::string& error);

}  // namespace spotify
