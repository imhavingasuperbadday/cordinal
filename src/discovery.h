// Discover outside my library (Step 5 of docs/effortless-library.md): the optional weave in
// Keep playing like this that follows a local song with one from an artist the library does not
// have yet. The choice is made from MusicBrainz's related artists and their releases, against a
// snapshot of what the library owns, so nothing already in the library is offered. The pure
// parsers and choosers are here, offline-tested; Find does the network reads on a worker.
#pragma once
#include "metadata_fetch.h"
#include "release_radar.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace discovery {

// What the library already owns, reduced to the names MusicBrainz also uses. The snapshot belongs
// to the worker: it never reads the live library.
struct Snapshot {
    std::vector<std::string> artists;  // loose names
    std::vector<std::string> albums;   // "artist\x1falbum"
    std::vector<std::string> tracks;   // "artist\x1ftitle"
    void Add(const std::string& artist, const std::string& album, const std::string& title);
    bool HasArtist(std::string_view artist) const;
    bool HasAlbum(std::string_view artist, std::string_view album) const;
    bool HasTrack(std::string_view artist, std::string_view title) const;
};

// One related-artist edge from MusicBrainz (/ws/2/artist/<mbid>?inc=artist-rels).
struct Relation {
    std::string mbid, name, type;
};
std::vector<Relation> ParseArtistRelations(std::string_view json);

// The one song to offer: everything the save queue needs to look it up on YouTube and tag it.
struct Track {
    std::string site = "musicbrainz";
    std::string id;          // the release id and track position, unique for the saved/failed record
    std::string artist, title, album, date;
    std::string releaseId, releaseGroup;
    unsigned trackNo = 0, trackTotal = 0;
    unsigned durationSec = 0;
};

// One of the related artists not already in the snapshot, or -1. `draw` is in [0, 1); a negative
// value takes the first eligible entry, which is what the self-test uses.
int ChooseRelatedArtist(const std::vector<Relation>& relations, const Snapshot& owned, double draw = -1);
// The first unowned release that is not a compilation or soundtrack, preferring albums and EPs
// over singles. -1 when the library owns everything MusicBrainz listed.
int ChooseRelease(const std::vector<online::ReleaseGroup>& groups, const std::string& artist,
                  const Snapshot& owned, double draw = -1);
// A track of the release the snapshot does not have by that artist; 20 seconds or longer where
// MusicBrainz knows the length. -1 when every track is owned.
int ChooseTrack(const radar::Album& release, const std::string& artist, const Snapshot& owned,
                double draw = -1);

// Whether a local track that just started should be followed, later, by an unowned one. After the
// first offer, watches wait `kBetween` local tracks, so discovery stays occasional.
constexpr unsigned kFirstAfter = 2, kBetween = 6;
bool ShouldDiscover(bool enabled, bool radio, bool playingLocal, bool pending, bool offeredBefore,
                    unsigned localSinceOffer);

// Blocking, on the caller's worker: find one unowned song near `seedArtist` (the playing track's
// artist) using `topArtists` as fallbacks. The fetcher is for the self-test; empty uses
// online::Get. False when nothing new could be found or the worker was cancelled.
bool Find(const std::string& seedArtist, const std::vector<std::string>& topArtists,
          const Snapshot& owned, online::Cancel& cancel, Track& out, uint32_t seed,
          const std::function<online::Reply(const std::string&)>& fetch = {});

}  // namespace discovery
