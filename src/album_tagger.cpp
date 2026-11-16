// Tagging a whole album from MusicBrainz (album_tagger.h). The parsers take only the fields the
// preview and the edit need; matching is greedy over pair scores, which is exact enough at album
// sizes and keeps a wrong pairing from ever scoring well, since every pair has to agree on its own.
#include "album_tagger.h"

#include "web_remote.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>

namespace albumtag {
namespace {

using Json = web::JsonValue;

bool IsArray(const Json& v) { return v.kind == Json::Kind::Array; }
double Number(const Json& v) { return v.kind == Json::Kind::Number ? std::atof(v.text.c_str()) : 0; }

// An artist credit as MusicBrainz shows it: each name, then its join phrase (" feat. ", " & ").
std::string Credit(const Json& credits) {
    std::string out;
    if (!IsArray(credits)) return out;
    for (const auto& c : credits.items) {
        std::string name = c.String("name");
        if (name.empty()) name = c["artist"].String("name");
        out += name + c.String("joinphrase");
    }
    return out;
}

// Titles compared without case, spaces or punctuation; letters outside ASCII are kept as they are.
std::string Fold(std::string_view text) {
    std::string out;
    for (unsigned char c : text) {
        if (c >= 0x80) out += (char)c;
        else if (std::isalnum(c)) out += (char)std::tolower(c);
    }
    return out;
}

void Summary(const Json& r, Release& out) {
    out.id = r.String("id");
    out.title = r.String("title");
    out.date = r.String("date");
    out.country = r.String("country");
    out.artist = Credit(r["artist-credit"]);
    out.groupId = r["release-group"].String("id");
    out.score = (int)Number(r["score"]);
    out.trackCount = (uint32_t)Number(r["track-count"]);
    if (const Json& labels = r["label-info"]; IsArray(labels))
        for (const auto& l : labels.items)
            if (!l["label"].String("name").empty()) { out.label = l["label"].String("name"); break; }
    if (const Json& media = r["media"]; IsArray(media)) {
        out.discs = (uint32_t)media.items.size();
        std::vector<std::string> formats;
        for (const auto& m : media.items) {
            const std::string& f = m.String("format");
            if (!f.empty() && std::find(formats.begin(), formats.end(), f) == formats.end()) formats.push_back(f);
        }
        for (const auto& f : formats) out.format += (out.format.empty() ? "" : " + ") + f;
    }
}

// MusicBrainz ids are UUIDs; anything else never reaches a URL.
bool IsMbid(std::string_view id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23 ? c != '-' : !std::isxdigit((unsigned char)c)) return false;
    }
    return true;
}

// How well one local track and one release track agree, from -0.4 to 1.
double PairScore(const LocalTrack& l, const ReleaseTrack& t) {
    double s = 0;
    const std::string a = Fold(l.title), b = Fold(t.title);
    if (!a.empty() && a == b) s += 0.5;
    else if (a.size() >= 4 && b.size() >= 4 && (a.find(b) != a.npos || b.find(a) != b.npos)) s += 0.3;
    if (l.seconds > 0 && t.seconds > 0) {
        const double d = std::fabs(l.seconds - t.seconds);
        s += d <= 3 ? 0.4 : d <= 6 ? 0.2 : d <= 15 ? 0 : -0.4;
    }
    if (l.track && l.track == t.position && (l.disc == t.disc || (!l.disc && t.disc == 1))) s += 0.1;
    return std::min(s, 1.0);
}

}  // namespace

std::vector<Release> ParseReleaseSearch(std::string_view json) {
    std::vector<Release> out;
    Json root;
    if (!web::ParseJson(json, root) || root.kind != Json::Kind::Object || !IsArray(root["releases"])) return out;
    for (const auto& r : root["releases"].items) {
        Release release;
        Summary(r, release);
        if (IsMbid(release.id)) out.push_back(std::move(release));
    }
    return out;
}

