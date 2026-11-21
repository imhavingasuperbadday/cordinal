// YouTube's own audio-only streams, resolved through InnerTube as the visionOS app (ytaudio.h).
// The client table below is the one place to update when YouTube stops answering that client.
#include "ytaudio.h"

#include "web_remote.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace yt {
namespace {

// The InnerTube client cordinal asks as. These values are load-bearing: YouTube decides from
// them which API and which answer it gives, and a wrong or stale version alone is enough to get
// its "not a bot" refusal back for every request.
struct InnerTubeClient {
    const char* name;
    const char* version;
    const char* deviceMake;
    const char* deviceModel;
    const char* osName;
    const char* osVersion;
    const char* language;
    int headerName;  // the X-YouTube-Client-Name header
};

// YouTube's visionOS app, yt-dlp's default since 2026.08 and the only client that still answers
// with direct googlevideo URLs: no signature cipher, no n-parameter, no PO token. Copied value
// for value from Puto's youtube.js CLIENT constant.
//
// Puto's maintenance rule, kept here: when downloads start failing with the bot refusal or HTTP
// 403s, check which client yt-dlp currently defaults to and update this table (name, version,
// device, OS and the client-name header together, and the user agent below if it names a browser
// version that YouTube has retired).
constexpr InnerTubeClient kClient{"VISIONOS", "1.02", "Apple", "RealityDevice17,1", "visionOS",
                                  "26.5.23O471", "en", 101};

const char kBaseUrl[] = "https://www.youtube.com";
// The shape metadata_fetch.cpp uses: YouTube's endpoints dislike an empty agent, and cordinal
// identifies itself rather than pretending to be a browser it is not.
const char kUserAgent[] = "cordinal/0.1.15 (https://github.com/imhavingasuperbadday/cordinal)";
constexpr unsigned kApiTimeoutMs = 15000, kVisitorTimeoutMs = 8000;
// Player replies carry every adaptive format and pass 1 MB on a long video.
constexpr size_t kMaxReply = 8u << 20;
// Between playlist entries: Puto's ALBUM_PAUSE_MS. A burst of player requests is what earns a
// network YouTube's bot check, so the list is read at the pace of an album.
constexpr unsigned kTrackPauseMs = 600;

// Puto's BOT_MESSAGE, which its users have seen clear on its own within hours. Cordinal asks
// anonymously, so this is about the network (a shared address YouTube has flagged), not about
// the user's own YouTube account.
const char kBotMessage[] =
    "YouTube is temporarily refusing downloads from your network (its \"not a bot\" check). It "
    "usually clears up within a few hours; signing in to YouTube won't help.";
const char kAccountMessage[] = "This video needs a YouTube account, so it cannot be saved.";
const char kNoAudioMessage[] = "This video has no audio stream.";

// --- text helpers ---------------------------------------------------------------------------

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return out;
}

std::string Trim(std::string_view text) {
    size_t begin = 0, end = text.size();
    while (begin < end && (unsigned char)text[begin] <= ' ') ++begin;
    while (end > begin && (unsigned char)text[end - 1] <= ' ') --end;
    return std::string(text.substr(begin, end - begin));
}

bool Contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// A whole word only: a reply mentioning "robotics" must not read as YouTube's bot check.
bool HasWord(std::string_view text, std::string_view word) {
    for (size_t at = text.find(word); at != std::string_view::npos; at = text.find(word, at + 1)) {
        const bool before = at == 0 || !std::isalnum((unsigned char)text[at - 1]);
        const size_t end = at + word.size();
        const bool after = end == text.size() || !std::isalnum((unsigned char)text[end]);
        if (before && after) return true;
    }
    return false;
}

// A leading run of digits. Every InnerTube number worth keeping (itags, seconds, bitrates) is a
// plain count, so anything else in the field is ignored rather than guessed at.
unsigned Number(std::string_view text) {
    unsigned value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') break;
        if (value > 100000000u) return 0;
        value = value * 10 + (unsigned)(c - '0');
    }
    return value;
}

