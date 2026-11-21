// SoundCloud links (soundcloud.h): the site's own api-v2 read the way its player reads it, with
// the key its own pages publish. Parsing stays pure; only Resolve goes to the network.
#include "soundcloud.h"

#include "web_remote.h"

#include <windows.h>  // the file API the HLS join writes with (the Linux compat layer provides it)

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace soundcloud {

// The pure request builders below are shared with the self-test, so they are declared before the
// implementation helpers that call them.
namespace detail {
std::string UrlEncode(std::string_view text);
std::string WithClientId(std::string url, std::string_view clientId);
std::string ResolveUrl(std::string_view permalink, std::string_view clientId);
std::string LikesUrl(std::string_view userId, std::string_view clientId);
std::string ArtistTracksUrl(std::string_view userId, std::string_view clientId);
std::string SetTracksUrl(std::string_view setId, std::string_view clientId, size_t offset);
std::vector<std::string> AssetScripts(std::string_view html);
bool FindClientId(std::string_view text, std::string& id);
bool FindWidgetClientId(std::string_view text, std::string& id);
bool ParseHlsVariant(std::string_view playlist, std::string_view baseUrl, std::string& url);
bool ParseHlsSegments(std::string_view playlist, std::string_view baseUrl, std::string& initUrl,
                      std::vector<std::string>& segments);
}  // namespace detail

namespace {

// The api-v2 key is asked for politely and one page at a time, so a slow site never stacks
// requests; these sizes leave room for a set whose reply inlines every track.
constexpr unsigned kApiTimeoutMs = 15000, kPageTimeoutMs = 15000;
constexpr size_t kApiMaxBody = 16u << 20, kHtmlMaxBody = 4u << 20, kScriptMaxBody = 2u << 20;
constexpr size_t kWidgetScriptMaxBody = 8u << 20, kSegmentMaxBody = 8u << 20;
constexpr size_t kMaxTracks = 1000;
constexpr int kMaxPages = 40, kMaxRedirects = 5;

// soundcloud.com hands browsers a page; the site answers a browser far more readily than a bare
// library user agent, and the key is only in the assets that page names.
const char kPageHeaders[] =
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36\r\n"
    "Accept: text/html,application/xhtml+xml,application/json;q=0.9,*/*;q=0.8\r\n"
    "Accept-Language: en-US,en;q=0.9\r\n";
const char kApiHeaders[] =
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36\r\n"
    "Accept: application/json\r\n"
    "Referer: https://soundcloud.com/\r\n"
    "Origin: https://soundcloud.com\r\n";

// User-facing sentences, kept in one place so the wording stays consistent.
constexpr const char* kCancelled = "cancelled";
constexpr const char* kNotFound = "that page does not exist on SoundCloud";
constexpr const char* kNoAnswer = "SoundCloud did not answer; try again later";
constexpr const char* kUnreadable = "SoundCloud's answer could not be read";
constexpr const char* kNetwork = "couldn't reach SoundCloud; check your connection and try again";
constexpr const char* kNoKey = "couldn't find SoundCloud's stream key; try again later";
constexpr const char* kShortLink = "couldn't follow that SoundCloud short link";
constexpr const char* kArtistPage =
    "that is an artist page, not a track; add /tracks to take everything the artist posted";
constexpr const char* kNotALink = "that link does not point to a track, a set or an artist's likes";
constexpr const char* kNoStream = "SoundCloud has no playable stream for this track";
constexpr const char* kNotStreamable = "this track is no longer streamable on SoundCloud";
constexpr const char* kPreview = "SoundCloud only allows a preview of this track in your region";
constexpr const char* kNoSetId = "SoundCloud did not say which set that is";
constexpr const char* kNoArtistId = "SoundCloud did not say which artist that is";

bool IsAlnumByte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

std::string_view TrimView(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
                             text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n'))
        text.remove_suffix(1);
    return text;
}

std::string Trim(std::string_view text) { return std::string(TrimView(text)); }

std::string Lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) out += (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return out;
}

bool IsReserved(std::string_view word) {
    static constexpr std::string_view kWords[] = {"sets",  "playlists", "likes", "tracks", "you",   "stream",
                                                  "charts", "discover", "upload", "popular", "stations"};
    const std::string lower = Lower(word);
    for (std::string_view known : kWords)
        if (known == lower) return true;
    return false;
}

bool IsSegment(const std::string& segment, std::string_view word) { return Lower(segment) == word; }

std::string JoinPath(const std::vector<std::string>& segments) {
    std::string path;
    for (const std::string& segment : segments) {
        path += '/';
        path += segment;
    }
    return path;
}

std::vector<std::string> Segments(std::string_view path) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= path.size()) {
        size_t end = path.find('/', at);
        if (end == std::string_view::npos) end = path.size();
        if (end > at) out.emplace_back(path.substr(at, end - at));
        if (end == path.size()) break;
        at = end + 1;
    }
    return out;
}

// One pasted address, taken apart without the network. `shortLink` marks an on.soundcloud.com
// code, whose target only the redirect reveals; `profile` marks a bare artist page, which has a
// permalink but no list of its own.
struct Link {
    Kind kind = Kind::None;
    std::string permalink;
    std::string user;
    bool profile = false;
    bool shortLink = false;
};

