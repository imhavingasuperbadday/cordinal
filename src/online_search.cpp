// Unified online song search implementation (online_search.h).
#include "online_search.h"

#include "library.h"
#include "web_remote.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>

namespace online {
namespace {

constexpr unsigned kSearchTimeoutMs = 10000;
constexpr size_t kMaxCacheEntries = 32;

std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return out;
}

std::string TrimUtf8(std::string_view text) {
    size_t a = 0, b = text.size();
    while (a < b && (unsigned char)text[a] <= ' ') ++a;
    while (b > a && (unsigned char)text[b - 1] <= ' ') --b;
    std::string out;
    bool space = false;
    for (size_t i = a; i < b; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += (char)c;
    }
    return out;
}

unsigned ParseUInt(std::string_view text) {
    if (text.empty()) return 0;
    unsigned long long val = 0;
    for (char c : text) {
        if (c < '0' || c > '9') break;
        val = val * 10 + (unsigned)(c - '0');
        if (val > 86400000ull) return 0;
    }
    return (unsigned)val;
}

std::string StableFallbackId(const SearchTrack& t) {
    return "name:" + t.artist + "\x1f" + t.title;
}

int MetadataScore(const SearchTrack& t) {
    int score = 0;
    if (t.source == "deezer") score += 3;
    else if (t.source == "itunes") score += 2;
    else score += 1;
    if (!t.album.empty()) score += 2;
    if (t.albumId != 0) ++score;
    if (t.durationSec > 0) score += 2;
    if (!t.artworkUrl.empty()) ++score;
    return score;
}

uint64_t ParseAlbumId(std::string_view s) {
    if (s.empty() || s.size() > 20) return 0;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (uint64_t)(c - '0');
    }
    return v;
}

std::string UpgradeItunesArt(std::string art) {
    if (!art.starts_with("https://")) return {};
    size_t sz = art.rfind("100x100");
    if (sz != std::string::npos) art = art.substr(0, sz) + "600x600" + art.substr(sz + 7);
    return art;
}

std::string DeezerCoverUrl(const web::JsonValue& obj) {
    for (const char* key : {"cover_xl", "cover_big", "cover_medium"}) {
        const std::string& url = obj.String(key);
        if (url.starts_with("https://")) return url;
    }
    return {};
}

std::string ValidReleaseDate(std::string_view raw) {
    if (raw.size() < 4) return {};
    for (size_t i = 0; i < 4; ++i)
        if (raw[i] < '0' || raw[i] > '9') return {};
    if (raw.size() >= 10 && raw[4] == '-' && raw[7] == '-') return std::string(raw.substr(0, 10));
    return std::string(raw.substr(0, 4));
}

void FinaliseAlbumTracks(WebFetchTrackList& list) {
    unsigned maxDisc = 1;
    for (const WebTrack& w : list.tracks)
        if (w.discNo > maxDisc) maxDisc = w.discNo;
    const bool multiDisc = maxDisc > 1;
    const unsigned total = (unsigned)list.tracks.size();
    for (size_t i = 0; i < list.tracks.size(); ++i) {
        WebTrack& w = list.tracks[i];
        if (!multiDisc) {
            w.discNo = 0;
            w.discTotal = 0;
            if (w.trackNo == 0) w.trackNo = (unsigned)(i + 1);
            w.trackTotal = total;
            continue;
        }
        const unsigned d = w.discNo ? w.discNo : 1;
        unsigned inDisc = 0, before = 0;
        for (size_t j = 0; j < list.tracks.size(); ++j) {
            const unsigned dj = list.tracks[j].discNo ? list.tracks[j].discNo : 1;
            if (dj != d) continue;
            ++inDisc;
            if (j <= i) ++before;
        }
        w.discNo = d;
        w.discTotal = maxDisc;
        if (w.trackNo == 0) w.trackNo = before;
        w.trackTotal = inDisc;
    }
}

struct AlbumSchema {
    const char* site;
    const char* idKey;
    const char* titleKey;
    const char* discKey;
    const char* trackKey;
    const char* durKey;
    bool nestedArtist;
    bool durMillis;
    bool itunesFilter;
};