bool ParseRelease(std::string_view json, Release& out) {
    out = Release{};
    Json root;
    if (!web::ParseJson(json, root) || root.kind != Json::Kind::Object) return false;
    Summary(root, out);
    if (!IsMbid(out.id) || !IsArray(root["media"])) return false;
    uint32_t disc = 0;
    for (const auto& m : root["media"].items) {
        ++disc;
        const Json& tracks = m["tracks"];
        if (!IsArray(tracks)) continue;
        const uint32_t number = Number(m["position"]) > 0 ? (uint32_t)Number(m["position"]) : disc;
        for (const auto& t : tracks.items) {
            ReleaseTrack track;
            track.title = t.String("title");
            if (track.title.empty()) track.title = t["recording"].String("title");
            track.artist = Credit(t["artist-credit"]);
            if (track.artist.empty()) track.artist = Credit(t["recording"]["artist-credit"]);
            if (track.artist.empty()) track.artist = out.artist;
            track.disc = number;
            track.position = (uint32_t)Number(t["position"]);
            track.discTracks = (uint32_t)tracks.items.size();
            double ms = Number(t["length"]);
            if (ms <= 0) ms = Number(t["recording"]["length"]);
            track.seconds = ms / 1000;
            out.tracks.push_back(std::move(track));
        }
    }
    out.trackCount = (uint32_t)out.tracks.size();
    return !out.tracks.empty();
}

Match MatchRelease(const std::vector<LocalTrack>& local, const Release& release) {
    Match match;
    match.pick.assign(local.size(), -1);
    if (local.empty() || release.tracks.empty()) return match;
    struct Pair { double score; size_t i, j; };
    std::vector<Pair> pairs;
    for (size_t i = 0; i < local.size(); ++i)
        for (size_t j = 0; j < release.tracks.size(); ++j)
            if (const double s = PairScore(local[i], release.tracks[j]); s >= 0.3) pairs.push_back({s, i, j});
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.score > b.score; });
    std::vector<bool> taken(release.tracks.size());
    double total = 0;
    for (const auto& p : pairs) {
        if (match.pick[p.i] >= 0 || taken[p.j]) continue;
        match.pick[p.i] = (int)p.j;
        taken[p.j] = true;
        total += p.score;
        const LocalTrack& l = local[p.i];
        const ReleaseTrack& t = release.tracks[p.j];
        if (l.seconds > 0 && t.seconds > 0) {
            ++match.timed;
            if (std::fabs(l.seconds - t.seconds) <= 3) ++match.close;
        }
    }
    match.confidence = total / (double)std::max(local.size(), release.tracks.size());
    return match;
}

std::vector<std::pair<TrackId, TagEdit>> Propose(const std::vector<LocalTrack>& local, const Release& release,
                                                 const Match& match, std::vector<Change>* changes) {
    std::vector<std::pair<TrackId, TagEdit>> out;
    for (size_t i = 0; i < local.size() && i < match.pick.size(); ++i) {
        const int j = match.pick[i];
        if (j < 0 || (size_t)j >= release.tracks.size()) continue;
        const LocalTrack& l = local[i];
        const ReleaseTrack& t = release.tracks[(size_t)j];
        TagEdit e;
        auto text = [&](std::optional<std::string> TagEdit::* field, const char* name, const std::string& now,
                        const std::string& want) {
            if (want.empty() || now == want) return;
            e.*field = want;
            if (changes) changes->push_back({i, name, now, want});
        };
        auto number = [&](std::optional<uint32_t> TagEdit::* field, const char* name, uint32_t now, uint32_t want) {
            if (!want || now == want) return;
            e.*field = want;
            if (changes) changes->push_back({i, name, now ? std::to_string(now) : std::string(), std::to_string(want)});
        };
        text(&TagEdit::title, "Title", l.title, t.title);
        text(&TagEdit::artist, "Artist", l.artist, t.artist);
        text(&TagEdit::album, "Album", l.album, release.title);
        text(&TagEdit::albumArtist, "Album artist", l.albumArtist, release.artist);
        // A year the release's date already starts with stays as it is: no churn for a finer date.
        const bool sameYear = l.date.size() >= 4 && release.date.size() >= 4 && l.date.compare(0, 4, release.date, 0, 4) == 0;
        if (!sameYear) text(&TagEdit::date, "Date", l.date, release.date);
        number(&TagEdit::track, "Track", l.track, t.position);
        number(&TagEdit::trackTotal, "Tracks", l.trackTotal, t.discTracks);
        if (release.discs > 1 || l.disc) {
            number(&TagEdit::disc, "Disc", l.disc, t.disc);
            number(&TagEdit::discTotal, "Discs", l.discTotal, release.discs);
        }
        text(&TagEdit::label, "Label", l.label, release.label);
        text(&TagEdit::musicBrainzRelease, "MusicBrainz release", l.release, release.id);
        text(&TagEdit::musicBrainzReleaseGroup, "MusicBrainz release group", l.releaseGroup, release.groupId);
        if (!e.Empty()) out.emplace_back(l.id, std::move(e));
    }
    return out;
}