// "3:42", "1:02:03" or a bare count of seconds; anything else is 0.
unsigned ClockSeconds(std::string_view text) {
    unsigned parts[3] = {0, 0, 0};
    int count = 0, digits = 0;
    if (text.size() > 12) return 0;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            if (count >= 3 || digits >= 3) return 0;
            parts[count] = parts[count] * 10 + (unsigned)(c - '0');
            ++digits;
        } else if (c == ':' && digits && count < 2) {
            ++count;
            digits = 0;
        } else {
            return 0;
        }
    }
    if (!digits || (count >= 1 && parts[1] > 59) || (count == 2 && parts[2] > 59)) return 0;
    if (count == 2) return parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (count == 1) return parts[0] * 60 + parts[1];
    return parts[0];
}

std::string Quote(std::string_view value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += (char)c;
        } else if (c < 0x20) {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)c);
            out += escaped;
        } else {
            out += (char)c;
        }
    }
    return out + '"';
}

bool IdChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// Every YouTube video id is 11 characters of base64url.
bool VideoId(std::string_view id) {
    return id.size() == 11 && std::all_of(id.begin(), id.end(), IdChar);
}

// Playlist ids ("PL...", "RD...", "OLAK5uy_...") have no fixed length, only a safe alphabet.
bool ListId(std::string_view id) {
    return id.size() >= 2 && id.size() <= 64 && std::all_of(id.begin(), id.end(), IdChar);
}

std::string Thumbnail(std::string_view videoId) {
    if (!VideoId(videoId)) return {};
    return "https://i.ytimg.com/vi/" + std::string(videoId) + "/hqdefault.jpg";
}

// --- InnerTube JSON -------------------------------------------------------------------------

using JsonKind = web::JsonValue::Kind;

// One node of InnerTube's loosely typed text: a plain string, {"simpleText":...}, or
// {"runs":[{"text":...}]}.
std::string TextOf(const web::JsonValue& node) {
    if (node.kind == JsonKind::String) return node.text;
    if (node.kind == JsonKind::Object) {
        const std::string simple = node.String("simpleText");
        if (!simple.empty()) return simple;
        std::string runs;
        for (const web::JsonValue& run : node["runs"].items) runs += run.String("text");
        if (!runs.empty()) return runs;
        return node.String("text");
    }
    std::string out;
    for (const web::JsonValue& item : node.items) out += TextOf(item);
    return out;
}

// Everything a playabilityStatus says about why it is not "OK": its reason, its messages (in the
// visionOS and the web shapes) and its error screen.
std::string PlayabilityText(const web::JsonValue& status) {
    std::string text = TextOf(status["reason"]);
    auto append = [&text](const std::string& part) {
        if (part.empty()) return;
        if (!text.empty()) text += ' ';
        text += part;
    };
    for (const web::JsonValue& message : status["messages"].items) append(TextOf(message));
    const web::JsonValue& screen = status["errorScreen"]["playerErrorMessageRenderer"];
    append(TextOf(screen["reason"]));
    append(TextOf(screen["subreason"]));
    return text;
}

bool Flag(const web::JsonValue& object, const char* key) {
    const web::JsonValue& value = object[key];
    return value.kind == JsonKind::Bool && value.text == "true";
}

// Visits every named member of every object in the reply, in document order. Playlist and search
// replies wrap their rows differently between clients (and between a browse page and a watch
// page's playlist panel), so the rows are found by renderer name rather than by a fixed path.
template <typename F>
void WalkMembers(const web::JsonValue& node, F&& visit) {
    if (node.kind == JsonKind::Object) {
        for (const web::JsonMember& member : node.members) visit(member.first, member.second);
        for (const web::JsonMember& member : node.members) WalkMembers(member.second, visit);
    } else if (node.kind == JsonKind::Array) {
        for (const web::JsonValue& item : node.items) WalkMembers(item, visit);
    }
}

// A badge's exact renderer name differs between clients; anything carrying a musicTopicBadge
// member marks the channel as one of YouTube's auto-generated "- Topic" artist channels.
bool HasTopicBadge(const web::JsonValue& node, int depth = 0) {
    if (depth > 4) return false;
    if (node.kind == JsonKind::Object) {
        for (const web::JsonMember& member : node.members) {
            if (Contains(member.first, "musicTopicBadge")) return true;
            if (HasTopicBadge(member.second, depth + 1)) return true;
        }
    } else if (node.kind == JsonKind::Array) {
        for (const web::JsonValue& item : node.items)
            if (HasTopicBadge(item, depth + 1)) return true;
    }
    return false;
}

// --- client context and request bodies ------------------------------------------------------

