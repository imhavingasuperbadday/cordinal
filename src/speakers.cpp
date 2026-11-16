// Network speakers (speakers.h): SSDP discovery, device descriptions, AVTransport control on a
// thread of its own, and the HTTP server a speaker pulls cordinal's rendered audio from.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif
#include "speakers.h"

#include "audio_output.h"
#include "web_internal.h"
#include "web_remote.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace speakers {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kBad = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
void NonBlocking(Socket s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
void Startup() { static const bool started = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }(); (void)started; }
constexpr int kNoSignal = 0;
#else
using Socket = int;
constexpr Socket kBad = -1;
void CloseSocket(Socket s) { close(s); }
void NonBlocking(Socket s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK); }
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
void Startup() {}
constexpr int kNoSignal = MSG_NOSIGNAL;
#endif

constexpr unsigned kLeadMs = 5000;     // how far rendering may run ahead of real time
constexpr unsigned kChunkMs = 20;      // one render call
constexpr unsigned kReplayMs = 3000;   // a speaker that reconnects this early gets the start again
constexpr unsigned kStartupMs = 1500;  // assumed buffering before sound, for a speaker that won't say
constexpr const char* kFlags = "DLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000";

std::string Trim(std::string_view t) {
    while (!t.empty() && (unsigned char)t.front() <= ' ') t.remove_prefix(1);
    while (!t.empty() && (unsigned char)t.back() <= ' ') t.remove_suffix(1);
    return std::string(t);
}
std::string Origin(const std::string& url) {
    size_t scheme = url.find("//");
    if (scheme == url.npos) return {};
    return url.substr(0, url.find('/', scheme + 2));
}
std::string ResolveUrl(const std::string& base, const std::string& location, std::string path) {
    path = Trim(path);
    if (path.starts_with("http://") || path.starts_with("https://")) return path;
    const std::string& from = base.empty() ? location : base;
    if (path.starts_with('/')) return Origin(from) + path;
    std::string dir = from;
    if (!base.empty()) { if (!dir.ends_with('/')) dir += '/'; }
    else {
        std::string origin = Origin(from);
        size_t slash = from.rfind('/');
        dir = slash == from.npos || slash < origin.size() ? origin + '/' : from.substr(0, slash + 1);
    }
    return dir + path;
}
// "http://host:port/..." -> host and port.
bool HostPort(const std::string& url, std::string& host, std::string& port) {
    std::string origin = Origin(url);
    size_t start = origin.find("//");
    if (start == origin.npos) return false;
    std::string authority = origin.substr(start + 2);
    if (size_t at = authority.rfind('@'); at != authority.npos) authority.erase(0, at + 1);
    size_t colon = authority.rfind(':');
    host = colon == authority.npos ? authority : authority.substr(0, colon);
    port = colon == authority.npos ? std::string(url.starts_with("https") ? "443" : "80") : authority.substr(colon + 1);
    return !host.empty() && host[0] != '[';
}
// This computer's IPv4 address on its route to `url`'s host: what the speaker can reach us at.
std::string LocalAddressToward(const std::string& url) {
    std::string host, port;
    if (!HostPort(url, host, port)) return {};
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) || !found) return {};
    std::string local;
    Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != kBad) {
        sockaddr_in name{};
#ifdef _WIN32
        int size = sizeof(name);
#else
        socklen_t size = sizeof(name);
#endif
        char text[INET_ADDRSTRLEN] = {};
        if (!connect(s, found->ai_addr, (int)found->ai_addrlen) && !getsockname(s, (sockaddr*)&name, &size) &&
            inet_ntop(AF_INET, &name.sin_addr, text, sizeof(text)))
            local = text;
        CloseSocket(s);
    }
    freeaddrinfo(found);
    return local;
}
web::HttpReply Soap(const std::string& control, const std::string& type, const char* action, const std::string& args,
                    unsigned ms = 4000) {
    std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + std::string(action) + " xmlns:u=\"" + type +
        "\">" + args + "</u:" + action + "></s:Body></s:Envelope>";
    return web::HttpFetch(nullptr, control, "POST", body, "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" +
                          type + '#' + action + "\"\r\n", ms, 1u << 18);
}
std::string UpnpError(const web::HttpReply& reply) {
    if (!reply.status) return "no answer";
    std::string code = web::XmlElement(reply.body, "errorCode"), text = web::XmlUnescape(web::XmlElement(reply.body, "errorDescription"));
    if (code.empty()) return "HTTP " + std::to_string(reply.status);
    return "UPnP error " + code + (text.empty() ? "" : ": " + text);
}
// "H:MM:SS", "H:MM:SS.fff" or "H:MM:SS.n/d" -> milliseconds; -1 when not a time.
int64_t ParseTime(std::string_view text) {
    int64_t parts[3] = {};
    size_t at = 0;
    for (int i = 0; i < 3; ++i) {
        if (i && (at >= text.size() || text[at++] != ':')) return -1;
        size_t start = at;
        while (at < text.size() && at - start < 9 && text[at] >= '0' && text[at] <= '9') parts[i] = parts[i] * 10 + (text[at++] - '0');
        if (at == start) return -1;
    }
    if (parts[1] > 59 || parts[2] > 59) return -1;
    int64_t ms = (parts[0] * 3600 + parts[1] * 60 + parts[2]) * 1000;
    if (at < text.size() && text[at] == '.' && text.find('/', at) == text.npos) {
        int64_t fraction = 0, digits = 0;
        for (++at; at < text.size() && text[at] >= '0' && text[at] <= '9' && digits < 3; ++at, ++digits)
            fraction = fraction * 10 + (text[at] - '0');
        while (digits++ < 3) fraction *= 10;
        ms += fraction;
    }
    return ms;
}
void Put16(char* p, uint16_t v) { p[0] = (char)v; p[1] = (char)(v >> 8); }
void Put32(char* p, uint32_t v) { Put16(p, (uint16_t)v); Put16(p + 2, (uint16_t)(v >> 16)); }
// An endless 16-bit stereo WAV: both sizes at their maximum, which readers take as "streaming".
std::string WavHeader(unsigned rate) {
    std::string h(44, '\0');
    memcpy(h.data(), "RIFF", 4); Put32(h.data() + 4, 0xFFFFFFFFu);
    memcpy(h.data() + 8, "WAVEfmt ", 8); Put32(h.data() + 16, 16);
    Put16(h.data() + 20, 1); Put16(h.data() + 22, 2); Put32(h.data() + 24, rate);
    Put32(h.data() + 28, rate * 4); Put16(h.data() + 32, 4); Put16(h.data() + 34, 16);
    memcpy(h.data() + 36, "data", 4); Put32(h.data() + 40, 0xFFFFFFFFu);
    return h;
}
}  // namespace