void ParseAlbumTrackArray(const web::JsonValue& arr, const AlbumSchema& s, const std::string& cover,
                          WebFetchTrackList& out) {
    out.playlist = false;
    if (arr.kind != web::JsonValue::Kind::Array) return;
    for (const auto& item : arr.items) {
        if (s.itunesFilter) {
            const std::string& wrapper = item.String("wrapperType");
            const std::string& kind = item.String("kind");
            if ((!wrapper.empty() && wrapper != "track") || (!kind.empty() && kind != "song")) continue;
        }
        std::string title = TrimUtf8(item.String(s.titleKey));
        if (title.empty()) continue;
        std::string artist = TrimUtf8(s.nestedArtist ? item["artist"].String("name") : item.String("artistName"));
        if (artist.empty()) artist = out.albumArtist;
        WebTrack w;
        w.site = s.site;
        w.id = TrimUtf8(item.String(s.idKey));
        if (w.id.empty()) w.id = "name:" + artist + "\x1f" + title;
        w.title = std::move(title);
        w.artist = std::move(artist);
        w.album = out.title;
        w.artworkUrl = cover;
        const unsigned rawDur = ParseUInt(item.String(s.durKey));
        w.durationSec = s.durMillis ? (rawDur ? (rawDur + 500) / 1000 : 0) : rawDur;
        w.discNo = ParseUInt(item.String(s.discKey));
        w.trackNo = ParseUInt(item.String(s.trackKey));
        out.tracks.push_back(std::move(w));
    }
    if (!out.tracks.empty()) FinaliseAlbumTracks(out);
}

Reply SearchGet(const SearchFetchFn& fetch, Cancel& cancel, const std::string& url) {
    return fetch ? fetch(url) : Get(url, cancel, kSearchTimeoutMs);
}

bool FetchAlbumById(const SearchFetchFn& fetch, Cancel& cancel, bool deezer, uint64_t id,
                    const SearchTrack& fallback, WebFetchTrackList& out) {
    if (!id || cancel.Stopped()) return false;
    const std::string idStr = std::to_string(id);
    if (deezer) {
        const Reply album = SearchGet(fetch, cancel, "https://api.deezer.com/album/" + idStr);
        if (cancel.Stopped() || album.status != 200) return false;
        const Reply tracks = SearchGet(fetch, cancel, "https://api.deezer.com/album/" + idStr + "/tracks?limit=200");
        if (cancel.Stopped()) return false;
        return ParseDeezerAlbum(album.body, tracks.status == 200 ? std::string_view(tracks.body) : std::string_view{},
                                fallback, out);
    }
    const Reply lookup =
        SearchGet(fetch, cancel, "https://itunes.apple.com/lookup?id=" + idStr + "&entity=song&limit=200");
    return !cancel.Stopped() && lookup.status == 200 && ParseItunesAlbum(lookup.body, fallback, out);
}

bool SameLoose(std::string_view a, std::string_view b) {
    const std::string la = LooseTitle(a), lb = LooseTitle(b);
    return !la.empty() && la == lb;
}

uint64_t FindMatchingAlbumId(const web::JsonValue& arr, const char* idKey, const char* titleKey,
                               bool nestedArtist, std::string_view cleanAlbum, std::string_view cleanArtist) {
    if (arr.kind != web::JsonValue::Kind::Array) return 0;
    uint64_t fallbackId = 0;
    for (const auto& item : arr.items) {
        const uint64_t id = ParseAlbumId(TrimUtf8(item.String(idKey)));
        if (!id) continue;
        const std::string title = TrimUtf8(item.String(titleKey));
        if (!SameLoose(title, cleanAlbum)) continue;
        const std::string artist =
            TrimUtf8(nestedArtist ? item["artist"].String("name") : item.String("artistName"));
        if (cleanArtist.empty() || SameLoose(artist, cleanArtist)) return id;
        if (!fallbackId) fallbackId = id;
    }
    return fallbackId;
}

struct CacheEntry {
    std::string key;
    std::vector<SearchTrack> tracks;
};

std::mutex& CacheMutex() {
    static auto* m = new std::mutex;
    return *m;
}

std::vector<CacheEntry>& Cache() {
    static auto* c = new std::vector<CacheEntry>;
    return *c;
}

bool LookupCache(const std::string& key, std::vector<SearchTrack>& out) {
    std::lock_guard lock(CacheMutex());
    auto& cache = Cache();
    for (size_t i = 0; i < cache.size(); ++i) {
        if (cache[i].key != key) continue;
        out = cache[i].tracks;
        if (i + 1 < cache.size()) {
            CacheEntry moved = std::move(cache[i]);
            cache.erase(cache.begin() + (ptrdiff_t)i);
            cache.push_back(std::move(moved));
        }
        return true;
    }
    return false;
}