std::string ClientContext(const std::string& visitor) {
    std::string client = std::string("\"clientName\":") + Quote(kClient.name) +
        ",\"clientVersion\":" + Quote(kClient.version) + ",\"deviceMake\":" + Quote(kClient.deviceMake) +
        ",\"deviceModel\":" + Quote(kClient.deviceModel) + ",\"osName\":" + Quote(kClient.osName) +
        ",\"osVersion\":" + Quote(kClient.osVersion) + ",\"hl\":" + Quote(kClient.language);
    if (!visitor.empty()) client += ",\"visitorData\":" + Quote(visitor);
    return "\"context\":{\"client\":{" + client + "}}";
}

// The visitor-id request carries only what Puto sends: the name, the version and the language.
std::string VisitorContext() {
    return std::string("{\"context\":{\"client\":{\"clientName\":") + Quote(kClient.name) +
        ",\"clientVersion\":" + Quote(kClient.version) + ",\"hl\":" + Quote(kClient.language) + "}}}";
}

std::string PlayerBody(const std::string& videoId, const std::string& visitor) {
    return "{" + ClientContext(visitor) + ",\"videoId\":" + Quote(videoId) +
        ",\"playbackContext\":{\"contentPlaybackContext\":{\"html5Preference\":\"HTML5_PREF_WANTS\"}},"
        "\"contentCheckOk\":true,\"racyCheckOk\":true}";
}

std::string BrowseBody(const std::string& playlistId, const std::string& visitor) {
    return "{" + ClientContext(visitor) + ",\"browseId\":" + Quote("VL" + playlistId) + "}";
}

std::string SearchBody(const std::string& query, const std::string& visitor) {
    return "{" + ClientContext(visitor) + ",\"query\":" + Quote(query) + "}";
}

// --- network --------------------------------------------------------------------------------

bool Cancelled(const std::atomic<bool>* cancel) { return cancel && *cancel; }

// One POST to InnerTube. Every request made here runs on the queue's own worker thread, one at a
// time, so there is no shared connection to keep and no lock around any of it.
bool Post(const char* endpoint, const std::string& body, const std::string& visitor,
          const std::atomic<bool>* cancel, std::string& json, std::string& error) {
    if (Cancelled(cancel)) { error = "Stopped."; return false; }
    std::string headers = std::string("User-Agent: ") + kUserAgent +
        "\r\nContent-Type: application/json\r\nX-YouTube-Client-Name: " +
        std::to_string(kClient.headerName) + "\r\nX-YouTube-Client-Version: " + kClient.version + "\r\n";
    if (!visitor.empty()) headers += "X-Goog-Visitor-Id: " + visitor + "\r\n";
    web::HttpReply reply = web::HttpFetch(nullptr, std::string(kBaseUrl) + "/youtubei/v1/" + endpoint,
                                          "POST", body, headers, kApiTimeoutMs, kMaxReply);
    if (Cancelled(cancel)) { error = "Stopped."; return false; }
    if (!reply.status) {
        error = "Couldn't get an answer from YouTube. Check the connection and try again.";
        return false;
    }
    if (reply.status != 200) {
        error = "YouTube answered HTTP " + std::to_string(reply.status) + ". Try again in a moment.";
        return false;
    }
    json = std::move(reply.body);
    return true;
}

// A visitor id ends up in a request header, so only base64url characters and percent escapes are
// ever sent, whatever the reply says.
bool UsableVisitor(const std::string& value) {
    if (value.size() < 8 || value.size() > 512) return false;
    return std::all_of(value.begin(), value.end(), [](char c) {
        return IdChar(c) || c == '%' || c == '=' || c == '.';
    });
}

// The value after `key` in a reply, up to its closing quote.
std::string QuotedValue(std::string_view text, std::string_view key) {
    const size_t at = text.find(key);
    if (at == std::string_view::npos) return {};
    size_t end = at + key.size();
    const size_t begin = end;
    while (end < text.size() && text[end] != '"' && text[end] != '\\') ++end;
    return std::string(text.substr(begin, end - begin));
}

