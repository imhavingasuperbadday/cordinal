// Unified online song search (Step 3 of docs/effortless-library.md): queries Deezer's and
// iTunes' keyless track-search APIs (with YouTube search as a fallback) on a worker thread,
// deduplicates against the local library so songs you already own are omitted, and keeps a small
// bounded in-memory LRU cache so re-typing a query doesn't re-hit the network.
#pragma once
#include "metadata_fetch.h"
#include "playlist_import.h"
#include "webfetch.h"
#include "ytaudio.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace online {

struct SearchTrack {
    std::string source;      // "deezer", "itunes", or "youtube"
    std::string id;          // provider track id, or YouTube video id
    std::string title;
    std::string artist;
    std::string album;
    std::string artworkUrl;  // https:// artwork URL when provided
    unsigned durationSec = 0;
    uint64_t albumId = 0;    // provider album id ("deezer" album id or "itunes" collectionId)

    // Converts into a WebTrack that RunWebFetch / MatchSpotifyTrack can resolve and tag.
    WebTrack ToWebTrack() const;
};

struct SearchReply {
    bool ok = false;
    bool fromCache = false;
    std::vector<SearchTrack> tracks;
    std::string error;
};

// True when `query` is a real search string (at least two non-space characters, and not one of
// Library > Find missing information's "missing:*" terms).
bool SearchableQuery(std::wstring_view query);
std::string NormalizeSearchQuery(std::wstring_view query);

// Pure parsers over provider replies (offline-tested).
std::vector<SearchTrack> ParseDeezerSearchTracks(std::string_view json);
std::vector<SearchTrack> ParseItunesSearchTracks(std::string_view json);
std::vector<SearchTrack> FromYouTubeCandidates(const std::vector<yt::Candidate>& candidates);
bool ParseDeezerAlbum(std::string_view albumJson, std::string_view tracksJson, const SearchTrack& fallback,
                      WebFetchTrackList& out);
bool ParseItunesAlbum(std::string_view json, const SearchTrack& fallback, WebFetchTrackList& out);

// Drops duplicate (artist, title) entries using LooseTitle, keeping the richest metadata, and
// caps at `maxResults` when nonzero.
void DedupeSearchTracks(std::vector<SearchTrack>& tracks, size_t maxResults = 0);

// Drops songs already present in the library snapshot (`owned`).
void FilterUnownedTracks(std::vector<SearchTrack>& tracks, const import::OwnedIndex& owned);

// Clears the in-memory search result cache (on shutdown or TrimMemory).
void ClearSearchCache();

using SearchFetchFn = std::function<Reply(const std::string& url)>;
using SearchYtFn = std::function<bool(const std::string& query, const std::atomic<bool>& cancel,
                                      std::vector<yt::Candidate>& out, std::string& error)>;

// Blocking search for a worker thread: checks the in-memory LRU cache, else queries Deezer,
// falling back to iTunes and then YouTube when empty or unreachable. Filters out tracks already
// in `owned` and caps at `maxResults`.
SearchReply SearchTracks(const std::string& query, const import::OwnedIndex& owned, Cancel& cancel,
                         size_t maxResults = 15, const SearchFetchFn& fetch = {},
                         const SearchYtFn& ytSearch = {});

// Blocking album lookup for a worker thread: fetches the full tracklist for a search result's
// album (Deezer album id / iTunes collectionId first, falling back to album search by artist and
// album name) as a WebFetchTrackList (`playlist = false`) ready for ShowWebFetchLists / WebFetchLists.
bool FetchSearchAlbum(const SearchTrack& track, Cancel& cancel, WebFetchTrackList& out, std::string& error,
                      const SearchFetchFn& fetch = {});

}  // namespace online