Link ParseLink(std::string_view url) {
    Link link;
    std::string_view text = TrimView(url);
    size_t scheme = text.find("://");
    std::string_view rest = text;
    if (scheme != std::string_view::npos) {
        const std::string name = Lower(text.substr(0, scheme));
        if (name != "http" && name != "https") return link;
        rest = text.substr(scheme + 3);
    }
    if (rest.empty()) return link;
    size_t hostEnd = rest.find_first_of("/?#");
    std::string_view hostView = rest.substr(0, hostEnd);
    if (size_t at = hostView.find('@'); at != std::string_view::npos) hostView = hostView.substr(at + 1);
    std::string host = Lower(hostView);
    if (size_t colon = host.find(':'); colon != std::string::npos) host.resize(colon);
    const bool shortHost = host == "on.soundcloud.com";
    if (host == "www.soundcloud.com" || host == "m.soundcloud.com") host = "soundcloud.com";
    if (host != "soundcloud.com" && !shortHost) return link;
    std::string_view path = hostEnd == std::string_view::npos ? std::string_view() : rest.substr(hostEnd);
    if (size_t mark = path.find_first_of("?#"); mark != std::string_view::npos) path = path.substr(0, mark);
    const std::vector<std::string> segments = Segments(path);
    if (shortHost) {
        link.shortLink = true;
        if (segments.size() == 1) {
            // A short code says nothing about what it opens, so it is guessed as a track and
            // Resolve reclassifies whatever the redirect lands on.
            link.kind = Kind::Track;
            link.permalink = "https://on.soundcloud.com/" + segments[0];
        }
        return link;
    }
    if (segments.empty()) return link;
    link.permalink = "https://soundcloud.com" + JoinPath(segments);
    const size_t count = segments.size();
    if (count == 1) {
        if (!IsReserved(segments[0])) link.profile = true;
        return link;
    }
    if (count == 2 && (IsSegment(segments[0], "sets") || IsSegment(segments[0], "playlists"))) {
        link.kind = Kind::Playlist;
        return link;
    }
    if (count == 2 && IsSegment(segments[1], "likes") && !IsReserved(segments[0])) {
        link.kind = Kind::Likes;
        link.user = segments[0];
        return link;
    }
    if (count == 2 && IsSegment(segments[1], "tracks") && !IsReserved(segments[0])) {
        link.kind = Kind::ArtistTracks;
        link.user = segments[0];
        return link;
    }
    if (count == 3 && (IsSegment(segments[1], "sets") || IsSegment(segments[1], "playlists")) &&
        !IsReserved(segments[0])) {
        link.kind = Kind::Playlist;
        return link;
    }
    if (count == 2 && !IsReserved(segments[0]) && !IsReserved(segments[1])) {
        link.kind = Kind::Track;
        return link;
    }
    return link;
}

std::string ProfilePermalink(std::string_view user) { return "https://soundcloud.com/" + std::string(user); }

// A set that arrives without a title still needs a name in the saving list.
std::string NameFromPermalink(const std::string& permalink) {
    const size_t slash = permalink.rfind('/');
    std::string name = slash == std::string::npos ? permalink : permalink.substr(slash + 1);
    for (char& c : name)
        if (c == '-') c = ' ';
    return name;
}

std::string Str(const web::JsonValue& value, std::string_view key) {
    const web::JsonValue& member = value[key];
    return member.kind == web::JsonValue::Kind::String ? member.text : std::string();
}

// Ids and durations arrive as JSON numbers; a missing member gives "", never "null".
std::string NumberText(const web::JsonValue& value) {
    if ((value.kind == web::JsonValue::Kind::Number || value.kind == web::JsonValue::Kind::String) &&
        !value.text.empty())
        return value.text;
    return {};
}

unsigned long long Number(const web::JsonValue& value) {
    if (value.kind != web::JsonValue::Kind::Number) return 0;
    unsigned long long number = 0;
    for (char c : value.text) {
        if (c < '0' || c > '9') return 0;
        number = number * 10 + (unsigned long long)(c - '0');
        if (number > 1000000000ull) return 1000000000ull;
    }
    return number;
}

// api-v2 answers durations in milliseconds; anything longer than a day is not a track length.
unsigned DurationSeconds(const web::JsonValue& track) {
    const unsigned long long ms = Number(track["duration"]);
    if (ms > 24ull * 60 * 60 * 1000) return 0;
    return (unsigned)(ms / 1000);
}

// artwork_url and avatar_url name a size token in the file name ("-large.jpg"); the big JPEG is
// "-t500x500.jpg". URLs without a known token are left alone rather than guessed at.
std::string BigArtwork(std::string url) {
    static constexpr std::string_view kTokens[] = {"large",     "small",     "tiny",    "badge",     "crop",
                                                   "mini",      "original",  "t67x67",  "t120x120",  "t200x200",
                                                   "t240x240",  "t300x300",  "t500x500"};
    const size_t dot = url.rfind('.');
    if (dot == std::string::npos) return url;
    const size_t slash = url.rfind('/');
    const size_t dash = url.rfind('-', dot);
    if (dash == std::string::npos || dash < slash || dash + 1 >= dot) return url;
    const std::string_view token(url.data() + dash + 1, dot - dash - 1);
    for (std::string_view known : kTokens) {
        if (known == token) {
            url.replace(dash + 1, token.size(), "t500x500");
            return url;
        }
    }
    return url;
}

std::string Protocol(const web::JsonValue& transcoding) {
    std::string protocol = Str(transcoding, "protocol");
    if (protocol.empty()) protocol = Str(transcoding["format"], "protocol");
    return protocol;
}

std::string MimeType(const web::JsonValue& transcoding) {
    std::string mime = Str(transcoding, "mime_type");
    if (mime.empty()) mime = Str(transcoding["format"], "mime_type");
    return mime;
}