// Without a visitor id the player API answers every request with "Sign in to confirm you're not a
// bot". It is fetched once per process and kept: every network function here runs on the queue's
// single worker thread, so the cache needs no lock. A failed handshake leaves it empty and the
// resolution goes ahead anyway, because YouTube sometimes answers without one; if the bot wall
// then appears, the missing id is the likely cause.
std::string VisitorId(const std::atomic<bool>* cancel, bool renew) {
    static std::string cached;
    static bool asked = false;
    if (asked && !renew) return cached;
    asked = true;
    std::string value;
    std::string json, error;
    if (Post("visitor_id", VisitorContext(), {}, cancel, json, error)) {
        web::JsonValue root;
        if (web::ParseJson(json, root)) {
            // Puto reads responseContext.visitorData; the reply has also carried a plain
            // visitorId, so either name is accepted rather than losing the id over a rename.
            value = root["responseContext"].String("visitorData");
            if (value.empty()) value = root.String("visitorData");
            if (value.empty()) value = root.String("visitorId");
            if (value.empty()) value = root["responseContext"].String("visitorId");
        }
    }
    if (value.empty() && !Cancelled(cancel)) {
        // The service worker's bootstrap data carries a fresh id as a last resort.
        const std::string headers = std::string("User-Agent: ") + kUserAgent + "\r\n";
        const web::HttpReply reply =
            web::HttpFetch(nullptr, std::string(kBaseUrl) + "/sw.js_data", "GET", {}, headers,
                           kVisitorTimeoutMs, 1u << 18);
        if (reply.status == 200) {
            value = QuotedValue(reply.body, "\"visitorId\":\"");
            if (value.empty()) value = QuotedValue(reply.body, "\"VISITOR_DATA\":\"");
        }
    }
    if (UsableVisitor(value)) cached = value;
    return cached;
}

// --- audio streams --------------------------------------------------------------------------

unsigned Itag(const web::JsonValue& format) { return Number(format.String("itag")); }

int Base64Digit(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// xtags is a base64url protobuf, and "drc" shows up in it verbatim once decoded (Puto's rule).
// Only a short value is decoded, so a hostile reply cannot make this expensive.
std::string DecodedXtags(std::string_view value) {
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    if (value.size() > 4096) value = value.substr(0, 4096);
    for (char c : value) {
        const int digit = Base64Digit(c);
        if (digit < 0) break;
        buffer = buffer << 6 | (unsigned)digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buffer >> bits) & 0xff);
        }
    }
    return out;
}

std::string_view XtagsParam(std::string_view url) {
    const size_t at = url.find("xtags=");
    if (at == std::string_view::npos) return {};
    std::string_view value = url.substr(at + 6);
    const size_t end = value.find('&');
    return end == std::string_view::npos ? value : value.substr(0, end);
}

bool XtagsDrc(std::string_view value) {
    if (value.empty()) return false;
    if (Contains(Lower(value), "drc")) return true;
    return Contains(Lower(DecodedXtags(value)), "drc");
}

bool AudioOnly(const web::JsonValue& format) {
    const std::string mime = Lower(format.String("mimeType"));
    return mime.starts_with("audio/") && format.String("url").starts_with("https://");
}

// What the stream is, for the queue's file name. AAC arrives in an MP4 container, Opus in WebM;
// anything else is left unknown rather than guessed at.
std::string ExtensionOf(const web::JsonValue& format) {
    const std::string mime = Lower(format.String("mimeType"));
    if (mime.starts_with("audio/mp4")) return "m4a";
    if (mime.starts_with("audio/webm")) return "webm";
    return {};
}

// A dubbed language track: not the video's own audio. It is only used when nothing else exists.
bool SecondaryTrack(const web::JsonValue& format) {
    const web::JsonValue& track = format["audioTrack"];
    if (track.kind != JsonKind::Object || track.String("id").empty()) return false;
    return !Flag(track, "audioIsDefault");
}

// DRC tracks are loudness-squashed copies of the regular audio: they sound different, so they are
// skipped whenever a plain copy exists. Puto skips them by flag, by xtags and by a non-default
// audio track; the same three signs are checked here.
bool DrcCopy(const web::JsonValue& format) {
    if (Flag(format, "isDrc")) return true;
    if (XtagsDrc(format.String("xtags")) || XtagsDrc(XtagsParam(format.String("url")))) return true;
    if (Contains(Lower(format["audioTrack"].String("displayName")), "drc")) return true;
    return SecondaryTrack(format);
}

