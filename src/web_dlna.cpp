// Sharing the library with network players (web_server.h, Share): a UPnP/DLNA MediaServer on the web
// server's own port, so foobar2000, VLC, Kodi, BubbleUPnP or a TV on the home network can browse
// albums, artists, playlists and every song, and play each as its original file (or as WAV, for a
// format the player doesn't know), and an SSDP announcer so they find it. Small XML helpers shared
// with the network speakers (speakers.cpp) live here too.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "web_internal.h"
#include "cue.h"
#include "library.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <thread>

namespace web {

std::string XmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}
std::string XmlUnescape(std::string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        size_t end = text[i] == '&' ? text.find(';', i) : text.npos;
        if (end == text.npos || end - i > 10) { out += text[i]; continue; }
        std::string_view name = text.substr(i + 1, end - i - 1);
        unsigned long code = 0;
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name.size() > 1 && name[0] == '#' &&
                 (code = strtoul(std::string(name.substr(name[1] == 'x' ? 2 : 1)).c_str(), nullptr, name[1] == 'x' ? 16 : 10)) &&
                 code < 0x110000) {
            if (code < 0x80) out += (char)code;
            else if (code < 0x800) { out += (char)(0xC0 | (code >> 6)); out += (char)(0x80 | (code & 0x3F)); }
            else if (code < 0x10000) {
                out += (char)(0xE0 | (code >> 12)); out += (char)(0x80 | ((code >> 6) & 0x3F)); out += (char)(0x80 | (code & 0x3F));
            } else {
                out += (char)(0xF0 | (code >> 18)); out += (char)(0x80 | ((code >> 12) & 0x3F));
                out += (char)(0x80 | ((code >> 6) & 0x3F)); out += (char)(0x80 | (code & 0x3F));
            }
        } else { out += text[i]; continue; }
        i = end;
    }
    return out;
}
std::string XmlElement(std::string_view xml, std::string_view name) {
    auto trim = [](std::string_view t) {
        while (!t.empty() && (unsigned char)t.front() <= ' ') t.remove_prefix(1);
        while (!t.empty() && (unsigned char)t.back() <= ' ') t.remove_suffix(1);
        return std::string(t);
    };
    for (size_t at = 0; (at = xml.find(name, at)) != xml.npos; at += name.size()) {
        if (!at || (xml[at - 1] != '<' && xml[at - 1] != ':')) continue;
        if (xml[at - 1] == ':') {
            size_t open = xml.rfind('<', at);
            if (open == xml.npos || xml.substr(open, at - open).find_first_of(" >/") != xml.npos) continue;
        }
        size_t after = at + name.size();
        if (after >= xml.size() || (xml[after] != '>' && xml[after] != ' ')) continue;
        size_t start = xml.find('>', after);
        if (start == xml.npos || xml[start - 1] == '/') return {};
        size_t end = xml.find("</", ++start);
        return end == xml.npos ? std::string{} : trim(xml.substr(start, end - start));
    }
    return {};
}

std::vector<std::string> LocalIPv4() {
    std::vector<std::string> out;
    auto add = [&](const in_addr& a) {
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &a, text, sizeof(text)) && std::find(out.begin(), out.end(), text) == out.end()) out.push_back(text);
    };
#ifdef _WIN32
    ULONG size = 16384;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; ++tries) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, (IP_ADAPTER_ADDRESSES*)buffer.data(), &size);
    }
    if (result == NO_ERROR)
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buffer.data(); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
                if (u->Address.lpSockaddr->sa_family == AF_INET) add(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr);
        }
#else
    ifaddrs* list = nullptr;
    if (!getifaddrs(&list)) {
        for (auto* a = list; a; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && (a->ifa_flags & IFF_UP) && !(a->ifa_flags & IFF_LOOPBACK))
                add(((sockaddr_in*)a->ifa_addr)->sin_addr);
        freeifaddrs(list);
    }
#endif
    return out;
}

namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kBad = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
constexpr Socket kBad = -1;
void CloseSocket(Socket s) { close(s); }
#endif
constexpr const char* kMediaServer = "urn:schemas-upnp-org:device:MediaServer:1";
constexpr const char* kContentDirectory = "urn:schemas-upnp-org:service:ContentDirectory:1";
constexpr const char* kConnectionManager = "urn:schemas-upnp-org:service:ConnectionManager:1";
constexpr const char* kFlags = "DLNA.ORG_FLAGS=01700000000000000000000000000000";
constexpr const char* kServer = "cordinal/1.0 UPnP/1.0 DLNADOC/1.50";