void StoreCache(const std::string& key, const std::vector<SearchTrack>& tracks) {
    std::lock_guard lock(CacheMutex());
    auto& cache = Cache();
    for (size_t i = 0; i < cache.size(); ++i) {
        if (cache[i].key == key) {
            cache.erase(cache.begin() + (ptrdiff_t)i);
            break;
        }
    }
    if (cache.size() >= kMaxCacheEntries) cache.erase(cache.begin());
    cache.push_back({key, tracks});
}

}  // namespace

WebTrack SearchTrack::ToWebTrack() const {
    WebTrack w;
    w.site = source.empty() ? std::string("deezer") : source;
    w.id = !id.empty() ? id : StableFallbackId(*this);
    w.title = title;
    w.artist = artist;
    w.album = album;
    w.artworkUrl = artworkUrl;
    w.durationSec = durationSec;
    return w;
}

bool SearchableQuery(std::wstring_view query) {
    std::string q = NormalizeSearchQuery(query);
    if (q.size() < 2) return false;
    std::string low = LowerAscii(q);
    if (low.find("missing:") != std::string::npos) return false;
    return true;
}

std::string NormalizeSearchQuery(std::wstring_view query) {
    std::string s = TrimUtf8(ToUtf8(std::wstring(query)));
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = TrimUtf8(std::string_view(s).substr(1, s.size() - 2));
    return s;
}

void PushParsedTrack(std::vector<SearchTrack>& out, SearchTrack& t, std::string_view title, std::string_view artist,
                     std::string_view album) {
    t.title = TrimUtf8(title);
    if (t.title.empty()) return;
    t.artist = TrimUtf8(artist);
    t.album = TrimUtf8(album);
    if (t.id.empty()) t.id = StableFallbackId(t);
    out.push_back(std::move(t));
}

std::vector<SearchTrack> ParseDeezerSearchTracks(std::string_view json) {
    std::vector<SearchTrack> out;
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return out;
    const web::JsonValue& data = root["data"];
    if (data.kind != web::JsonValue::Kind::Array) return out;
    for (const auto& item : data.items) {
        SearchTrack t;
        t.source = "deezer";
        t.id = item.String("id");
        const web::JsonValue& album = item["album"];
        t.albumId = ParseAlbumId(TrimUtf8(album.String("id")));
        t.durationSec = ParseUInt(item.String("duration"));
        t.artworkUrl = DeezerCoverUrl(album);
        PushParsedTrack(out, t, item.String("title"), item["artist"].String("name"), album.String("title"));
    }
    return out;
}

std::vector<SearchTrack> ParseItunesSearchTracks(std::string_view json) {
    std::vector<SearchTrack> out;
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return out;
    const web::JsonValue& results = root["results"];
    if (results.kind != web::JsonValue::Kind::Array) return out;
    for (const auto& item : results.items) {
        const std::string kind = item.String("kind");
        if (!kind.empty() && kind != "song") continue;
        SearchTrack t;
        t.source = "itunes";
        t.id = item.String("trackId");
        t.albumId = ParseAlbumId(TrimUtf8(item.String("collectionId")));
        const unsigned ms = ParseUInt(item.String("trackTimeMillis"));
        t.durationSec = ms ? (ms + 500) / 1000 : 0;
        t.artworkUrl = UpgradeItunesArt(item.String("artworkUrl100"));
        PushParsedTrack(out, t, item.String("trackName"), item.String("artistName"), item.String("collectionName"));
    }
    return out;
}

std::vector<SearchTrack> FromYouTubeCandidates(const std::vector<yt::Candidate>& candidates) {
    std::vector<SearchTrack> out;
    for (const yt::Candidate& c : candidates) {
        if (c.id.empty() || c.title.empty()) continue;
        SearchTrack t;
        t.source = "youtube";
        t.id = c.id;
        t.title = TrimUtf8(c.title);
        t.artist = TrimUtf8(c.channel);
        t.durationSec = c.durationSec;
        t.artworkUrl = "https://i.ytimg.com/vi/" + c.id + "/hqdefault.jpg";
        out.push_back(std::move(t));
    }
    return out;
}

void DedupeSearchTracks(std::vector<SearchTrack>& tracks, size_t maxResults) {
    std::vector<SearchTrack> out;
    std::vector<std::pair<std::string, std::string>> keys;
    for (auto& t : tracks) {
        if (t.title.empty()) continue;
        std::string a = LooseTitle(t.artist);
        std::string title = LooseTitle(t.title);
        if (a.empty()) a = LowerAscii(t.artist);
        if (title.empty()) title = LowerAscii(t.title);
        auto key = std::pair{std::move(a), std::move(title)};
        bool replaced = false;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                if (MetadataScore(t) > MetadataScore(out[i])) out[i] = std::move(t);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            keys.push_back(std::move(key));
            out.push_back(std::move(t));
        }
    }
    if (maxResults && out.size() > maxResults) out.resize(maxResults);
    tracks = std::move(out);
}

