// Discord status over the Discord app's local IPC: the activity worked out on the UI thread from what plays,
// and a worker that owns the pipe (Windows) or Unix socket (Linux), sends only changes and keeps within
// Discord's limit of five updates in twenty seconds. The connection is held only while something is shown.
#include "discord.h"

#include "coverupload.h"
#include "customcover.h"
#include "library.h"
#include "metadata_fetch.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace discord {
namespace {

// The Discord application whose name reads "Listening to cordinal". An application id is public, not a secret.
// Empty: the feature stays hidden and the status is never sent.
constexpr char kClientId[] = "1567896417932935351";
// The cover beside the status: the public repository's icon, which Discord fetches and caches itself, so the
// application needs no uploaded art.
constexpr char kImage[] = "https://raw.githubusercontent.com/imhavingasuperbadday/cordinal/main/docs/icon.png";
bool g_testing = false;  // UI thread: a self-test's fake Discord stands in for the missing id
constexpr char kTestClientId[] = "1";

// What Discord accepts as activity art: an https address of at most 256 characters with nothing in it that
// would need escaping.
bool UsableCover(std::string_view url) {
    return url.size() > 8 && url.size() <= 256 && url.starts_with("https://") &&
           url.find_first_of("\"\\ \t\r\n") == std::string_view::npos;
}

using Clock = std::chrono::steady_clock;

// UI thread.
Show g_mode = Show::Off;
NowPlaying g_last;
std::string g_albumKey, g_shown;
int64_t g_albumStart = 0, g_pausedAt = 0, g_songStart = 0;

// Shared with the worker.
std::mutex g_mu;
std::condition_variable g_cv;
std::thread g_worker;
std::string g_want, g_endpoint;  // guarded by g_mu
uint64_t g_version = 0;          // guarded by g_mu
bool g_stop = false;             // guarded by g_mu
std::atomic<bool> g_stopping{false};  // read inside the worker's waits for a reply
std::atomic<uint32_t> g_sent{0};

int64_t UnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void AppendString(std::string& out, std::string_view s) {
    // Discord takes 2 to 128 characters; bytes never undercount characters, so 128 bytes is always within it.
    size_t cut = s.size();
    if (cut > 128) {
        cut = 128;
        while (cut && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;
    }
    out += '"';
    for (unsigned char c : s.substr(0, cut)) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else out += (char)c;
    }
    if (cut == 1) out += "\\u200b";  // a one-letter title still meets the two-character minimum
    out += '"';
}

void Field(std::string& out, const char* key, std::string_view value) {
    if (value.empty()) return;
    if (out.back() != '{') out += ',';
    out += '"';
    out += key;
    out += "\":";
    AppendString(out, value);
}

// ---------------------------------------------------------------------------
// Transport: one connection to the first Discord endpoint that answers.

#ifdef _WIN32
struct Conn {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool IsOpen() const { return h != INVALID_HANDLE_VALUE; }
    bool Open(const std::string& endpoint) {
        std::string base = endpoint.empty() ? "\\\\.\\pipe\\discord-ipc-" : endpoint;
        for (int i = 0; i < 10; ++i) {
            std::wstring name(base.begin(), base.end());
            name += (wchar_t)(L'0' + i);
            h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (IsOpen()) return true;
        }
        return false;
    }
    void Close() {
        if (IsOpen()) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    bool Write(const std::string& bytes) {
        DWORD n = 0;
        return WriteFile(h, bytes.data(), (DWORD)bytes.size(), &n, nullptr) && n == bytes.size();
    }
    // Bytes ready to read without blocking, or -1 once the pipe is broken.
    long Available() {
        DWORD n = 0;
        return PeekNamedPipe(h, nullptr, 0, nullptr, &n, nullptr) ? (long)n : -1;
    }
    bool ReadSome(char* p, size_t n, size_t& got) {
        DWORD read = 0;
        if (!ReadFile(h, p, (DWORD)n, &read, nullptr)) return false;
        got = read;
        return read > 0;
    }
};
uint32_t ProcessId() { return GetCurrentProcessId(); }
#else
struct Conn {
    int fd = -1;
    bool IsOpen() const { return fd >= 0; }
    bool Connect(const std::string& path) {
        sockaddr_un addr{};
        if (path.size() >= sizeof(addr.sun_path)) return false;
        addr.sun_family = AF_UNIX;
        memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0) {
            timeval tv{2, 0};  // a stalled Discord must not hold the worker past Stop for long
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            return true;
        }
        Close();
        return false;
    }
    // Discord's own folders, then the Flatpak and Snap sandboxes it may run in.
    bool Open(const std::string& endpoint) {
        auto tryBase = [&](const std::string& base) {
            for (int i = 0; i < 10; ++i)
                if (Connect(base + (char)('0' + i))) return true;
            return false;
        };
        if (!endpoint.empty()) return tryBase(endpoint);
        for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
            const char* dir = getenv(var);
            if (!dir || !*dir) continue;
            for (const char* sub : {"/", "/app/com.discordapp.Discord/", "/snap.discord/", "/.flatpak/dev.vencord.Vesktop/xdg-run/"})
                if (tryBase(std::string(dir) + sub + "discord-ipc-")) return true;
        }
        return tryBase("/tmp/discord-ipc-");
    }
    void Close() {
        if (fd >= 0) close(fd);
        fd = -1;
    }
    bool Write(const std::string& bytes) {
        size_t done = 0;
        while (done < bytes.size()) {
            ssize_t n = send(fd, bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
            if (n <= 0) return false;
            done += (size_t)n;
        }
        return true;
    }
    long Available() {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 0) < 0) return -1;
        if (!(p.revents & POLLIN)) return p.revents & (POLLERR | POLLHUP | POLLNVAL) ? -1 : 0;
        int n = 0;
        if (ioctl(fd, FIONREAD, &n) < 0 || n <= 0) return -1;  // readable with nothing to read: Discord closed
        return n;
    }
    bool ReadSome(char* p, size_t n, size_t& got) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return false;
        got = (size_t)r;
        return true;
    }
};
uint32_t ProcessId() { return (uint32_t)getpid(); }
#endif