// The MIME type a player is told for a file: the browser's without its codecs parameter, else by extension.
std::string PlayerMime(const Track& t) {
    std::string mime = t.mime.substr(0, t.mime.find(';'));
    if (!mime.empty()) return mime;
    std::wstring file = AudioFileOf(t.path);
    size_t dot = file.find_last_of(L'.');
    std::string ext = dot == file.npos ? std::string() : Lower(ToUtf8(file.substr(dot + 1)));
    static const std::pair<const char*, const char*> kTypes[] = {
        {"aif", "audio/aiff"}, {"aiff", "audio/aiff"}, {"aifc", "audio/aiff"}, {"ape", "audio/x-ape"},
        {"wv", "audio/x-wavpack"}, {"wma", "audio/x-ms-wma"}, {"mka", "audio/x-matroska"}, {"dsf", "audio/x-dsf"},
        {"dff", "audio/x-dff"}, {"mpc", "audio/x-musepack"}, {"tta", "audio/x-tta"}, {"ac3", "audio/ac3"},
        {"mp2", "audio/mpeg"}, {"wav", "audio/wav"}, {"flac", "audio/flac"}};
    for (const auto& [e, m] : kTypes) if (ext == e) return m;
    return "application/octet-stream";
}
std::string Extension(const Track& t) {
    std::wstring file = AudioFileOf(t.path);
    size_t dot = file.find_last_of(L'.'), slash = file.find_last_of(L"\\/");
    if (dot == file.npos || (slash != file.npos && dot < slash) || file.size() - dot > 6) return {};
    return Lower(ToUtf8(file.substr(dot)));
}
std::string Duration(double seconds) {
    if (!(seconds > 0)) return {};
    uint64_t ms = (uint64_t)(seconds * 1000 + 0.5);
    char text[32];
    snprintf(text, sizeof(text), "%llu:%02llu:%02llu.%03llu", (unsigned long long)(ms / 3600000), (unsigned long long)(ms / 60000 % 60),
             (unsigned long long)(ms / 1000 % 60), (unsigned long long)(ms % 1000));
    return text;
}
// dc:date wants a whole date: a year alone becomes its first day.
std::string IsoDate(const std::string& date) {
    if (date.size() >= 10 && date[4] == '-' && date[7] == '-') return date.substr(0, 10);
    if (date.size() >= 4 && std::all_of(date.begin(), date.begin() + 4, [](char c) { return c >= '0' && c <= '9'; }))
        return date.substr(0, 4) + "-01-01";
    return {};
}

