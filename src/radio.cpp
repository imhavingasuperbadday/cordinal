// Radio Browser's public directory and the portable, atomically written station favorites.
#include "radio.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

#include "backup.h"
#include "web_internal.h"
#include "web_remote.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <random>

namespace radio {
namespace {
bool Url(std::string_view value) {
    if (value.size() > 8192 || (!value.starts_with("https://") && !value.starts_with("http://"))) return false;
    size_t host = value.starts_with("https://") ? 8 : 7;
    if (value.size() <= host || value[host] == '/' || value[host] == '?' || value[host] == '#') return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; });
}
bool Text(const std::string& s, size_t max) { return s.size() <= max && s.find('\0') == s.npos; }

bool Parse(std::string_view json, std::vector<Station>& stations, bool strict) {
    web::JsonValue root;
    if (json.size() > (8u << 20) || !web::ParseJson(json, root) || root.kind != web::JsonValue::Kind::Array ||
        root.items.size() > 20000) return false;
    std::vector<Station> result;
    for (const auto& item : root.items) {
        if (item.kind != web::JsonValue::Kind::Object) { if (strict) return false; else continue; }
        Station s;
        s.id = item.String("stationuuid"); s.name = item.String("name");
        s.url = item.String("url_resolved"); if (s.url.empty()) s.url = item.String("url");
        s.icon = item.String("favicon"); s.country = item.String("countrycode");
        s.state = item.String("state");
        s.tags = item.String("tags"); s.codec = item.String("codec");
        const std::string& bitrate = item.String("bitrate");
        if (!bitrate.empty()) {
            auto parsed = std::from_chars(bitrate.data(), bitrate.data() + bitrate.size(), s.bitrate);
            if (parsed.ec != std::errc() || parsed.ptr != bitrate.data() + bitrate.size() || s.bitrate > 100000)
                s.bitrate = 0;
        }
        s.hls = item.String("hls") == "1" || item.String("hls") == "true";
        if (!s.icon.empty() && !Url(s.icon)) s.icon.clear();
        bool valid = !s.id.empty() && Text(s.id, 128) && !s.name.empty() && Text(s.name, 512) && Url(s.url) &&
                     Text(s.country, 128) && Text(s.state, 256) && Text(s.tags, 2048) && Text(s.codec, 128);
        if (!valid) { if (strict) return false; else continue; }
        result.push_back(std::move(s));
    }
    stations = std::move(result);
    return true;
}

