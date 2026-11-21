// Spotify's public pages say what an item is - title, artists, cover, durations, disc and
// track numbers - and YouTube supplies the audio. Nothing here touches Spotify's stream.
#include "spotifylink.h"

#include "web_remote.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace spotify {
namespace {

using web::JsonValue;

// The embed pages are served to any client, but the browser page is the one that carries
// __NEXT_DATA__; a script's User-Agent keeps Spotify from answering with a bare app shell.
const char kRequestHeaders[] =
    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/127.0.0.0 Safari/537.36\r\n"
    "Accept: text/html,application/xhtml+xml\r\n"
    "Accept-Language: en-US,en;q=0.9\r\n";
// An album embed carries the whole track list, and a large playlist's page is not much
// smaller; 8 MiB leaves room for either without letting a wrong answer eat memory.
constexpr size_t kMaxEmbedBytes = 8u << 20;
constexpr unsigned kEmbedTimeoutMs = 15000;
constexpr unsigned kMaxTracks = 200;  // what Puto keeps, and plenty for one album or playlist
constexpr unsigned kMaxSeconds = 24 * 3600;

std::string LowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) out += (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return out;
}

bool IsAlnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

std::string_view TrimAscii(std::string_view text) {
    while (!text.empty() && (unsigned char)text.front() <= ' ') text.remove_prefix(1);
    while (!text.empty() && (unsigned char)text.back() <= ' ') text.remove_suffix(1);
    return text;
}

// A Spotify id as Puto accepts it: base-62 and no shorter than an old-style id.
bool ValidId(std::string_view id) {
    if (id.size() < 10 || id.size() > 40) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) { return IsAlnum(c); });
}

// "spotify:track:<id>" and ".../track/<id>" both name the same item.
std::string IdFromUri(std::string_view uri) {
    if (uri.empty()) return {};
    size_t at = uri.find_last_of(":/");
    std::string_view id = TrimAscii(at == std::string_view::npos ? uri : uri.substr(at + 1));
    return ValidId(id) ? std::string(id) : std::string();
}