// One entry of a container's listing: a container (`track` null) or a song.
struct Entry {
    std::string id, title, cls, artist, date, art;  // art: a song key whose cover stands for it
    const Track* track = nullptr;
    size_t children = 0;
};
struct Group {
    std::string title, artist, date;
    std::vector<const Track*> tracks;
};
std::vector<Group> Albums(const Snapshot& s) {
    std::vector<Group> out;
    for (const auto& t : s.tracks) {
        if (!t.library) continue;
        if (out.empty() || out.back().artist != t.albumArtist || out.back().title != t.album || out.back().date != t.date)
            out.push_back({t.album, t.albumArtist, t.date, {}});
        out.back().tracks.push_back(&t);
    }
    return out;
}
std::vector<Group> Artists(const Snapshot& s) {
    std::map<std::string, std::vector<const Track*>> byName;
    for (const auto& t : s.tracks)
        if (t.library) byName[t.albumArtist.empty() ? t.artist : t.albumArtist].push_back(&t);
    std::vector<Group> out;
    for (auto& [name, tracks] : byName) out.push_back({name, name, {}, std::move(tracks)});
    return out;
}
std::vector<const Track*> PlaylistTracks(const Snapshot& s, const Playlist& p) {
    std::vector<const Track*> out;
    for (const auto& key : p.tracks)
        if (auto it = s.byKey.find(key); it != s.byKey.end()) out.push_back(&s.tracks[it->second]);
    return out;
}
std::vector<Entry> Songs(const std::string& parent, const std::vector<const Track*>& tracks) {
    std::vector<Entry> out;
    out.reserve(tracks.size());
    for (size_t i = 0; i < tracks.size(); ++i) {
        Entry e;
        e.id = parent + '/' + std::to_string(i);  // the position: a playlist may hold a song twice
        e.track = tracks[i];
        out.push_back(std::move(e));
    }
    return out;
}
// The children of container `id`; false when there is no such container.
bool Children(const Snapshot& s, const std::string& id, const std::string& name, std::vector<Entry>& out) {
    out.clear();
    auto folder = [](std::string id, std::string title, size_t children) {
        Entry e; e.id = std::move(id); e.title = std::move(title); e.cls = "object.container.storageFolder"; e.children = children;
        return e;
    };
    auto index = [&](const char* prefix, size_t limit, size_t& n) {
        size_t length = strlen(prefix);
        if (id.compare(0, length, prefix) || id.size() == length) return false;
        char* end = nullptr;
        n = strtoul(id.c_str() + length, &end, 10);
        return *end == 0 && n < limit;
    };
    if (id == "0") {
        size_t songs = (size_t)std::count_if(s.tracks.begin(), s.tracks.end(), [](const Track& t) { return t.library; });
        size_t played = (size_t)std::count_if(s.tracks.begin(), s.tracks.end(), [](const Track& t) { return t.library && t.plays; });
        out.push_back(folder("albums", "Albums", Albums(s).size()));
        out.push_back(folder("artists", "Artists", Artists(s).size()));
        out.push_back(folder("playlists", "Playlists", s.playlists.size()));
        out.push_back(folder("songs", "All songs", songs));
        out.push_back(folder("top", "Most played", std::min<size_t>(played, 100)));
        (void)name;
        return true;
    }
    if (id == "albums" || id == "artists") {
        const bool albums = id == "albums";
        auto groups = albums ? Albums(s) : Artists(s);
        for (size_t i = 0; i < groups.size(); ++i) {
            Entry e;
            e.id = (albums ? "al/" : "ar/") + std::to_string(i);
            e.title = !groups[i].title.empty() ? groups[i].title : albums ? "Unknown album" : "Unknown artist";
            e.cls = albums ? "object.container.album.musicAlbum" : "object.container.person.musicArtist";
            e.artist = groups[i].artist;
            e.date = groups[i].date;
            e.art = groups[i].tracks.front()->key;
            e.children = groups[i].tracks.size();
            out.push_back(std::move(e));
        }
        return true;
    }
    if (id == "playlists") {
        for (size_t i = 0; i < s.playlists.size(); ++i) {
            Entry e;
            e.id = "pl/" + std::to_string(i);
            e.title = s.playlists[i].name;
            e.cls = "object.container.playlistContainer";
            auto tracks = PlaylistTracks(s, s.playlists[i]);
            e.children = tracks.size();
            if (!tracks.empty()) e.art = tracks.front()->key;
            out.push_back(std::move(e));
        }
        return true;
    }
    if (id == "songs" || id == "top") {
        std::vector<const Track*> tracks;
        for (const auto& t : s.tracks) if (t.library && (id == "songs" || t.plays)) tracks.push_back(&t);
        if (id == "top") {
            std::stable_sort(tracks.begin(), tracks.end(), [](const Track* a, const Track* b) { return a->plays > b->plays; });
            if (tracks.size() > 100) tracks.resize(100);
        }
        out = Songs(id, tracks);
        return true;
    }
    size_t n = 0;
    if (index("al/", SIZE_MAX, n) || index("ar/", SIZE_MAX, n)) {
        auto groups = id[1] == 'l' ? Albums(s) : Artists(s);
        if (n >= groups.size()) return false;
        out = Songs(id, groups[n].tracks);
        return true;
    }
    if (index("pl/", s.playlists.size(), n)) {
        out = Songs(id, PlaylistTracks(s, s.playlists[n]));
        return true;
    }
    return false;
}
// The container an object sits in: "-1" for the root, empty for an id that names nothing.
std::string ParentOf(const std::string& id) {
    if (id == "0") return "-1";
    if (id == "albums" || id == "artists" || id == "playlists" || id == "songs" || id == "top") return "0";
    size_t slash = id.find('/');
    if (slash == id.npos) return {};
    const std::string head = id.substr(0, slash);
    const bool nested = head == "al" || head == "ar" || head == "pl";
    size_t second = id.find('/', slash + 1);
    if (nested && second == id.npos) return head == "al" ? "albums" : head == "ar" ? "artists" : "playlists";
    return nested ? id.substr(0, second) : head;
}
std::string Didl(const Entry& e, const std::string& parent, const std::string& base) {
    if (!e.track) {
        std::string xml = "<container id=\"" + XmlEscape(e.id) + "\" parentID=\"" + XmlEscape(parent) +
            "\" restricted=\"1\" searchable=\"0\" childCount=\"" + std::to_string(e.children) + "\"><dc:title>" +
            XmlEscape(e.title) + "</dc:title>";
        if (!e.artist.empty()) xml += "<upnp:artist>" + XmlEscape(e.artist) + "</upnp:artist>";
        if (std::string date = IsoDate(e.date); !date.empty()) xml += "<dc:date>" + date + "</dc:date>";
        if (!e.art.empty()) xml += "<upnp:albumArtURI>" + XmlEscape(base + "/upnp/art/" + e.art) + "</upnp:albumArtURI>";
        return xml + "<upnp:class>" + e.cls + "</upnp:class></container>";
    }
    const Track& t = *e.track;
    std::string xml = "<item id=\"" + XmlEscape(e.id) + "\" parentID=\"" + XmlEscape(parent) + "\" restricted=\"1\"><dc:title>" +
        XmlEscape(t.title) + "</dc:title>";
    if (!t.artist.empty()) xml += "<dc:creator>" + XmlEscape(t.artist) + "</dc:creator><upnp:artist>" + XmlEscape(t.artist) + "</upnp:artist>";
    if (!t.albumArtist.empty()) xml += "<upnp:artist role=\"AlbumArtist\">" + XmlEscape(t.albumArtist) + "</upnp:artist>";
    if (!t.album.empty()) xml += "<upnp:album>" + XmlEscape(t.album) + "</upnp:album>";
    if (!t.genre.empty()) xml += "<upnp:genre>" + XmlEscape(t.genre) + "</upnp:genre>";
    if (std::string date = IsoDate(t.date); !date.empty()) xml += "<dc:date>" + date + "</dc:date>";
    if (t.number) xml += "<upnp:originalTrackNumber>" + std::to_string(t.number) + "</upnp:originalTrackNumber>";
    xml += "<upnp:albumArtURI>" + XmlEscape(base + "/upnp/art/" + t.key) + "</upnp:albumArtURI>"
           "<upnp:class>object.item.audioItem.musicTrack</upnp:class>";
    const std::string duration = Duration(t.duration);
    const std::string length = duration.empty() ? std::string() : " duration=\"" + duration + "\"";
    // The file itself, byte-seekable; then 16-bit WAV that cordinal decodes, for a format the player
    // doesn't know (a song inside a cue sheet has only that).
    if (!IsSubtrack(t.path))
        xml += "<res protocolInfo=\"http-get:*:" + PlayerMime(t) + ":DLNA.ORG_OP=01;DLNA.ORG_CI=0;" + kFlags + "\"" +
            (t.fileSize ? " size=\"" + std::to_string(t.fileSize) + "\"" : std::string()) + length + ">" +
            XmlEscape(base + "/upnp/media/" + t.key + Extension(t)) + "</res>";
    xml += "<res protocolInfo=\"http-get:*:audio/wav:DLNA.ORG_OP=01;DLNA.ORG_CI=1;" + std::string(kFlags) + "\"" + length +
        " bitsPerSample=\"16\" sampleFrequency=\"48000\" nrAudioChannels=\"2\">" + XmlEscape(base + "/upnp/wav/" + t.key + ".wav") +
        "</res>";
    return xml + "</item>";
}
std::string Envelope(const char* service, std::string_view action, const std::string& inner) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + std::string(action) +
           "Response xmlns:u=\"" + service + "\">" + inner + "</u:" + std::string(action) + "Response></s:Body></s:Envelope>";
}
std::string Fault(int code, const char* text) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
           "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><s:Fault><faultcode>s:Client</faultcode>"
           "<faultstring>UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>" +
           std::to_string(code) + "</errorCode><errorDescription>" + text +
           "</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
}
bool PrivateIPv4(const sockaddr_in& a) {
    const uint8_t* v = (const uint8_t*)&a.sin_addr;
    return v[0] == 127 || v[0] == 10 || (v[0] == 172 && (v[1] & 0xF0) == 16) || (v[0] == 192 && v[1] == 168) ||
           (v[0] == 169 && v[1] == 254) || (v[0] == 100 && (v[1] & 0xC0) == 64);
}
}  // namespace