std::string DeviceId(const Speaker& s) { return "upnp:" + s.udn + ' ' + s.location + ' ' + s.name; }

bool ParseDeviceId(std::string_view id, Speaker& s) {
    if (!IsSpeakerId(id)) return false;
    id.remove_prefix(5);
    size_t a = id.find(' ');
    if (a == id.npos || !a) return false;
    size_t b = id.find(' ', a + 1);
    s.udn = std::string(id.substr(0, a));
    s.location = std::string(id.substr(a + 1, b == id.npos ? id.npos : b - a - 1));
    s.name = b == id.npos ? std::string() : std::string(id.substr(b + 1));
    if (s.name.empty()) s.name = "Network speaker";
    return s.location.starts_with("http://");
}

bool ParseDescription(std::string_view xml, const std::string& location, Speaker& s) {
    const std::string base = web::XmlUnescape(web::XmlElement(xml, "URLBase"));
    s.location = location;
    s.udn = web::XmlElement(xml, "UDN");
    s.name = web::XmlUnescape(web::XmlElement(xml, "friendlyName"));
    s.model = web::XmlUnescape(web::XmlElement(xml, "modelName"));
    s.transport.clear();
    s.connection.clear();
    for (size_t at = 0; (at = xml.find("<service>", at)) != xml.npos;) {
        size_t end = xml.find("</service>", at);
        if (end == xml.npos) break;
        std::string_view block = xml.substr(at, end - at);
        std::string type = web::XmlElement(block, "serviceType"), control = web::XmlUnescape(web::XmlElement(block, "controlURL"));
        if (!control.empty() && type.find(":AVTransport:") != type.npos && s.transport.empty()) {
            s.transport = ResolveUrl(base, location, control);
            s.transportType = type;
        } else if (!control.empty() && type.find(":ConnectionManager:") != type.npos && s.connection.empty()) {
            s.connection = ResolveUrl(base, location, control);
            s.connectionType = type;
        }
        at = end;
    }
    // Names go into the output id, one line of settings: no line breaks.
    std::replace_if(s.name.begin(), s.name.end(), [](char c) { return c == '\r' || c == '\n' || c == '\t'; }, ' ');
    if (s.name.empty()) s.name = s.model.empty() ? "Network speaker" : s.model;
    return !s.transport.empty() && !s.udn.empty() && s.udn.find(' ') == s.udn.npos;
}