// The pure half of ParseTrack, so lists that are already parsed JSON need no reserializing.
bool FillTrack(const web::JsonValue& track, WebTrack& out) {
    if (track.kind != web::JsonValue::Kind::Object) return false;
    const std::string kind = Str(track, "kind");
    if (kind == "user" || kind == "playlist" || kind == "system-playlist") return false;
    // A torn reply can leave a placeholder entry behind; without a real id it is not a track.
    const std::string id = NumberText(track["id"]);
    if (id.empty() || id == "0" ||
        !std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    const std::string title = Trim(Str(track, "title"));
    if (title.empty()) return false;

    WebTrack filled;
    filled.site = "soundcloud";
    filled.id = id;
    filled.title = title;
    filled.artist = Trim(Str(track["user"], "username"));
    filled.durationSec = DurationSeconds(track);
    std::string art = Str(track, "artwork_url");
    if (art.empty()) art = Str(track["user"], "avatar_url");
    filled.artworkUrl = BigArtwork(Trim(art));

    // A removed, blocked or regional-only track keeps its name in the list but is never fetched.
    const std::string policy = Str(track, "policy");
    const bool snipped = track["snipped"].text == "true";
    const bool streamableOff = track["streamable"].text == "false";
    if (policy == "NOT_STREAMABLE" || policy == "BLOCK") {
        filled.unavailable = true;
        filled.why = kNotStreamable;
    } else if (snipped) {
        filled.unavailable = true;
        filled.why = kPreview;
    } else if (streamableOff) {
        filled.unavailable = true;
        filled.why = kNotStreamable;
    }
    if (filled.unavailable) {
        out = std::move(filled);
        return true;
    }

    // Only a progressive transcoding is a plain file. Most of SoundCloud's music now
    // streams as HLS, and its audio/mp4 flavour is a fragmented MP4: writing the init
    // segment and then the media segments, in order, gives a real .m4a - the same
    // lossless join Puto makes of Movy's streams. The audio/mpegurl flavour is a
    // transport stream with no init section and no place for tags, so it is reported
    // instead of saved.
    std::string progressive, mpeg, hlsMp4;
    bool hlsOther = false;
    const web::JsonValue& transcodings = track["media"]["transcodings"];
    if (transcodings.kind == web::JsonValue::Kind::Array) {
        for (const web::JsonValue& transcoding : transcodings.items) {
            const std::string url = Str(transcoding, "url");
            if (url.empty()) continue;
            const std::string protocol = Protocol(transcoding);
            if (protocol == "progressive") {
                if (progressive.empty()) progressive = url;
                if (mpeg.empty() && MimeType(transcoding) == "audio/mpeg") mpeg = url;
            } else if (protocol == "hls" || protocol.find("hls") != std::string::npos) {
                if (hlsMp4.empty() && MimeType(transcoding).rfind("audio/mp4", 0) == 0)
                    hlsMp4 = url;
                else
                    hlsOther = true;
            }
        }
    }
    // The stream URL is stored exactly as api-v2 gave it; Resolve attaches the client id
    // afterwards, and the download queue must never append a second one.
    if (!mpeg.empty())
        filled.streamUrl = mpeg;
    else if (!progressive.empty())
        filled.streamUrl = progressive;
    else if (!hlsMp4.empty()) {
        filled.streamUrl = hlsMp4;
        filled.hls = true;
    } else if (hlsOther)
        filled.hlsOnly = true;
    else {
        filled.unavailable = true;
        filled.why = kNoStream;
    }
    // The progressive presets SoundCloud serves are MP3; an HLS audio/mp4 joins into an
    // M4A; unavailable tracks keep "".
    if (!filled.streamUrl.empty()) filled.ext = filled.hls ? "m4a" : "mp3";
    out = std::move(filled);
    return true;
}

// One entry of a playlist, a likes page or an artist's tracks page. A likes entry wraps its track
// ("{"track": {...}}"); a liked set ("{"playlist": {...}}") is skipped and reported once.
bool AddListEntry(const web::JsonValue& entry, std::vector<WebTrack>& out, bool* sawSet) {
    if (entry.kind != web::JsonValue::Kind::Object) return false;
    const web::JsonValue& wrapped = entry["track"];
    if (wrapped.kind == web::JsonValue::Kind::Object) {
        WebTrack track;
        if (!FillTrack(wrapped, track)) return false;
        out.push_back(std::move(track));
        return true;
    }
    if (entry["playlist"].kind == web::JsonValue::Kind::Object) {
        if (sawSet) *sawSet = true;
        return false;
    }
    WebTrack track;
    if (!FillTrack(entry, track)) return false;
    out.push_back(std::move(track));
    return true;
}

// The api-v2 key the site's own player uses. Resolve runs on the caller's single worker thread,
// so this cache needs no lock; a 401/403 clears it and the key is looked up once more.
std::string& CachedClientId() {
    static std::string id;
    return id;
}

// The widget page that carries the key fallback answers only a real permalink, so the
// last one this process resolved is what discovery asks it for. A run that never
// resolved anything has nothing to ask, and reports the key as missing.
std::string& DiscoveryPermalink() {
    static std::string permalink;
    return permalink;
}

bool DiscoverClientId(const std::atomic<bool>& cancel, std::string& error) {
    web::HttpReply home =
        web::HttpFetch(nullptr, "https://soundcloud.com/", "GET", {}, kPageHeaders, kPageTimeoutMs, kHtmlMaxBody);
    if (cancel.load()) {
        error = kCancelled;
        return false;
    }
    if (home.status == 0) {
        error = kNetwork;
        return false;
    }
    std::string id;
    if (home.status == 200 && detail::FindClientId(home.body, id)) {
        CachedClientId() = id;
        return true;
    }
    const std::vector<std::string> scripts = detail::AssetScripts(home.body);
    const size_t tried = std::min<size_t>(scripts.size(), 4);
    for (size_t i = 0; i < tried; ++i) {
        if (cancel.load()) {
            error = kCancelled;
            return false;
        }
        web::HttpReply reply =
            web::HttpFetch(nullptr, scripts[i], "GET", {}, kPageHeaders, kPageTimeoutMs, kScriptMaxBody);
        if (cancel.load()) {
            error = kCancelled;
            return false;
        }
        if (reply.status == 200 && detail::FindClientId(reply.body, id)) {
            CachedClientId() = id;
            return true;
        }
    }
    // The web app now fetches its key through a runtime handshake instead of naming it
    // in a script. The standalone widget player is served to every anonymous embed, and
    // its script still names the pair of keys; the pair's anonymous member is what
    // api-v2 accepts without a login. The page only answers a real permalink.
    if (!DiscoveryPermalink().empty()) {
        web::HttpReply widget = web::HttpFetch(
            nullptr, "https://w.soundcloud.com/player/?url=" + detail::UrlEncode(DiscoveryPermalink()),
            "GET", {}, kPageHeaders, kPageTimeoutMs, kHtmlMaxBody);
        if (cancel.load()) {
            error = kCancelled;
            return false;
        }
        if (widget.status == 200) {
            static constexpr std::string_view kWidgetMarker = "widget.sndcdn.com/";
            std::vector<std::string> widgetScripts;
            for (size_t at = widget.body.find(kWidgetMarker); at != std::string::npos;
                 at = widget.body.find(kWidgetMarker, at + 1)) {
                size_t start = at + kWidgetMarker.size(), end = start;
                while (end < widget.body.size() &&
                       (IsAlnumByte(widget.body[end]) || widget.body[end] == '-' ||
                        widget.body[end] == '_' || widget.body[end] == '.'))
                    ++end;
                const std::string_view name = std::string_view(widget.body).substr(start, end - start);
                if (name.size() < 4u || !name.ends_with(".js")) continue;
                std::string url = "https://widget.sndcdn.com/" + std::string(name);
                if (std::find(widgetScripts.begin(), widgetScripts.end(), url) == widgetScripts.end())
                    widgetScripts.push_back(std::move(url));
            }
            const size_t widgetTried = std::min<size_t>(widgetScripts.size(), 4);
            for (size_t i = 0; i < widgetTried; ++i) {
                if (cancel.load()) {
                    error = kCancelled;
                    return false;
                }
                // The widget's main script is a few MB, larger than the site's own assets.
                web::HttpReply reply = web::HttpFetch(nullptr, widgetScripts[i], "GET", {}, kPageHeaders,
                                                      kPageTimeoutMs, kWidgetScriptMaxBody);
                if (cancel.load()) {
                    error = kCancelled;
                    return false;
                }
                if (reply.status == 200 && (detail::FindClientId(reply.body, id) ||
                                            detail::FindWidgetClientId(reply.body, id))) {
                    CachedClientId() = id;
                    return true;
                }
            }
        }
    }
    error = kNoKey;
    return false;
}

std::string Absolute(std::string_view base, std::string_view location) {
    if (location.find("://") != std::string_view::npos) return std::string(location);
    if (location.starts_with("//")) return "https:" + std::string(location);
    const size_t scheme = base.find("://");
    const size_t hostEnd = scheme == std::string_view::npos ? std::string_view::npos : base.find('/', scheme + 3);
    const std::string origin(base.substr(0, hostEnd));
    if (!location.empty() && location[0] == '/') return origin + std::string(location);
    return origin + "/" + std::string(location);
}

// HLS resolves its addresses against the playlist's own directory, the way the spec
// says; api-v2 redirects resolve against the origin above. Both live here so each
// caller says which it means.
std::string PlaylistAbsolute(std::string_view baseUrl, std::string_view location) {
    if (location.find("://") != std::string_view::npos) return std::string(location);
    if (location.starts_with("//")) return "https:" + std::string(location);
    const size_t scheme = baseUrl.find("://");
    const size_t hostEnd = scheme == std::string_view::npos ? std::string_view::npos
                                                            : baseUrl.find('/', scheme + 3);
    const std::string origin(baseUrl.substr(0, hostEnd == std::string_view::npos ? baseUrl.size() : hostEnd));
    if (!location.empty() && location[0] == '/') return origin + std::string(location);
    const size_t dir = baseUrl.rfind('/');
    return dir == std::string_view::npos ? origin + "/" + std::string(location)
                                         : std::string(baseUrl.substr(0, dir + 1)) + std::string(location);
}

// One api-v2 request with the current key. `url` may carry a stale client_id (a next_href does);
// WithClientId replaces it, so a key refreshed mid-run applies to the next page too.
bool ApiGet(const std::atomic<bool>& cancel, const std::string& start, web::JsonValue& reply, std::string& error) {
    std::string url = start;
    for (int redirect = 0; redirect < 4; ++redirect) {
        bool followed = false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (cancel.load()) {
                error = kCancelled;
                return false;
            }
            if (CachedClientId().empty() && !DiscoverClientId(cancel, error)) return false;
            if (cancel.load()) {
                error = kCancelled;
                return false;
            }
            web::HttpReply http = web::HttpFetch(nullptr, detail::WithClientId(url, CachedClientId()), "GET", {},
                                                 kApiHeaders, kApiTimeoutMs, kApiMaxBody);
            if (cancel.load()) {
                error = kCancelled;
                return false;
            }
            if ((http.status == 401 || http.status == 403) && attempt == 0) {
                CachedClientId().clear();
                continue;
            }
            const auto location = http.headers.find("location");
            if (http.status >= 300 && http.status < 400 && location != http.headers.end() &&
                !location->second.empty()) {
                // Resolve answers 302 for an old permalink's canonical address.
                url = Absolute(url, location->second);
                followed = true;
                break;
            }
            if (http.status == 404) {
                error = kNotFound;
                return false;
            }
            if (http.status != 200) {
                error = http.status == 0 ? kNetwork : kNoAnswer;
                return false;
            }
            if (!web::ParseJson(http.body, reply)) {
                error = kUnreadable;
                return false;
            }
            return true;
        }
        if (!followed) break;
    }
    error = kNoAnswer;
    return false;
}