void FilterUnownedTracks(std::vector<SearchTrack>& tracks, const import::OwnedIndex& owned) {
    if (owned.Size() == 0) return;
    std::wstring unused;
    size_t write = 0;
    for (size_t i = 0; i < tracks.size(); ++i) {
        SearchTrack& t = tracks[i];
        if (!t.artist.empty() && !t.title.empty() && owned.Find(t.artist, t.title, unused)) continue;
        if (write != i) tracks[write] = std::move(t);
        ++write;
    }
    tracks.resize(write);
}

void ClearSearchCache() {
    std::lock_guard lock(CacheMutex());
    Cache().clear();
    Cache().shrink_to_fit();
}

SearchReply SearchTracks(const std::string& query, const import::OwnedIndex& owned, Cancel& cancel,
                         size_t maxResults, const SearchFetchFn& fetch, const SearchYtFn& ytSearch) {
    SearchReply reply;
    const std::string clean = TrimUtf8(query);
    if (clean.size() < 2) {
        reply.ok = true;
        return reply;
    }
    const std::string cacheKey = LowerAscii(clean);
    if (LookupCache(cacheKey, reply.tracks)) {
        reply.ok = true;
        reply.fromCache = true;
        FilterUnownedTracks(reply.tracks, owned);
        if (maxResults && reply.tracks.size() > maxResults) reply.tracks.resize(maxResults);
        return reply;
    }

    auto doGet = [&](const std::string& url) {
        return fetch ? fetch(url) : Get(url, cancel, kSearchTimeoutMs);
    };

    bool anyAnswered = false;
    std::vector<SearchTrack> found;

    // Clean catalog metadata first (Deezer, then iTunes), so results carry real song, artist,
    // album and cover tags rather than YouTube upload titles.
    const Reply deezer = doGet("https://api.deezer.com/search/track?limit=15&q=" + UrlEncode(clean));
    if (cancel.Stopped()) {
        reply.error = "cancelled";
        return reply;
    }
    if (deezer.status == 200) {
        anyAnswered = true;
        found = ParseDeezerSearchTracks(deezer.body);
    }

    if (found.empty() && !cancel.Stopped()) {
        const Reply itunes =
            doGet("https://itunes.apple.com/search?media=music&entity=song&limit=15&term=" + UrlEncode(clean));
        if (cancel.Stopped()) {
            reply.error = "cancelled";
            return reply;
        }
        if (itunes.status == 200) {
            anyAnswered = true;
            found = ParseItunesSearchTracks(itunes.body);
        }
    }

    // YouTube search is the fallback when neither catalog had matches or answered.
    std::string ytError;
    if (found.empty() && !cancel.Stopped()) {
        std::vector<yt::Candidate> candidates;
        const bool ytOk = ytSearch ? ytSearch(clean, cancel.Flag(), candidates, ytError)
                                   : yt::Search(clean, cancel.Flag(), candidates, ytError);
        if (cancel.Stopped()) {
            reply.error = "cancelled";
            return reply;
        }
        if (ytOk) {
            anyAnswered = true;
            found = FromYouTubeCandidates(candidates);
        }
    }

    if (!anyAnswered) {
        reply.error = ytError.empty() ? "Online search couldn't reach Deezer, iTunes or YouTube." : ytError;
        return reply;
    }

    DedupeSearchTracks(found, 20);
    StoreCache(cacheKey, found);
    FilterUnownedTracks(found, owned);
    if (maxResults && found.size() > maxResults) found.resize(maxResults);
    reply.ok = true;
    reply.tracks = std::move(found);
    return reply;
}