bool Describe(const std::string& location, Speaker& s) {
    if (!location.starts_with("http://")) return false;
    auto reply = web::HttpFetch(nullptr, location, "GET", {}, {}, 3000, 1u << 18);
    return reply.status == 200 && ParseDescription(reply.body, location, s);
}

std::vector<Speaker> Discover(unsigned ms) {
    Startup();
    std::vector<Socket> sockets;
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
    // One question per network this computer is on, so a speaker behind a second adapter answers too.
    for (const std::string& address : web::LocalIPv4()) {
        Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == kBad) continue;
        sockaddr_in from{};
        from.sin_family = AF_INET;
        inet_pton(AF_INET, address.c_str(), &from.sin_addr);
        const in_addr local = from.sin_addr;
        if (bind(s, (sockaddr*)&from, sizeof(from))) { CloseSocket(s); continue; }
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&local, sizeof(local));
        int ttl = 2;
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
        for (const char* target : {"urn:schemas-upnp-org:device:MediaRenderer:1", "urn:schemas-upnp-org:service:AVTransport:1"}) {
            std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                             "MX: 1\r\nST: ") + target + "\r\nUSER-AGENT: cordinal UPnP/1.1\r\n\r\n";
            sendto(s, search.data(), (int)search.size(), 0, (sockaddr*)&group, sizeof(group));
        }
        sockets.push_back(s);
    }
    std::vector<std::string> locations;
    for (uint64_t until = web::ClockMs() + ms; !sockets.empty();) {
        uint64_t now = web::ClockMs();
        if (now >= until) break;
        fd_set ready;
        FD_ZERO(&ready);
        Socket top = 0;
        for (Socket s : sockets) { FD_SET(s, &ready); top = std::max(top, s); }
        timeval wait{(long)((until - now) / 1000), (long)((until - now) % 1000) * 1000};
        if (select((int)top + 1, &ready, nullptr, nullptr, &wait) <= 0) continue;
        for (Socket s : sockets) {
            if (!FD_ISSET(s, &ready)) continue;
            char reply[2048];
            int n = recv(s, reply, sizeof(reply) - 1, 0);
            if (n <= 0) continue;
            std::string location = web::SsdpLocation({reply, (size_t)n});
            if (location.starts_with("http://") && locations.size() < 64 &&
                std::find(locations.begin(), locations.end(), location) == locations.end())
                locations.push_back(location);
        }
    }
    for (Socket s : sockets) CloseSocket(s);
    // Descriptions are read side by side: one slow device shouldn't hold up the list.
    std::vector<Speaker> found(locations.size());
    std::vector<char> ok(locations.size(), 0);
    std::vector<std::thread> readers;
    for (size_t i = 0; i < locations.size(); ++i)
        readers.emplace_back([&, i] { ok[i] = Describe(locations[i], found[i]); });
    for (auto& t : readers) t.join();
    std::vector<Speaker> out;
    for (size_t i = 0; i < found.size(); ++i)
        if (ok[i] && std::none_of(out.begin(), out.end(), [&](const Speaker& s) { return s.udn == found[i].udn; }))
            out.push_back(std::move(found[i]));
    std::sort(out.begin(), out.end(), [](const Speaker& a, const Speaker& b) { return a.name < b.name; });
    return out;
}