std::string DlnaDescription(const Share& share, bool icon) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\" "
           "xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion><device>"
           "<deviceType>" + std::string(kMediaServer) + "</deviceType><dlna:X_DLNADOC>DMS-1.50</dlna:X_DLNADOC>"
           "<friendlyName>" + XmlEscape(share.name) + "</friendlyName><manufacturer>cordinal</manufacturer>"
           "<modelName>cordinal</modelName><modelDescription>Music library</modelDescription><modelNumber>1</modelNumber>"
           "<UDN>" + XmlEscape(share.udn) + "</UDN>" +
           (icon ? "<iconList><icon><mimetype>image/png</mimetype><width>256</width><height>256</height><depth>32</depth>"
                   "<url>/upnp/icon.png</url></icon></iconList>" : "") +
           "<serviceList><service><serviceType>" + kContentDirectory + "</serviceType><serviceId>urn:upnp-org:serviceId:"
           "ContentDirectory</serviceId><SCPDURL>/upnp/cd.xml</SCPDURL><controlURL>/upnp/cd</controlURL><eventSubURL>"
           "/upnp/cd/events</eventSubURL></service><service><serviceType>" + kConnectionManager + "</serviceType><serviceId>"
           "urn:upnp-org:serviceId:ConnectionManager</serviceId><SCPDURL>/upnp/cm.xml</SCPDURL><controlURL>/upnp/cm"
           "</controlURL><eventSubURL>/upnp/cm/events</eventSubURL></service></serviceList></device></root>";
}