// Site text as the library shows it: runs of whitespace become one space, and a cut never
// splits a UTF-8 sequence.
std::string Clip(std::string_view raw, size_t max) {
    std::string out;
    out.reserve(std::min(raw.size(), max + 1));
    bool space = false;
    for (unsigned char c : raw) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += (char)c;
    }
    if (out.size() > max) {
        size_t cut = max;
        while (cut && ((unsigned char)out[cut] & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    return out;
}

const JsonValue* Member(const JsonValue* value, std::string_view key) {
    if (!value || value->kind != JsonValue::Kind::Object) return nullptr;
    for (const auto& member : value->members)
        if (std::string_view(member.first) == key) return &member.second;
    return nullptr;
}

std::string Text(const JsonValue* value) {
    return value && value->kind == JsonValue::Kind::String ? value->text : std::string();
}

// A number, a numeric string, or an object that states it as totalMilliseconds (Spotify's
// durations are the last shape). The nested value is read by kind, not through Text(),
// which is for strings only.
double Num(const JsonValue* value) {
    if (!value) return 0;
    if (value->kind == JsonValue::Kind::Number || value->kind == JsonValue::Kind::String)
        return std::strtod(value->text.c_str(), nullptr);
    if (value->kind == JsonValue::Kind::Object) {
        const JsonValue* nested = Member(value, "totalMilliseconds");
        if (nested && (nested->kind == JsonValue::Kind::Number || nested->kind == JsonValue::Kind::String))
            return std::strtod(nested->text.c_str(), nullptr);
    }
    return 0;
}

unsigned Seconds(double milliseconds) {
    if (!(milliseconds > 0)) return 0;
    double seconds = milliseconds / 1000.0;
    if (seconds >= kMaxSeconds) return kMaxSeconds;
    return (unsigned)std::llround(seconds);
}

std::vector<std::string> ArtistNames(const JsonValue* node) {
    const JsonValue* list = node;
    if (node && node->kind == JsonValue::Kind::Object) list = Member(node, "items");
    std::vector<std::string> names;
    if (!list || list->kind != JsonValue::Kind::Array) return names;
    for (const JsonValue& entry : list->items) {
        std::string name;
        if (entry.kind == JsonValue::Kind::String) {
            name = Clip(entry.text, 200);
        } else if (entry.kind == JsonValue::Kind::Object) {
            name = Clip(Text(Member(Member(&entry, "profile"), "name")), 200);
            if (name.empty()) name = Clip(Text(Member(&entry, "name")), 200);
        }
        if (!name.empty()) names.push_back(std::move(name));
    }
    return names;
}

std::string JoinArtists(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

// Embed track entities list {name}, the album page's state lists {profile:{name}}, and a
// row may only carry a subtitle line; every shape ends up as one string here.
std::string RowArtist(const JsonValue* row, const std::string& fallback) {
    std::string subtitle = Clip(Text(Member(row, "subtitle")), 200);
    if (!subtitle.empty()) return subtitle;
    std::vector<std::string> names = ArtistNames(Member(row, "artists"));
    if (!names.empty()) return names.front();
    return fallback;
}

double ImageSize(const JsonValue& entry) {
    double size = std::max(Num(Member(&entry, "width")), Num(Member(&entry, "height")));
    if (size > 0) return size;
    // The now-playing DOM uses "sizes":"300x300" where the state uses numbers.
    std::string sizes = Text(Member(&entry, "sizes"));
    size_t x = sizes.find_first_of("xX");
    if (x == std::string::npos) return 0;
    return std::max(std::strtod(sizes.substr(0, x).c_str(), nullptr),
                    std::strtod(sizes.substr(x + 1).c_str(), nullptr));
}

// The largest https image, as Puto picks it; a tie goes to the later source, which is the
// larger DPI variant Spotify appends after the plain one.
std::string LargestImage(const JsonValue* node) {
    const JsonValue* list = node;
    if (list && list->kind == JsonValue::Kind::Object) {
        const JsonValue* sources = Member(list, "sources");
        if (sources && sources->kind == JsonValue::Kind::Array) list = sources;
        else list = Member(list, "image");
    }
    if (!list || list->kind != JsonValue::Kind::Array) return {};
    std::string best;
    double bestSize = -1;
    for (const JsonValue& entry : list->items) {
        std::string url = Text(Member(&entry, "url"));
        if (url.empty()) continue;
        double size = ImageSize(entry);
        if (size >= bestSize) {
            best = std::move(url);
            bestSize = size;
        }
    }
    if (!best.starts_with("https://")) return {};
    return Clip(best, 500);
}

std::string EntityArt(const JsonValue* entity) {
    std::string art = LargestImage(Member(entity, "coverArt"));
    if (art.empty()) art = LargestImage(Member(entity, "visualIdentity"));
    if (art.empty()) art = LargestImage(Member(Member(entity, "visualIdentity"), "image"));
    return art;
}

bool TagHasId(std::string_view tag, std::string_view id) {
    size_t at = 0;
    while (at < tag.size() && (unsigned char)tag[at] > ' ' && tag[at] != '>') ++at;  // "<script"
    while (at < tag.size()) {
        while (at < tag.size() && ((unsigned char)tag[at] <= ' ' || tag[at] == '/')) ++at;
        if (at >= tag.size() || tag[at] == '>') break;
        size_t nameAt = at;
        while (at < tag.size() && tag[at] != '=' && (unsigned char)tag[at] > ' ' && tag[at] != '>') ++at;
        std::string_view name = tag.substr(nameAt, at - nameAt);
        while (at < tag.size() && (unsigned char)tag[at] <= ' ') ++at;
        if (at >= tag.size() || tag[at] != '=') continue;
        ++at;
        while (at < tag.size() && (unsigned char)tag[at] <= ' ') ++at;
        size_t valueAt = at;
        if (at < tag.size() && (tag[at] == '"' || tag[at] == '\'')) {
            char quote = tag[at++];
            valueAt = at;
            while (at < tag.size() && tag[at] != quote) ++at;
            if (at < tag.size() && LowerAscii(name) == "id" &&
                tag.substr(valueAt, at - valueAt) == id) return true;
            if (at < tag.size()) ++at;
        } else {
            while (at < tag.size() && (unsigned char)tag[at] > ' ' && tag[at] != '>') ++at;
            if (LowerAscii(name) == "id" && tag.substr(valueAt, at - valueAt) == id) return true;
        }
    }
    return false;
}

// An inline script's body, found by its id attribute: Spotify uses __NEXT_DATA__ for the
// embed's JSON and initialState for the album page's base64 state.
bool ScriptById(std::string_view html, std::string_view id, std::string_view& body) {
    size_t at = 0;
    while ((at = html.find('<', at)) != std::string_view::npos) {
        size_t nameEnd = at + 1;
        while (nameEnd < html.size() && IsAlnum((unsigned char)html[nameEnd])) ++nameEnd;
        if (LowerAscii(html.substr(at + 1, nameEnd - at - 1)) != "script") {
            at = nameEnd;
            continue;
        }
        size_t close = html.find('>', nameEnd);
        if (close == std::string_view::npos) return false;
        if (TagHasId(html.substr(at, close - at), id)) {
            size_t end = html.find("</script", close + 1);
            if (end == std::string_view::npos) end = html.size();
            body = html.substr(close + 1, end - close - 1);
            return true;
        }
        at = close + 1;
    }
    return false;
}

bool NextData(std::string_view html, JsonValue& root) {
    std::string_view body;
    if (!ScriptById(html, "__NEXT_DATA__", body)) return false;
    return web::ParseJson(TrimAscii(body), root) && root.kind == JsonValue::Kind::Object;
}

// props.pageProps.state.data.entity, with the shallower shapes later pages use as fallbacks.
const JsonValue* EmbedEntity(const JsonValue& root) {
    const JsonValue* page = Member(&root, "props");
    page = Member(page, "pageProps");
    const JsonValue* state = Member(page, "state");
    const JsonValue* data = Member(state, "data");
    const JsonValue* entity = Member(data, "entity");
    if (!entity) entity = Member(page, "entity");
    if (!entity && (Member(data, "name") || Member(data, "title") || Member(data, "trackList"))) entity = data;
    if (!entity && (Member(state, "name") || Member(state, "title") || Member(state, "trackList"))) entity = state;
    return entity && entity->kind == JsonValue::Kind::Object ? entity : nullptr;
}

// The album page's initialState is plain base64 in a text/plain script; it is not the
// URL-safe spelling, but '+' and '/' are accepted alongside '-' and '_' as Puto's decoder
// effectively does, and line breaks and padding are skipped.
std::string DecodeBase64(std::string_view in) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A'
              : c >= 'a' && c <= 'z' ? c - 'a' + 26
              : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '+' || c == '-' ? 62
              : c == '/' || c == '_' ? 63
              : -1;
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)(acc >> bits & 0xFF);
        }
    }
    return out;
}

