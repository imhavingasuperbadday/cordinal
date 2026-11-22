// Discover outside my library (discovery.h). MusicBrainz asks at most once a second through
// online::Get, so a pick costs a few seconds while the local queue keeps playing.
#include "discovery.h"

#include "release_radar.h"
#include "web_remote.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace discovery {
namespace {

std::string Loose(std::string_view text) { return online::LooseTitle(text); }

std::string AlbumKey(std::string_view artist, std::string_view album) {
    return Loose(artist) + '\x1f' + Loose(album);
}

std::string TrackKey(std::string_view artist, std::string_view title) {
    return Loose(artist) + '\x1f' + Loose(title);
}

std::vector<std::string> AlbumTitlesBy(const Snapshot& owned, std::string_view artist) {
    const std::string prefix = Loose(artist) + '\x1f';
    std::vector<std::string> out;
    for (const std::string& key : owned.albums)
        if (key.starts_with(prefix)) out.push_back(key.substr(prefix.size()));
    return out;
}

double Fraction(double draw) {
    if (draw <= 0) return 0;
    return std::min(draw, 0.999999);
}

// A stable fraction from the seed and a salt, so one run varies its artists, releases and tracks
// without keeping a random generator on the worker.
double Draw(uint32_t seed, uint32_t salt) {
    uint32_t x = seed ^ (salt * 0x9e3779b9u);
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (double)(x & 0xffffff) / 16777216.0;
}

bool Get(const std::string& url, std::string& body, online::Cancel& cancel,
         const std::function<online::Reply(const std::string&)>& fetch) {
    if (cancel.Stopped()) return false;
    online::Reply reply = fetch ? fetch(url) : online::Get(url, cancel, 8000);
    if (reply.status != 200) return false;
    body = std::move(reply.body);
    return true;
}

bool ArtistId(const std::string& name, std::string& mbid, online::Cancel& cancel,
              const std::function<online::Reply(const std::string&)>& fetch) {
    std::string body;
    if (!Get("https://musicbrainz.org/ws/2/artist?fmt=json&limit=10&query=" +
                 online::UrlEncode("artist:\"" + name + "\""),
             body, cancel, fetch))
        return false;
    mbid = online::ParseArtistSearch(body, name);
    return !mbid.empty();
}

bool ReleaseGroups(const std::string& mbid, std::vector<online::ReleaseGroup>& groups,
                   online::Cancel& cancel, const std::function<online::Reply(const std::string&)>& fetch) {
    groups.clear();
    std::string body;
    if (!Get("https://musicbrainz.org/ws/2/release-group?artist=" + mbid +
                 "&type=album%7Cep%7Csingle&limit=100&fmt=json",
             body, cancel, fetch))
        return false;
    groups = online::ParseReleaseGroups(body);
    return true;
}

// One release group as an album: the fullest official tracklist MusicBrainz lists for it.
bool AlbumOf(const online::ReleaseGroup& group, const std::string& artist, radar::Album& album,
             online::Cancel& cancel, const std::function<online::Reply(const std::string&)>& fetch) {
    radar::Item item;
    item.artist = artist;
    item.groupId = group.id;
    item.title = group.title;
    item.type = group.type;
    item.firstDate = group.firstDate;
    return radar::FetchAlbum(item, album, cancel, fetch);
}

}  // namespace

void Snapshot::Add(const std::string& artist, const std::string& album, const std::string& title) {
    if (!artist.empty()) {
        const std::string loose = Loose(artist);
        if (std::find(artists.begin(), artists.end(), loose) == artists.end()) artists.push_back(loose);
    }
    if (!artist.empty() && !album.empty()) {
        const std::string key = AlbumKey(artist, album);
        if (std::find(albums.begin(), albums.end(), key) == albums.end()) albums.push_back(key);
    }
    if (!artist.empty() && !title.empty()) {
        const std::string key = TrackKey(artist, title);
        if (std::find(tracks.begin(), tracks.end(), key) == tracks.end()) tracks.push_back(key);
    }
}