using Notice = std::function<void(const std::string&)>;

bool FollowShortLink(const std::atomic<bool>& cancel, std::string& url, const Notice& notice, std::string& error) {
    if (notice) notice("Following the short link...");
    for (int hop = 0; hop < kMaxRedirects; ++hop) {
        if (cancel.load()) {
            error = kCancelled;
            return false;
        }
        web::HttpReply reply =
            web::HttpFetch(nullptr, url, "GET", {}, kPageHeaders, kPageTimeoutMs, kHtmlMaxBody);
        if (cancel.load()) {
            error = kCancelled;
            return false;
        }
        const auto location = reply.headers.find("location");
        if (reply.status >= 300 && reply.status < 400 && location != reply.headers.end() &&
            !location->second.empty()) {
            url = Absolute(url, location->second);
            continue;
        }
        if (reply.status == 404) {
            error = kNotFound;
            return false;
        }
        if (reply.status == 0) {
            error = kNetwork;
            return false;
        }
        if (ParseLink(url).shortLink) {
            error = kShortLink;
            return false;
        }
        return true;
    }
    error = kShortLink;
    return false;
}

// Stream URLs go out ready to fetch: the media endpoint answers 401 without the key, and the
// queue must not append a second one.
void AttachClientIds(std::vector<WebTrack>& tracks) {
    for (WebTrack& track : tracks)
        if (!track.streamUrl.empty()) track.streamUrl = detail::WithClientId(track.streamUrl, CachedClientId());
}

