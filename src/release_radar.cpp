// Quiet Release Radar (release_radar.h). MusicBrainz asks at most once a second; online::Get
// enforces that, and the radar makes two requests per artist and one per saved album.
#include "release_radar.h"

#include "album_tagger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

namespace radar {

namespace {

// How much an artist's tracks in the library count toward being asked about. A heart is a
// deliberate choice, a play a habit, and a skip takes one listen's worth back.
unsigned Weight(const Library& library, TrackId id) {
    const Track& t = library[id];
    unsigned weight = library.Favorite(id) ? 6 : 0;
    weight += std::min(t.playCount, 200u);
    if (t.skipCount > t.playCount) weight /= 2;
    return weight;
}

std::string_view ArtistOf(const Library& library, TrackId id) {
    const Track& t = library[id];
    if (t.albumArtist) return library.View(t.albumArtist);
    return library.View(t.artist);
}

// One reply, from the fake or the network; false unless MusicBrainz answered 200.
bool Get(const std::string& url, std::string& body, online::Cancel& cancel, const Fetch& fetch) {
    if (cancel.Stopped()) return false;
    if (fetch) {
        online::Reply reply = fetch(url);
        if (reply.status != 200) return false;
        body = std::move(reply.body);
        return true;
    }
    online::Reply reply = online::Get(url, cancel, 8000);
    if (reply.status != 200) return false;
    body = std::move(reply.body);
    return true;
}

}  // namespace

std::vector<Artist> TopArtists(const Library& library, size_t count) {
    std::map<std::string_view, unsigned> weights;
    for (TrackId id = 0; id < (TrackId)library.Count(); ++id) {
        const Track& t = library[id];
        if (!t.InLibrary() || !(t.artist || t.albumArtist)) continue;
        const std::string_view artist = ArtistOf(library, id);
        // "Various Artists" names a compilation, not somebody to follow.
        if (artist == "Various Artists" || artist == "Various Artists (V.A.)") continue;
        weights[artist] += Weight(library, id);
    }
    std::vector<Artist> out;
    out.reserve(weights.size());
    for (const auto& [name, weight] : weights) out.push_back({std::string(name), weight});
    std::sort(out.begin(), out.end(), [](const Artist& a, const Artist& b) {
        return a.weight != b.weight ? a.weight > b.weight : a.name < b.name;
    });
    if (out.size() > count) out.resize(count);
    return out;
}

void MarkOwned(std::vector<Item>& items, const std::vector<std::string>& albums) {
    std::vector<online::ReleaseGroup> groups;
    groups.reserve(items.size());
    for (Item& item : items) groups.push_back({item.groupId, item.title, item.type, 0, item.firstDate, false});
    online::MarkOwned(groups, albums);
    for (size_t i = 0; i < items.size() && i < groups.size(); ++i) items[i].owned = groups[i].owned;
}

bool RecentReleases(const std::string& artist, const std::string& since, std::vector<Item>& out,
                    online::Cancel& cancel, const Fetch& fetch) {
    out.clear();
    if (artist.empty()) return true;
    std::string body;
    if (!Get("https://musicbrainz.org/ws/2/artist?fmt=json&limit=10&query=" +
                 online::UrlEncode("artist:\"" + artist + "\""),
             body, cancel, fetch))
        return false;
    const std::string mbid = online::ParseArtistSearch(body, artist);
    if (mbid.empty()) return true;  // no artist of exactly that name: nobody to check
    if (!Get("https://musicbrainz.org/ws/2/release-group?artist=" + mbid +
                 "&type=album%7Cep&limit=100&fmt=json",
             body, cancel, fetch))
        return false;
    for (const online::ReleaseGroup& group : online::ParseReleaseGroups(body)) {
        if (group.firstDate.empty() || group.firstDate < since) continue;
        out.push_back({artist, group.id, group.title, group.type, group.firstDate, false});
    }
    std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) {
        if (a.firstDate != b.firstDate) return a.firstDate > b.firstDate;
        return a.title < b.title;
    });
    return true;
}