// Waits up to `ms` for exactly n bytes. Replies come within milliseconds; the wait only covers a busy Discord.
bool ReadExact(Conn& c, char* p, size_t n, int ms) {
    auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (n) {
        long ready = c.Available();
        if (ready < 0) return false;
        if (!ready) {
            if (g_stopping || Clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            continue;
        }
        size_t got = 0;
        if (!c.ReadSome(p, std::min(n, (size_t)ready), got)) return false;
        p += got;
        n -= got;
    }
    return true;
}

bool ReadFrame(Conn& c, uint32_t& op, std::string& body, int ms) {
    char header[8];
    if (!ReadExact(c, header, 8, ms)) return false;
    uint32_t length = 0;
    memcpy(&op, header, 4);
    memcpy(&length, header + 4, 4);
    if (length > 1 << 20) return false;
    body.resize(length);
    return !length || ReadExact(c, body.data(), length, ms);
}

// Reads the reply to what was just sent, answering pings on the way. False when Discord closed the connection
// (an unknown application id, or Discord quitting).
bool Reply(Conn& c, std::string& body) {
    for (;;) {
        uint32_t op = 0;
        if (!ReadFrame(c, op, body, 3000)) return false;
        if (op == 3) {
            if (!c.Write(Frame(4, body))) return false;
            continue;
        }
        return op == 1;
    }
}

// Takes whatever Discord sent unasked (pings) so the pipe never fills while the connection idles.
bool Drain(Conn& c) {
    for (;;) {
        long ready = c.Available();
        if (ready < 0) return false;
        if (ready < 8) return true;
        uint32_t op = 0;
        std::string body;
        if (!ReadFrame(c, op, body, 1000)) return false;
        if (op == 2) return false;
        if (op == 3 && !c.Write(Frame(4, body))) return false;
    }
}

bool Handshake(Conn& c, const std::string& endpoint) {
    const char* id = endpoint.empty() ? kClientId : kTestClientId;
    if (!*id || !c.Open(endpoint)) return false;
    std::string body;
    return c.Write(Frame(0, std::string("{\"v\":1,\"client_id\":\"") + id + "\"}")) && Reply(c, body);
}

bool SetActivity(Conn& c, const std::string& activity) {
    static uint32_t nonce = 0;
    std::string json = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(ProcessId());
    if (!activity.empty()) json += ",\"activity\":" + activity;  // without one, Discord clears the status
    json += "},\"nonce\":\"" + std::to_string(++nonce) + "\"}";
    std::string body;
    if (!Drain(c) || !c.Write(Frame(1, json)) || !Reply(c, body)) return false;
    if (body.find("\"evt\":\"ERROR\"") == std::string::npos) ++g_sent;  // a refused field still leaves the link up
    return true;
}

void Worker() {
    Conn conn;
    uint64_t sent = 0;
    Clock::time_point recent[5]{};  // the last five sends, oldest first: Discord allows five per twenty seconds
    Clock::time_point retryAt{};
    int retrySeconds = 15;
    std::unique_lock lock(g_mu);
    while (!g_stop) {
        if (g_version == sent) {
            g_cv.wait(lock);
            continue;
        }
        const std::string want = g_want, endpoint = g_endpoint;
        const uint64_t version = g_version;
        Clock::time_point now = Clock::now();
        if (want.empty() && !conn.IsOpen()) {  // nothing shown and nothing to clear
            sent = version;
            continue;
        }
        if (!conn.IsOpen() && now < retryAt) {
            g_cv.wait_until(lock, retryAt);
            continue;
        }
        if (recent[0] != Clock::time_point{} && now < recent[0] + std::chrono::seconds(20)) {
            g_cv.wait_until(lock, recent[0] + std::chrono::seconds(20));
            continue;
        }
        lock.unlock();
        bool ok = conn.IsOpen() || Handshake(conn, endpoint);
        ok = ok && SetActivity(conn, want);
        if (!ok || want.empty()) conn.Close();  // cleared: let go of Discord until there is something to show
        lock.lock();
        if (ok) {
            sent = version;
            std::move(recent + 1, recent + 5, recent);
            recent[4] = Clock::now();
            retrySeconds = 15;
        } else {
            retryAt = Clock::now() + std::chrono::seconds(retrySeconds);  // Discord closed: try again later, slowly
            retrySeconds = std::min(retrySeconds * 2, 120);
        }
    }
    lock.unlock();
    if (conn.IsOpen()) {
        g_stopping = false;  // one last, quick clear on the way out
        SetActivity(conn, {});
        conn.Close();
    }
}

void Want(const std::string& activity) {
    if (activity == g_shown) return;
    g_shown = activity;
    std::lock_guard lock(g_mu);
    g_want = activity;
    ++g_version;
    if (!g_worker.joinable()) {
        if (g_want.empty()) return;  // never started and nothing to show: stay idle
        g_stop = false;
        g_stopping = false;
        g_worker = std::thread(Worker);
    }
    g_cv.notify_all();
}

// --- album covers (UI thread) ---

struct Lookup {
    std::string key;
    online::CoverQuery query;
    std::string release;
    std::vector<online::CoverSource> sources;
    online::Cancel cancel;
    std::string url;  // written by the thread before `done`
    std::atomic<bool> done{false};
    std::thread thread;
};
std::unique_ptr<Lookup> g_lookup;              // at most one at a time
std::map<std::string, std::string> g_covers;  // album and sources -> address ("" when none), this session

// --- covers the user's own upload tool has to name (UI thread) ---

// An image that has no address of its own can still get one from the command in settings.ini: the tool is handed
// the image's path and prints the address it uploaded it to. One run at a time, remembered for the session and
// kept beside the cover so the next start does not run it again.
struct Upload {
    std::wstring image;
    std::string url;  // written by the thread before `done`
    std::atomic<bool> done{false};
    std::thread thread;
};
std::unique_ptr<Upload> g_upload;
std::map<std::wstring, std::string> g_uploaded;  // image -> address ("" when the tool gave none), this session

std::string UploadedCover(const std::wstring& image) {
    if (image.empty() || !coverupload::Configured() || g_testing) return {};
    if (g_upload && g_upload->done) {
        g_upload->thread.join();
        if (g_uploaded.size() >= 256) g_uploaded.clear();  // a session's worth of images
        g_uploaded[g_upload->image] = g_upload->url;
        if (!g_upload->url.empty()) customcover::RememberImageUrl(g_upload->image, g_upload->url);
        const bool finished = g_upload->image == image;
        const std::string url = std::move(g_upload->url);
        g_upload.reset();
        if (finished) return url;
    }
    if (g_uploaded.contains(image)) return g_uploaded[image];
    if (!g_upload) {  // a tool that fails is not asked again for this image this session
        g_upload = std::make_unique<Upload>();
        Upload* u = g_upload.get();
        u->image = image;
        u->thread = std::thread([u] {
            u->url = coverupload::Run(u->image);
            u->done = true;
        });
    }
    return {};
}

// The album's cover address once known; empty meanwhile, and for radio, loose tracks or no cover source. A lookup
// for an album no longer playing is cancelled, and the next call after it ends starts the current one.
std::string CoverFor(const NowPlaying& now) {
    // The cover chosen in cordinal wins: when it was downloaded, its own address is the exact picture the player
    // shows and Discord renders it directly. An image picked from a file has no address to offer, so the album's
    // online cover stands in for it.
    if (now.radio) return {};
    std::string chosen = now.path.empty() ? std::string() : ToUtf8(customcover::SourceUrlFor(now.path));
    if (chosen.empty() && !now.path.empty()) chosen = UploadedCover(customcover::ImageFor(now.path));
    if (const std::string preferred = PreferredCover(chosen, {}); !preferred.empty()) return preferred;
    // A track with no album is looked up by its title, and the song's album cover shown.
    if ((now.album.empty() && now.title.empty()) || !online::AnyCoverSource() || g_testing) return {};
    auto sources = online::CoverOrder();
    online::CoverQuery query{now.album.empty() || now.albumArtist.empty() ? now.artist : now.albumArtist, now.album,
                             now.album.empty() ? now.title : std::string()};
    std::string key = query.artist + '\x1f' + query.album + query.title + '\x1f' + now.release + '\x1f';
    for (auto s : sources) key += (char)('0' + (int)s);
    if (g_lookup && g_lookup->done) {
        g_lookup->thread.join();
        if (!g_lookup->cancel.Stopped()) {
            if (g_covers.size() >= 256) g_covers.clear();  // a session's worth; the online cache keeps the rest
            g_covers[g_lookup->key] = std::move(g_lookup->url);
        }
        g_lookup.reset();
    }
    // The found address is the fallback for a chosen cover whose own address could not be used.
    if (auto it = g_covers.find(key); it != g_covers.end()) return PreferredCover(chosen, it->second);
    if (!g_lookup) {
        g_lookup = std::make_unique<Lookup>();
        Lookup* l = g_lookup.get();
        l->key = std::move(key);
        l->query = std::move(query);
        l->release = now.release;
        l->sources = std::move(sources);
        l->thread = std::thread([l] {
            l->url = online::FindCoverUrl(l->query, l->release, l->sources, l->cancel);
            l->done = true;
        });
    } else if (g_lookup->key != key) {
        g_lookup->cancel.Stop();
    }
    return {};
}

}  // namespace