// Case, punctuation and whitespace, with anything outside ASCII dropped the way Puto's
// NFKD pass leaves it. Two spellings of the same accented title still meet each other:
// both sides go through the same sieve.
std::string NormText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (unsigned char c : text) {
        if (c >= 0x80 || !IsAlnum(c)) {
            if (!out.empty()) space = true;
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return out;
}

std::vector<std::string_view> Words(const std::string& normalized) {
    std::vector<std::string_view> words;
    size_t at = 0;
    while (at < normalized.size()) {
        size_t end = normalized.find(' ', at);
        if (end == std::string::npos) end = normalized.size();
        if (end > at) words.emplace_back(normalized.data() + at, end - at);
        at = end + 1;
    }
    return words;
}

bool HasWord(const std::string& normalized, std::string_view word) {
    for (std::string_view token : Words(normalized))
        if (token == word) return true;
    return false;
}

// What a bunch of brackets say about an upload rather than about the recording: a video
// announcement, a remaster date, a guest credit. Matching keeps the plain title; the
// ladder's second query uses this too.
bool Decoration(std::string_view inner) {
    static constexpr std::string_view kWords[] = {
        "audio", "video", "hd", "4k", "remaster", "remastered", "live", "edit",
        "mix", "version", "feat", "ft",
    };
    const std::string normalized = NormText(inner);
    for (std::string_view token : Words(normalized))
        for (std::string_view word : kWords)
            if (token == word) return true;
    return false;
}

std::string SimplifyTitle(std::string_view title) {
    std::string out;
    out.reserve(title.size());
    for (size_t at = 0; at < title.size();) {
        char open = title[at];
        char close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}' : '\0';
        if (close) {
            size_t end = title.find(close, at + 1);
            if (end != std::string_view::npos && Decoration(title.substr(at + 1, end - at - 1))) {
                out += ' ';
                at = end + 1;
                continue;
            }
        }
        out += open;
        ++at;
    }
    std::string text(TrimAscii(out));
    // Spotify's own titles end decoration with " - Remastered 2011" as often as brackets.
    for (;;) {
        size_t dash = text.rfind(" - ");
        if (dash == std::string::npos) break;
        if (!Decoration(std::string_view(text).substr(dash + 3))) break;
        text = Clip(std::string_view(text).substr(0, dash), 400);
    }
    return Clip(text, 400);
}

bool HasNoise(const std::string& normalized) {
    static constexpr std::string_view kWords[] = {
        "cover", "karaoke", "instrumental", "remix", "reaction", "tutorial", "nightcore",
        "slowed", "reverb", "mashup", "acapella", "shorts", "live", "8d", "hour", "loop",
        "looped",
    };
    for (std::string_view token : Words(normalized))
        for (std::string_view word : kWords)
            if (token == word) return true;
    return normalized.find("bass boost") != std::string::npos ||
           normalized.find("sped up") != std::string::npos ||
           normalized.find("speed up") != std::string::npos;
}