void NumberTracks(std::vector<WebTrack>& tracks) {
    for (size_t i = 0; i < tracks.size(); ++i) {
        tracks[i].trackNo = (unsigned)(i + 1);
        tracks[i].trackTotal = (unsigned)tracks.size();
    }
}

bool ResolveTrack(const Link& link, const std::atomic<bool>& cancel, std::vector<WebTrack>& tracks,
                  std::string& error, const Notice& notice) {
    DiscoveryPermalink() = link.permalink;
    if (notice) notice("Reading the track...");
    web::JsonValue reply;
    if (!ApiGet(cancel, detail::ResolveUrl(link.permalink, {}), reply, error)) return false;
    WebTrack track;
    if (!FillTrack(reply, track)) {
        error = kNotFound;
        return false;
    }
    if (!track.streamUrl.empty()) track.streamUrl = detail::WithClientId(track.streamUrl, CachedClientId());
    tracks.push_back(std::move(track));
    return true;
}

bool ResolveSet(const Link& link, const std::atomic<bool>& cancel, std::vector<WebTrack>& tracks,
                std::string& listTitle, std::string& error, const Notice& notice) {
    DiscoveryPermalink() = link.permalink;
    if (notice) notice("Reading the set...");
    web::JsonValue reply;
    if (!ApiGet(cancel, detail::ResolveUrl(link.permalink, {}), reply, error)) return false;
    if (reply.kind != web::JsonValue::Kind::Object) {
        error = kNotFound;
        return false;
    }
    listTitle = Trim(Str(reply, "title"));
    if (listTitle.empty()) listTitle = NameFromPermalink(link.permalink);

    const web::JsonValue& inlineTracks = reply["tracks"];
    size_t inlineCount = 0;
    if (inlineTracks.kind == web::JsonValue::Kind::Array) {
        inlineCount = inlineTracks.items.size();
        for (const web::JsonValue& entry : inlineTracks.items) {
            if (tracks.size() >= kMaxTracks) break;
            AddListEntry(entry, tracks, nullptr);
        }
    }
    // A resolve usually inlines every track; when it gives none, or only the first page, the
    // rest come from the set's own tracks endpoint, 50 at a time.
    unsigned long long declared = Number(reply["track_count"]);
    declared = std::min<unsigned long long>(declared, kMaxTracks);
    if (declared < tracks.size()) declared = tracks.size();
    if (tracks.size() < declared) {
        const std::string setId = NumberText(reply["id"]);
        if (setId.empty()) {
            if (tracks.empty()) {
                error = kNoSetId;
                return false;
            }
        } else {
            size_t offset = tracks.empty() ? 0 : inlineCount;
            while (tracks.size() < declared && tracks.size() < kMaxTracks) {
                web::JsonValue page;
                const std::string pageUrl = detail::SetTracksUrl(setId, CachedClientId(), offset);
                if (!ApiGet(cancel, pageUrl, page, error)) {
                    // The per-set tracks endpoint is retired (404 on every spelling of
                    // it, measured November 20, 2026): a resolve's own inline list is
                    // what the site serves now, and removed tracks leave torn placeholders
                    // behind that FillTrack rightly drops. The playable tracks are
                    // saved instead of failing the whole set.
                    if (notice) notice("SoundCloud listed only part of this set; saving what it listed");
                    error.clear();
                    break;
                }
                // The endpoint answers with a plain array; a collection wrapper is tolerated too.
                const web::JsonValue& entries =
                    page.kind == web::JsonValue::Kind::Array ? page : page["collection"];
                if (entries.kind != web::JsonValue::Kind::Array || entries.items.empty()) break;
                for (const web::JsonValue& entry : entries.items) {
                    if (tracks.size() >= kMaxTracks) break;
                    AddListEntry(entry, tracks, nullptr);
                }
                offset += entries.items.size();
            }
        }
    }
    if (tracks.empty()) return true;  // an empty set was read; there is simply nothing to save
    NumberTracks(tracks);
    AttachClientIds(tracks);
    return true;
}