struct Stream::Impl {
    // Fixed once the threads run.
    Speaker wanted;
    unsigned rate = 48000;
    Callbacks cb;
    std::function<void()> onFailure;
    Socket listener = kBad;
    uint16_t port = 0;
    std::thread server;
    std::atomic<bool> quit{false}, active{true}, flush{false}, failed{false};

    // Shared by the server and control threads.
    mutable std::mutex mx;
    std::condition_variable cv;
    struct Command {
        enum Kind { Start, Pause, Play, Stop } kind;
        std::string token;
        uint64_t session = 0;
    };
    std::deque<Command> commands;
    bool controlQuit = false;
    bool resolved = false;     // the speaker answered: `speaker`, `host` and `l16` are set
    Speaker speaker;
    std::string host;          // this computer's address toward the speaker
    bool l16 = false;          // DLNA LPCM (big-endian, no header) instead of WAV
    std::string problem;
    bool failNow = false;      // set by the control thread; the server thread reports it
    bool started = false;      // a session was started: pause and resume go to the speaker
    bool stopped = false;      // the speaker couldn't pause and was stopped: resuming starts it again
    uint64_t reportSession = 0, startedAt = 0;  // the session the speaker plays, and when it was told to
    int64_t relMs = -1;        // its last reported position in that session
    uint64_t relAt = 0;

    void Fail(std::string why) {
        std::lock_guard lock(mx);
        if (problem.empty()) problem = std::move(why);
        failNow = true;
    }
    std::string Url(const std::string& token) const {
        return "http://" + host + ':' + std::to_string(port) + '/' + token + (l16 ? ".pcm" : ".wav");
    }
    std::string Mime() const { return l16 ? "audio/L16;rate=" + std::to_string(rate) + ";channels=2" : "audio/wav"; }
    void Server();
    static void Control(std::shared_ptr<Impl> self);
};