bool GoodUpload(const std::string& channelAndTitle) {
    return HasWord(channelAndTitle, "topic") || HasWord(channelAndTitle, "vevo") ||
           HasWord(channelAndTitle, "audio") || HasWord(channelAndTitle, "official");
}

// The artist's own channel carries the clean recording: the channel itself, its "- Topic"
// collection or its VEVO.
bool OfficialChannel(const yt::Candidate& candidate, const WebTrack& want) {
    const std::string channel = NormText(candidate.channel);
    const std::string artist = NormText(want.artist);
    if (channel.empty() || artist.empty()) return false;
    if (channel == artist || channel == artist + " topic" || channel == artist + " vevo") return true;
    return channel.starts_with(artist + " ") &&
           (channel.ends_with(" topic") || channel.ends_with(" vevo"));
}

double ScoreCandidate(const yt::Candidate& candidate, const WebTrack& want) {
    const std::string title = NormText(candidate.title);
    const std::string stripped = NormText(SimplifyTitle(candidate.title));
    const std::string wanted = NormText(want.title);
    const std::string artist = NormText(want.artist);
    const std::string channel = NormText(candidate.channel);
    double score = 0;

    // Title agreement after decoration is the strongest signal; the exact words outrank
    // an upload that merely contains them ("Artist - Song (Official Video)" still counts).
    if (!wanted.empty()) {
        if (stripped == wanted) {
            score += 5;
        } else if (stripped.find(wanted) != std::string::npos || title.find(wanted) != std::string::npos) {
            score += 4;
        } else {
            std::vector<std::string_view> words;
            for (std::string_view word : Words(wanted))
                if (word.size() > 2) words.push_back(word);
            size_t found = 0;
            for (std::string_view word : words)
                if (title.find(word) != std::string::npos) ++found;
            if (!words.empty()) score += 2.0 * (double)found / (double)words.size();
        }
    }
    if (!artist.empty() &&
        (title.find(artist) != std::string::npos || channel.find(artist) != std::string::npos))
        score += 3;
    if (candidate.topic || OfficialChannel(candidate, want)) score += 1.5;
    if (GoodUpload(channel + " " + title)) score += 1.5;
    // A live or remix upload only spoils a match when the wanted item is not that version.
    if (HasNoise(title) && !HasNoise(wanted)) score -= 4;
    if (want.durationSec && candidate.durationSec) {
        unsigned diff = candidate.durationSec > want.durationSec ? candidate.durationSec - want.durationSec
                                                                 : want.durationSec - candidate.durationSec;
        if (diff <= 1)  // a second either way is the same recording
            score += 3;
        else if (diff <= 4)
            score += 1.5;
        else
            score -= 1;
    }
    return score;
}

// Two percent for a recording: a radio edit, a video with its own outro or a live cut is a
// different thing to listen to. Episodes carry ad breaks that move the length around.
bool SameLength(const yt::Candidate& candidate, const WebTrack& want, bool episode) {
    if (!want.durationSec || !candidate.durationSec) return true;
    double tolerance = episode ? std::max(30.0, want.durationSec * 0.05)
                               : std::max(3.0, want.durationSec * 0.02);
    double diff = std::fabs((double)candidate.durationSec - (double)want.durationSec);
    return diff <= tolerance;
}

// The words that identify the item itself. An episode is often titled "Show: Episode", and
// the show is searched for separately, so a leading artist name is dropped here too.
std::string TitleCore(const WebTrack& want) {
    std::string core = NormText(SimplifyTitle(want.title));
    const std::string show = NormText(want.artist);
    if (!show.empty() && core.starts_with(show + " ")) core.erase(0, show.size() + 1);
    return core;
}

bool NamesCore(const std::string& title, const std::string& core) {
    if (core.empty()) return true;
    const std::string normalized = NormText(title);
    if (normalized.find(core) != std::string::npos) return true;
    // An upload may reorder or rejoin the episode's words; every significant one must
    // still appear, so a different programme with one overlapping word does not count.
    std::vector<std::string_view> have = Words(normalized);
    for (std::string_view word : Words(core))
        if (std::find(have.begin(), have.end(), word) == have.end()) return false;
    return true;
}

