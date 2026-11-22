// Tagging a whole album from MusicBrainz (docs/library-roadmap.md, step 2): the search, the parsers
// for its replies, matching local tracks to a release's tracks by position, title and duration, and
// the proposed edits with every visible change listed for the preview. Pure apart from Search, which
// runs on a front end's worker; the front ends (app_albumtag.cpp, Linux albumtag_ui.cpp) show the
// candidates and apply the chosen one through the tag writer as one undoable edit.
#pragma once
#include "library.h"
#include "metadata_fetch.h"
#include "tagwriter.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace albumtag {

// One local track as the library has it.
struct LocalTrack {
    TrackId id = 0;
    std::string title, artist, album, albumArtist, date, label, release, releaseGroup;
    uint32_t track = 0, trackTotal = 0, disc = 0, discTotal = 0;
    double seconds = 0;  // 0 = unknown
};

struct ReleaseTrack {
    std::string title, artist;
    uint32_t disc = 0, position = 0, discTracks = 0;  // discTracks: tracks on this track's disc
    double seconds = 0;                               // 0 = unknown
};

struct Release {
    std::string id, groupId, title, artist, date, country, label, format;
    int score = 0;          // the search's own score, 0..100
    uint32_t discs = 0;
    uint32_t trackCount = 0;
    std::vector<ReleaseTrack> tracks;  // empty until the release itself is looked up
};

// Release search replies (/ws/2/release?query=...) and release lookups (/ws/2/release/<id>).
std::vector<Release> ParseReleaseSearch(std::string_view json);
bool ParseRelease(std::string_view json, Release& out);
// A release-group's releases with their tracks (/ws/2/release?release-group=<id>&inc=recordings),
// each parsed like ParseRelease. `best` is the fullest tracklist, earliest on a tie.
bool ParseReleaseBrowse(std::string_view json, std::vector<Release>& all, Release* best = nullptr);
// A disc id lookup (/ws/2/discid/<id>?toc=...): each release with only the medium holding that disc
// (or, for a lookup by table of contents, the first medium with `discTracks` tracks) in `tracks`.
std::vector<Release> ParseDiscLookup(std::string_view json, const std::string& discId, size_t discTracks);

// Which release track each local track takes, and how sure the match is. A pair needs a matching
// title, duration or position to count; confidence is the pairs' average agreement over the larger
// of the two track lists, so missing or extra tracks lower it.
struct Match {
    std::vector<int> pick;  // per local track: the release track's index, or -1
    double confidence = 0;  // 0..1
    unsigned timed = 0;     // pairs where both durations are known
    unsigned close = 0;     // of those, within three seconds
};
Match MatchRelease(const std::vector<LocalTrack>& local, const Release& release);
// At or above this, and clear of the runner-up, a match is offered as the answer; below it the
// candidates are shown but none is chosen for you.
constexpr double kConfident = 0.8;

struct Change {
    size_t track;       // index into the local tracks
    std::string field;  // "Title", "Album artist", ...
    std::string before, after;
};
// The edits that make the local tracks match the release: only fields whose value would change.
// A date is left alone when it already names the release's year. `changes` lists each one.
std::vector<std::pair<TrackId, TagEdit>> Propose(const std::vector<LocalTrack>& local, const Release& release,
                                                 const Match& match, std::vector<Change>* changes = nullptr);

struct Candidate {
    Release release;
    Match match;
};
struct SearchResult {
    std::vector<Candidate> candidates;  // best first
    bool offline = false;               // MusicBrainz couldn't be reached
    bool confident = false;             // candidates[0] is the answer
};
// Worker thread: searches by the album's artist and title, looks up the few best releases with
// their tracks, and ranks them by how well the local tracks match. Returns early when cancelled.
// `fetch` stands in for the network in the self-tests; by default online::Get asks MusicBrainz.
using Fetch = std::function<online::Reply(const std::string& url)>;
SearchResult Search(const std::vector<LocalTrack>& local, online::Cancel& cancel, const Fetch& fetch = nullptr);
// Ranking alone, for the self-test: sorts by confidence and decides `confident`.
void Rank(SearchResult& result);

}  // namespace albumtag