std::string Frame(uint32_t opcode, std::string_view json) {
    std::string frame(8, '\0');
    uint32_t length = (uint32_t)json.size();
    memcpy(frame.data(), &opcode, 4);
    memcpy(frame.data() + 4, &length, 4);
    frame += json;
    return frame;
}

std::string ActivityJson(const NowPlaying& now, Show mode, int64_t nowMs, int64_t albumStartMs, std::string_view cover) {
    if (mode == Show::Off || !now.active || now.paused) return {};
    const double rate = now.rate > 0 ? now.rate : 1;
    std::string_view details, state, hover;
    int64_t start = 0, end = 0;
    if (now.radio) {  // the stream's song by the station, or just the station
        details = now.title.empty() ? now.artist : now.title;
        state = now.title.empty() ? std::string_view{} : now.artist;
        hover = now.artist;
    } else if (mode == Show::Album && !now.album.empty()) {
        details = now.album;
        state = now.albumArtist.empty() ? now.artist : now.albumArtist;
        hover = now.album;
        start = albumStartMs;
    } else {
        details = now.title;
        state = now.artist;
        hover = now.album;
        start = nowMs - (int64_t)(now.position * 1000 / rate);
        if (now.duration > now.position) end = start + (int64_t)(now.duration * 1000 / rate);
    }
    if (details.empty()) return {};
    std::string json = "{\"type\":2";
    Field(json, "details", details);
    Field(json, "state", state);
    // Discord takes image addresses up to 256 characters; one it cannot take falls back to the icon.
    const bool useCover = UsableCover(cover);
    json += ",\"assets\":{\"large_image\":\"";
    json += useCover ? cover : std::string_view(kImage);
    json += '"';
    Field(json, "large_text", hover);
    if (useCover) {
        json += ",\"small_image\":\"";
        json += kImage;
        json += "\",\"small_text\":\"cordinal\"";
    }
    json += '}';
    if (start > 0) {
        json += ",\"timestamps\":{\"start\":" + std::to_string(start);
        if (end > start) json += ",\"end\":" + std::to_string(end);
        json += '}';
    }
    json += ",\"instance\":false}";
    return json;
}