std::string ClockText(unsigned seconds) {
    unsigned hours = seconds / 3600, minutes = seconds % 3600 / 60, rest = seconds % 60;
    std::string out;
    if (hours) {
        out += std::to_string(hours) + ":";
        out += minutes < 10 ? "0" : "";
    }
    out += std::to_string(minutes) + ":";
    out += rest < 10 ? "0" : "";
    out += std::to_string(rest);
    return out;
}

bool SetTitleFrom(const JsonValue* entity, std::string& setTitle) {
    std::string name = Clip(Text(Member(entity, "name")), 200);
    if (name.empty()) name = Clip(Text(Member(entity, "title")), 200);
    if (name.empty()) return false;
    setTitle = std::move(name);
    return true;
}

}  // namespace

Kind Classify(std::string_view url) {
    url = TrimAscii(url);
    size_t scheme = url.find("://");
    if (scheme != std::string_view::npos) {
        std::string protocol = LowerAscii(url.substr(0, scheme));
        if (protocol != "http" && protocol != "https") return Kind::None;
        url.remove_prefix(scheme + 3);
    }
    size_t slash = url.find('/');
    if (slash == std::string_view::npos) return Kind::None;
    std::string host = LowerAscii(url.substr(0, slash));
    if (host != "open.spotify.com") return Kind::None;

    std::string_view path = url.substr(slash);
    size_t noise = path.find_first_of("?#");
    if (noise != std::string_view::npos) path = path.substr(0, noise);
    // The link may carry a locale segment: /intl-de/album/<id>.
    if (path.starts_with("/intl-")) {
        size_t next = path.find('/', 6);
        if (next == std::string_view::npos) return Kind::None;
        path.remove_prefix(next);
    }
    struct Route {
        std::string_view prefix;
        Kind kind;
    };
    static constexpr Route kRoutes[] = {
        {"/track/", Kind::Track},
        {"/album/", Kind::Album},
        {"/playlist/", Kind::Playlist},
    };
    for (const Route& route : kRoutes) {
        if (!path.starts_with(route.prefix)) continue;
        std::string_view id = path.substr(route.prefix.size());
        if (!id.empty() && id.back() == '/') id.remove_suffix(1);
        return ValidId(id) ? route.kind : Kind::None;
    }
    return Kind::None;
}

bool ParseTrackEmbed(std::string_view html, WebTrack& single) {
    JsonValue root;
    if (!NextData(html, root)) return false;
    const JsonValue* entity = EmbedEntity(root);
    if (!entity) return false;

    std::string id = Text(Member(entity, "id"));
    if (!ValidId(id)) id = IdFromUri(Text(Member(entity, "uri")));
    std::string title = Clip(Text(Member(entity, "name")), 200);
    if (title.empty()) title = Clip(Text(Member(entity, "title")), 200);
    if (id.empty() || title.empty()) return false;

    std::vector<std::string> artists = ArtistNames(Member(entity, "artists"));
    std::string artist;
    if (!artists.empty()) {
        artist = artists.front();  // the first credit is the primary one
    } else {
        artist = Clip(Text(Member(entity, "subtitle")), 200);  // the embed's joined credit line
    }
    std::string album;
    if (const JsonValue* value = Member(entity, "album")) {
        album = value->kind == JsonValue::Kind::String ? Clip(value->text, 200)
                                                       : Clip(Text(Member(value, "name")), 200);
    }
    double duration = Num(Member(entity, "duration"));
    if (!(duration > 0)) duration = Num(Member(entity, "durationMs"));

    single = WebTrack{};
    single.site = "spotify";
    single.id = std::move(id);
    single.title = std::move(title);
    single.artist = std::move(artist);
    single.album = std::move(album);
    single.artworkUrl = EntityArt(entity);
    single.durationSec = Seconds(duration);
    return true;
}