std::string Encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::vector<std::string> Servers() {
    std::vector<std::string> result;
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return result;
#endif
    addrinfo hints{}, *list = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (!getaddrinfo("all.api.radio-browser.info", "443", &hints, &list)) {
        for (const auto* a = list; a; a = a->ai_next) {
            char host[1025]{};
            if (getnameinfo(a->ai_addr, (int)a->ai_addrlen, host, sizeof(host), nullptr, 0, NI_NAMEREQD)) continue;
            std::string name = host;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (name.ends_with(".api.radio-browser.info") && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") == name.npos &&
                std::find(result.begin(), result.end(), name) == result.end()) result.push_back(std::move(name));
        }
        freeaddrinfo(list);
    }
#ifdef _WIN32
    WSACleanup();
#endif
    std::mt19937 random((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    std::shuffle(result.begin(), result.end(), random);
    return result;
}

std::vector<std::string> DirectoryServers(std::atomic<bool>& cancel) {
    auto servers = Servers();
    // The documented fallback supplies server names when DNS/reverse DNS isn't available.
    if (servers.empty() && !cancel) {
#ifdef _WIN32
        void* session = nullptr;
#else
        void* session = &cancel;
#endif
        auto reply = web::HttpFetch(session, "https://all.api.radio-browser.info/json/servers", "GET", {}, {}, 6000);
        web::JsonValue root;
        if (reply.status == 200 && web::ParseJson(reply.body, root) && root.kind == web::JsonValue::Kind::Array)
            for (const auto& item : root.items) {
                std::string name = item.String("name");
                if (name.ends_with(".api.radio-browser.info") && name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") == name.npos &&
                    std::find(servers.begin(), servers.end(), name) == servers.end()) servers.push_back(std::move(name));
            }
    }
    return servers;
}
}  // namespace

bool ParseStations(std::string_view json, std::vector<Station>& stations) { return Parse(json, stations, false); }

static std::string Fold(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    while (!value.empty() && (unsigned char)value.front() <= ' ') value.erase(0, 1);
    while (!value.empty() && (unsigned char)value.back() <= ' ') value.pop_back();
    return value;
}
static std::string Query(std::string text) {
    text = Fold(std::move(text));
    for (const char* suffix : {" mhz", " khz", " fm", " am", "-fm", "-am"})
        if (text.ends_with(suffix)) { text.resize(text.size() - strlen(suffix)); break; }
    return text;
}
std::string RadioCountry(std::string value) {
    std::string folded = Fold(value);
    if (folded == "us" || folded == "usa" || folded == "u.s." || folded == "u.s.a." ||
        folded == "united states" || folded == "united states of america") return "US";
    if (folded == "all" || folded == "worldwide") return {};
    if (folded.size() == 2) {
        value = folded;
        for (char& c : value) c = (char)std::toupper((unsigned char)c);
    }
    return value;
}
static std::string State(std::string value) {
    std::string code = Fold(value);
    if (code.size() != 2) return value;
    static constexpr std::string_view states =
        "al:Alabama;ak:Alaska;az:Arizona;ar:Arkansas;ca:California;co:Colorado;ct:Connecticut;de:Delaware;dc:District of Columbia;"
        "fl:Florida;ga:Georgia;hi:Hawaii;id:Idaho;il:Illinois;in:Indiana;ia:Iowa;ks:Kansas;ky:Kentucky;la:Louisiana;me:Maine;"
        "md:Maryland;ma:Massachusetts;mi:Michigan;mn:Minnesota;ms:Mississippi;mo:Missouri;mt:Montana;ne:Nebraska;nv:Nevada;"
        "nh:New Hampshire;nj:New Jersey;nm:New Mexico;ny:New York;nc:North Carolina;nd:North Dakota;oh:Ohio;ok:Oklahoma;"
        "or:Oregon;pa:Pennsylvania;ri:Rhode Island;sc:South Carolina;sd:South Dakota;tn:Tennessee;tx:Texas;ut:Utah;"
        "vt:Vermont;va:Virginia;wa:Washington;wv:West Virginia;wi:Wisconsin;wy:Wyoming;";
    size_t at = states.find(code + ':');
    return at == states.npos ? value : std::string(states.substr(at + 3, states.find(';', at) - at - 3));
}
bool StationMatches(const Station& station, const Search& search) {
    auto contains = [](const std::string& field, const std::string& value) {
        auto fold = [](const std::string& text) {
            std::wstring wide = FromUtf8(text);
            std::transform(wide.begin(), wide.end(), wide.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
            return wide;
        };
        return fold(field).find(fold(value)) != std::wstring::npos;
    };
    std::string country = RadioCountry(search.country);
    if (!country.empty() && !contains(station.country, country)) return false;
    if (!search.state.empty() && !contains(station.state, State(search.state))) return false;
    if (!search.genre.empty() && !contains(station.tags, search.genre)) return false;
    return search.name.empty() || contains(station.name + " " + station.state + " " + station.tags + " " + station.country, Query(search.name));
}
std::string SearchPath(const Search& s) {
    std::string path = "/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=100&offset=" +
                       std::to_string(s.offset);
    if (!s.name.empty()) path += "&name=" + Encode(Query(s.name));
    if (!s.genre.empty()) path += "&tag=" + Encode(s.genre);
    std::string country = RadioCountry(s.country);
    if (!country.empty()) path += (country.size() == 2 ? "&countrycode=" : "&country=") + Encode(country);
    if (!s.state.empty()) path += "&state=" + Encode(State(s.state));
    return path;
}

const std::vector<Station>& BuiltInStations() {
    // Broadcasters in the Kokomo and Frankfort area stream on their own sites but are absent from
    // Radio Browser. Sources: rock985.com, z925fm.com, wwki.com, wfrn.com, shine.fm and
    // clintoncountydailynews.com (October 30, 2026).
    static const std::vector<Station> stations = {
        {"local-wmyk", "Rock 98.5 WMYK - Kokomo", "https://streaming.live365.com/a70319", {}, "US", "rock", "MP3", 192, false, "Indiana"},
        {"local-wzwz", "Z 92.5 WZWZ - Kokomo", "https://streaming.live365.com/a66695", {}, "US", "pop,rock,hits", "MP3", 192, false, "Indiana"},
        {"local-wwki", "KI 100.5 WWKI - Kokomo", "https://playerservices.streamtheworld.com/api/livestream-redirect/WWKIFMAAC.aac",
         {}, "US", "country", "AAC", 48, false, "Indiana"},
        {"local-wfrn", "WFRN 93.7 - Kokomo", "https://streaming.live365.com/a00051", {}, "US", "christian,contemporary", "MP3", 128, false, "Indiana"},
        {"local-wshw", "Shine 99 WSHW - Frankfort", "https://mist.olivet.edu/shine128", {}, "US", "christian,adult contemporary", "MP3", 128, false, "Indiana"},
        {"local-shine-worship", "Shine Worship - Olivet", "https://mist.olivet.edu/shineworship128", {}, "US", "christian,worship", "MP3", 128, false, "Illinois"},
        {"local-wilo", "WILO 96.9 - Frankfort", "https://ice23.securenetsystems.net/WILO", {}, "US", "classic hits", "AAC", 32, false, "Indiana"}
    };
    return stations;
}

bool Discover(const Search& search, std::vector<Station>& stations, std::atomic<bool>& cancel) {
    std::vector<Station> extra;
    for (const auto& station : BuiltInStations()) if (StationMatches(station, search)) extra.push_back(station);
    auto servers = DirectoryServers(cancel);
    for (const auto& server : servers) {
        if (cancel) return false;
#ifdef _WIN32
        void* session = nullptr;
#else
        void* session = &cancel;
#endif
        auto reply = web::HttpFetch(session, "https://" + server + SearchPath(search), "GET", {}, {}, 6000);
        if (!cancel && reply.status == 200 && ParseStations(reply.body, stations)) {
            for (auto it = extra.rbegin(); it != extra.rend(); ++it)
                if (std::none_of(stations.begin(), stations.end(), [&](const Station& s) { return s.url == it->url; })) stations.insert(stations.begin(), *it);
            return true;
        }
    }
    if (!cancel && !extra.empty()) { stations = std::move(extra); return true; }
    return false;
}

bool CountPlay(const std::string& stationId, std::atomic<bool>& cancel) {
    if (stationId.size() != 36) return false;
    for (size_t i = 0; i < stationId.size(); ++i)
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (stationId[i] != '-') return false;
        } else if (!std::isxdigit((unsigned char)stationId[i])) return false;
    for (const auto& server : DirectoryServers(cancel)) {
        if (cancel) return false;
#ifdef _WIN32
        void* session = nullptr;
#else
        void* session = &cancel;
#endif
        auto reply = web::HttpFetch(session, "https://" + server + "/json/url/" + Encode(stationId), "GET", {}, {}, 6000);
        if (!cancel && reply.status == 200) return true;
    }
    return false;
}

bool LoadFavorites(const std::wstring& path, std::vector<Station>& stations) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) { stations.clear(); return true; }
    std::string text;
    return backup::ReadBytes(path, text, 8u << 20) && Parse(text, stations, true);
}

bool SaveFavorites(const std::wstring& path, const std::vector<Station>& stations) {
    std::string text = "[";
    for (const auto& s : stations) {
        if (text.size() > 1) text += ',';
        text += "{\"stationuuid\":" + web::Json(s.id) + ",\"name\":" + web::Json(s.name) +
                ",\"url_resolved\":" + web::Json(s.url) + ",\"favicon\":" + web::Json(s.icon) +
                ",\"countrycode\":" + web::Json(s.country) + ",\"state\":" + web::Json(s.state) + ",\"tags\":" + web::Json(s.tags) +
                ",\"codec\":" + web::Json(s.codec) + ",\"bitrate\":" + std::to_string(s.bitrate) +
                ",\"hls\":" + (s.hls ? "1" : "0") + '}';
    }
    text += "]\n";
    std::vector<Station> verified;
    if (!Parse(text, verified, true) || verified.size() != stations.size()) return false;
    static std::atomic<unsigned> serial{0};
    std::wstring temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++serial) + L".tmp";
    if (!backup::WriteBytes(temp, text)) return false;
    bool ok = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ?
        MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE :
        ReplaceFileW(path.c_str(), temp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}
}  // namespace radio