bool ParseDeezerAlbum(std::string_view albumJson, std::string_view tracksJson, const SearchTrack& fallback,
                      WebFetchTrackList& out) {
    out = {};
    web::JsonValue root;
    if (!web::ParseJson(albumJson, root) || root.kind != web::JsonValue::Kind::Object) return false;
    out.title = TrimUtf8(root.String("title"));
    if (out.title.empty()) out.title = TrimUtf8(fallback.album);
    out.albumArtist = TrimUtf8(root["artist"].String("name"));
    if (out.albumArtist.empty()) out.albumArtist = TrimUtf8(fallback.artist);
    out.date = ValidReleaseDate(TrimUtf8(root.String("release_date")));
    std::string cover = DeezerCoverUrl(root);
    if (cover.empty() && fallback.artworkUrl.starts_with("https://")) cover = fallback.artworkUrl;

    web::JsonValue tracksRoot;
    const web::JsonValue* data = nullptr;
    if (!tracksJson.empty() && web::ParseJson(tracksJson, tracksRoot) &&
        tracksRoot["data"].kind == web::JsonValue::Kind::Array && !tracksRoot["data"].items.empty()) {
        data = &tracksRoot["data"];
    } else if (root["tracks"]["data"].kind == web::JsonValue::Kind::Array) {
        data = &root["tracks"]["data"];
    }
    if (!data) return false;
    static const AlbumSchema kDeezer{"deezer", "id", "title", "disk_number", "track_position", "duration",
                                     true, false, false};
    ParseAlbumTrackArray(*data, kDeezer, cover, out);
    return !out.tracks.empty();
}

bool ParseItunesAlbum(std::string_view json, const SearchTrack& fallback, WebFetchTrackList& out) {
    out = {};
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return false;
    const web::JsonValue& results = root["results"];
    if (results.kind != web::JsonValue::Kind::Array || results.items.empty()) return false;

    std::string cover;
    for (const auto& item : results.items) {
        const std::string& wt = item.String("wrapperType");
        if (wt != "collection" && !out.title.empty()) continue;
        if (wt == "collection" || out.title.empty()) {
            std::string t = TrimUtf8(item.String("collectionName"));
            if (!t.empty()) out.title = std::move(t);
        }
        if (wt == "collection" || out.albumArtist.empty()) {
            std::string a = TrimUtf8(item.String("artistName"));
            if (!a.empty()) out.albumArtist = std::move(a);
        }
        if (out.date.empty()) out.date = ValidReleaseDate(TrimUtf8(item.String("releaseDate")));
        if (cover.empty()) cover = UpgradeItunesArt(item.String("artworkUrl100"));
        if (wt == "collection") break;
    }
    if (out.title.empty()) out.title = TrimUtf8(fallback.album);
    if (out.albumArtist.empty()) out.albumArtist = TrimUtf8(fallback.artist);
    if (cover.empty() && fallback.artworkUrl.starts_with("https://")) cover = fallback.artworkUrl;
    static const AlbumSchema kItunes{"itunes", "trackId", "trackName", "discNumber", "trackNumber", "trackTimeMillis",
                                     false, true, true};
    ParseAlbumTrackArray(results, kItunes, cover, out);
    return !out.tracks.empty();
}

bool FetchSearchAlbum(const SearchTrack& track, Cancel& cancel, WebFetchTrackList& out, std::string& error,
                      const SearchFetchFn& fetch) {
    out = {};
    error.clear();
    const std::string cleanAlbum = TrimUtf8(track.album);
    const std::string cleanArtist = TrimUtf8(track.artist);
    if (cleanAlbum.empty() && !track.albumId) {
        error = "That song has no album information.";
        return false;
    }

    if (track.albumId && (track.source == "deezer" || track.source == "itunes") &&
        FetchAlbumById(fetch, cancel, track.source == "deezer", track.albumId, track, out))
        return true;

    if (!cancel.Stopped() && !cleanAlbum.empty()) {
        const std::string enc = UrlEncode(cleanArtist.empty() ? cleanAlbum : (cleanArtist + " " + cleanAlbum));
        struct Provider {
            const char* url;
            const char* arrKey;
            const char* idKey;
            const char* titleKey;
            bool deezer;
        };
        static const Provider providers[] = {
            {"https://api.deezer.com/search/album?limit=5&q=", "data", "id", "title", true},
            {"https://itunes.apple.com/search?media=music&entity=album&limit=5&term=", "results", "collectionId",
             "collectionName", false},
        };
        for (const Provider& p : providers) {
            if (cancel.Stopped()) break;
            const Reply r = SearchGet(fetch, cancel, p.url + enc);
            if (cancel.Stopped() || r.status != 200) continue;
            web::JsonValue root;
            if (!web::ParseJson(r.body, root)) continue;
            const uint64_t id = FindMatchingAlbumId(root[p.arrKey], p.idKey, p.titleKey, p.deezer, cleanAlbum, cleanArtist);
            if (id && FetchAlbumById(fetch, cancel, p.deezer, id, track, out)) return true;
        }
    }

    if (cancel.Stopped()) {
        error = "cancelled";
        return false;
    }
    error = cleanAlbum.empty() ? "Couldn't find the full tracklist for that album."
                               : "Couldn't find the full tracklist for \"" + cleanAlbum + "\".";
    return false;
}

}  // namespace online