bool ParseListEmbed(std::string_view html, std::vector<WebTrack>& list, std::string& setTitle) {
    JsonValue root;
    if (!NextData(html, root)) return false;
    const JsonValue* entity = EmbedEntity(root);
    if (!entity) return false;
    std::string name;
    if (!SetTitleFrom(entity, name)) return false;
    const JsonValue* rows = Member(entity, "trackList");
    if (!rows || rows->kind != JsonValue::Kind::Array) rows = Member(entity, "tracklist");
    if (!rows || rows->kind != JsonValue::Kind::Array || rows->items.empty()) return false;

    const bool playlist = LowerAscii(Text(Member(entity, "type"))) == "playlist";
    const std::string setArt = EntityArt(entity);
    const std::string setArtist = Clip(Text(Member(entity, "subtitle")), 200);
    unsigned statedTotal = (unsigned)Num(Member(entity, "trackCount"));
    if (!statedTotal) statedTotal = (unsigned)Num(Member(entity, "trackTotal"));
    if (!statedTotal) statedTotal = (unsigned)rows->items.size();
    statedTotal = std::min(statedTotal, kMaxTracks);

    std::vector<WebTrack> out;
    std::vector<std::string> seen;
    unsigned discWas = 1;
    for (const JsonValue& row : rows->items) {
        if (row.kind != JsonValue::Kind::Object || out.size() >= kMaxTracks) continue;
        std::string id = IdFromUri(Text(Member(&row, "uri")));
        if (id.empty()) id = Text(Member(&row, "id"));
        std::string title = Clip(Text(Member(&row, "title")), 200);
        if (title.empty()) title = Clip(Text(Member(&row, "name")), 200);
        if (!ValidId(id) || title.empty()) continue;
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);

        WebTrack track;
        track.site = "spotify";
        track.id = std::move(id);
        track.title = std::move(title);
        // The album's own artist files every track under one folder: a row's credit line
        // names the guests, and the title already carries them. A playlist has no one
        // artist, so its rows keep their own.
        track.artist = playlist ? RowArtist(&row, std::string()) : setArtist;
        track.album = name;
        track.artworkUrl = EntityArt(&row);
        if (track.artworkUrl.empty()) track.artworkUrl = setArt;
        track.durationSec = Seconds(Num(Member(&row, "duration")));
        if (!playlist) {
            unsigned disc = std::max(1u, (unsigned)Num(Member(&row, "discNumber")));
            discWas = std::max(discWas, disc);
            track.trackNo = (unsigned)Num(Member(&row, "trackNumber"));
            track.trackTotal = statedTotal;
        }
        // Playlist rows keep both numbers at 0: the queue numbers the playlist itself.
        out.push_back(std::move(track));
    }
    if (out.empty()) return false;

    // WebTrack has no disc field, so a multi-disc set is numbered across the whole album
    // in playing order; a single-disc set keeps the numbers the page states. A playlist
    // is left at zero for the queue to number.
    if (!playlist) {
        const bool multiDisc = discWas > 1;
        for (size_t index = 0; index < out.size(); ++index) {
            out[index].trackTotal = std::max(out[index].trackTotal, (unsigned)out.size());
            if (multiDisc || out[index].trackNo == 0) out[index].trackNo = (unsigned)index + 1;
        }
    }
    list = std::move(out);
    setTitle = std::move(name);
    return true;
}

