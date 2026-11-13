// Online metadata providers (metadata_fetch.h). Only keyless, documented JSON services are asked,
// always with cordinal's own user agent: Deezer, the iTunes Search API and MusicBrainz with the
// Cover Art Archive for covers, LrcLib and lyrics.ovh for lyrics. Replies and images are untrusted input: parsers tolerate anything, images
// must be complete JPEG or PNG files that the platform decoder accepts, and every request has a
// timeout. MusicBrainz and the Cover Art Archive share one-per-second slots with musicbrainz.cpp.
#include "metadata_fetch.h"

#include "backup.h"
#include "library.h"
#include "web_remote.h"

#include <windows.h>
#ifdef _WIN32
#include <winhttp.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

namespace online {

// The version is kept with the others in tools/check-updates.ps1.
const char kRequestHeaders[] =
    "User-Agent: cordinal/0.1.11 (https://github.com/imhavingasuperbadday/cordinal)\r\n"
    "Accept: application/json\r\n";

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kSearchTime = std::chrono::seconds(20);
constexpr unsigned kApiTimeoutMs = 8000, kImageTimeoutMs = 6000;
constexpr auto kFoundTtl = std::chrono::hours(24 * 30);
constexpr auto kMissTtl = std::chrono::hours(24);

std::string Lower(std::string_view text) {
    std::string out;
    bool space = false;
    for (unsigned char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { space = !out.empty(); continue; }
        if (space) out += ' ';
        space = false;
        out += (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return out;
}

bool Contains(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

int Int(const web::JsonValue& value) { return atoi(value.text.c_str()); }

}  // namespace

// --- requests ------------------------------------------------------------------------------------

Cancel::Cancel() {
#ifdef _WIN32
    session_ = WinHttpOpen(L"cordinal/0.1.11 (https://github.com/imhavingasuperbadday/cordinal)",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#endif
}

Cancel::~Cancel() {
#ifdef _WIN32
    if (void* s = session_.exchange(nullptr)) WinHttpCloseHandle((HINTERNET)s);
#endif
}

void Cancel::Stop() {
    stop_ = true;
#ifdef _WIN32
    if (void* s = session_.exchange(nullptr)) WinHttpCloseHandle((HINTERNET)s);
#endif
}

void* Cancel::Session() const {
#ifdef _WIN32
    return session_.load();
#else
    return (void*)&stop_;
#endif
}

bool WaitTurn(std::string_view host, const std::atomic<bool>* stop) {
    static std::mutex mutex;
    static std::map<std::string, Clock::time_point, std::less<>> next;
    Clock::time_point slot;
    {
        std::lock_guard lock(mutex);
        auto now = Clock::now();
        auto& at = next[std::string(host)];
        slot = std::max(now, at);
        // The iTunes Search API asks for about 20 calls a minute; MusicBrainz and the archive one a second.
        at = slot + std::chrono::milliseconds(host == "itunes.apple.com" ? 3000 : 1100);
    }
    while (Clock::now() < slot) {
        if (stop && *stop) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !(stop && *stop);
}

std::string UrlEncode(std::string_view text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out += (char)c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

Reply Get(const std::string& start, Cancel& cancel, unsigned timeoutMs, size_t maxBody) {
    Reply reply;
    std::string url = start;
    for (int hop = 0; hop < 5; ++hop) {
        if (!url.starts_with("https://") || cancel.Stopped()) return reply;
        size_t hostEnd = url.find('/', 8);
        std::string host = url.substr(8, hostEnd == std::string::npos ? std::string::npos : hostEnd - 8);
        if (host == "musicbrainz.org" || host == "coverartarchive.org" || host == "itunes.apple.com")
            if (!WaitTurn(host, &cancel.Flag())) return reply;
        void* session = cancel.Session();
#ifdef _WIN32
        if (!session) return reply;  // stopped: never fall back to an uncancellable session
#endif
        web::HttpReply r = web::HttpFetch(session, url, "GET", {}, kRequestHeaders, timeoutMs, maxBody);
        if (cancel.Stopped()) return reply;
        if (r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308) {
            std::string location = r.headers["location"];
            if (location.starts_with("/")) location = "https://" + host + location;
            if (location.empty()) return reply;
            url = location;
            continue;
        }
        reply.status = r.status;
        reply.body = std::move(r.body);
        return reply;
    }
    return reply;
}

// --- covers: parsing -----------------------------------------------------------------------------

const wchar_t* CoverSourceName(CoverSource source) {
    switch (source) {
        case CoverSource::Deezer: return L"Deezer";
        case CoverSource::ITunes: return L"iTunes";
        case CoverSource::MusicBrainz: return L"MusicBrainz";
        default: return L"";
    }
}

const char* CoverSourceHost(CoverSource source) {
    switch (source) {
        case CoverSource::Deezer: return "api.deezer.com";
        case CoverSource::ITunes: return "itunes.apple.com";
        case CoverSource::MusicBrainz: return "musicbrainz.org";
        default: return "";
    }
}

std::vector<CoverCandidate> ParseDeezerAlbums(std::string_view json) {
    std::vector<CoverCandidate> out;
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return out;
    const web::JsonValue& data = root["data"];
    if (data.kind != web::JsonValue::Kind::Array) return out;
    for (const auto& item : data.items) {
        CoverCandidate c;
        c.source = CoverSource::Deezer;
        c.album = item.String("title");
        c.artist = item["artist"].String("name");
        // cover_xl is 1000 pixels and cover_big 500; anything else is too small to be worth saving.
        c.url = item.String("cover_xl");
        c.width = c.height = 1000;
        if (c.url.empty()) {
            c.url = item.String("cover_big");
            c.width = c.height = 500;
        }
        if (!c.url.starts_with("https://") || c.album.empty()) continue;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<CoverCandidate> ParseDeezerTracks(std::string_view json) {
    std::vector<CoverCandidate> out;
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return out;
    const web::JsonValue& data = root["data"];
    if (data.kind != web::JsonValue::Kind::Array) return out;
    for (const auto& item : data.items) {
        const web::JsonValue& album = item["album"];
        CoverCandidate c;
        c.source = CoverSource::Deezer;
        c.title = item.String("title");
        c.album = album.String("title");
        c.artist = item["artist"].String("name");
        c.url = album.String("cover_xl");
        c.width = c.height = 1000;
        if (c.url.empty()) {
            c.url = album.String("cover_big");
            c.width = c.height = 500;
        }
        if (!c.url.starts_with("https://") || c.title.empty()) continue;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<CoverCandidate> ParseItunesTracks(std::string_view json) {
    std::vector<CoverCandidate> out;
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return out;
    const web::JsonValue& results = root["results"];
    if (results.kind != web::JsonValue::Kind::Array) return out;
    for (const auto& item : results.items) {
        CoverCandidate c;
        c.source = CoverSource::ITunes;
        c.title = item.String("trackName");
        c.album = item.String("collectionName");
        c.artist = item.String("artistName");
        std::string art = item.String("artworkUrl100");
        size_t size = art.rfind("100x100");
        if (size == std::string::npos || !art.starts_with("https://") || c.title.empty()) continue;
        c.url = art.substr(0, size) + "1200x1200" + art.substr(size + 7);
        c.width = c.height = 1200;
        out.push_back(std::move(c));
    }
    return out;
}

std::string LooseTitle(std::string_view text) {
    std::string lower = Lower(text), kept;
    // Bracketed notes go: "(Deluxe Edition)", "[2011 Remaster]", "(1 hour)".
    int depth = 0;
    for (char c : lower) {
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
            continue;
        }
        if (c == ')' || c == ']' || c == '}') {
            if (depth) --depth;
            continue;
        }
        if (!depth) kept += c;
    }
    // "Title - 2011 Remaster", "Title - Deluxe Version": an edition note after a dash.
    static const char* const notes[] = {"remaster", "deluxe", "edition", "version", "mono", "stereo", "anniversary",
                                        "expanded", "bonus", "live", "mix", "edit"};
    if (size_t dash = kept.find(" - "); dash != std::string::npos)
        for (const char* note : notes)
            if (kept.find(note, dash) != std::string::npos) {
                kept.resize(dash);
                break;
            }
    // A featured artist is credited differently everywhere.
    for (const char* feat : {" feat. ", " feat ", " ft. ", " featuring "})
        if (size_t at = kept.find(feat); at != std::string::npos) kept.resize(at);
    std::string out;
    bool gap = false;
    auto put = [&](std::string_view w) {
        if (gap && !out.empty()) out += ' ';
        gap = false;
        out += w;
    };
    for (char ch : kept) {
        unsigned char c = (unsigned char)ch;
        if (c == '&') {
            gap = true;
            put("and");
            gap = true;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) {
            put(std::string_view(&ch, 1));
        } else if (c == ' ' || c == '-' || c == '/' || c == '_' || c == '.' || c == ',' || c == ':' || c == ';') {
            gap = true;
        }
        // Apostrophes, quotes and other marks go without a gap, so "don't" and "dont" agree.
    }
    return out.empty() ? lower : out;
}

std::vector<CoverCandidate> ParseItunesAlbums(std::string_view json) {
    std::vector<CoverCandidate> out;
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return out;
    const web::JsonValue& results = root["results"];
    if (results.kind != web::JsonValue::Kind::Array) return out;
    for (const auto& item : results.items) {
        CoverCandidate c;
        c.source = CoverSource::ITunes;
        c.album = item.String("collectionName");
        c.artist = item.String("artistName");
        std::string art = item.String("artworkUrl100");
        // The artwork URL ends in its size ("100x100bb.jpg"); asking for 1200 returns the largest
        // the store has up to that size.
        size_t size = art.rfind("100x100");
        if (size == std::string::npos || !art.starts_with("https://") || c.album.empty()) continue;
        c.url = art.substr(0, size) + "1200x1200" + art.substr(size + 7);
        c.width = c.height = 1200;
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<CoverCandidate> ParseMusicBrainzReleases(std::string_view json) {
    std::vector<CoverCandidate> out;
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return out;
    const web::JsonValue& releases = root["releases"];
    if (releases.kind != web::JsonValue::Kind::Array) return out;
    for (const auto& item : releases.items) {
        if (Int(item["score"]) < 80) continue;
        std::string id = item.String("id");
        // An id is a UUID; anything else must not reach a URL.
        if (id.size() != 36 || id.find_first_not_of("0123456789abcdef-") != std::string::npos) continue;
        // The search marks releases the archive has no front image for; unmarked ones are tried.
        const web::JsonValue& archive = item["cover-art-archive"];
        if (archive.kind == web::JsonValue::Kind::Object && archive["front"].text == "false") continue;
        CoverCandidate c;
        c.source = CoverSource::MusicBrainz;
        c.album = item.String("title");
        const web::JsonValue& credits = item["artist-credit"];
        if (credits.kind == web::JsonValue::Kind::Array)
            for (const auto& credit : credits.items) {
                std::string name = credit.String("name");
                if (name.empty()) name = credit["artist"].String("name");
                c.artist += name + credit.String("joinphrase");
            }
        c.url = "https://coverartarchive.org/release/" + id + "/front-1200";
        if (c.album.empty()) continue;
        out.push_back(std::move(c));
    }
    return out;
}

// --- covers: scoring -----------------------------------------------------------------------------

double ProviderQuality(CoverSource source) {
    switch (source) {
        case CoverSource::Deezer: return 2.0;
        case CoverSource::ITunes: return 1.5;
        case CoverSource::MusicBrainz: return 1.5;
        default: return 0;
    }
}

double MatchScore(const CoverQuery& query, std::string_view artist, std::string_view album) {
    std::string wantArtist = Lower(query.artist), wantAlbum = Lower(query.album);
    std::string gotArtist = Lower(artist), gotAlbum = Lower(album);
    bool sameArtist = !wantArtist.empty() && wantArtist == gotArtist;
    if (!wantAlbum.empty()) {
        bool sameAlbum = wantAlbum == gotAlbum;
        if (!sameArtist && !sameAlbum) return -1.5;
        return (sameArtist ? 0.5 : 0) + (sameAlbum ? 0.5 : 0);
    }
    // No album to compare: penalize what is rarely the right picture for a loose track.
    double score = sameArtist ? 0 : -1;
    if (gotArtist == "various artists" || gotArtist == "various" || Contains(gotAlbum, "compilation")) score -= 4;
    static const char* const generic[] = {"greatest hits", "best of", "the best of", "the very best of", "hits",
                                          "the hits", "collection", "the collection", "the essential", "essential",
                                          "anthology", "singles", "live"};
    for (const char* title : generic)
        if (gotAlbum == title || (gotAlbum.starts_with(title) && std::string_view(title).find(' ') != std::string_view::npos)) {
            score -= 3;
            break;
        }
    if (Contains(gotAlbum, "soundtrack") || Contains(gotAlbum, "motion picture") || gotAlbum.ends_with(" ost")) score -= 0.5;
    return score;
}

double ImageScore(int width, int height) {
    if (width <= 0 || height <= 0) width = height = 500;  // unknown until downloaded
    double w = width, h = height;
    return std::sqrt(w * h) / 500.0 + (1.0 - std::fabs(w - h) / std::max(w, h));
}

namespace {

double Score(const CoverQuery& query, const CoverCandidate& c, const std::vector<CoverSource>& order) {
    double bonus = 0;
    for (size_t i = 0; i < order.size() && i < 2; ++i)
        if (order[i] == c.source) bonus = i == 0 ? 0.2 : 0.1;
    return ProviderQuality(c.source) + MatchScore(query, c.artist, c.album) + ImageScore(c.width, c.height) + bonus;
}

}  // namespace

void RankCovers(const CoverQuery& query, std::vector<CoverCandidate>& candidates, const std::vector<CoverSource>& order) {
    for (auto& c : candidates) c.score = Score(query, c, order);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const CoverCandidate& a, const CoverCandidate& b) { return a.score > b.score; });
}

bool ConfidentCover(const CoverQuery& query, const CoverCandidate& c) {
    return !query.album.empty() && MatchScore(query, c.artist, c.album) >= 1.0 && std::min(c.width, c.height) >= 500 &&
           std::min(c.width, c.height) * 5 >= std::max(c.width, c.height) * 4;
}

// --- images --------------------------------------------------------------------------------------

ImageType InspectImage(std::string_view b, int& width, int& height) {
    width = height = 0;
    if (b.size() < 32 || b.size() > kMaxImageBytes) return ImageType::None;
    auto u8 = [&](size_t at) { return (unsigned)(unsigned char)b[at]; };
    auto be16 = [&](size_t at) { return u8(at) << 8 | u8(at + 1); };
    auto be32 = [&](size_t at) { return (uint32_t)be16(at) << 16 | be16(at + 2); };
    ImageType type = ImageType::None;
    if (u8(0) == 0xFF && u8(1) == 0xD8 && u8(2) == 0xFF) {
        // Walk the segments to the frame header; the file must also end with its end marker.
        size_t at = 2;
        while (at + 9 < b.size()) {
            if (u8(at) != 0xFF) return ImageType::None;
            unsigned marker = u8(at + 1);
            if (marker == 0xFF) { ++at; continue; }
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { at += 2; continue; }
            if (marker == 0xD9 || marker == 0xDA) return ImageType::None;  // no frame header before the data
            unsigned length = be16(at + 2);
            if (length < 2) return ImageType::None;
            bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
            if (frame) {
                height = (int)be16(at + 5);
                width = (int)be16(at + 7);
                break;
            }
            at += 2 + length;
        }
        bool ended = false;
        for (size_t i = b.size() - 2, stop = b.size() > 1024 ? b.size() - 1024 : 2; i >= stop && !ended; --i)
            ended = u8(i) == 0xFF && u8(i + 1) == 0xD9;
        if (!ended) return ImageType::None;
        type = ImageType::Jpeg;
    } else if (b.substr(0, 8) == std::string_view("\x89PNG\r\n\x1a\n", 8)) {
        if (be32(8) != 13 || b.substr(12, 4) != "IHDR") return ImageType::None;
        uint32_t w = be32(16), h = be32(20);
        if (w > 65535 || h > 65535) return ImageType::None;
        width = (int)w;
        height = (int)h;
        std::string_view tail = b.substr(b.size() - std::min<size_t>(b.size(), 64));
        if (tail.find("IEND") == std::string_view::npos) return ImageType::None;
        type = ImageType::Png;
    }
    if (type == ImageType::None || width <= 0 || height <= 0 || width > 30000 || height > 30000) {
        width = height = 0;
        return ImageType::None;
    }
    return type;
}

// --- settings and cache ----------------------------------------------------------------------------

Settings& Config() {
    static Settings settings;
    return settings;
}

std::vector<CoverSource> CoverOrder() {
    std::vector<CoverSource> order;
    const Settings& s = Config();
    int first = std::clamp(s.coverFirst, 0, kCoverSources - 1);
    if (s.covers[first]) order.push_back((CoverSource)first);
    for (int i = 0; i < kCoverSources; ++i)
        if (i != first && s.covers[i]) order.push_back((CoverSource)i);
    return order;
}

bool AnyCoverSource() { return !CoverOrder().empty(); }

namespace {

struct CacheEntry {
    std::string value;
    long long checked = 0;  // unix seconds
    bool found = false;
};

struct Cache {
    std::mutex mutex;
    std::wstring path;
    std::map<std::string, CacheEntry> entries;
    bool loaded = false, dirty = false;
} g_cache;

long long Now() {
    return (long long)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Escape(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string Unescape(std::string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 == text.size()) { out += text[i]; continue; }
        char e = text[++i];
        out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
    }
    return out;
}

void LoadCacheLocked() {
    if (g_cache.loaded) return;
    g_cache.loaded = true;
    std::string bytes;
    if (g_cache.path.empty() || !backup::ReadBytes(g_cache.path, bytes, 16u << 20) || !bytes.starts_with("1\n")) return;
    for (size_t at = 2; at < bytes.size();) {
        size_t end = bytes.find('\n', at);
        if (end == std::string::npos) end = bytes.size();
        std::string_view line(bytes.data() + at, end - at);
        at = end + 1;
        size_t a = line.find('\t'), b = a == line.npos ? a : line.find('\t', a + 1),
               c = b == line.npos ? b : line.find('\t', b + 1);
        if (c == line.npos) continue;
        CacheEntry entry;
        entry.checked = atoll(std::string(line.substr(a + 1, b - a - 1)).c_str());
        entry.found = line.substr(b + 1, c - b - 1) == "1";
        entry.value = Unescape(line.substr(c + 1));
        g_cache.entries[Unescape(line.substr(0, a))] = std::move(entry);
    }
}

}  // namespace

// Found entries stay 30 days and misses one, so a rerun of the batch does not ask again at once.
bool CacheGet(const std::string& key, std::string& value, bool& found) {
    std::lock_guard lock(g_cache.mutex);
    LoadCacheLocked();
    auto it = g_cache.entries.find(key);
    if (it == g_cache.entries.end()) return false;
    long long age = Now() - it->second.checked;
    long long ttl = std::chrono::duration_cast<std::chrono::seconds>(it->second.found ? kFoundTtl : kMissTtl).count();
    if (age < 0 || age > ttl) return false;
    value = it->second.value;
    found = it->second.found;
    return true;
}

void CachePut(const std::string& key, const std::string& value, bool found) {
    std::lock_guard lock(g_cache.mutex);
    LoadCacheLocked();
    g_cache.entries[key] = {value, Now(), found};
    g_cache.dirty = true;
}

void Start(const std::wstring& dataDir) {
    std::lock_guard lock(g_cache.mutex);
    g_cache.path = dataDir.empty() ? std::wstring{} : dataDir + kPathSep + L"online-cache.txt";
    g_cache.entries.clear();
    g_cache.loaded = g_cache.dirty = false;
}

void Stop() {
    std::lock_guard lock(g_cache.mutex);
    if (!g_cache.dirty || g_cache.path.empty()) return;
    std::string bytes = "1\n";
    long long now = Now(), keep = 24ll * 3600 * 180;
    for (const auto& [key, entry] : g_cache.entries)
        if (now - entry.checked < keep)
            bytes += Escape(key) + '\t' + std::to_string(entry.checked) + '\t' + (entry.found ? "1" : "0") + '\t' +
                     Escape(entry.value) + '\n';
    std::wstring temp = g_cache.path + L".tmp";
    DeleteFileW(temp.c_str());
    if (backup::WriteBytes(temp, bytes) &&
        MoveFileExW(temp.c_str(), g_cache.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        g_cache.dirty = false;
    else
        DeleteFileW(temp.c_str());
}

// --- covers: searching -----------------------------------------------------------------------------

namespace {

std::string Quoted(std::string_view text) {
    std::string out;
    for (char c : text)
        if (c != '"' && c != '\\') out += c;
    return out;
}

unsigned Remaining(Clock::time_point deadline, unsigned limit) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left <= 500 ? 0 : (unsigned)std::min<long long>(left, limit);
}

std::vector<CoverCandidate> Query(CoverSource source, const CoverQuery& q, Cancel& cancel, Clock::time_point deadline) {
    std::string url;
    switch (source) {
        case CoverSource::Deezer: {
            // Deezer's field syntax (artist:"...") no longer matches: it returns unrelated albums or none.
            // Plain words find the album, and the match scoring sorts out look-alikes.
            std::string term = q.album.empty() ? q.artist : q.artist + " " + q.album;
            url = "https://api.deezer.com/search/album?limit=10&q=" + UrlEncode(term);
            break;
        }
        case CoverSource::ITunes:
            url = "https://itunes.apple.com/search?media=music&entity=album&limit=10&term=" +
                  UrlEncode(q.artist + (q.album.empty() ? "" : " " + q.album));
            break;
        case CoverSource::MusicBrainz: {
            std::string term = "artist:\"" + Quoted(q.artist) + "\"";
            if (!q.album.empty()) term = "release:\"" + Quoted(q.album) + "\" AND " + term;
            url = "https://musicbrainz.org/ws/2/release?fmt=json&limit=5&query=" + UrlEncode(term);
            break;
        }
        default: return {};
    }
    unsigned timeout = Remaining(deadline, kApiTimeoutMs);
    if (!timeout) return {};
    Reply reply = Get(url, cancel, timeout);
    if (reply.status != 200) return {};
    switch (source) {
        case CoverSource::Deezer: return ParseDeezerAlbums(reply.body);
        case CoverSource::ITunes: return ParseItunesAlbums(reply.body);
        case CoverSource::MusicBrainz: {
            auto found = ParseMusicBrainzReleases(reply.body);
            if (found.size() > 3) found.resize(3);  // each archive request costs a rate-limited second
            return found;
        }
        default: return {};
    }
}

// A song search for a track with no album: Deezer and iTunes only, as MusicBrainz names recordings, not covers.
std::vector<CoverCandidate> QueryTracks(CoverSource source, const CoverQuery& q, Cancel& cancel, Clock::time_point deadline) {
    std::string url;
    if (source == CoverSource::Deezer)
        url = "https://api.deezer.com/search/track?limit=10&q=" +
              UrlEncode(q.artist + " " + q.title);  // plain words: the field syntax finds nothing
    else if (source == CoverSource::ITunes)
        url = "https://itunes.apple.com/search?media=music&entity=song&limit=10&term=" + UrlEncode(q.artist + " " + q.title);
    else
        return {};
    unsigned timeout = Remaining(deadline, kApiTimeoutMs);
    if (!timeout) return {};
    Reply reply = Get(url, cancel, timeout);
    if (reply.status != 200) return {};
    return source == CoverSource::Deezer ? ParseDeezerTracks(reply.body) : ParseItunesTracks(reply.body);
}

// Downloads and proves one image. The candidate's size becomes the decoded one.
bool Download(CoverCandidate& c, Cancel& cancel, Clock::time_point deadline, const ImageDecoder& decode, int thumbSize,
              CoverFound& out) {
    unsigned timeout = Remaining(deadline, kImageTimeoutMs);
    if (!timeout) return false;
    Reply reply = Get(c.url, cancel, timeout, kMaxImageBytes);
    if (reply.status != 200) return false;
    int w = 0, h = 0;
    if (InspectImage(reply.body, w, h) == ImageType::None) return false;
    Decoded decoded;
    if (!decode || !decode(reply.body, thumbSize, decoded) || decoded.width <= 0 || decoded.height <= 0) return false;
    c.width = decoded.width;
    c.height = decoded.height;
    out.candidate = c;
    out.image = std::move(reply.body);
    out.decoded = std::move(decoded);
    return true;
}

std::string CacheKey(const CoverSearch& search) {
    std::string key = "cover\x1f" + Lower(search.query.artist) + '\x1f' + Lower(search.query.album) + '\x1f';
    for (CoverSource s : search.sources) key += (char)('0' + (int)s);
    return key;
}

}  // namespace

bool SearchCovers(const CoverSearch& search, Cancel& cancel, const ImageDecoder& decode,
                  const std::function<void(CoverFound&&)>& found) {
    if (search.sources.empty() || (search.query.artist.empty() && search.query.album.empty())) return false;
    const auto deadline = Clock::now() + kSearchTime;
    std::mutex mutex;
    bool any = false;
    if (search.manual) {
        // Every provider on its own thread; each shows its covers as they download.
        std::vector<std::thread> threads;
        for (CoverSource source : search.sources)
            threads.emplace_back([&, source] {
                auto candidates = Query(source, search.query, cancel, deadline);
                RankCovers(search.query, candidates, search.sources);
                if (candidates.size() > 8) candidates.resize(8);
                for (auto& c : candidates) {
                    if (cancel.Stopped()) return;
                    CoverFound result;
                    if (!Download(c, cancel, deadline, decode, search.thumbSize, result)) continue;
                    std::vector<CoverCandidate> one{result.candidate};
                    RankCovers(search.query, one, search.sources);
                    result.candidate.score = one[0].score;
                    std::lock_guard lock(mutex);
                    if (cancel.Stopped()) return;
                    any = true;
                    found(std::move(result));
                }
            });
        for (auto& t : threads) t.join();
        return any;
    }

    // Batch: a remembered answer first, then every provider's candidates ranked together.
    std::string key = CacheKey(search), cached;
    bool cachedFound = false;
    if (CacheGet(key, cached, cachedFound)) {
        if (!cachedFound) return false;
        size_t tab = cached.find('\t');
        CoverCandidate c;
        c.source = (CoverSource)std::clamp(atoi(cached.c_str()), 0, kCoverSources - 1);
        c.url = tab == std::string::npos ? std::string{} : cached.substr(tab + 1);
        size_t a = c.url.find('\t'), b = a == std::string::npos ? a : c.url.find('\t', a + 1);
        if (b != std::string::npos) {
            c.artist = c.url.substr(a + 1, b - a - 1);
            c.album = c.url.substr(b + 1);
            c.url.resize(a);
        }
        CoverFound result;
        if (!c.url.empty() && Download(c, cancel, deadline, decode, search.thumbSize, result)) {
            std::vector<CoverCandidate> one{result.candidate};
            RankCovers(search.query, one, search.sources);
            result.candidate.score = one[0].score;
            found(std::move(result));
            return true;
        }
        if (cancel.Stopped()) return false;
    }
    std::vector<CoverCandidate> all;
    {
        std::vector<std::thread> threads;
        for (CoverSource source : search.sources)
            threads.emplace_back([&, source] {
                auto candidates = Query(source, search.query, cancel, deadline);
                std::lock_guard lock(mutex);
                all.insert(all.end(), candidates.begin(), candidates.end());
            });
        for (auto& t : threads) t.join();
    }
    if (cancel.Stopped()) return false;
    RankCovers(search.query, all, search.sources);
    CoverFound best;
    bool have = false;
    for (size_t round = 0; round < 3 && round * 3 < all.size() && !cancel.Stopped(); ++round) {
        for (size_t i = round * 3; i < all.size() && i < round * 3 + 3; ++i) {
            CoverFound result;
            if (!Download(all[i], cancel, deadline, decode, search.thumbSize, result)) continue;
            std::vector<CoverCandidate> one{result.candidate};
            RankCovers(search.query, one, search.sources);
            result.candidate.score = one[0].score;
            if (!have || result.candidate.score > best.candidate.score) {
                best = std::move(result);
                have = true;
            }
        }
        if (have && best.candidate.score >= 4.0) break;
    }
    if (cancel.Stopped()) return false;
    if (!have) {
        // A provider that could not be reached is not a miss worth remembering.
        if (!all.empty() || Clock::now() < deadline) CachePut(key, {}, false);
        return false;
    }
    CachePut(key, std::to_string((int)best.candidate.source) + '\t' + best.candidate.url + '\t' +
                      best.candidate.artist + '\t' + best.candidate.album, true);
    found(std::move(best));
    return true;
}

namespace {

// A Cover Art Archive release may have no art, and its address answers 404 then; the 250-pixel thumbnail is the
// cheapest proof there is an image. `shown` becomes the 500-pixel front, plenty for a small picture.
bool ArchiveHasArt(const std::string& url, Cancel& cancel, Clock::time_point deadline, std::string& shown) {
    size_t at = url.rfind("/front");
    unsigned timeout = Remaining(deadline, kImageTimeoutMs);
    if (at == std::string::npos || !url.starts_with("https://coverartarchive.org/") || !timeout) return false;
    std::string base = url.substr(0, at);
    Reply reply = Get(base + "/front-250", cancel, timeout);
    int w = 0, h = 0;
    if (reply.status != 200 || InspectImage(reply.body, w, h) == ImageType::None) return false;
    shown = base + "/front-500";
    return true;
}

bool IsMbid(std::string_view id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        char c = id[i];
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

}  // namespace

std::string FindCoverUrl(const CoverQuery& query, const std::string& release, const std::vector<CoverSource>& sources,
                         Cancel& cancel) {
    const bool byTrack = query.album.empty();
    if (sources.empty() || query.artist.empty() || (byTrack && query.title.empty())) return {};
    std::string key = (byTrack ? "coverurl-track" + Lower(query.title) : std::string()) + "coverurl\x1f" + Lower(query.artist) + '\x1f' + Lower(query.album) + '\x1f' + release + '\x1f',
                cached;
    for (CoverSource s : sources) key += (char)('0' + (int)s);
    bool found = false;
    if (CacheGet(key, cached, found)) return found ? cached : std::string();
    const auto deadline = Clock::now() + kSearchTime;
    std::string url;
    // The release the tags name is the surest match there is; it needs MusicBrainz's archive turned on.
    if (!byTrack && std::find(sources.begin(), sources.end(), CoverSource::MusicBrainz) != sources.end() &&
        IsMbid(release))
        ArchiveHasArt("https://coverartarchive.org/release/" + release + "/front", cancel, deadline, url);
    // Then the providers one at a time in the user's order, stopping at the first match. Names are compared loosely,
    // as stores add edition notes ("(Deluxe Edition)", "- 2011 Remaster") that tags often leave out.
    const std::string artist = LooseTitle(query.artist), wanted = LooseTitle(byTrack ? query.title : query.album);
    bool answered = false;  // some provider replied with results, so a miss is worth remembering
    for (CoverSource source : sources) {
        if (!url.empty() || cancel.Stopped()) break;
        if (byTrack && source == CoverSource::MusicBrainz) continue;
        auto candidates = byTrack ? QueryTracks(source, query, cancel, deadline) : Query(source, query, cancel, deadline);
        answered = answered || !candidates.empty();
        if (!byTrack) RankCovers(query, candidates, sources);  // the best image first; song results stay in relevance order
        for (const auto& c : candidates) {
            if (!c.url.starts_with("https://") || std::min(c.width, c.height) < 500 || LooseTitle(c.artist) != artist ||
                LooseTitle(byTrack ? c.title : c.album) != wanted)
                continue;
            if (c.source != CoverSource::MusicBrainz) url = c.url;
            else if (!ArchiveHasArt(c.url, cancel, deadline, url)) continue;
            break;
        }
    }
    if (cancel.Stopped()) return {};
    if (!url.empty()) CachePut(key, url, true);
    else if (answered) CachePut(key, {}, false);  // providers that could not be reached are not a miss
    return url;
}

// --- writing ---------------------------------------------------------------------------------------

namespace {

std::string DecodeBase64(std::string_view in) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
        if (v < 0) continue;  // padding, line breaks and the %-escapes a browser may leave in
        acc = acc << 6 | (uint32_t)v;
        if ((bits += 6) >= 8) out += (char)(acc >> (bits -= 8) & 0xFF);
    }
    return out;
}

}  // namespace

TextImage ImageFromText(std::string_view text, Cancel& cancel, std::string& image, std::string* sourceUrl) {
    image.clear();
    if (sourceUrl) sourceUrl->clear();
    while (!text.empty() && (unsigned char)text.front() <= ' ') text.remove_prefix(1);
    while (!text.empty() && (unsigned char)text.back() <= ' ') text.remove_suffix(1);
    int w = 0, h = 0;
    std::string head = Lower(text.substr(0, 32));
    if (head.starts_with("data:image/")) {
        size_t comma = text.find(',');
        if (comma == text.npos || text.size() > kMaxImageBytes * 2 ||
            Lower(text.substr(0, comma)).find(";base64") == std::string::npos)
            return TextImage::Failed;
        image = DecodeBase64(text.substr(comma + 1));
    } else if (head.starts_with("https://") && text.find_first_of(" \t\r\n") == text.npos) {
        Reply reply = Get(std::string(text), cancel, 15000, kMaxImageBytes);
        if (reply.status != 200) return TextImage::Failed;
        image = std::move(reply.body);
        if (sourceUrl) *sourceUrl = std::string(text);  // Discord can show the very address it came from
    } else {
        return head.starts_with("http://") ? TextImage::Failed : TextImage::NotImage;
    }
    if (InspectImage(image, w, h) != ImageType::None) return TextImage::Got;
    image.clear();
    return TextImage::Failed;
}

bool FolderHasCover(const std::wstring& dir) {
    for (const wchar_t* name : {L"cover.jpg", L"folder.jpg", L"front.jpg", L"cover.png", L"folder.png", L"front.png",
                                L"cover.jpeg"}) {
        DWORD attributes = GetFileAttributesW((dir + kPathSep + name).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return true;
    }
    return false;
}

FolderWrite WriteFolderCover(const std::wstring& dir, const std::string& image, bool overwrite, std::wstring& error) {
    int w = 0, h = 0;
    ImageType type = InspectImage(image, w, h);
    if (type == ImageType::None) {
        error = L"the image is not a complete JPEG or PNG file";
        return FolderWrite::Failed;
    }
    if (!overwrite && FolderHasCover(dir)) return FolderWrite::Exists;
    // Players look for the .jpg names first, so a new cover.png would hide behind an old JPEG.
    if (type == ImageType::Png)
        for (const wchar_t* name : {L"cover.jpg", L"folder.jpg", L"front.jpg"}) {
            DWORD attributes = GetFileAttributesW((dir + kPathSep + name).c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                error = L"the new cover is a PNG, and the folder's " + std::wstring(name) + L" would still be shown first";
                return FolderWrite::Failed;
            }
        }
    std::wstring target = dir + kPathSep + (type == ImageType::Png ? L"cover.png" : L"cover.jpg");
    std::wstring temp = dir + kPathSep + L".cordinal-cover-" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    DeleteFileW(temp.c_str());
    if (!backup::WriteBytes(temp, image)) {
        DeleteFileW(temp.c_str());
        error = L"couldn't write in the album folder";
        return FolderWrite::Failed;
    }
    DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temp.c_str(), target.c_str(), flags)) {
        DWORD code = GetLastError();
        DeleteFileW(temp.c_str());
        if (!overwrite && (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS)) return FolderWrite::Exists;
        error = L"couldn't save " + target;
        return FolderWrite::Failed;
    }
    return FolderWrite::Written;
}

// --- lyrics --------------------------------------------------------------------------------------

const wchar_t* LyricsSourceHost(LyricsSource source) {
    switch (source) {
        case LyricsSource::LrcLib: return L"lrclib.net";
        case LyricsSource::LyricsOvh: return L"lyrics.ovh";
        default: return L"";
    }
}

bool AnyLyricsSource() {
    for (bool on : Config().lyrics)
        if (on) return true;
    return false;
}

namespace {

void AddLrcLib(const web::JsonValue& item, std::vector<LyricsFound>& out) {
    if (item.kind != web::JsonValue::Kind::Object || item["instrumental"].text == "true") return;
    LyricsFound found;
    found.source = LyricsSource::LrcLib;
    found.artist = item.String("artistName");
    found.album = item.String("albumName");
    found.title = item.String("trackName");
    // A missing kind of lyrics is null, whose spelling must not pass for text.
    auto text = [&](const char* key) { return item[key].kind == web::JsonValue::Kind::String ? item[key].text : std::string(); };
    found.text = text("syncedLyrics");
    found.synced = !found.text.empty();
    if (found.text.empty()) found.text = text("plainLyrics");
    if (found.text.find_first_not_of(" \t\r\n") == std::string::npos) return;
    out.push_back(std::move(found));
}

}  // namespace

std::vector<LyricsFound> ParseLrcLib(std::string_view json) {
    std::vector<LyricsFound> out;
    web::JsonValue root;
    if (!web::ParseJson(json, root)) return out;
    if (root.kind == web::JsonValue::Kind::Array)
        for (const auto& item : root.items) AddLrcLib(item, out);
    else
        AddLrcLib(root, out);
    return out;
}

std::vector<LyricsFound> ParseLyricsOvh(std::string_view json, const LyricsQuery& query) {
    std::vector<LyricsFound> out;
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return out;
    LyricsFound found;
    found.source = LyricsSource::LyricsOvh;
    found.artist = query.artist;
    found.title = query.title;
    found.text = root.String("lyrics");
    // The service separates lines with CR LF and sometimes leads with a credit line ending in CR.
    std::string clean;
    for (char c : found.text)
        if (c != '\r') clean += c;
    found.text = std::move(clean);
    if (found.text.find_first_not_of(" \t\n") == std::string::npos) return out;
    out.push_back(std::move(found));
    return out;
}

double LyricsScore(const LyricsQuery& query, const LyricsFound& found) {
    bool artist = !query.artist.empty() && Lower(query.artist) == Lower(found.artist);
    bool title = !query.title.empty() && Lower(query.title) == Lower(found.title);
    bool album = !query.album.empty() && Lower(query.album) == Lower(found.album);
    double score = (artist ? 0.5 : 0) + (title ? 0.5 : 0) + (album ? 0.5 : 0);
    if (!artist && !title) score -= 1.5;
    if (found.text.size() > 60) score += 1;
    return score;
}

bool BestLyrics(const LyricsQuery& query, std::vector<LyricsFound> found, LyricsFound& best) {
    bool have = false;
    for (auto& f : found) {
        f.score = LyricsScore(query, f);
        // Another song's lyrics are worse than none: the title has to be the one asked for.
        if (f.score < 1.5 || Lower(f.title) != Lower(query.title)) continue;
        if (!have || (f.synced && !best.synced) || (f.synced == best.synced && f.score > best.score)) {
            best = std::move(f);
            have = true;
        }
    }
    return have;
}

namespace {

std::string LyricsKey(const LyricsQuery& q) {
    std::string key = "lyrics\x1f" + Lower(q.artist) + '\x1f' + Lower(q.title) + '\x1f' + std::to_string(q.seconds) + '\x1f';
    for (int i = 0; i < kLyricsSources; ++i)
        if (Config().lyrics[i]) key += (char)('0' + i);
    return key;
}

}  // namespace

bool FetchLyrics(const LyricsQuery& query, Cancel& cancel, LyricsFound& out) {
    if (!AnyLyricsSource() || query.artist.empty() || query.title.empty()) return false;
    std::string key = LyricsKey(query), cached;
    bool cachedFound = false;
    if (CacheGet(key, cached, cachedFound)) {
        if (!cachedFound) return false;
        size_t a = cached.find('\t'), b = a == std::string::npos ? a : cached.find('\t', a + 1);
        if (b != std::string::npos) {
            out = {};
            out.source = (LyricsSource)std::clamp(atoi(cached.c_str()), 0, kLyricsSources - 1);
            out.synced = cached.substr(a + 1, b - a - 1) == "1";
            out.text = cached.substr(b + 1);
            out.artist = query.artist;
            out.title = query.title;
            return true;
        }
    }
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    std::vector<LyricsFound> found;
    bool reached = false;
    if (Config().lyrics[(int)LyricsSource::LrcLib]) {
        std::string url = "https://lrclib.net/api/get?artist_name=" + UrlEncode(query.artist) +
                          "&track_name=" + UrlEncode(query.title);
        if (!query.album.empty()) url += "&album_name=" + UrlEncode(query.album);
        if (query.seconds) url += "&duration=" + std::to_string(query.seconds);
        if (unsigned timeout = Remaining(deadline, 6000)) {
            Reply reply = Get(url, cancel, timeout);
            reached = reached || reply.status == 200 || reply.status == 404;
            if (reply.status == 200) found = ParseLrcLib(reply.body);
        }
    }
    LyricsFound best;
    bool have = BestLyrics(query, found, best);
    // Synced lyrics are the best there is; otherwise the next source may still do better.
    if (!(have && best.synced) && Config().lyrics[(int)LyricsSource::LyricsOvh] && !cancel.Stopped()) {
        std::string url = "https://api.lyrics.ovh/v1/" + UrlEncode(query.artist) + "/" + UrlEncode(query.title);
        if (unsigned timeout = Remaining(deadline, 6000)) {
            Reply reply = Get(url, cancel, timeout);
            reached = reached || reply.status == 200 || reply.status == 404;
            if (reply.status == 200)
                for (auto& f : ParseLyricsOvh(reply.body, query)) found.push_back(std::move(f));
            have = BestLyrics(query, found, best);
        }
    }
    if (cancel.Stopped()) return false;
    // Only an answer from a source that was reached is remembered; a dropped connection is not a miss.
    if (!have) {
        if (reached) CachePut(key, {}, false);
        return false;
    }
    CachePut(key, std::to_string((int)best.source) + '\t' + (best.synced ? "1" : "0") + '\t' + best.text, true);
    out = std::move(best);
    return true;
}

// --- artist info ---------------------------------------------------------------------------------

std::string ParseArtistSearch(std::string_view json, std::string_view name) {
    web::JsonValue root;
    if (name.empty() || !web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return {};
    const web::JsonValue& artists = root["artists"];
    if (artists.kind != web::JsonValue::Kind::Array) return {};
    std::string want = Lower(name), found;
    int matches = 0;
    for (const auto& item : artists.items) {
        if (Int(item["score"]) < 90 || Lower(item.String("name")) != want) continue;
        std::string id = item.String("id");
        if (id.size() != 36 || id.find_first_not_of("0123456789abcdef-") != std::string::npos) continue;
        found = id;
        ++matches;
    }
    return matches == 1 ? found : std::string();
}

bool ParseArtistLookup(std::string_view json, ArtistInfo& info) {
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return false;
    std::string id = root.String("id");
    if (id.size() != 36 || id.find_first_not_of("0123456789abcdef-") != std::string::npos) return false;
    info.mbid = id;
    info.name = root.String("name");
    info.musicbrainz = "https://musicbrainz.org/artist/" + id;
    const web::JsonValue& relations = root["relations"];
    if (relations.kind != web::JsonValue::Kind::Array) return true;
    for (const auto& r : relations.items) {
        std::string type = r.String("type"), url = r["url"].String("resource");
        if (type == "wikipedia" && url.starts_with("https://en.wikipedia.org/wiki/") && info.wikipedia.empty())
            info.wikipedia = url;
        else if (type == "wikidata" && url.starts_with("https://www.wikidata.org/wiki/Q")) {
            std::string qid = url.substr(30);
            if (qid.size() > 1 && qid.find_first_not_of("0123456789", 1) == std::string::npos) info.wikidata = qid;
        } else if (type == "official homepage" && info.official.empty() &&
                   (url.starts_with("https://") || url.starts_with("http://")) && url.size() < 2048 &&
                   url.find_first_of(" \"<>\r\n\t") == std::string::npos)
            info.official = url;
    }
    return true;
}

std::string ParseWikidataTitle(std::string_view json, std::string_view qid) {
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return {};
    return root["entities"][qid]["sitelinks"]["enwiki"].String("title");
}

bool ParseWikipediaSummary(std::string_view json, std::string& extract, std::string& page) {
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return false;
    if (root.String("type") == "disambiguation") return false;
    extract = root.String("extract");
    page = root["content_urls"]["desktop"].String("page");
    if (!page.starts_with("https://en.wikipedia.org/")) page.clear();
    return !extract.empty();
}

namespace {

std::string ArtistValue(const ArtistInfo& i) {
    return i.mbid + '\t' + i.name + '\t' + i.wikipedia + '\t' + i.official + '\t' + i.wikidata + '\t' + i.summary;
}

bool ReadArtistValue(const std::string& value, ArtistInfo& i) {
    std::vector<std::string> parts;
    size_t at = 0;
    for (int k = 0; k < 5; ++k) {
        size_t tab = value.find('\t', at);
        if (tab == std::string::npos) return false;
        parts.push_back(value.substr(at, tab - at));
        at = tab + 1;
    }
    i.mbid = parts[0];
    i.name = parts[1];
    i.wikipedia = parts[2];
    i.official = parts[3];
    i.wikidata = parts[4];
    i.summary = value.substr(at);
    i.musicbrainz = "https://musicbrainz.org/artist/" + i.mbid;
    return true;
}

}  // namespace

bool FetchArtistInfo(const std::string& name, const std::string& knownId, Cancel& cancel, ArtistInfo& out) {
    if (!Config().artistInfo || (name.empty() && knownId.empty())) return false;
    std::string key = "artist\x1f" + Lower(name) + '\x1f' + knownId, cached;
    bool cachedFound = false;
    if (CacheGet(key, cached, cachedFound)) return cachedFound && ReadArtistValue(cached, out);
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    auto get = [&](const std::string& url, Reply& reply) {
        unsigned timeout = Remaining(deadline, 8000);
        if (!timeout) return false;
        reply = Get(url, cancel, timeout);
        return reply.status == 200;
    };
    Reply reply;
    std::string mbid = knownId;
    bool reached = false;
    if (mbid.empty()) {
        if (!get("https://musicbrainz.org/ws/2/artist?fmt=json&limit=10&query=" +
                     UrlEncode("artist:\"" + Quoted(name) + "\""), reply))
            return false;
        reached = true;
        mbid = ParseArtistSearch(reply.body, name);
    }
    ArtistInfo info;
    if (!mbid.empty() && get("https://musicbrainz.org/ws/2/artist/" + mbid + "?inc=url-rels&fmt=json", reply)) {
        reached = true;
        if (!ParseArtistLookup(reply.body, info)) info = {};
    }
    if (cancel.Stopped()) return false;
    if (info.mbid.empty()) {
        if (reached) CachePut(key, {}, false);
        return false;
    }
    std::string title;
    if (!info.wikipedia.empty()) title = info.wikipedia.substr(30);
    else if (!info.wikidata.empty() &&
             get("https://www.wikidata.org/w/api.php?action=wbgetentities&props=sitelinks&sitefilter=enwiki&format=json&ids=" +
                     info.wikidata, reply)) {
        title = ParseWikidataTitle(reply.body, info.wikidata);
        for (char& c : title) if (c == ' ') c = '_';
        title = UrlEncode(title);
    }
    if (!title.empty() && get("https://en.wikipedia.org/api/rest_v1/page/summary/" + title, reply)) {
        std::string extract, page;
        if (ParseWikipediaSummary(reply.body, extract, page)) {
            info.summary = extract;
            if (!page.empty()) info.wikipedia = page;
        }
    }
    if (cancel.Stopped()) return false;
    CachePut(key, ArtistValue(info), true);
    out = std::move(info);
    return true;
}

FolderWrite WriteLrcFile(const std::wstring& audioPath, const std::string& text, std::wstring& error) {
    size_t dot = audioPath.find_last_of(L'.'), slash = audioPath.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) || text.empty()) {
        error = L"the song has no file name to put a .lrc beside";
        return FolderWrite::Failed;
    }
    std::wstring target = audioPath.substr(0, dot) + L".lrc";
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) return FolderWrite::Exists;
    std::wstring temp = target + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    DeleteFileW(temp.c_str());
    if (!backup::WriteBytes(temp, text)) {
        DeleteFileW(temp.c_str());
        error = L"couldn't write beside the song";
        return FolderWrite::Failed;
    }
    // Without MOVEFILE_REPLACE_EXISTING, a .lrc that appeared meanwhile is kept, not replaced.
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DWORD code = GetLastError();
        DeleteFileW(temp.c_str());
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) return FolderWrite::Exists;
        error = L"couldn't save " + target;
        return FolderWrite::Failed;
    }
    return FolderWrite::Written;
}

}  // namespace online