bool ResolveCollection(const Link& link, bool likes, const std::atomic<bool>& cancel,
                       std::vector<WebTrack>& tracks, std::string& listTitle, std::string& error,
                       const Notice& notice) {
    DiscoveryPermalink() = ProfilePermalink(link.user);
    if (notice) notice(likes ? "Reading the likes..." : "Reading the artist's tracks...");
    web::JsonValue profile;
    if (!ApiGet(cancel, detail::ResolveUrl(ProfilePermalink(link.user), {}), profile, error)) return false;
    const std::string userId = NumberText(profile["id"]);
    if (userId.empty()) {
        error = kNoArtistId;
        return false;
    }
    std::string name = Trim(Str(profile, "username"));
    if (name.empty()) name = link.user;
    listTitle = name + (likes ? "'s likes" : "'s tracks");

    std::string next = likes ? detail::LikesUrl(userId, CachedClientId())
                             : detail::ArtistTracksUrl(userId, CachedClientId());
    bool sawSet = false;
    for (int page = 0; !next.empty() && page < kMaxPages && tracks.size() < kMaxTracks; ++page) {
        web::JsonValue reply;
        if (!ApiGet(cancel, next, reply, error)) return false;
        if (reply.kind != web::JsonValue::Kind::Object) break;
        const web::JsonValue& collection = reply["collection"];
        if (collection.kind == web::JsonValue::Kind::Array) {
            for (const web::JsonValue& entry : collection.items) {
                if (tracks.size() >= kMaxTracks) break;
                AddListEntry(entry, tracks, &sawSet);
            }
        }
        next = Str(reply, "next_href");
    }
    if (likes && sawSet && notice) notice("A set in these likes was skipped; paste its own link to save it too.");
    if (tracks.empty()) return true;
    AttachClientIds(tracks);
    return true;
}

}  // namespace