bool ParseAlbumState(std::string_view html, std::vector<WebTrack>& list, std::string& setTitle) {
    std::string_view body;
    if (!ScriptById(html, "initialState", body)) return false;
    std::string decoded = DecodeBase64(body);
    if (decoded.empty()) return false;
    JsonValue root;
    if (!web::ParseJson(decoded, root) || root.kind != JsonValue::Kind::Object) return false;

    // Puto looks the album up by the id in the address bar. The parser is handed only the
    // page, so it takes the first entity that is an album kind and actually has tracks.
    const JsonValue* entity = nullptr;
    const JsonValue* fallback = nullptr;
    const JsonValue* items = Member(Member(&root, "entities"), "items");
    if (items && items->kind == JsonValue::Kind::Object) {
        for (const auto& member : items->members) {
            const JsonValue* value = &member.second;
            if (value->kind != JsonValue::Kind::Object) continue;
            const JsonValue* tracksNode = Member(value, "tracks");
            bool hasTracks = Member(Member(value, "tracksV2"), "items") != nullptr ||
                             Member(tracksNode, "items") != nullptr ||
                             (tracksNode && tracksNode->kind == JsonValue::Kind::Array &&
                              !tracksNode->items.empty());
            std::string type = LowerAscii(Text(Member(value, "type")));
            if (hasTracks && (type == "album" || type == "single" || type == "ep" || type == "compilation")) {
                entity = value;
                break;
            }
            if (!fallback && hasTracks) fallback = value;
        }
    }
    if (!entity) entity = fallback;
    if (!entity) entity = Member(&root, "entity");
    if (!entity || entity->kind != JsonValue::Kind::Object) return false;

    std::string name;
    if (!SetTitleFrom(entity, name)) return false;
    std::vector<std::string> albumArtists = ArtistNames(Member(entity, "artists"));
    const std::string albumArtist = JoinArtists(albumArtists);
    const std::string albumArt = EntityArt(entity);

    const JsonValue* tracksV2 = Member(entity, "tracksV2");
    const JsonValue* tracksNode = Member(entity, "tracks");
    const JsonValue* wrappers = Member(tracksV2, "items");
    if (!wrappers || wrappers->kind != JsonValue::Kind::Array || wrappers->items.empty())
        wrappers = Member(tracksNode, "items");
    // A few page versions list the tracks directly under "tracks" instead of wrapping
    // them in an items array.
    if ((!wrappers || wrappers->kind != JsonValue::Kind::Array || wrappers->items.empty()) &&
        tracksNode && tracksNode->kind == JsonValue::Kind::Array)
        wrappers = tracksNode;
    if (!wrappers || wrappers->kind != JsonValue::Kind::Array || wrappers->items.empty()) return false;
    unsigned statedTotal = (unsigned)Num(Member(tracksV2, "totalCount"));
    if (!statedTotal) statedTotal = (unsigned)Num(Member(tracksNode, "totalCount"));

    struct Entry {
        WebTrack track;
        unsigned disc = 1;
    };
    std::vector<Entry> entries;
    std::vector<std::string> seen;
    for (const JsonValue& wrapper : wrappers->items) {
        if (entries.size() >= kMaxTracks) break;
        const JsonValue* raw = Member(&wrapper, "track");
        if (!raw || raw->kind != JsonValue::Kind::Object) raw = &wrapper;
        std::string id = Text(Member(raw, "id"));
        if (!ValidId(id)) id = IdFromUri(Text(Member(raw, "uri")));
        std::string title = Clip(Text(Member(raw, "name")), 200);
        if (title.empty()) title = Clip(Text(Member(raw, "title")), 200);
        if (!ValidId(id) || title.empty()) continue;
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) continue;
        seen.push_back(id);

        WebTrack track;
        track.site = "spotify";
        track.id = std::move(id);
        track.title = std::move(title);
        std::vector<std::string> artists = ArtistNames(Member(raw, "artists"));
        track.artist = artists.empty() ? albumArtist : artists.front();
        track.album = name;
        track.artworkUrl = albumArt;
        track.durationSec = Seconds(Num(Member(raw, "duration")));
        track.trackNo = (unsigned)Num(Member(raw, "trackNumber"));
        entries.push_back({std::move(track), std::max(1u, (unsigned)Num(Member(raw, "discNumber")))});
    }
    if (entries.empty()) return false;
    const unsigned total = std::min(kMaxTracks, std::max(statedTotal, (unsigned)entries.size()));

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.disc != b.disc) return a.disc < b.disc;
        if (a.track.trackNo != b.track.trackNo) return a.track.trackNo < b.track.trackNo;
        return NormText(a.track.title) < NormText(b.track.title);
    });
    const bool multiDisc =
        std::any_of(entries.begin(), entries.end(), [](const Entry& e) { return e.disc > 1; });
    std::vector<WebTrack> out;
    out.reserve(entries.size());
    for (size_t index = 0; index < entries.size(); ++index) {
        WebTrack track = std::move(entries[index].track);
        if (multiDisc || track.trackNo == 0) track.trackNo = (unsigned)index + 1;  // WebTrack has no disc
        track.trackTotal = total;
        out.push_back(std::move(track));
    }
    list = std::move(out);
    setTitle = std::move(name);
    return true;
}

std::vector<std::string> BuildQueries(const WebTrack& track) {
    const std::string title = Clip(track.title, 300);
    const std::string artist = Clip(track.artist, 300);
    const std::string simplified = SimplifyTitle(track.title);
    std::vector<std::string> queries;
    auto add = [&queries](std::string_view value) {
        std::string query = Clip(value, 400);
        if (query.empty()) return;
        const std::string key = NormText(query);
        for (const std::string& existing : queries)
            if (NormText(existing) == key) return;
        queries.push_back(std::move(query));
    };
    // Step one needs the artist. Without one the ladder starts at the plain title, which
    // is the same recording's words with decoration removed. A song's last try is its
    // plain words - the decoration only hides them - while an episode's last try is its
    // exact wording, because only an upload that names it counts.
    if (!artist.empty()) {
        add(artist + " - " + title);
        if (NormText(simplified) != NormText(title)) add(artist + " - " + simplified);
        add(simplified);
    } else {
        add(simplified);
        add(title);
    }
    return queries;
}

