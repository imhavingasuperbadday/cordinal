// Optional private-network music server. All published data is owned; no network worker reads
// the live library or application. Stop joins every worker before the frontend can disappear.
#pragma once
#include "theme_palette.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace web {

class TlsIdentity;  // web_tls.h

struct Track {
    std::string key, title, artist, album, albumArtist, date, genre, codec, mime;
    std::wstring path;
    uint32_t id = 0, number = 0, disc = 0, plays = 0;
    uint64_t fileSize = 0, fileTime = 0;
    double duration = 0;
    bool library = false, original = false;
};
struct Playlist {
    std::string key, name;
    std::vector<std::string> tracks;
};
struct Snapshot {
    uint64_t revision = 0;
    uint64_t serial = 0;  // assigned by Publish, independent of the browser-visible content revision
    std::vector<Track> tracks;
    std::vector<Playlist> playlists;
    std::unordered_map<std::string, size_t> byKey;
    std::function<void(Snapshot&)> prepare;  // consumes only owned copies, on a server worker
};
struct ListenEvent {
    std::wstring path;
    double seconds = 0;
    bool manual = false;
    uint64_t time = 0;  // unix seconds the play ended, for one played offline; 0 = just now
};
struct Device {
    std::string id, name;
    uint64_t seen = 0;  // seconds since its last request this session, plus one; 0 when not seen yet
    bool away = false;  // that request came from outside the home network
};
struct Status {
    bool running = false;
    uint16_t port = 0;
    unsigned streams = 0;
    std::string error, pairingCode;
    uint64_t pairingSeconds = 0;  // 0 with a code shown: the fixed code, open until five wrong tries
    bool fixedCode = false;       // set even while five wrong tries have locked it
    std::vector<std::string> addresses;
    std::vector<Device> devices;
    std::string away;              // the secure address for away from home, once it has a certificate
    std::vector<uint16_t> ports;   // every port listening now, the main one first
    std::string portError;         // an extra port that couldn't be opened, in words
};
struct Config {
    uint16_t port = 8787;  // 0 selects a loopback-only ephemeral port for the existing test harness.
    std::wstring devicesPath;
    // The tracks each browser downloaded to keep, and the offline plays already counted; empty
    // keeps them in memory only. Plays reported later are credited only against these.
    std::wstring offlinePath;
    std::function<void()> notify;
    std::string_view font;  // embedded bytes, immutable for the server's lifetime
    std::string_view icon;  // the app icon as PNG, for home-screen shortcuts; may be empty
    std::string fixedCode;  // empty: Pair makes a new code each time; checked with CheckFixedCode
    // More names browsers may use, as host or host:port: a reverse proxy's, such as Tailscale Serve's.
    std::vector<std::string> names;
};
// Away from home: what the frontend's remote-access manager gives the running server. Browsers on
// other networks must use HTTPS and pair with a link; the home network keeps plain HTTP and codes.
struct Away {
    std::vector<uint16_t> ports;      // extra ports to listen on (443, 80), best effort
    std::vector<std::string> hosts;   // "host:port" names browsers away from home may use
    std::string url;                  // the address to show and redirect to, ending in '/'
    std::shared_ptr<TlsIdentity> identity;  // HTTPS certificate; null until there is one
    std::vector<std::pair<std::string, std::string>> challenges;  // ACME http-01 token, key authorization
};

// Normalises a code the user typed for Config::fixedCode: spaces and dashes go, and 6 to 12 digits
// remain. Returns null when `code` is usable (empty clears it), otherwise the sentence to show.
const char* CheckFixedCode(std::string_view text, std::string& code);

class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    bool Start(const Config& config);
    bool LoadDevices(const std::wstring& path);  // while stopped, so access can also be revoked offline
    void Stop();
    Status GetStatus() const;
    bool Pair();  // with a fixed code, reopens it and forgives earlier wrong tries
    // A one-time pairing link token, valid for ten minutes from anywhere: the QR code carries it.
    std::string PairingLink();
    void SetFixedCode(const std::string& code);  // applies at once while running; paired browsers stay
    void Revoke(const std::string& device);  // empty: revoke all
    void Publish(std::shared_ptr<Snapshot> snapshot);
    void SetTheme(ThemeMode theme, bool systemDark);
    void SetAway(Away away);  // applies to new connections at once; extra ports open within a moment
    void TreatLoopbackAsAwayForTest(bool away);
    std::vector<ListenEvent> TakeListens();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::string_view Page();
std::string_view Script();
std::string_view Style();
std::string_view ServiceWorker();  // sw.js: the page and kept music without a connection
std::string PhoneAddress(const Status& status);

}  // namespace web
