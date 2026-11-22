// Quiet Release Radar (Step 5 of docs/effortless-library.md): recent MusicBrainz releases by the
// artists the library says someone cares about, offered to save as owned albums. Nothing here
// touches the network by itself: the worker functions take a fetcher, and the self-test replays
// canned replies. The front ends own the threads, the consent prompt and the list.
#pragma once
#include "library.h"
#include "metadata_fetch.h"
#include "webfetch.h"

#include <ctime>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace radar {

// An artist the library says someone cares about. Hearts weigh more than plays, and an artist is
// counted through the album artist when there is one, so a guest credit on one track does not
// make a star of the guest.
struct Artist {
    std::string name;
    unsigned weight = 0;
};
std::vector<Artist> TopArtists(const Library& library, size_t count);

// One recent release worth offering: its MusicBrainz release group and first release date.
struct Item {
    std::string artist, groupId, title, type, firstDate;  // firstDate: YYYY[-MM[-DD]]
    bool owned = false;                                   // the library already has this album
};
// Marks the items whose album title the library already holds by that artist (LooseTitle, as
// online::MarkOwned), so a rescan never offers an album the user owns.
void MarkOwned(std::vector<Item>& items, const std::vector<std::string>& albums);

// One track of an album, ready to become a webfetch.h WebTrack.
struct AlbumTrack {
    std::string title, artist;
    unsigned disc = 0, position = 0;
    unsigned durationSec = 0;
};
struct Album {
    std::string title, artist, date, releaseId;
    std::vector<AlbumTrack> tracks;
    std::string groupId;
    unsigned discs = 0;
};

// A MusicBrainz fetcher; empty means online::Get. The self-test passes canned replies.
using Fetch = std::function<online::Reply(const std::string& url)>;

// The releases `artist` first put out on or after `since` (YYYY-MM-DD), newest first: the artist
// search then their release-group browse, one request each. True when both answered; `out` may be
// empty when the artist is unknown or had no new release.
bool RecentReleases(const std::string& artist, const std::string& since, std::vector<Item>& out,
                    online::Cancel& cancel, const Fetch& fetch = {});

// The album behind an item: its release group's fullest official tracklist. False when
// MusicBrainz had no release with tracks.
bool FetchAlbum(const Item& item, Album& out, online::Cancel& cancel, const Fetch& fetch = {});
// The release browse reply (/ws/2/release?release-group=...&inc=recordings), as one album.
bool ParseAlbum(std::string_view json, const Item& item, Album& out);
// Turns an album's MusicBrainz tracklist into a WebFetchTrackList (`playlist = false`) carrying
// artist, album, album artist, date, disc/track numbering, MusicBrainz ids and Cover Art Archive
// cover URL so every track lands grouped and tagged as one clean album.
WebFetchTrackList AlbumToWebFetchList(const Album& album);

// The date `months` months before `now`, at the first of that month (YYYY-MM-DD): the window a
// check looks back over, so a run in the middle of a month still sees its start.
std::string SinceDate(std::time_t now, int months);

}  // namespace radar