// AAC first (140, then 139, an .m4a that plays and tags everywhere), then Opus (251, 250, 249).
const web::JsonValue* PickAudio(const web::JsonValue& root) {
    const web::JsonValue& formats = root["streamingData"]["adaptiveFormats"];
    std::vector<const web::JsonValue*> plain, any;
    for (const web::JsonValue& format : formats.items) {
        if (format.kind != JsonKind::Object || !AudioOnly(format)) continue;
        any.push_back(&format);
        if (!DrcCopy(format)) plain.push_back(&format);
    }
    const std::vector<const web::JsonValue*>& pool = plain.empty() ? any : plain;
    if (pool.empty()) return nullptr;
    for (unsigned wanted : {140u, 139u, 251u, 250u, 249u}) {
        const web::JsonValue* fallback = nullptr;
        for (const web::JsonValue* format : pool) {
            if (Itag(*format) != wanted) continue;
            if (!SecondaryTrack(*format)) return format;
            if (!fallback) fallback = format;
        }
        if (fallback) return fallback;
    }
    // An itag the table does not know yet: take the best AAC, then the best Opus, so YouTube
    // adding one costs a better choice rather than a failed download.
    for (bool aac : {true, false}) {
        const web::JsonValue* best = nullptr;
        for (const web::JsonValue* format : pool) {
            const bool isAac = Lower(format->String("mimeType")).find("mp4") != std::string::npos;
            if (isAac != aac || SecondaryTrack(*format)) continue;
            if (!best || Number(format->String("bitrate")) > Number(best->String("bitrate"))) best = format;
        }
        if (best) return best;
    }
    return pool.front();
}

// --- titles and the channel as artist -------------------------------------------------------

// "Rick Astley - Topic" is YouTube's auto-generated artist channel, not part of the artist's name.
std::string TopicArtist(std::string author) {
    const std::string lower = Lower(author);
    if (lower.size() > 8 && lower.ends_with(" - topic")) author.erase(author.size() - 8);
    return Trim(author);
}