namespace detail {

std::string UrlEncode(std::string_view text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

std::string WithClientId(std::string url, std::string_view clientId) {
    std::string fragment;
    if (size_t hash = url.find('#'); hash != std::string::npos) {
        fragment = url.substr(hash);
        url.resize(hash);
    }
    std::string query;
    if (size_t mark = url.find('?'); mark != std::string::npos) {
        query = url.substr(mark + 1);
        url.resize(mark);
    }
    std::string kept;
    for (size_t at = 0; at < query.size();) {
        size_t end = query.find('&', at);
        if (end == std::string::npos) end = query.size();
        const std::string_view param(query.data() + at, end - at);
        // A stale key from an earlier reply must not travel beside the current one.
        if (!param.empty() && !param.starts_with("client_id=")) {
            if (!kept.empty()) kept += '&';
            kept += param;
        }
        at = end + 1;
    }
    if (!kept.empty()) kept += '&';
    kept += "client_id=";
    kept += UrlEncode(clientId);
    return url + "?" + kept + fragment;
}

std::string ResolveUrl(std::string_view permalink, std::string_view clientId) {
    return "https://api-v2.soundcloud.com/resolve?url=" + UrlEncode(permalink) +
           "&client_id=" + UrlEncode(clientId);
}

std::string LikesUrl(std::string_view userId, std::string_view clientId) {
    return "https://api-v2.soundcloud.com/users/" + UrlEncode(userId) +
           "/likes?limit=50&linked_partitioning=1&client_id=" + UrlEncode(clientId);
}

std::string ArtistTracksUrl(std::string_view userId, std::string_view clientId) {
    return "https://api-v2.soundcloud.com/users/" + UrlEncode(userId) +
           "/tracks?limit=50&linked_partitioning=1&client_id=" + UrlEncode(clientId);
}

std::string SetTracksUrl(std::string_view setId, std::string_view clientId, size_t offset) {
    return "https://api-v2.soundcloud.com/playlists/" + UrlEncode(setId) +
           "/tracks?limit=50&offset=" + std::to_string(offset) + "&client_id=" + UrlEncode(clientId);
}

// The site's asset scripts are named in the page; escaped or protocol-relative spellings are
// common in the hydration data, so only the host-and-path part is matched.
std::vector<std::string> AssetScripts(std::string_view html) {
    std::vector<std::string> scripts;
    static constexpr std::string_view kMarker = "a-v2.sndcdn.com/assets/";
    for (size_t at = html.find(kMarker); at != std::string_view::npos; at = html.find(kMarker, at + 1)) {
        size_t start = at + kMarker.size(), end = start;
        while (end < html.size() &&
               (IsAlnumByte(html[end]) || html[end] == '-' || html[end] == '_' || html[end] == '.'))
            ++end;
        const std::string_view name = html.substr(start, end - start);
        if (name.size() < 4u || !name.ends_with(".js")) continue;
        std::string url = "https://a-v2.sndcdn.com/assets/" + std::string(name);
        if (std::find(scripts.begin(), scripts.end(), url) == scripts.end()) scripts.push_back(std::move(url));
    }
    return scripts;
}

bool FindClientId(std::string_view text, std::string& id) {
    for (size_t at = text.find("client_id"); at != std::string_view::npos; at = text.find("client_id", at + 1)) {
        size_t i = at + 9;
        // The pair may be plain JSON ("client_id":"...") or escaped inside a script string
        // (client_id\":\"..."), where the key's closing quote follows a backslash.
        while (i < text.size() && (text[i] == '\\' || text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
                                   text[i] == '\n'))
            ++i;
        if (i < text.size() && text[i] == '"') ++i;
        while (i < text.size() && (text[i] == '\\' || text[i] == ' ' || text[i] == '\t' || text[i] == '\r' ||
                                   text[i] == '\n'))
            ++i;
        if (i >= text.size() || text[i] != ':') continue;
        ++i;
        while (i < text.size() && (text[i] == '\\' || text[i] == ' ')) ++i;
        if (i >= text.size() || text[i] != '"') continue;
        const size_t start = ++i;
        while (i < text.size() && IsAlnumByte(text[i])) ++i;
        if (i - start == 32u) {
            id.assign(text.substr(start, 32));
            return true;
        }
    }
    return false;
}

// The widget's own script names its keys in a ternary - client_id:<cond>?"signed-in
// key":"anonymous key" - so the last quoted 32-char value of the pair is the anonymous
// one api-v2 serves without a login.
bool FindWidgetClientId(std::string_view text, std::string& id) {
    for (size_t at = text.find("client_id"); at != std::string_view::npos;
         at = text.find("client_id", at + 1)) {
        const size_t colon = text.find(':', at + 9);
        if (colon == std::string_view::npos || colon > at + 40) continue;
        std::string last;
        size_t found = 0;
        const size_t stop = std::min(text.size(), colon + 200);
        for (size_t i = colon + 1; i < stop; ++i) {
            if (text[i] != '"') continue;
            const size_t start = i + 1;
            size_t end = start;
            while (end < text.size() && IsAlnumByte(text[end])) ++end;
            if (end - start == 32u) {
                last.assign(text.substr(start, 32));
                ++found;
                i = end;
            }
        }
        if (found > 0) {
            id = last;
            return true;
        }
    }
    return false;
}

// A master playlist names its variants after #EXT-X-STREAM-INF; the first line after
// the tag is what the entry was for (SoundCloud's entries point at one bitrate each).
bool ParseHlsVariant(std::string_view playlist, std::string_view baseUrl, std::string& url) {
    const size_t variant = playlist.find("#EXT-X-STREAM-INF");
    if (variant == std::string_view::npos) return false;
    const size_t lineEnd = playlist.find('\n', variant);
    if (lineEnd == std::string_view::npos) return false;
    size_t at = lineEnd + 1;
    while (at < playlist.size() && (playlist[at] == '\r' || playlist[at] == ' ')) ++at;
    size_t end = at;
    while (end < playlist.size() && playlist[end] != '\n' && playlist[end] != '\r') ++end;
    if (end == at) return false;
    url = PlaylistAbsolute(baseUrl, playlist.substr(at, end - at));
    return !url.empty();
}

// A media playlist's init section (#EXT-X-MAP) and media segments, made absolute
// against the playlist's own address. Without an init section there is no file header
// to join onto, so the caller refuses rather than saving fragments that cannot play.
bool ParseHlsSegments(std::string_view playlist, std::string_view baseUrl, std::string& initUrl,
                      std::vector<std::string>& segments) {
    segments.clear();
    initUrl.clear();
    bool hasMap = false;
    std::vector<std::string> found;
    for (size_t line = 0; line < playlist.size();) {
        size_t end = playlist.find('\n', line);
        if (end == std::string_view::npos) end = playlist.size();
        std::string_view row = playlist.substr(line, end - line);
        while (!row.empty() && (row.back() == '\r' || row.back() == ' ')) row.remove_suffix(1);
        if (!row.empty() && row.front() != '#') {
            std::string url = PlaylistAbsolute(baseUrl, row);
            if (!url.empty()) found.push_back(std::move(url));
        } else if (row.starts_with("#EXT-X-MAP")) {
            const size_t uri = row.find("URI=\"");
            if (uri != std::string_view::npos) {
                const size_t start = uri + 5;
                const size_t close = row.find('"', start);
                if (close != std::string_view::npos) {
                    initUrl = PlaylistAbsolute(baseUrl, row.substr(start, close - start));
                    hasMap = true;
                }
            }
        }
        line = end + 1;
    }
    if (!hasMap || found.empty()) return false;
    segments = std::move(found);
    return true;
}

}  // namespace detail

namespace {

// An HLS entry answers with a JSON redirect to a signed playlist; the signed address
// lives for minutes, so the hop happens here, at download time, never at resolve time.
bool HlsPlaylistUrl(const std::string& entry, const std::atomic<bool>& cancel, std::string& playlistUrl,
                    std::string& error) {
    if (cancel.load()) {
        error = kCancelled;
        return false;
    }
    web::HttpReply reply = web::HttpFetch(nullptr, entry, "GET", {}, kApiHeaders, kApiTimeoutMs, kApiMaxBody);
    if (cancel.load()) {
        error = kCancelled;
        return false;
    }
    if (reply.status == 200 && reply.body.compare(0, 7, "#EXTM3U") == 0) {
        playlistUrl = entry;  // the entry answered with the playlist itself
        return true;
    }
    if (reply.status != 200) {
        error = reply.status == 0 ? kNetwork : kNoAnswer;
        return false;
    }
    web::JsonValue json;
    if (web::ParseJson(reply.body, json)) {
        const std::string url = Str(json, "url");
        if (!url.empty()) {
            playlistUrl = url;
            return true;
        }
    }
    error = kUnreadable;
    return false;
}

}  // namespace

// An HLS save is a lossless copy: the playlist's init segment is the file header, and
// the media segments appended after it are the whole audio - the same join Puto makes
// of Movy's fragmented-MP4 streams. One request at a time, `cancel` polled around each,
// progress by segment.
bool DownloadHlsTrack(const WebTrack& track, const std::atomic<bool>& cancel, const std::wstring& file,
                     const std::function<void(int)>& progress, std::string& error) {
    error.clear();
    if (track.streamUrl.empty()) {
        error = "the track has no stream to save";
        return false;
    }
    std::string playlistUrl;
    if (!HlsPlaylistUrl(track.streamUrl, cancel, playlistUrl, error)) return false;
    web::HttpReply media =
        web::HttpFetch(nullptr, playlistUrl, "GET", {}, kApiHeaders, kApiTimeoutMs, kApiMaxBody);
    if (cancel.load()) {
        error = kCancelled;
        return false;
    }
    if (media.status != 200) {
        error = media.status == 0 ? kNetwork : kNoAnswer;
        return false;
    }
    // A master playlist names variants after #EXT-X-STREAM-INF; an entry usually points
    // at the one media playlist directly.
    std::string variantUrl;
    if (detail::ParseHlsVariant(media.body, playlistUrl, variantUrl)) {
        if (cancel.load()) {
            error = kCancelled;
            return false;
        }
        media = web::HttpFetch(nullptr, variantUrl, "GET", {}, kApiHeaders, kApiTimeoutMs, kApiMaxBody);
        if (cancel.load()) {
            error = kCancelled;
            return false;
        }
        if (media.status != 200) {
            error = media.status == 0 ? kNetwork : kNoAnswer;
            return false;
        }
        playlistUrl = variantUrl;
    }
    std::string initUrl;
    std::vector<std::string> segments;
    if (!detail::ParseHlsSegments(media.body, playlistUrl, initUrl, segments)) {
        error = "SoundCloud streams this track in a form that cannot be joined into a file";
        return false;
    }
    HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        error = "couldn't create the temporary file";
        return false;
    }
    auto writeAll = [&](const std::string& bytes) {
        DWORD at = 0;
        while (at < bytes.size()) {
            DWORD chunk = (DWORD)std::min<size_t>(bytes.size() - at, 1u << 20), wrote = 0;
            if (!WriteFile(out, bytes.data() + at, chunk, &wrote, nullptr) || wrote != chunk) return false;
            at += chunk;
        }
        return true;
    };
    auto fetchBytes = [&](const std::string& url, std::string& bytes) {
        web::HttpReply reply =
            web::HttpFetch(nullptr, url, "GET", {}, kApiHeaders, kApiTimeoutMs, kSegmentMaxBody);
        if (cancel.load()) return false;
        if (reply.status != 200) return false;
        bytes = std::move(reply.body);
        return true;
    };
    std::string bytes;
    bool ok = fetchBytes(initUrl, bytes) && writeAll(bytes);
    for (size_t i = 0; ok && i < segments.size(); ++i) {
        if (cancel.load()) {
            ok = false;
            break;
        }
        std::string segment;
        if (!fetchBytes(segments[i], segment) && (cancel.load() || !fetchBytes(segments[i], segment))) {
            ok = false;
            break;
        }
        ok = writeAll(segment);
        if (progress) progress((int)((i + 1) * 100 / segments.size()));
    }
    CloseHandle(out);
    if (!ok) {
        DeleteFileW(file.c_str());
        error = cancel.load() ? kCancelled : kNoAnswer;
        return false;
    }
    return true;
}