std::string PreferredCover(std::string_view customUrl, std::string_view onlineUrl) {
    if (UsableCover(customUrl)) return std::string(customUrl);
    return UsableCover(onlineUrl) ? std::string(onlineUrl) : std::string();
}

bool Available() { return kClientId[0] || g_testing; }

Show Mode() { return g_mode; }

void SetMode(Show mode) {
    g_mode = mode;
    g_albumKey.clear();  // a new mode starts its clock afresh
    g_songStart = 0;
    Update(g_last);
}

void Update(const NowPlaying& now) {
    g_last = now;
    int64_t nowMs = UnixMs();
    const double rate = now.rate > 0 ? now.rate : 1;
    const int64_t elapsed = (int64_t)(now.position * 1000 / rate);
    if (!now.active) {
        g_albumKey.clear();
        g_pausedAt = 0;
    } else if (g_mode == Show::Album && !now.radio && !now.album.empty()) {
        std::string key = now.album + '\x1f' + now.albumArtist;
        if (key != g_albumKey) {
            g_albumKey = std::move(key);
            g_albumStart = nowMs - elapsed;
            g_pausedAt = 0;
        }
    }
    // The album's clock stands still while paused.
    if (now.active && now.paused) {
        if (!g_pausedAt) g_pausedAt = nowMs;
    } else if (g_pausedAt) {
        g_albumStart += nowMs - g_pausedAt;
        g_pausedAt = 0;
    }
    // A song's start time is sampled from the position each call; hold it steady unless it moved by more than a
    // moment (a seek), so the same song never resends.
    int64_t start = nowMs - elapsed;
    if (g_songStart && std::llabs(start - g_songStart) < 1500) nowMs += g_songStart - start;
    else g_songStart = start;
    const bool shown = Available() && g_mode != Show::Off && now.active && !now.paused;
    Want(shown ? ActivityJson(now, g_mode, nowMs, g_albumStart, CoverFor(now)) : std::string());
}

void Stop() {
    if (g_lookup) {
        g_lookup->cancel.Stop();
        g_lookup->thread.join();
        g_lookup.reset();
    }
    if (g_upload) {
        coverupload::CancelAll();  // a stuck tool must not hold the player open
        g_upload->thread.join();
        g_upload.reset();
    }
    g_uploaded.clear();
    {
        std::lock_guard lock(g_mu);
        if (!g_worker.joinable()) return;
        g_stop = true;
        g_stopping = true;
        g_want.clear();
        ++g_version;
    }
    g_cv.notify_all();
    g_worker.join();
    std::lock_guard lock(g_mu);
    g_worker = std::thread();
    g_stop = false;
    g_shown.clear();
}

void SetEndpointForTest(const std::string& endpoint) {
    g_testing = !endpoint.empty();
    std::lock_guard lock(g_mu);
    g_endpoint = endpoint;
    g_sent = 0;
}

uint32_t SentForTest() { return g_sent; }

}  // namespace discord
