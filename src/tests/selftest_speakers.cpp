// Network speakers (speakers.h) against a fake UPnP renderer on loopback: descriptions and ids, the
// control sequence, the WAV and LPCM streams byte for byte, the lead over real time, the position
// taken from the speaker's clock, pause, a probing reconnect, a fresh stream on flush, refusal and a
// speaker that isn't there. No real network device is needed.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "selftest_common.h"
#include "speakers.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace {
constexpr unsigned kRate = 48000;

// The sample the test's render callback makes for frame `n` (left; right is its negative): exact
// 16-bit values, which the output converts without dither.
int16_t Pattern(uint64_t n) { return (int16_t)((int)(n % 2000) - 1000); }

std::string Between(const std::string& text, const std::string& open, const std::string& close) {
    size_t a = text.find(open);
    if (a == text.npos) return {};
    a += open.size();
    size_t b = text.find(close, a);
    return b == text.npos ? std::string() : text.substr(a, b - a);
}
std::string Unescape(std::string text) {
    for (auto [from, to] : {std::pair{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}})
        for (size_t at = 0; (at = text.find(from, at)) != text.npos; at += strlen(to)) text.replace(at, strlen(from), to);
    return text;
}
SOCKET Connect(unsigned short port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s != INVALID_SOCKET && connect(s, (sockaddr*)&to, sizeof(to))) { closesocket(s); s = INVALID_SOCKET; }
    if (s != INVALID_SOCKET) { DWORD wait = 3000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&wait, sizeof(wait)); }
    return s;
}
// A GET of `url` (on loopback): the status line's code, or 0.
int Status(const std::string& url) {
    unsigned short port = (unsigned short)atoi(Between(url, "127.0.0.1:", "/").c_str());
    SOCKET s = Connect(port);
    if (s == INVALID_SOCKET) return 0;
    std::string path = url.substr(url.find('/', 7));
    std::string request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(s, request.data(), (int)request.size(), 0);
    char reply[64] = {};
    recv(s, reply, sizeof(reply) - 1, 0);
    closesocket(s);
    return atoi(reply + 9);
}

// A UPnP media renderer that plays in real time: it reads the stream as fast as cordinal sends it
// (a big buffer) but reports its position by the clock since Play, less a short start-up.
struct FakeRenderer {
    SOCKET listener = INVALID_SOCKET;
    unsigned short port = 0;
    std::thread thread, reader;
    std::atomic<bool> quit{false}, stopReader{false}, paused{false};
    std::mutex mx;
    std::vector<std::string> actions, uris;
    std::string first;              // the first bytes of the last stream connection, after its headers
    std::atomic<uint64_t> read{0};  // stream bytes read from the current connection, after its headers
    ULONGLONG playedAt = 0, pausedAt = 0, pausedFor = 0;
    int refuse = 0;                 // SetAVTransportURI answers this UPnP error
    bool lpcm = false;              // lists only audio/L16 in its protocol info
    bool probe = false;             // reads a little of the first stream, hangs up and comes back