std::string_view DlnaScpd(bool contentDirectory) {
    if (contentDirectory) return R"xml(<?xml version="1.0" encoding="utf-8"?><scpd xmlns="urn:schemas-upnp-org:service-1-0"><specVersion><major>1</major><minor>0</minor></specVersion><actionList>
<action><name>Browse</name><argumentList><argument><name>ObjectID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument><argument><name>BrowseFlag</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_BrowseFlag</relatedStateVariable></argument><argument><name>Filter</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Filter</relatedStateVariable></argument><argument><name>StartingIndex</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Index</relatedStateVariable></argument><argument><name>RequestedCount</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument><argument><name>SortCriteria</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SortCriteria</relatedStateVariable></argument><argument><name>Result</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument><argument><name>NumberReturned</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument><argument><name>TotalMatches</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument><argument><name>UpdateID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_UpdateID</relatedStateVariable></argument></argumentList></action>
<action><name>GetSearchCapabilities</name><argumentList><argument><name>SearchCaps</name><direction>out</direction><relatedStateVariable>SearchCapabilities</relatedStateVariable></argument></argumentList></action>
<action><name>GetSortCapabilities</name><argumentList><argument><name>SortCaps</name><direction>out</direction><relatedStateVariable>SortCapabilities</relatedStateVariable></argument></argumentList></action>
<action><name>GetSystemUpdateID</name><argumentList><argument><name>Id</name><direction>out</direction><relatedStateVariable>SystemUpdateID</relatedStateVariable></argument></argumentList></action>
</actionList><serviceStateTable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ObjectID</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_BrowseFlag</name><dataType>string</dataType><allowedValueList><allowedValue>BrowseMetadata</allowedValue><allowedValue>BrowseDirectChildren</allowedValue></allowedValueList></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Filter</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Index</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Count</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_SortCriteria</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Result</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_UpdateID</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>SearchCapabilities</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>SortCapabilities</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="yes"><name>SystemUpdateID</name><dataType>ui4</dataType></stateVariable>
</serviceStateTable></scpd>)xml";
    return R"xml(<?xml version="1.0" encoding="utf-8"?><scpd xmlns="urn:schemas-upnp-org:service-1-0"><specVersion><major>1</major><minor>0</minor></specVersion><actionList>