bool RefreshTrack(WebTrack& track, const std::atomic<bool>& cancel, std::string& error) {
    error.clear();
    if (track.id.empty()) {
        error = "the track has no id to read again";
        return false;
    }
    web::JsonValue reply;
    if (!ApiGet(cancel, "https://api-v2.soundcloud.com/tracks/" + track.id, reply, error)) return false;
    WebTrack fresh;
    if (!FillTrack(reply, fresh)) {
        error = kUnreadable;
        return false;
    }
    if (fresh.unavailable) {
        error = fresh.why;
        return false;
    }
    // The list's identity stays - title, album, numbering, artwork are the real item's;
    // only the stream and its format are new, and the current key is attached.
    track.streamUrl = fresh.streamUrl;
    track.ext = fresh.ext;
    track.hls = fresh.hls;
    track.hlsOnly = fresh.hlsOnly;
    if (!track.streamUrl.empty()) track.streamUrl = detail::WithClientId(track.streamUrl, CachedClientId());
    return !track.streamUrl.empty();
}

Kind Classify(std::string_view url) { return ParseLink(url).kind; }

bool ParseTrack(std::string_view json, WebTrack& out) {
    web::JsonValue track;
    if (!web::ParseJson(json, track)) return false;
    return FillTrack(track, out);
}

size_t ParseTrackList(std::string_view json, std::vector<WebTrack>& out) {
    out.clear();
    web::JsonValue root;
    if (!web::ParseJson(json, root)) return 0;
    if (root.kind == web::JsonValue::Kind::Array) {
        for (const web::JsonValue& entry : root.items) AddListEntry(entry, out, nullptr);
        return out.size();
    }
    if (root.kind != web::JsonValue::Kind::Object) return 0;
    // A set answers with "tracks"; a likes page and an artist's tracks page with "collection".
    const web::JsonValue& tracks = root["tracks"];
    if (tracks.kind == web::JsonValue::Kind::Array) {
        for (const web::JsonValue& entry : tracks.items) AddListEntry(entry, out, nullptr);
        return out.size();
    }
    const web::JsonValue& collection = root["collection"];
    if (collection.kind == web::JsonValue::Kind::Array)
        for (const web::JsonValue& entry : collection.items) AddListEntry(entry, out, nullptr);
    return out.size();
}

bool ParseSetTitle(std::string_view json, std::string& title) {
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Object) return false;
    std::string found = Trim(Str(root, "title"));
    if (found.empty()) return false;
    title = std::move(found);
    return true;
}

bool Resolve(const std::string& url, const std::atomic<bool>& cancel, std::vector<WebTrack>& tracks,
             std::string& listTitle, std::string& error, const std::function<void(const std::string&)>& notice) {
    tracks.clear();
    listTitle.clear();
    error.clear();
    if (cancel.load()) {
        error = kCancelled;
        return false;
    }
    std::string target = Trim(url);
    if (target.find("://") == std::string::npos) target = "https://" + target;
    Link link = ParseLink(target);
    if (link.shortLink) {
        if (!FollowShortLink(cancel, target, notice, error)) return false;
        link = ParseLink(target);
    }
    if (link.kind == Kind::None) {
        error = link.profile ? kArtistPage : kNotALink;
        return false;
    }
    switch (link.kind) {
        case Kind::Track: return ResolveTrack(link, cancel, tracks, error, notice);
        case Kind::Playlist: return ResolveSet(link, cancel, tracks, listTitle, error, notice);
        case Kind::Likes: return ResolveCollection(link, true, cancel, tracks, listTitle, error, notice);
        case Kind::ArtistTracks: return ResolveCollection(link, false, cancel, tracks, listTitle, error, notice);
        default: break;
    }
    error = kNotALink;
    return false;
}

}  // namespace soundcloud