std::vector<int> RankMatches(const WebTrack& want, const std::vector<yt::Candidate>& have,
                             std::string& why) {
    why.clear();
    if (have.empty()) {
        why = "YouTube returned nothing to compare with that recording.";
        return {};
    }
    // A want-track without an artist is a podcast episode: its title is the only thing that
    // can identify it, so only an upload that names the episode may rank at all.
    const bool episode = want.artist.empty();
    const std::string core = TitleCore(want);
    std::vector<int> named;
    for (size_t index = 0; index < have.size(); ++index)
        if (NamesCore(have[index].title, core)) named.push_back((int)index);
    std::vector<int> pool = named;
    if (pool.empty()) {
        if (episode) {
            why = "No YouTube upload names that episode.";
            return {};
        }
        pool.resize(have.size());
        for (size_t index = 0; index < have.size(); ++index) pool[index] = (int)index;
    }

    struct Scored {
        int index = 0;
        double score = 0;
        bool clock = false;
    };
    std::vector<Scored> scored;
    unsigned closest = 0, closestDiff = ~0u;
    for (int index : pool) {
        const yt::Candidate& candidate = have[(size_t)index];
        if (want.durationSec && candidate.durationSec) {
            unsigned diff = candidate.durationSec > want.durationSec
                                ? candidate.durationSec - want.durationSec
                                : want.durationSec - candidate.durationSec;
            if (diff < closestDiff) {
                closestDiff = diff;
                closest = candidate.durationSec;
            }
            if (!SameLength(candidate, want, episode)) continue;
        }
        scored.push_back({index, ScoreCandidate(candidate, want), candidate.durationSec > 0});
    }
    if (scored.empty()) {
        why = closestDiff != ~0u
                  ? "The closest YouTube upload is " + ClockText(closest) +
                        ", which is not the same recording."
                  : "No YouTube upload of that recording could be used.";
        return {};
    }
    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        // A row without a duration is kept, but the queue has to check it against the
        // player's own clock, so it waits behind every row that could be checked now.
        if (a.clock != b.clock) return a.clock;
        if (a.score != b.score) return a.score > b.score;
        return a.index < b.index;
    });
    std::vector<int> order;
    order.reserve(scored.size());
    for (const Scored& match : scored) order.push_back(match.index);
    return order;
}

bool ReadEmbed(Kind kind, const std::string& id, const std::atomic<bool>& cancel,
               std::string& html, std::string& error) {
    html.clear();
    error.clear();
    const char* route = kind == Kind::Track   ? "track"
                        : kind == Kind::Album ? "album"
                        : kind == Kind::Playlist ? "playlist"
                                                 : nullptr;
    if (!route) {
        error = "That is not a Spotify link.";
        return false;
    }
    if (!ValidId(id)) {
        error = "That Spotify link is missing its item id.";
        return false;
    }
    if (cancel.load()) {
        error = "Reading the Spotify page was cancelled.";
        return false;
    }
    const std::string url = std::string("https://open.spotify.com/embed/") + route + "/" + id;
    web::HttpReply reply = web::HttpFetch(nullptr, url, "GET", {}, kRequestHeaders, kEmbedTimeoutMs,
                                         kMaxEmbedBytes);
    if (cancel.load()) {
        error = "Reading the Spotify page was cancelled.";
        return false;
    }
    if (reply.status == 404) {
        error = "That page does not exist on Spotify.";
        return false;
    }
    if (reply.status != 200) {
        error = reply.status
                    ? "Spotify could not be reached (HTTP " + std::to_string(reply.status) +
                          "). Try again in a moment."
                    : "Spotify could not be reached. Check your internet connection and try again.";
        return false;
    }
    if (reply.body.empty()) {
        error = "Spotify sent an empty page. Try again in a moment.";
        return false;
    }
    html = std::move(reply.body);
    return true;
}

bool ReadAlbumPage(const std::string& id, const std::atomic<bool>& cancel,
                   std::string& html, std::string& error) {
    html.clear();
    error.clear();
    if (!ValidId(id)) {
        error = "That Spotify link is missing its album id.";
        return false;
    }
    if (cancel.load()) {
        error = "Reading the Spotify page was cancelled.";
        return false;
    }
    // The real album page server-renders the initialState blob with the complete,
    // precisely numbered track list; the embed page is the fallback when it cannot be
    // read (Puto reads them in this order too).
    const std::string url = "https://open.spotify.com/album/" + id;
    web::HttpReply reply =
        web::HttpFetch(nullptr, url, "GET", {}, kRequestHeaders, kEmbedTimeoutMs, kMaxEmbedBytes);
    if (cancel.load()) {
        error = "Reading the Spotify page was cancelled.";
        return false;
    }
    if (reply.status != 200 || reply.body.empty()) {
        error = reply.status ? "Spotify's album page could not be read (HTTP " + std::to_string(reply.status) +
                                   ")."
                             : "Spotify's album page could not be read.";
        return false;
    }
    html = std::move(reply.body);
    return true;
}

}  // namespace spotify