<action><name>GetProtocolInfo</name><argumentList><argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument><argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument></argumentList></action>
<action><name>GetCurrentConnectionIDs</name><argumentList><argument><name>ConnectionIDs</name><direction>out</direction><relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument></argumentList></action>
<action><name>GetCurrentConnectionInfo</name><argumentList><argument><name>ConnectionID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument><argument><name>RcsID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument><argument><name>AVTransportID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument><argument><name>ProtocolInfo</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument><argument><name>PeerConnectionManager</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument><argument><name>PeerConnectionID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument><argument><name>Direction</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument><argument><name>Status</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument></argumentList></action>
</actionList><serviceStateTable>
<stateVariable sendEvents="yes"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="yes"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="yes"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionStatus</name><dataType>string</dataType><allowedValueList><allowedValue>OK</allowedValue><allowedValue>ContentFormatMismatch</allowedValue><allowedValue>InsufficientBandwidth</allowedValue><allowedValue>UnreliableChannel</allowedValue><allowedValue>Unknown</allowedValue></allowedValueList></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionManager</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Direction</name><dataType>string</dataType><allowedValueList><allowedValue>Input</allowedValue><allowedValue>Output</allowedValue></allowedValueList></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionID</name><dataType>i4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_AVTransportID</name><dataType>i4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_RcsID</name><dataType>i4</dataType></stateVariable>
</serviceStateTable></scpd>)xml";
}

std::string DlnaControl(const Snapshot& s, bool contentDirectory, std::string_view action, std::string_view body,
                        const std::string& base, int& status) {
    status = 200;
    if (!contentDirectory) {
        if (action == "GetProtocolInfo")
            return Envelope(kConnectionManager, action,
                "<Source>http-get:*:audio/flac:*,http-get:*:audio/mpeg:*,http-get:*:audio/mp4:*,http-get:*:audio/ogg:*,"
                "http-get:*:audio/wav:*,http-get:*:audio/aac:*,http-get:*:audio/aiff:*,http-get:*:audio/x-ape:*,"
                "http-get:*:audio/x-wavpack:*,http-get:*:audio/x-ms-wma:*</Source><Sink></Sink>");
        if (action == "GetCurrentConnectionIDs") return Envelope(kConnectionManager, action, "<ConnectionIDs>0</ConnectionIDs>");
        if (action == "GetCurrentConnectionInfo")
            return Envelope(kConnectionManager, action, "<RcsID>-1</RcsID><AVTransportID>-1</AVTransportID><ProtocolInfo>"
                "</ProtocolInfo><PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
                "<Direction>Output</Direction><Status>OK</Status>");
        status = 500;
        return Fault(401, "Invalid Action");
    }
    const std::string update = std::to_string((uint32_t)s.serial);
    if (action == "GetSearchCapabilities") return Envelope(kContentDirectory, action, "<SearchCaps></SearchCaps>");
    if (action == "GetSortCapabilities") return Envelope(kContentDirectory, action, "<SortCaps></SortCaps>");
    if (action == "GetSystemUpdateID") return Envelope(kContentDirectory, action, "<Id>" + update + "</Id>");
    if (action != "Browse") { status = 500; return Fault(401, "Invalid Action"); }
    const std::string id = XmlUnescape(XmlElement(body, "ObjectID")), flag = XmlElement(body, "BrowseFlag");
    const size_t start = (size_t)strtoull(XmlElement(body, "StartingIndex").c_str(), nullptr, 10);
    size_t count = (size_t)strtoull(XmlElement(body, "RequestedCount").c_str(), nullptr, 10);
    if (!count || count > 5000) count = 5000;  // 0 asks for everything; a page that size is still bounded
    std::vector<Entry> entries;
    std::string didl;
    size_t returned = 0, total = 0;
    if (flag == "BrowseMetadata") {
        // The object itself, found among its parent's children (the root has no parent).
        const std::string parent = ParentOf(id);
        if (id == "0") {
            std::vector<Entry> top;
            Children(s, "0", {}, top);
            Entry root; root.id = "0"; root.title = "cordinal"; root.cls = "object.container.storageFolder"; root.children = top.size();
            didl = Didl(root, "-1", base);
        } else if (!parent.empty() && Children(s, parent, {}, entries)) {
            auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.id == id; });
            if (it != entries.end()) didl = Didl(*it, parent, base);
        }
        if (didl.empty()) { status = 500; return Fault(701, "No such object"); }
        returned = total = 1;
    } else if (flag == "BrowseDirectChildren") {
        if (!Children(s, id, {}, entries)) { status = 500; return Fault(701, "No such object"); }
        total = entries.size();
        for (size_t i = start; i < entries.size() && returned < count; ++i, ++returned) didl += Didl(entries[i], id, base);
    } else {
        status = 500;
        return Fault(402, "Invalid Args");
    }
    didl = "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
           "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" xmlns:dlna=\"urn:schemas-dlna-org:metadata-1-0/\">" + didl +
           "</DIDL-Lite>";
    return Envelope(kContentDirectory, action, "<Result>" + XmlEscape(didl) + "</Result><NumberReturned>" +
        std::to_string(returned) + "</NumberReturned><TotalMatches>" + std::to_string(total) + "</TotalMatches><UpdateID>" +
        update + "</UpdateID>");
}