bool ParseAlbum(std::string_view json, const Item& item, Album& out) {
    out = {};
    std::vector<albumtag::Release> releases;
    albumtag::Release best;
    if (!albumtag::ParseReleaseBrowse(json, releases, &best) || best.tracks.empty()) return false;
    out.title = best.title.empty() ? item.title : best.title;
    out.artist = best.artist.empty() ? item.artist : best.artist;
    out.date = best.date.empty() ? item.firstDate : best.date;
    out.releaseId = best.id;
    out.groupId = best.groupId.empty() ? item.groupId : best.groupId;
    out.discs = best.discs;
    for (const albumtag::ReleaseTrack& track : best.tracks) {
        if (track.title.empty()) continue;
        AlbumTrack row;
        row.title = track.title;
        row.artist = track.artist.empty() ? out.artist : track.artist;
        row.disc = track.disc;
        row.position = track.position;
        row.durationSec = track.seconds > 0 ? (unsigned)std::lround(track.seconds) : 0;
        out.tracks.push_back(std::move(row));
    }
    return !out.tracks.empty();
}

bool FetchAlbum(const Item& item, Album& out, online::Cancel& cancel, const Fetch& fetch) {
    out = {};
    if (!online::ValidMbid(item.groupId)) return false;
    std::string body;
    const std::string base =
        "https://musicbrainz.org/ws/2/release?release-group=" + item.groupId;
    if (!Get(base + "&status=official&inc=recordings+artist-credits&limit=25&fmt=json", body, cancel, fetch))
        return false;
    if (ParseAlbum(body, item, out)) return true;
    // Some release groups (mixtapes, promos, unreleased status) have no "official" release.
    if (!Get(base + "&inc=recordings+artist-credits&limit=25&fmt=json", body, cancel, fetch)) return false;
    return ParseAlbum(body, item, out);
}

WebFetchTrackList AlbumToWebFetchList(const Album& album) {
    WebFetchTrackList list;
    list.title = album.title;
    list.playlist = false;
    list.albumArtist = album.artist;
    list.date = album.date;
    list.releaseId = album.releaseId;
    list.releaseGroupId = album.groupId;
    std::string coverUrl;
    if (online::ValidMbid(album.releaseId))
        coverUrl = "https://coverartarchive.org/release/" + album.releaseId + "/front-500";
    unsigned discs = album.discs;
    if (discs == 0)
        for (const AlbumTrack& row : album.tracks) discs = std::max(discs, row.disc);
    const bool multiDisc = discs > 1;
    const unsigned total = (unsigned)album.tracks.size();
    for (size_t i = 0; i < album.tracks.size(); ++i) {
        const AlbumTrack& row = album.tracks[i];
        const std::string& trackArtist = row.artist.empty() ? album.artist : row.artist;
        WebTrack track;
        track.site = "musicbrainz";
        track.id = "name:" + trackArtist + '\x1f' + row.title;
        track.title = row.title;
        track.artist = trackArtist;
        track.album = album.title;
        track.artworkUrl = coverUrl;
        track.durationSec = row.durationSec;
        if (multiDisc) {
            const unsigned d = row.disc ? row.disc : 1;
            unsigned discCount = 0;
            for (const AlbumTrack& o : album.tracks)
                if ((o.disc ? o.disc : 1) == d) ++discCount;
            track.discNo = d;
            track.discTotal = discs;
            track.trackNo = row.position ? row.position : (unsigned)i + 1;
            track.trackTotal = discCount;
        } else {
            track.trackNo = row.position ? row.position : (unsigned)i + 1;
            track.trackTotal = total;
        }
        list.tracks.push_back(std::move(track));
    }
    return list;
}

std::string SinceDate(std::time_t now, int months) {
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    int month = local.tm_mon - std::max(0, months);
    int year = local.tm_year + 1900;
    while (month < 0) {
        month += 12;
        --year;
    }
    char text[32];
    std::snprintf(text, sizeof text, "%04d-%02d-01", year, month + 1);
    return text;
}

}  // namespace radar