// Finds the speaker, chooses the stream format, then carries out the server thread's commands and
// reads the speaker's position once a second while it plays. It may outlive the Stream (it is
// detached so that telling a vanished speaker to stop never holds up the caller), so it only ever
// touches the shared state, never the callbacks.
void Stream::Impl::Control(std::shared_ptr<Impl> self) {
    Impl& p = *self;
    Speaker s;
    bool found = Describe(p.wanted.location, s) && s.udn == p.wanted.udn;
    if (!found && !p.quit) {
        for (auto& d : Discover(2500))
            if (d.udn == p.wanted.udn) { found = Describe(d.location, s); break; }
    }
    std::string host = found ? LocalAddressToward(s.transport) : std::string();
    if (!found) p.Fail(p.wanted.name + " isn't answering on the network. Is it switched on?");
    else if (host.empty()) p.Fail("This computer has no route to " + s.name + '.');
    else {
        // WAV unless the speaker lists only LPCM among what it plays.
        bool l16 = false;
        if (!s.connection.empty() && !p.quit) {
            auto reply = Soap(s.connection, s.connectionType, "GetProtocolInfo", {}, 3000);
            std::string sink = web::Lower(web::XmlUnescape(web::XmlElement(reply.body, "Sink")));
            bool wav = sink.empty() || sink.find("audio/wav") != sink.npos || sink.find("audio/x-wav") != sink.npos ||
                       sink.find("audio/wave") != sink.npos || sink.find("http-get:*:*:") != sink.npos;
            l16 = reply.status == 200 && !wav && sink.find("audio/l16") != sink.npos;
        }
        std::lock_guard lock(p.mx);
        p.speaker = s;
        p.host = host;
        p.l16 = l16;
        p.resolved = true;
    }
    p.cv.notify_all();
    for (;;) {
        Command c{Command::Stop};
        bool have = false, poll = false;
        uint64_t pollSession = 0;
        {
            std::unique_lock lock(p.mx);
            p.cv.wait_for(lock, std::chrono::milliseconds(1000), [&] { return !p.commands.empty() || p.controlQuit; });
            if (!p.commands.empty()) { c = std::move(p.commands.front()); p.commands.pop_front(); have = true; }
            else if (p.controlQuit) break;
            else { poll = p.resolved && p.startedAt && !p.stopped && p.active; pollSession = p.reportSession; }
        }
        if (!found || host.empty()) continue;
        const std::string& t = s.transport;
        const std::string& type = s.transportType;
        if (poll) {
            auto reply = Soap(t, type, "GetPositionInfo", "<InstanceID>0</InstanceID>", 2000);
            int64_t rel = reply.status == 200 ? ParseTime(web::XmlElement(reply.body, "RelTime")) : -1;
            std::lock_guard lock(p.mx);
            if (rel >= 0 && p.reportSession == pollSession) { p.relMs = rel; p.relAt = web::ClockMs(); }
            continue;
        }
        if (!have) continue;
        switch (c.kind) {
            case Command::Start: {
                Soap(t, type, "Stop", "<InstanceID>0</InstanceID>", 3000);  // some refuse a new URI while playing
                std::string url, mime;
                bool l16;
                {
                    std::lock_guard lock(p.mx);
                    url = p.Url(c.token);
                    mime = p.Mime();
                    l16 = p.l16;
                    p.startedAt = 0;
                }
                std::string didl = "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
                    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
                    "<item id=\"cordinal\" parentID=\"0\" restricted=\"1\"><dc:title>cordinal</dc:title>"
                    "<upnp:class>object.item.audioItem.musicTrack</upnp:class><res protocolInfo=\"http-get:*:" + mime + ':' +
                    (l16 ? std::string("DLNA.ORG_PN=LPCM;") : std::string()) + kFlags + "\">" + web::XmlEscape(url) +
                    "</res></item></DIDL-Lite>";
                std::string args = "<InstanceID>0</InstanceID><CurrentURI>" + web::XmlEscape(url) + "</CurrentURI><CurrentURIMetaData>" +
                    web::XmlEscape(didl) + "</CurrentURIMetaData>";
                auto set = Soap(t, type, "SetAVTransportURI", args);
                if (set.status != 200 && !p.quit) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    set = Soap(t, type, "SetAVTransportURI", args);
                }
                if (set.status != 200) { p.Fail(s.name + " refused cordinal's stream (" + UpnpError(set) + ")."); break; }
                auto play = Soap(t, type, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>");
                if (play.status != 200) { p.Fail(s.name + " wouldn't start playing (" + UpnpError(play) + ")."); break; }
                std::lock_guard lock(p.mx);
                p.reportSession = c.session;
                p.startedAt = web::ClockMs();
                p.relMs = -1;
                p.stopped = false;
                break;
            }
            case Command::Pause:
                // A speaker that can't pause a live stream is stopped instead; resuming starts it afresh.
                if (Soap(t, type, "Pause", "<InstanceID>0</InstanceID>").status != 200) {
                    Soap(t, type, "Stop", "<InstanceID>0</InstanceID>", 3000);
                    std::lock_guard lock(p.mx);
                    p.stopped = true;
                }
                break;
            case Command::Play:
                Soap(t, type, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>");
                break;
            case Command::Stop:
                Soap(t, type, "Stop", "<InstanceID>0</InstanceID>", 2000);
                break;
        }
    }
}

// Serves the stream to the speaker and renders into it as fast as it reads, up to kLeadMs ahead of
// real time. One connection at a time: the newest request wins, as a speaker that reconnects wants.
void Stream::Impl::Server() {
    const size_t frameBytes = 4, chunk = std::max<size_t>(1, rate * kChunkMs / 1000);
    const size_t replayBytes = (size_t)rate * frameBytes * kReplayMs / 1000;
    std::vector<float> floats(chunk * 2);
    std::vector<int16_t> samples(chunk * 2);
    std::string token, pending, history;
    size_t sent = 0;
    bool replayable = true, connectedOnce = false, anySession = false, deferred = false;
    Socket client = kBad;
    uint64_t session = 0, rendered = 0, base = 0, playedMax = 0, playingMs = 0, lostAt = 0;
    int64_t relBase = 0;
    uint64_t last = web::ClockMs();
    std::deque<uint64_t> restarts;
    auto closeClient = [&] {
        if (client != kBad) CloseSocket(client);
        client = kBad;
        pending.clear();
        sent = 0;
    };
    auto newSession = [&] {
        closeClient();
        history.clear();
        replayable = true;
        connectedOnce = false;
        base = rendered;
        playingMs = 0;
        relBase = 0;
        lostAt = 0;
        token = web::RandomHex(16);
        ++session;
        std::lock_guard lock(mx);
        started = true;
        stopped = false;
        startedAt = 0;
        commands.push_back({Command::Start, token, session});
        cv.notify_all();
    };
    // Frames of the stream the speaker has played: its own report when it gives one, moved on by the
    // time since; otherwise the time it has been reading, less a typical start-up buffer.
    auto played = [&](uint64_t now, bool on) {
        int64_t rel = -1;
        uint64_t at = 0;
        {
            std::lock_guard lock(mx);
            if (reportSession == session) { rel = relMs; at = relAt; }
        }
        if (rel >= 0 && rel < relBase) relBase = 0;  // the speaker began counting again
        uint64_t estimate;
        if (rel > relBase) estimate = (uint64_t)(rel - relBase) * rate / 1000 + (on && now > at ? (now - at) * rate / 1000 : 0);
        else estimate = playingMs > kStartupMs ? (playingMs - kStartupMs) * rate / 1000 : 0;
        estimate = std::min(estimate, rendered - base);
        playedMax = std::max(playedMax, base + estimate);
        return playedMax;
    };
    auto accept = [&] {
        Socket s = ::accept(listener, nullptr, nullptr);
        if (s == kBad) return;
        // Read the request line and headers: a couple of seconds at most.
        std::string request;
        for (uint64_t until = web::ClockMs() + 2000; request.find("\r\n\r\n") == request.npos && request.size() < 8192;) {
            uint64_t now = web::ClockMs();
            if (now >= until) break;
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(s, &ready);
            timeval wait{0, (long)std::min<uint64_t>(until - now, 200) * 1000};
            if (select((int)s + 1, &ready, nullptr, nullptr, &wait) <= 0) continue;
            char bytes[2048];
            int n = recv(s, bytes, sizeof(bytes), 0);
            if (n <= 0) break;
            request.append(bytes, (size_t)n);
        }
        size_t space = request.find(' '), second = space == request.npos ? request.npos : request.find(' ', space + 1);
        std::string method = request.substr(0, space), path = second == request.npos ? "" : request.substr(space + 1, second - space - 1);
        bool l16now, ours = !token.empty();
        std::string mime;
        {
            std::lock_guard lock(mx);
            l16now = l16;
            mime = Mime();
        }
        ours = ours && path == '/' + token + (l16now ? ".pcm" : ".wav");
        if (!ours || (method != "GET" && method != "HEAD")) {
            const char* reply = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(s, reply, (int)strlen(reply), kNoSignal);
            CloseSocket(s);
            return;
        }
        std::string headers = "HTTP/1.1 200 OK\r\nContent-Type: " + mime + "\r\nConnection: close\r\nCache-Control: no-cache\r\n"
            "Accept-Ranges: none\r\ntransferMode.dlna.org: Streaming\r\ncontentFeatures.dlna.org: " +
            (l16now ? std::string("DLNA.ORG_PN=LPCM;") : std::string()) + kFlags + "\r\nServer: cordinal UPnP/1.0 DLNADOC/1.50\r\n\r\n";
        if (method == "HEAD") {
            send(s, headers.data(), (int)headers.size(), kNoSignal);
            CloseSocket(s);
            return;
        }
        // A reconnect early in the stream gets it again from the start (speakers often probe first);
        // a later one carries on from now, counted from here.
        bool replay = replayable;
        closeClient();
        client = s;
        NonBlocking(client);
        pending = headers + (l16now ? std::string() : WavHeader(rate)) + (replay ? history : std::string());
        if (connectedOnce && !replay) {
            base = rendered;
            playingMs = 0;
            std::lock_guard lock(mx);
            relBase = reportSession == session ? std::max<int64_t>(relMs, 0) : 0;
        }
        connectedOnce = true;
        lostAt = 0;
    };
    while (!quit) {
        const uint64_t now = web::ClockMs(), elapsed = now - last;
        last = now;
        bool failing, ready;
        {
            std::lock_guard lock(mx);
            failing = failNow;
            ready = resolved;
        }
        if (failing && !failed) {
            closeClient();
            failed = true;
            if (onFailure) onFailure();
        }
        if (failed) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        const bool on = active;
        // A seek while paused starts the fresh stream on resuming, so the speaker isn't left waiting.
        if (flush.exchange(false) && anySession) deferred = true;
        if (on && deferred) { deferred = false; newSession(); }
        if (on && ready && !anySession) { anySession = true; newSession(); }
        if (on && client != kBad) playingMs += elapsed;
        if (cb.timing) cb.timing(played(now, on));

        // Render the next chunk once the last one is on its way, within the lead.
        if (on && client != kBad && sent == pending.size()) {
            pending.clear();
            sent = 0;
            if (rendered - base < (playingMs + kLeadMs) * rate / 1000) {
                cb.render(floats.data(), (uint32_t)chunk);
                ConvertSamples(floats.data(), floats.size(), SampleFormat::Int16, samples.data());
                pending.resize(samples.size() * 2);
                bool bigEndian;
                { std::lock_guard lock(mx); bigEndian = l16; }
                for (size_t i = 0; i < samples.size(); ++i) {
                    uint16_t v = (uint16_t)samples[i];
                    pending[i * 2 + (bigEndian ? 1 : 0)] = (char)(v & 0xFF);
                    pending[i * 2 + (bigEndian ? 0 : 1)] = (char)(v >> 8);
                }
                rendered += chunk;
                if (replayable) {
                    if (history.size() + pending.size() <= replayBytes) history += pending;
                    else { replayable = false; std::string().swap(history); }
                }
            }
        }
        fd_set read, write;
        FD_ZERO(&read);
        FD_ZERO(&write);
        FD_SET(listener, &read);
        Socket top = listener;
        if (client != kBad) {
            FD_SET(client, &read);  // to notice the speaker hanging up
            if (on && sent < pending.size()) FD_SET(client, &write);
            top = std::max(top, client);
        }
        timeval wait{0, 10000};
        if (select((int)top + 1, &read, &write, nullptr, &wait) > 0) {
            if (client != kBad && FD_ISSET(client, &read)) {
                char sink[512];
                int n = recv(client, sink, sizeof(sink), 0);
                if (n == 0 || (n < 0 && !WouldBlock())) { closeClient(); lostAt = now; }
            }
            if (client != kBad && FD_ISSET(client, &write)) {
                int n = send(client, pending.data() + sent, (int)std::min<size_t>(pending.size() - sent, 65536), kNoSignal);
                if (n > 0) sent += (size_t)n;
                else if (n < 0 && !WouldBlock()) { closeClient(); lostAt = now; }
            }
            if (FD_ISSET(listener, &read)) accept();
        }
        // A speaker that never fetches the stream, or drops it while playing, is started again, a few
        // times a minute at most.
        if (on && client == kBad && anySession) {
            uint64_t startedTime;
            {
                std::lock_guard lock(mx);
                startedTime = startedAt;
            }
            const bool neverCame = !connectedOnce && startedTime && now - startedTime > 12000;
            const bool dropped = connectedOnce && lostAt && now - lostAt > 5000;
            if (neverCame || dropped) {
                while (!restarts.empty() && now - restarts.front() > 60000) restarts.pop_front();
                if (restarts.size() >= 3) {
                    std::string name;
                    { std::lock_guard lock(mx); name = speaker.name; }
                    Fail(name + (connectedOnce ? " keeps dropping cordinal's stream." :
                                 " didn't fetch cordinal's stream. A firewall on this computer may be blocking it."));
                } else {
                    restarts.push_back(now);
                    newSession();
                }
            }
        }
    }
    closeClient();
}

Stream::Stream(std::shared_ptr<Impl> impl) : p_(std::move(impl)) {}

std::unique_ptr<Stream> Stream::Open(const std::string& id, unsigned rate, bool active, Callbacks callbacks,
                                     std::function<void()> onFailure, std::string& error) {
    Startup();
    auto p = std::make_shared<Impl>();
    if (!ParseDeviceId(id, p->wanted)) { error = "not a network speaker"; return nullptr; }
    p->rate = rate ? rate : 48000;
    p->cb = std::move(callbacks);
    p->onFailure = std::move(onFailure);
    p->active = active;
    // Every IPv4 interface, on a port the system chooses: the stream's URL names the one that leads
    // to the speaker.
    p->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    if (p->listener == kBad || bind(p->listener, (sockaddr*)&address, sizeof(address)) || listen(p->listener, 4) ||
        getsockname(p->listener, (sockaddr*)&address, &size)) {
        if (p->listener != kBad) CloseSocket(p->listener);
        error = "couldn't open a network port for the speaker";
        return nullptr;
    }
    NonBlocking(p->listener);
    p->port = ntohs(address.sin_port);
    Impl* raw = p.get();
    p->server = std::thread([raw] { raw->Server(); });
    std::thread([p] { Impl::Control(p); }).detach();
    return std::unique_ptr<Stream>(new Stream(std::move(p)));
}

Stream::~Stream() {
    p_->quit = true;
    if (p_->server.joinable()) p_->server.join();
    CloseSocket(p_->listener);
    {
        std::lock_guard lock(p_->mx);
        if (p_->started) p_->commands.push_back({Impl::Command::Stop});
        p_->controlQuit = true;
    }
    p_->cv.notify_all();
}

void Stream::SetActive(bool on) {
    if (p_->active.exchange(on) == on) return;
    std::lock_guard lock(p_->mx);
    if (!p_->started) return;  // the server thread starts the first session once active
    if (on && p_->stopped) p_->flush = true;  // it was stopped, not paused: start it again
    else p_->commands.push_back({on ? Impl::Command::Play : Impl::Command::Pause});
    p_->cv.notify_all();
}

void Stream::Flush() { p_->flush = true; }
bool Stream::Failed() const { return p_->failed; }
unsigned Stream::Rate() const { return p_->rate; }

std::string Stream::Name() const {
    std::lock_guard lock(p_->mx);
    return p_->resolved ? p_->speaker.name : p_->wanted.name;
}

std::string Stream::Problem() const {
    std::lock_guard lock(p_->mx);
    return p_->problem;
}

}  // namespace speakers