std::vector<std::string> SsdpAnswers(std::string_view request, const std::string& udn, const std::string& location) {
    std::vector<std::string> out;
    if (!request.starts_with("M-SEARCH * HTTP/1.1\r\n")) return out;
    std::string st, man;
    for (size_t at = request.find("\r\n") + 2; at < request.size();) {
        size_t end = request.find("\r\n", at);
        if (end == request.npos || end == at) break;
        std::string_view line = request.substr(at, end - at);
        size_t colon = line.find(':');
        if (colon != line.npos) {
            std::string name = Lower(std::string(line.substr(0, colon)));
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
            while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
            if (name == "st") st = value;
            else if (name == "man") man = value;
        }
        at = end + 2;
    }
    if (man != "\"ssdp:discover\"") return out;
    const std::pair<std::string, std::string> kinds[] = {
        {"upnp:rootdevice", udn + "::upnp:rootdevice"}, {udn, udn}, {kMediaServer, udn + "::" + kMediaServer},
        {kContentDirectory, udn + "::" + kContentDirectory}, {kConnectionManager, udn + "::" + kConnectionManager}};
    for (const auto& [type, usn] : kinds) {
        if (st != "ssdp:all" && st != type) continue;
        out.push_back("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nLOCATION: " + location + "\r\nSERVER: " + kServer +
                      "\r\nST: " + type + "\r\nUSN: " + usn + "\r\nContent-Length: 0\r\n\r\n");
    }
    return out;
}

std::vector<std::string> SsdpNotices(const std::string& udn, const std::string& location, bool alive) {
    std::vector<std::string> out;
    const std::pair<std::string, std::string> kinds[] = {
        {"upnp:rootdevice", udn + "::upnp:rootdevice"}, {udn, udn}, {kMediaServer, udn + "::" + kMediaServer},
        {kContentDirectory, udn + "::" + kContentDirectory}, {kConnectionManager, udn + "::" + kConnectionManager}};
    for (const auto& [type, usn] : kinds)
        out.push_back("NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n" +
                      (alive ? "CACHE-CONTROL: max-age=1800\r\nLOCATION: " + location + "\r\nSERVER: " + kServer + "\r\n" : std::string()) +
                      "NT: " + type + "\r\nNTS: " + (alive ? "ssdp:alive" : "ssdp:byebye") + "\r\nUSN: " + usn + "\r\n\r\n");
    return out;
}

struct Announcer::Impl {
    std::thread thread;
    std::atomic<bool> stop{false};
    std::mutex mx;
    std::string error;
    uint16_t port = 0;
    std::string udn;