void Rank(SearchResult& result) {
    auto& c = result.candidates;
    std::stable_sort(c.begin(), c.end(), [](const Candidate& a, const Candidate& b) {
        if (a.match.confidence != b.match.confidence) return a.match.confidence > b.match.confidence;
        return a.release.score > b.release.score;
    });
    // Two pressings of one release group differ only in their release details, so either is a fair
    // answer; two different groups that match about equally well are a question for the listener.
    result.confident = !c.empty() && c[0].match.confidence >= kConfident &&
                       (c.size() == 1 || c[1].match.confidence < c[0].match.confidence - 0.02 ||
                        c[1].release.groupId == c[0].release.groupId);
}

SearchResult Search(const std::vector<LocalTrack>& local, online::Cancel& cancel, const Fetch& fetch) {
    SearchResult result;
    auto get = [&](const std::string& url, size_t maxBody) {
        return fetch ? fetch(url) : online::Get(url, cancel, 10000, maxBody);
    };
    if (local.empty()) return result;
    // The album's title and artist as most of its tracks have them.
    std::map<std::string, size_t> albums, artists;
    for (const auto& t : local) {
        ++albums[t.album];
        ++artists[t.albumArtist.empty() ? t.artist : t.albumArtist];
    }
    auto most = [](const std::map<std::string, size_t>& counts) {
        return std::max_element(counts.begin(), counts.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
    };
    const std::string album = most(albums), artist = most(artists);
    if (album.empty()) return result;
    auto quoted = [](const std::string& text) {
        std::string out = "\"";
        for (char c : text) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out + '"';
    };
    std::string query = "release:" + quoted(album);
    if (!artist.empty()) query += " AND artist:" + quoted(artist);
    const online::Reply search = get("https://musicbrainz.org/ws/2/release?query=" + online::UrlEncode(query) +
                                     "&fmt=json&limit=15", 1u << 20);
    if (cancel.Stopped()) return result;
    if (search.status != 200) {
        result.offline = search.status == 0;
        return result;
    }
    std::vector<Release> found = ParseReleaseSearch(search.body);
    // Releases with the album's track count come first, then the search's own order. Each lookup
    // takes a second (MusicBrainz's rate limit), so only the five likeliest are fetched.
    found.erase(std::remove_if(found.begin(), found.end(), [](const Release& r) { return r.score < 50; }), found.end());
    std::stable_sort(found.begin(), found.end(), [&](const Release& a, const Release& b) {
        return (a.trackCount == local.size()) > (b.trackCount == local.size());
    });
    if (found.size() > 5) found.resize(5);
    for (const auto& summary : found) {
        const online::Reply reply = get("https://musicbrainz.org/ws/2/release/" + summary.id +
                                        "?inc=recordings+artist-credits+release-groups+labels&fmt=json", 4u << 20);
        if (cancel.Stopped()) return result;
        Candidate candidate;
        if (reply.status != 200 || !ParseRelease(reply.body, candidate.release)) continue;
        candidate.release.score = summary.score;
        candidate.match = MatchRelease(local, candidate.release);
        result.candidates.push_back(std::move(candidate));
    }
    Rank(result);
    return result;
}

}  // namespace albumtag