bool Snapshot::HasArtist(std::string_view artist) const {
    const std::string loose = Loose(artist);
    return std::find(artists.begin(), artists.end(), loose) != artists.end();
}

bool Snapshot::HasAlbum(std::string_view artist, std::string_view album) const {
    const std::string key = AlbumKey(artist, album);
    return std::find(albums.begin(), albums.end(), key) != albums.end();
}

bool Snapshot::HasTrack(std::string_view artist, std::string_view title) const {
    const std::string key = TrackKey(artist, title);
    return std::find(tracks.begin(), tracks.end(), key) != tracks.end();
}

std::vector<Relation> ParseArtistRelations(std::string_view json) {
    std::vector<Relation> out;
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return out;
    std::set<std::string> seen;
    for (const web::JsonValue& relation : root["relations"].items) {
        if (relation.kind != web::JsonValue::Kind::Object) continue;
        const web::JsonValue& artist = relation["artist"];
        if (artist.kind != web::JsonValue::Kind::Object) continue;
        const std::string id = artist.String("id");
        const std::string name = artist.String("name");
        if (!online::ValidMbid(id) || name.empty() || !seen.insert(id).second) continue;
        out.push_back({id, name, relation.String("type")});
    }
    return out;
}

int ChooseRelatedArtist(const std::vector<Relation>& relations, const Snapshot& owned, double draw) {
    std::vector<int> eligible;
    for (int i = 0; i < (int)relations.size(); ++i)
        if (!relations[i].name.empty() && !owned.HasArtist(relations[i].name)) eligible.push_back(i);
    if (eligible.empty()) return -1;
    return eligible[(size_t)(Fraction(draw) * eligible.size())];
}

int ChooseRelease(const std::vector<online::ReleaseGroup>& groups, const std::string& artist,
                  const Snapshot& owned, double draw) {
    std::vector<int> preferred, other;
    for (int i = 0; i < (int)groups.size(); ++i) {
        const online::ReleaseGroup& group = groups[i];
        if (group.id.empty() || group.title.empty() || group.owned) continue;
        if (owned.HasAlbum(artist, group.title)) continue;
        const std::string type = Loose(group.type);
        // A compilation, soundtrack or live record is a poor introduction to an artist. Albums and
        // EPs are the real discography; singles fill in when nothing else is new.
        if (type == "compilation" || type == "soundtrack" || type == "live" || type == "interview" ||
            type == "audiobook" || type == "spokenword")
            continue;
        if (type == "album" || type == "ep") preferred.push_back(i);
        else other.push_back(i);
    }
    const std::vector<int>& pool = preferred.empty() ? other : preferred;
    if (pool.empty()) return -1;
    return pool[(size_t)(Fraction(draw) * pool.size())];
}

int ChooseTrack(const radar::Album& release, const std::string& artist, const Snapshot& owned,
                double draw) {
    std::vector<int> eligible;
    for (int i = 0; i < (int)release.tracks.size(); ++i) {
        const radar::AlbumTrack& track = release.tracks[i];
        if (track.title.empty() || owned.HasTrack(artist, track.title)) continue;
        if (track.durationSec && track.durationSec < 20) continue;  // an intro or a scrap, not a song
        eligible.push_back(i);
    }
    if (eligible.empty()) return -1;
    return eligible[(size_t)(Fraction(draw) * eligible.size())];
}

bool ShouldDiscover(bool enabled, bool radio, bool playingLocal, bool pending, bool offeredBefore,
                    unsigned localSinceOffer) {
    if (!enabled || !radio || !playingLocal || pending) return false;
    return localSinceOffer >= (offeredBefore ? kBetween : kFirstAfter);
}