std::string Normalized(std::string_view text) {
    std::string out;
    bool space = false;
    for (unsigned char c : text) {
        if (!std::isalnum(c)) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return out;
}

// Whether a title prefix names the channel that uploaded the video: the artist itself, its
// "- Topic" channel or its VEVO channel, which is as far as Puto's matching goes.
bool SameArtist(const std::string& prefix, const std::string& artist) {
    const std::string wanted = Normalized(artist), named = Normalized(prefix);
    if (wanted.empty() || named.empty()) return false;
    if (named == wanted) return true;
    for (const char* suffix : {" topic", " vevo", "topic", "vevo"})
        if (named == wanted + suffix) return true;
    return false;
}

// "Artist - Title" loses its redundant prefix when the channel is that artist's; anything less
// certain keeps the title whole, because mangling a title is worse than repeating a name.
std::string SplitArtistPrefix(std::string_view title, const std::string& artist) {
    const size_t dash = title.find(" - ");
    if (dash == std::string_view::npos || dash == 0) return std::string(title);
    const std::string prefix = Trim(title.substr(0, dash));
    const std::string rest = Trim(title.substr(dash + 3));
    if (rest.empty() || !SameArtist(prefix, artist)) return std::string(title);
    return rest;
}

std::string PlayabilityReason(const std::string& text, const std::string& state) {
    if (!text.empty()) return text;
    if (state.empty()) return "YouTube didn't offer this video, so it cannot be saved.";
    return "YouTube won't play this video (" + state + ").";
}

bool MentionsBot(const std::string& text) {
    const std::string lower = Lower(text);
    return Contains(lower, "not a bot") || HasWord(lower, "bot") || HasWord(lower, "bots");
}

// Age checks and channel memberships both need the user's own YouTube account, which cordinal
// deliberately never uses: those videos are remembered as unavailable rather than retried.
bool NeedsAccount(const std::string& text) {
    const std::string lower = Lower(text);
    for (const char* phrase : {"confirm your age", "age-restricted", "age restricted",
                               "inappropriate for some users", "members-only", "members only",
                               "join this channel", "channel members", "membership",
                               "members of this channel", "this channel's members"})
        if (Contains(lower, phrase)) return true;
    return false;
}

// The playlist's own name, from whichever header renderer this client used.
std::string PlaylistTitle(std::string_view json) {
    web::JsonValue root;
    if (!web::ParseJson(json, root)) return {};
    std::string title;
    WalkMembers(root, [&title](const std::string& name, const web::JsonValue& value) {
        if (!title.empty()) return;
        if (name != "playlistHeaderRenderer" && name != "playlistSidebarPrimaryInfoRenderer") return;
        title = Trim(TextOf(value["title"]));
    });
    return title;
}

// --- links ----------------------------------------------------------------------------------

std::string_view Param(std::string_view query, std::string_view name) {
    for (size_t at = 0; at < query.size();) {
        size_t next = query.find('&', at);
        if (next == std::string_view::npos) next = query.size();
        const std::string_view part = query.substr(at, next - at);
        const size_t equals = part.find('=');
        if (part.substr(0, equals) == name)
            return equals == std::string_view::npos ? std::string_view{} : part.substr(equals + 1);
        at = next + 1;
    }
    return {};
}

bool YouTubeHost(const std::string& host) {
    if (host == "youtube.com" || host == "youtube-nocookie.com") return true;
    return host.ends_with(".youtube.com") || host.ends_with(".youtube-nocookie.com");
}

// The three parts of a pasted link that matter, read once so Classify, VideoIdOf and PlaylistIdOf
// cannot disagree about what a link is.
struct Link {
    std::string host;         // lowercase, without a port
    std::string_view target;  // the path, starting with "/"
    std::string_view query;   // after "?", without the fragment
    bool shortLink = false;   // youtu.be
    bool youtube = false;     // any *.youtube.com or youtube-nocookie.com
};

Link ParseLink(std::string_view url) {
    Link link;
    std::string_view rest = url;
    if (rest.size() >= 8 && Lower(rest.substr(0, 8)) == "https://")
        rest.remove_prefix(8);
    else if (rest.size() >= 7 && Lower(rest.substr(0, 7)) == "http://")
        rest.remove_prefix(7);
    if (const size_t hash = rest.find('#'); hash != std::string_view::npos) rest = rest.substr(0, hash);
    const size_t slash = rest.find('/');
    link.host = Lower(rest.substr(0, slash));
    if (const size_t port = link.host.find(':'); port != std::string::npos) link.host.erase(port);
    link.target = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
    if (const size_t mark = link.target.find('?'); mark != std::string_view::npos) {
        link.query = link.target.substr(mark + 1);
        link.target = link.target.substr(0, mark);
    }
    link.shortLink = link.host == "youtu.be" || link.host == "www.youtu.be";
    link.youtube = YouTubeHost(link.host);
    return link;
}

// The id after `prefix` in a path, allowing one trailing slash. Anything longer than an id fails
// VideoId's length check, so "/shorts/<id>/extra" reads as no id at all.
std::string_view PathId(std::string_view path, std::string_view prefix) {
    if (!path.starts_with(prefix)) return {};
    std::string_view id = path.substr(prefix.size());
    if (id.ends_with("/")) id.remove_suffix(1);
    return id;
}

// The video id a link names, across the shapes YouTube hands out: watch pages (www or music),
// youtu.be short links and /shorts.
std::string_view VideoFrom(const Link& link) {
    if (link.shortLink) return PathId(link.target, "/");
    if (!link.youtube) return {};
    if (link.target == "/watch" || link.target == "/watch/") return Param(link.query, "v");
    if (link.target.starts_with("/shorts/")) return PathId(link.target, "/shorts/");
    return {};
}

// Pacing between playlist entries, the one place here that waits: Puto pauses its album for
// 600 ms between tracks, and a cancelled run leaves the wait instead of starting one more
// request.
bool Pause(const std::atomic<bool>& cancel) {
    for (unsigned waited = 0; waited < kTrackPauseMs; waited += 50) {
        if (cancel) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !cancel;
}

void FinishTrack(WebTrack& track, const std::string& videoId) {
    if (track.id.empty()) track.id = videoId;
    if (track.artworkUrl.empty()) track.artworkUrl = Thumbnail(track.id);
    track.site = "youtube";
}

}  // namespace

// --- public API -----------------------------------------------------------------------------

Kind Classify(std::string_view url) {
    const Link link = ParseLink(url);
    if (!link.shortLink && !link.youtube) return Kind::None;
    // A list link is a playlist even when it also names a video: with only Video and Playlist to
    // choose from, the set the user probably meant is the one that holds everything.
    if (ListId(Param(link.query, "list"))) return Kind::Playlist;
    return VideoId(VideoFrom(link)) ? Kind::Video : Kind::None;
}

std::string VideoIdOf(std::string_view url) {
    const std::string_view id = VideoFrom(ParseLink(url));
    return VideoId(id) ? std::string(id) : std::string{};
}

std::string PlaylistIdOf(std::string_view url) {
    const Link link = ParseLink(url);
    if (!link.shortLink && !link.youtube) return {};
    const std::string_view id = Param(link.query, "list");
    return ListId(id) ? std::string(id) : std::string{};
}

bool ParsePlayer(std::string_view json, WebTrack& out, std::string& error) {
    out = WebTrack{};
    out.site = "youtube";
    error.clear();
    web::JsonValue root;
    if (!web::ParseJson(json, root) || root.kind != JsonKind::Object) {
        error = "YouTube sent back something unexpected. Try again in a moment.";
        return false;
    }
    const web::JsonValue& details = root["videoDetails"];
    const std::string videoId = details.String("videoId");
    out.id = videoId;
    out.artist = TopicArtist(Trim(details.String("author")));
    out.title = SplitArtistPrefix(Trim(details.String("title")), out.artist);
    out.durationSec = Number(details.String("lengthSeconds"));
    out.artworkUrl = Thumbnail(videoId);

    const web::JsonValue& status = root["playabilityStatus"];
    const std::string state = status.String("status");
    const bool hasStreams = root["streamingData"].kind == JsonKind::Object;
    if (state != "OK") {
        const std::string text = PlayabilityText(status);
        // The bot check is temporary network state, not a verdict on the video: it is reported as
        // a failed resolution so the queue can offer the run again later, never remembered.
        if ((state == "LOGIN_REQUIRED" || state == "ERROR") && MentionsBot(text)) {
            out = WebTrack{};
            error = kBotMessage;
            return false;
        }
        if (state == "LOGIN_REQUIRED" && NeedsAccount(text)) {
            out.unavailable = true;
            out.why = kAccountMessage;
            return true;
        }
        if (!state.empty() || !hasStreams) {
            out.unavailable = true;
            out.why = PlayabilityReason(text, state);
            return true;
        }
    }
    if (Flag(details, "isLive") || Flag(details, "isLiveNow") || Flag(details, "isUpcoming")) {
        out.unavailable = true;
        out.why = "This is a live stream, so it cannot be saved. Try again once the broadcast has ended.";
        return true;
    }
    if (!hasStreams) {
        out.unavailable = true;
        out.why = kNoAudioMessage;
        return true;
    }
    const web::JsonValue* audio = PickAudio(root);
    if (!audio) {
        out.unavailable = true;
        out.why = kNoAudioMessage;
        return true;
    }
    // The visionOS client's URL is the stream itself: kept untouched, since googlevideo checks
    // every part of its signature. `ext` names what landed in it so the queue does not have to
    // read the mime= parameter out of a signed URL.
    out.streamUrl = audio->String("url");
    out.ext = ExtensionOf(*audio);
    return true;
}

size_t ParsePlaylist(std::string_view json, std::vector<PlaylistEntry>& out) {
    out.clear();
    web::JsonValue root;
    if (!web::ParseJson(json, root)) return 0;
    WalkMembers(root, [&out](const std::string& name, const web::JsonValue& value) {
        if (name != "playlistVideoRenderer" && name != "playlistPanelVideoRenderer") return;
        if (value.kind != JsonKind::Object) return;
        const std::string id = value.String("videoId");
        if (!VideoId(id)) return;  // a deleted or private entry has a title and no id
        for (const PlaylistEntry& have : out)
            if (have.id == id) return;
        PlaylistEntry entry;
        entry.id = id;
        entry.title = Trim(TextOf(value["title"]));
        entry.durationSec = Number(value.String("lengthSeconds"));
        if (!entry.durationSec) entry.durationSec = ClockSeconds(TextOf(value["lengthText"]));
        out.push_back(std::move(entry));
    });
    return out.size();
}

size_t ParseSearch(std::string_view json, std::vector<Candidate>& out) {
    out.clear();
    web::JsonValue root;
    if (!web::ParseJson(json, root)) return 0;
    WalkMembers(root, [&out](const std::string& name, const web::JsonValue& value) {
        if (name != "videoRenderer" && name != "compactVideoRenderer") return;
        if (value.kind != JsonKind::Object) return;
        const std::string id = value.String("videoId");
        if (!VideoId(id)) return;
        for (const Candidate& have : out)
            if (have.id == id) return;
        Candidate candidate;
        candidate.id = id;
        candidate.title = Trim(TextOf(value["title"]));
        candidate.channel = Trim(TextOf(value["ownerText"]));
        if (candidate.channel.empty()) candidate.channel = Trim(TextOf(value["longBylineText"]));
        if (candidate.channel.empty()) candidate.channel = Trim(TextOf(value["shortBylineText"]));
        candidate.durationSec = ClockSeconds(TextOf(value["lengthText"]));
        candidate.topic = Lower(candidate.channel).ends_with(" - topic") ||
            HasTopicBadge(value["ownerBadges"]) || HasTopicBadge(value["badges"]);
        out.push_back(std::move(candidate));
    });
    return out.size();
}

bool ResolveVideo(const std::string& videoId, const std::atomic<bool>& cancel, WebTrack& out,
                  std::string& error) {
    out = WebTrack{};
    error.clear();
    if (!VideoId(videoId)) {
        error = "That link has no YouTube video id, so there is nothing to save.";
        return false;
    }
    std::string visitor = VisitorId(&cancel, false);
    std::string json;
    if (!Post("player", PlayerBody(videoId, visitor), visitor, &cancel, json, error)) return false;
    if (ParsePlayer(json, out, error)) {
        FinishTrack(out, videoId);
        return true;
    }
    if (error != kBotMessage) return false;
    // A visitor id YouTube has gone off is worth one fresh try, as Puto does. A refusal that
    // survives it is the network's rate limit, which no new id repairs.
    visitor = VisitorId(&cancel, true);
    if (!Post("player", PlayerBody(videoId, visitor), visitor, &cancel, json, error)) return false;
    if (ParsePlayer(json, out, error)) {
        FinishTrack(out, videoId);
        return true;
    }
    out = WebTrack{};
    return false;
}

bool Search(const std::string& query, const std::atomic<bool>& cancel, std::vector<Candidate>& out,
            std::string& error) {
    out.clear();
    error.clear();
    if (Trim(query).empty()) {
        error = "There is nothing to search for on YouTube.";
        return false;
    }
    const std::string visitor = VisitorId(&cancel, false);
    std::string json;
    if (!Post("search", SearchBody(query, visitor), visitor, &cancel, json, error)) return false;
    ParseSearch(json, out);
    return true;
}

bool ResolvePlaylist(const std::string& playlistId, const std::atomic<bool>& cancel,
                     std::vector<WebTrack>& tracks, std::string& listTitle, std::string& error,
                     const std::function<void(const std::string&)>& notice) {
    tracks.clear();
    listTitle.clear();
    error.clear();
    if (!ListId(playlistId)) {
        error = "That link has no YouTube playlist id, so there is nothing to save.";
        return false;
    }
    if (notice) notice("Reading playlist...");
    const std::string visitor = VisitorId(&cancel, false);
    std::string json;
    if (!Post("browse", BrowseBody(playlistId, visitor), visitor, &cancel, json, error)) return false;
    std::vector<PlaylistEntry> entries;
    ParsePlaylist(json, entries);
    listTitle = PlaylistTitle(json);
    if (entries.empty()) {
        error = "This playlist has no videos that can be saved.";
        return false;
    }

    const unsigned total = (unsigned)entries.size();
    for (unsigned index = 0; index < total; ++index) {
        if (Cancelled(&cancel)) {
            tracks.clear();
            listTitle.clear();
            error = "Stopped.";
            return false;
        }
        const PlaylistEntry& entry = entries[index];
        WebTrack track;
        std::string itemError;
        if (!ResolveVideo(entry.id, cancel, track, itemError)) {
            // A bot check or a dead network refuses every following track too, so the list stops
            // here rather than collecting one identical failure per row (Puto stops its album).
            tracks.clear();
            listTitle.clear();
            error = itemError;
            return false;
        }
        // The player's own metadata wins; the listing's title and length are the fallbacks for a
        // reply that somehow carries no videoDetails.
        if (track.title.empty()) track.title = entry.title;
        if (!track.durationSec) track.durationSec = entry.durationSec;
        track.album = listTitle;
        track.trackNo = index + 1;
        track.trackTotal = total;
        FinishTrack(track, entry.id);
        tracks.push_back(std::move(track));
        if (index + 1 < total && !Pause(cancel)) {
            tracks.clear();
            listTitle.clear();
            error = "Stopped.";
            return false;
        }
    }
    return true;
}

}  // namespace yt