    bool Start() {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        if (listener == INVALID_SOCKET || bind(listener, (sockaddr*)&address, sizeof(address)) || listen(listener, 8) ||
            getsockname(listener, (sockaddr*)&address, &length))
            return false;
        port = ntohs(address.sin_port);
        thread = std::thread([this] { Serve(); });
        return true;
    }
    ~FakeRenderer() {
        quit = true;
        StopReader();
        if (thread.joinable()) thread.join();
        if (listener != INVALID_SOCKET) closesocket(listener);
    }
    std::string Location() const { return "http://127.0.0.1:" + std::to_string(port) + "/desc.xml"; }
    std::vector<std::string> Actions() { std::lock_guard lock(mx); return actions; }
    std::vector<std::string> Uris() { std::lock_guard lock(mx); return uris; }
    std::string First() { std::lock_guard lock(mx); return first; }
    size_t Count(const std::string& action) {
        auto all = Actions();
        return (size_t)std::count(all.begin(), all.end(), action);
    }
    void StopReader() {
        stopReader = true;
        if (reader.joinable()) reader.join();
        stopReader = false;
    }
    void Read(std::string url, bool hangUpEarly) {
        SOCKET s = Connect(port == 0 ? 0 : (unsigned short)atoi(Between(url, "127.0.0.1:", "/").c_str()));
        if (s == INVALID_SOCKET) return;
        std::string request = "GET " + url.substr(url.find('/', 7)) + " HTTP/1.1\r\nHost: 127.0.0.1\r\nRange: bytes=0-\r\n\r\n";
        send(s, request.data(), (int)request.size(), 0);
        std::string head, body;
        read = 0;
        char buffer[16384];
        while (!stopReader && !quit) {
            if (paused) { Sleep(10); continue; }
            int n = recv(s, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            if (head.find("\r\n\r\n") == head.npos) {
                head.append(buffer, n);
                size_t end = head.find("\r\n\r\n");
                if (end == head.npos) continue;
                body = head.substr(end + 4);
                head.resize(end + 4);
            } else if (body.size() < 65536) {
                body.append(buffer, (size_t)n);
            } else {
                read += (uint64_t)n;
            }
            if (body.size() <= 65536) {
                std::lock_guard lock(mx);
                first = body;
                read = body.size();
            }
            if (hangUpEarly && body.size() >= 512) break;
        }
        closesocket(s);
    }
    void Serve() {
        while (!quit) {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(listener, &ready);
            timeval wait{0, 50000};
            if (select(0, &ready, nullptr, nullptr, &wait) <= 0) continue;
            SOCKET c = accept(listener, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            DWORD timeout = 2000;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            std::string request;
            char buffer[8192];
            for (;;) {
                size_t end = request.find("\r\n\r\n");
                if (end != request.npos) {
                    size_t length = (size_t)atoi(Between(request, "Content-Length: ", "\r\n").c_str());
                    if (request.size() >= end + 4 + length) break;
                }
                int n = recv(c, buffer, sizeof(buffer), 0);
                if (n <= 0) break;
                request.append(buffer, (size_t)n);
            }
            Answer(c, request);
            closesocket(c);
        }
    }
    void Reply(SOCKET c, int status, const std::string& body) {
        std::string head = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Error") +
            "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n";
        send(c, head.data(), (int)head.size(), 0);
        send(c, body.data(), (int)body.size(), 0);
    }
    void Answer(SOCKET c, const std::string& request) {
        if (request.starts_with("GET /desc.xml ")) {
            // A root device holding the renderer, relative control URLs and an escaped name.
            Reply(c, 200, "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device>"
                "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
                "<friendlyName>Fake &amp; Speaker</friendlyName><modelName>Loopback</modelName><UDN>uuid:fake-renderer</UDN>"
                "<serviceList><service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
                "<controlURL>cm</controlURL></service><service><serviceType>urn:schemas-upnp-org:service:AVTransport:1"
                "</serviceType><controlURL>/ctl</controlURL></service></serviceList></device></root>");
            return;
        }
        std::string soapAction = Between(request, "#", "\"");
        auto envelope = [](const std::string& action, const std::string& inner) {
            return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:" +
                action + "Response xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">" + inner + "</u:" + action +
                "Response></s:Body></s:Envelope>";
        };
        if (request.starts_with("POST /dev/cm ") || request.starts_with("POST /cm ")) {
            Reply(c, 200, envelope("GetProtocolInfo", std::string("<Source></Source><Sink>") +
                (lpcm ? "http-get:*:audio/L16;rate=48000;channels=2:*,http-get:*:audio/mpeg:*" :
                        "http-get:*:audio/mpeg:*,http-get:*:audio/wav:*") + "</Sink>"));
            return;
        }
        if (!request.starts_with("POST /ctl ")) { Reply(c, 404, {}); return; }
        if (soapAction != "GetPositionInfo") { std::lock_guard lock(mx); actions.push_back(soapAction); }
        if (soapAction == "SetAVTransportURI") {
            if (refuse) {
                Reply(c, 500, "<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>" + std::to_string(refuse) +
                    "</errorCode><errorDescription>Illegal MIME-type</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>");
                return;
            }
            std::lock_guard lock(mx);
            uris.push_back(Unescape(Between(request, "<CurrentURI>", "</CurrentURI>")));
        } else if (soapAction == "Play") {
            std::string url;
            bool fresh;
            {
                std::lock_guard lock(mx);
                url = uris.empty() ? std::string() : uris.back();
                fresh = !playedAt || paused == false;
                if (paused) { pausedFor += GetTickCount64() - pausedAt; paused = false; }
            }
            if (fresh && !url.empty() && (!reader.joinable() || stopReader)) {
                StopReader();
                playedAt = GetTickCount64();
                pausedFor = 0;
                bool early = probe;
                probe = false;
                reader = std::thread([this, url, early] {
                    if (early) Read(url, true);  // a probe: some bytes, then hang up
                    Read(url, false);
                });
            }
        } else if (soapAction == "Pause") {
            paused = true;
            pausedAt = GetTickCount64();
        } else if (soapAction == "Stop") {
            StopReader();
            playedAt = 0;
        } else if (soapAction == "GetPositionInfo") {
            ULONGLONG now = paused ? pausedAt : GetTickCount64();
            long long ms = playedAt ? (long long)(now - playedAt - pausedFor) - 300 : 0;
            if (ms < 0) ms = 0;
            char rel[32];
            snprintf(rel, sizeof(rel), "%lld:%02lld:%02lld.%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
            Reply(c, 200, envelope(soapAction, std::string("<Track>1</Track><RelTime>") + rel + "</RelTime>"));
            return;
        }
        Reply(c, 200, envelope(soapAction, {}));
    }
};

// What the test's speaker stream rendered and was told.
struct Probe {
    std::atomic<uint64_t> rendered{0}, played{0};
    std::atomic<int> failures{0};
    speakers::Callbacks Callbacks() {
        speakers::Callbacks cb;
        cb.render = [this](float* out, uint32_t frames) {
            uint64_t n = rendered;
            for (uint32_t i = 0; i < frames; ++i, ++n) {
                out[i * 2] = Pattern(n) / 32768.f;
                out[i * 2 + 1] = -Pattern(n) / 32768.f;
            }
            rendered = n;
        };
        cb.timing = [this](uint64_t frames) { played = frames; };
        return cb;
    }
};

bool PcmMatches(const std::string& bytes, size_t offset, bool bigEndian, size_t frames) {
    if (bytes.size() < offset + frames * 4) return false;
    for (size_t n = 0; n < frames; ++n) {
        auto sample = [&](size_t at) {
            uint8_t a = (uint8_t)bytes[offset + at], b = (uint8_t)bytes[offset + at + 1];
            return (int16_t)(bigEndian ? (a << 8 | b) : (b << 8 | a));
        };
        if (sample(n * 4) != Pattern(n) || sample(n * 4 + 2) != (int16_t)-Pattern(n)) return false;
    }
    return true;
}
uint32_t Le32(const std::string& b, size_t at) {
    return (uint8_t)b[at] | (uint8_t)b[at + 1] << 8 | (uint8_t)b[at + 2] << 16 | (uint32_t)(uint8_t)b[at + 3] << 24;
}
}  // namespace

void TestSpeakers() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    {
        testlog::Group group(L"speaker descriptions and ids");
        speakers::Speaker s;
        const std::string embedded = "<root><URLBase>http://10.0.0.5:49152/</URLBase><device><friendlyName>Zone</friendlyName>"
            "<UDN>uuid:root-1</UDN><deviceList><device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
            "<friendlyName>Ignored</friendlyName><serviceList><service><serviceType>urn:schemas-upnp-org:service:AVTransport:2"
            "</serviceType><controlURL>MediaRenderer/AVTransport/Control</controlURL></service><service><serviceType>"
            "urn:schemas-upnp-org:service:ConnectionManager:1</serviceType><controlURL>/cm?x=1&amp;y=2</controlURL></service>"
            "</serviceList></device></deviceList></device></root>";
        Check(speakers::ParseDescription(embedded, "http://10.0.0.5:1400/xml/device_description.xml", s) &&
                  s.name == "Zone" && s.udn == "uuid:root-1" &&
                  s.transport == "http://10.0.0.5:49152/MediaRenderer/AVTransport/Control" &&
                  s.transportType == "urn:schemas-upnp-org:service:AVTransport:2" && s.connection == "http://10.0.0.5:49152/cm?x=1&y=2",
              L"an embedded renderer's services resolve against URLBase, keeping the advertised AVTransport version");
        speakers::Speaker t;
        Check(speakers::ParseDescription("<root><device><friendlyName>Den&#233; TV</friendlyName><UDN>uuid:tv</UDN><service>"
                                         "<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><controlURL>ctl"
                                         "</controlURL></service></device></root>", "http://10.0.0.9:8080/dev/desc.xml", t) &&
                  t.transport == "http://10.0.0.9:8080/dev/ctl" && t.name == "Den\xC3\xA9 TV" && t.connection.empty(),
              L"without URLBase a relative control URL resolves beside the description, and character references decode");
        speakers::Speaker server;
        Check(!speakers::ParseDescription("<root><device><friendlyName>NAS</friendlyName><UDN>uuid:nas</UDN><service><serviceType>"
                                          "urn:schemas-upnp-org:service:ContentDirectory:1</serviceType><controlURL>/cd</controlURL>"
                                          "</service></device></root>", "http://10.0.0.2/", server),
              L"a media server without AVTransport is not offered as a speaker");
        speakers::Speaker back;
        Check(speakers::ParseDeviceId(speakers::DeviceId(s), back) && back.udn == s.udn && back.location == s.location &&
                  back.name == "Zone" && !speakers::ParseDeviceId("upnp:uuid:x ftp://host/ Name", back) &&
                  !speakers::IsSpeakerId("alsa_output.pci") && speakers::IsSpeakerId(speakers::DeviceId(s)),
              L"an output id keeps the speaker's identity, address and name, and nothing else passes for one");
    }
    {
        testlog::Group group(L"streaming to a network speaker");
        FakeRenderer fake;
        fake.probe = true;
        if (!Ready(fake.Start(), L"start a loopback renderer")) return;
        Probe probe;
        std::string error;
        const ULONGLONG opened = GetTickCount64();
        auto stream = speakers::Stream::Open("upnp:uuid:fake-renderer " + fake.Location() + " Fake", kRate, true,
                                             probe.Callbacks(), [&] { ++probe.failures; }, error);
        if (!Ready(stream != nullptr, L"open a speaker stream")) return;
        Pump(8000, [&] { return fake.First().size() >= 44 + 4000 * 4; });
        auto actions = fake.Actions();
        auto uris = fake.Uris();
        const std::string first = fake.First();
        Check(actions.size() >= 3 && actions[0] == "Stop" && actions[1] == "SetAVTransportURI" && actions[2] == "Play" &&
                  uris.size() == 1 && uris[0].starts_with("http://127.0.0.1:") && uris[0].ends_with(".wav") &&
                  stream->Name() == "Fake & Speaker",
              L"the speaker is stopped, given a WAV stream on this computer's address toward it, and told to play");
        Check(first.size() >= 44 && first.starts_with("RIFF") && first.substr(8, 8) == "WAVEfmt " && Le32(first, 24) == kRate &&
                  first[22] == 2 && first[34] == 16 && PcmMatches(first, 44, false, 4000),
              L"after a probing reconnect the stream still starts at the beginning: a 16-bit 48 kHz WAV of exactly what was rendered");
        // The speaker reads as fast as it may; rendering runs a few seconds ahead of real time, not unbounded.
        Pump(2500);
        const double elapsed = (GetTickCount64() - opened) / 1000.0;
        const double ahead = (double)probe.rendered / kRate;
        Check(ahead >= 4.0 && ahead <= elapsed + 5.3,
              L"rendering fills the speaker's buffer up to about five seconds ahead of real time and no further");
        // Position follows the speaker's own report (real time since Play), not what was sent.
        Pump(3000, [&] { return probe.played > kRate / 2; });
        const double played = (double)probe.played / kRate, rendered = (double)probe.rendered / kRate;
        Check(probe.played > 0 && played <= rendered && rendered - played >= 3.0,
              L"the position follows what the speaker says it has played, seconds behind what it was sent");
        stream->SetActive(false);
        Pump(2000, [&] { return fake.Count("Pause") == 1; });
        Pump(300);
        const uint64_t heldAt = probe.rendered;
        Pump(400);
        Check(fake.Count("Pause") == 1 && probe.rendered == heldAt, L"pausing pauses the speaker and stops rendering");
        stream->SetActive(true);
        Pump(2000, [&] { return fake.Count("Play") == 2; });
        Pump(3000, [&] { return probe.rendered > heldAt; });
        Check(fake.Count("Play") == 2 && probe.rendered > heldAt, L"resuming tells the speaker to play and rendering carries on");
        const uint64_t beforeFlush = probe.rendered;
        stream->Flush();
        Pump(6000, [&] { return fake.Uris().size() == 2 && fake.read > 44; });
        uris = fake.Uris();
        Check(uris.size() == 2 && uris[1] != uris[0] && Status(uris[0]) == 404 && Status(uris[1] + "x") == 404 &&
                  probe.played >= beforeFlush,
              L"a flush gives the speaker a fresh stream under a new address, the old one is gone, and what it held counts as passed");
        Check(probe.failures == 0 && !stream->Failed(), L"a working speaker never reports a failure");
        const size_t stops = fake.Count("Stop");
        stream.reset();
        Pump(3000, [&] { return fake.Count("Stop") > stops; });
        Check(fake.Count("Stop") > stops, L"closing the stream stops the speaker");
    }
    {
        testlog::Group group(L"speakers that take LPCM, refuse, or aren't there");
        FakeRenderer lpcm;
        lpcm.lpcm = true;
        if (!Ready(lpcm.Start(), L"start a loopback LPCM renderer")) return;
        Probe probe;
        std::string error;
        auto stream = speakers::Stream::Open("upnp:uuid:fake-renderer " + lpcm.Location() + " Fake", kRate, true,
                                             probe.Callbacks(), {}, error);
        Pump(8000, [&] { return lpcm.First().size() >= 4000 * 4; });
        auto uris = lpcm.Uris();
        Check(uris.size() == 1 && uris[0].ends_with(".pcm") && !lpcm.First().starts_with("RIFF") && PcmMatches(lpcm.First(), 0, true, 4000),
              L"a speaker that lists only LPCM gets headerless big-endian 16-bit samples");
        stream.reset();

        FakeRenderer refusing;
        refusing.refuse = 714;
        if (!Ready(refusing.Start(), L"start a refusing renderer")) return;
        Probe refused;
        stream = speakers::Stream::Open("upnp:uuid:fake-renderer " + refusing.Location() + " Fake", kRate, true,
                                        refused.Callbacks(), [&] { ++refused.failures; }, error);
        Pump(6000, [&] { return stream->Failed(); });
        Check(stream->Failed() && refused.failures == 1 && stream->Problem().find("714") != std::string::npos &&
                  stream->Problem().find("refused") != std::string::npos && refused.rendered == 0,
              L"a speaker that refuses the stream fails once, saying its UPnP error, without rendering anything");
        stream.reset();

        // Nothing listens on port 9 here: the description fails, and the network search can't find the id.
        Probe missing;
        stream = speakers::Stream::Open("upnp:uuid:cordinal-test-missing http://127.0.0.1:9/desc.xml Kitchen", kRate, true,
                                        missing.Callbacks(), [&] { ++missing.failures; }, error);
        Pump(9000, [&] { return stream->Failed(); });
        Check(stream->Failed() && missing.failures == 1 && stream->Problem().find("Kitchen isn't answering") != std::string::npos,
              L"a speaker that can't be found fails with its name and what to check");
        Check(!speakers::Stream::Open("alsa_output.pci", kRate, true, missing.Callbacks(), {}, error) && !error.empty(),
              L"an id that isn't a speaker's is refused at once");
    }
}