bool Find(const std::string& seedArtist, const std::vector<std::string>& topArtists,
          const Snapshot& owned, online::Cancel& cancel, Track& out, uint32_t seedValue,
          const std::function<online::Reply(const std::string&)>& fetch) {
    out = Track{};
    std::string seed = seedArtist;
    std::string seedId;
    if (!seed.empty()) ArtistId(seed, seedId, cancel, fetch);
    // The playing artist may be missing from MusicBrainz (a local band, a mistyped tag). One of the
    // artists the library plays most can stand in, so a look is not wasted.
    for (size_t i = 0; seedId.empty() && i < topArtists.size(); ++i) {
        if (topArtists[i].empty() || topArtists[i] == seed) continue;
        if (ArtistId(topArtists[i], seedId, cancel, fetch)) seed = topArtists[i];
    }
    if (seedId.empty()) return false;

    // Related artists come first, so a run can walk away from what the library already knows. The
    // seed itself follows when MusicBrainz lists no relation (a lone artist, or a new one).
    std::vector<std::pair<std::string, std::string>> candidates;  // name, known mbid
    std::string body;
    if (Get("https://musicbrainz.org/ws/2/artist/" + seedId + "?inc=artist-rels&fmt=json", body, cancel, fetch)) {
        std::vector<Relation> relations = ParseArtistRelations(body);
        if (int pick = ChooseRelatedArtist(relations, owned, Draw(seedValue, 1)); pick >= 0)
            candidates.emplace_back(relations[pick].name, relations[pick].mbid);
        // A second relation, then the seed, keeps a dead end from wasting the whole turn.
        for (int i = 0; i < (int)relations.size() && candidates.size() < 3; ++i) {
            if (owned.HasArtist(relations[i].name)) continue;
            if (!candidates.empty() && relations[i].name == candidates.front().first) continue;
            if (std::any_of(candidates.begin(), candidates.end(),
                            [&](const auto& have) { return have.second == relations[i].mbid; }))
                continue;
            candidates.emplace_back(relations[i].name, relations[i].mbid);
        }
    }
    candidates.emplace_back(seed, seedId);

    for (size_t c = 0; c < candidates.size() && c < 4; ++c) {
        if (cancel.Stopped()) return false;
        const std::string& artist = candidates[c].first;
        std::string mbid = candidates[c].second;
        if (mbid.empty() && !ArtistId(artist, mbid, cancel, fetch)) continue;
        if (cancel.Stopped()) return false;
        std::vector<online::ReleaseGroup> groups;
        if (!ReleaseGroups(mbid, groups, cancel, fetch)) continue;
        online::MarkOwned(groups, AlbumTitlesBy(owned, artist));
        // Two tries: the first release may hold only songs the library already has.
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (cancel.Stopped()) return false;
            const int pick = ChooseRelease(groups, artist, owned, Draw(seedValue, (uint32_t)(10 + c * 4 + attempt)));
            if (pick < 0) break;
            const online::ReleaseGroup group = groups[(size_t)pick];
            radar::Album album;
            if (!AlbumOf(group, artist, album, cancel, fetch)) {
                groups.erase(groups.begin() + pick);
                continue;
            }
            const int track = ChooseTrack(album, artist, owned,
                                          Draw(seedValue, (uint32_t)(100 + c * 16 + attempt * 4)));
            if (track < 0) {
                groups.erase(groups.begin() + pick);
                continue;
            }
            const radar::AlbumTrack& chosen = album.tracks[(size_t)track];
            const unsigned position = chosen.position ? chosen.position : (unsigned)(track + 1);
            out.id = album.releaseId + "#" + std::to_string(chosen.disc) + "." + std::to_string(position);
            out.artist = chosen.artist.empty() ? (album.artist.empty() ? artist : album.artist) : chosen.artist;
            out.title = chosen.title;
            out.album = album.title.empty() ? group.title : album.title;
            out.date = album.date.empty() ? group.firstDate : album.date;
            out.releaseId = album.releaseId;
            out.releaseGroup = group.id;
            out.trackNo = position;
            out.trackTotal = (unsigned)album.tracks.size();
            out.durationSec = chosen.durationSec;
            return true;
        }
    }
    return false;
}

}  // namespace discovery