    // This computer's address toward `to`: what a player there can reach the description at.
    static std::string LocalToward(const sockaddr_in& to) {
        Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kBad) return {};
        sockaddr_in name{};
#ifdef _WIN32
        int size = sizeof(name);
#else
        socklen_t size = sizeof(name);
#endif
        char text[INET_ADDRSTRLEN] = {};
        std::string local;
        if (!connect(s, (const sockaddr*)&to, sizeof(to)) && !getsockname(s, (sockaddr*)&name, &size) &&
            inet_ntop(AF_INET, &name.sin_addr, text, sizeof(text)))
            local = text;
        CloseSocket(s);
        return local;
    }
    std::string Location(const std::string& host) const {
        return "http://" + host + ':' + std::to_string(port) + "/upnp/device.xml";
    }
    // Announces (or withdraws) the server on every network this computer is on.
    void Notify(bool alive) {
        sockaddr_in group{};
        group.sin_family = AF_INET;
        group.sin_port = htons(1900);
        inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
        for (const std::string& address : LocalIPv4()) {
            Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s == kBad) continue;
            sockaddr_in from{};
            from.sin_family = AF_INET;
            inet_pton(AF_INET, address.c_str(), &from.sin_addr);
            if (!bind(s, (sockaddr*)&from, sizeof(from))) {
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&from.sin_addr, sizeof(from.sin_addr));
                int ttl = 2;
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
                for (const std::string& notice : SsdpNotices(udn, Location(address), alive))
                    sendto(s, notice.data(), (int)notice.size(), 0, (sockaddr*)&group, sizeof(group));
            }
            CloseSocket(s);
        }
    }
    void Run() {
        Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        int yes = 1;
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_port = htons(1900);
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        // Port 1900 is shared: the system's own SSDP service, and other servers, listen there too.
        if (s != kBad) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        if (s == kBad || bind(s, (sockaddr*)&any, sizeof(any))) {
            if (s != kBad) CloseSocket(s);
            std::lock_guard lock(mx);
            error = "Couldn't listen for players on this network (UDP port 1900), so they may not find cordinal.";
            Notify(true);
            return;
        }
        std::vector<std::string> joined;
        uint64_t nextNotice = 0, nextJoin = 0;
        int notices = 0;
        while (!stop) {
            const uint64_t now = ClockMs();
            if (now >= nextJoin) {
                // Networks come and go (a laptop moving between Wi-Fi and a dock): join each one's group.
                for (const std::string& address : LocalIPv4()) {
                    if (std::find(joined.begin(), joined.end(), address) != joined.end()) continue;
                    ip_mreq membership{};
                    inet_pton(AF_INET, "239.255.255.250", &membership.imr_multiaddr);
                    inet_pton(AF_INET, address.c_str(), &membership.imr_interface);
                    if (!setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&membership, sizeof(membership))) {
                        joined.push_back(address);
                        nextNotice = 0;  // say hello on the new network
                    }
                }
                nextJoin = now + 30000;
            }
            if (now >= nextNotice) {
                Notify(true);
                // Twice at first (UDP can drop one), then well inside the 30 minutes players remember it.
                nextNotice = now + (++notices < 2 ? 2000 : 600000);
            }
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(s, &ready);
            timeval wait{0, 200000};
            if (select((int)s + 1, &ready, nullptr, nullptr, &wait) <= 0) continue;
            char buffer[2048];
            sockaddr_in from{};
#ifdef _WIN32
            int size = sizeof(from);
#else
            socklen_t size = sizeof(from);
#endif
            int n = recvfrom(s, buffer, sizeof(buffer), 0, (sockaddr*)&from, &size);
            if (n <= 0 || from.sin_family != AF_INET || !PrivateIPv4(from)) continue;  // the home network only
            std::string_view request(buffer, (size_t)n);
            if (!request.starts_with("M-SEARCH")) continue;
            std::string local = LocalToward(from);
            if (local.empty()) continue;
            for (const std::string& answer : SsdpAnswers(request, udn, Location(local)))
                sendto(s, answer.data(), (int)answer.size(), 0, (sockaddr*)&from, sizeof(from));
        }
        Notify(false);
        CloseSocket(s);
    }
};

Announcer::Announcer() : p_(std::make_unique<Impl>()) {}
Announcer::~Announcer() { Stop(); }
void Announcer::Start(uint16_t port, const std::string& udn) {
    Stop();
    p_->stop = false;
    p_->port = port;
    p_->udn = udn;
    {
        std::lock_guard lock(p_->mx);
        p_->error.clear();
    }
    p_->thread = std::thread([p = p_.get()] { p->Run(); });
}
void Announcer::Stop() {
    p_->stop = true;
    if (p_->thread.joinable()) p_->thread.join();
}
bool Announcer::Running() const { return p_->thread.joinable(); }
std::string Announcer::Error() const {
    std::lock_guard lock(p_->mx);
    return p_->error;
}

std::string NewShareId() {
    std::string hex = RandomHex(16);
    if (hex.size() != 32) return {};
    return "uuid:" + hex.substr(0, 8) + '-' + hex.substr(8, 4) + '-' + hex.substr(12, 4) + '-' + hex.substr(16, 4) + '-' +
           hex.substr(20);
}

}  // namespace web
