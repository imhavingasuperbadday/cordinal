// Native ListenBrainz, Last.fm and Libre.fm submission: queues finished listens on disk and sends
// them from workers. The UI thread only queues work and reads status; nothing here blocks it on the
// network. Shared state is guarded by g_mu except where a comment says otherwise.
#include "listenbrainz.h"

#include "library.h"
#include "md5.h"
#include "web_remote.h"
#include <atomic>
#include <map>

#include <windows.h>
#include <shlobj.h>
#ifdef _WIN32
#include <wincrypt.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <ctime>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace listenbrainz {
static std::wstring Trim(const std::wstring& s) {
    size_t first = s.find_first_not_of(L" \t\r\n");
    return first == s.npos ? L"" : s.substr(first, s.find_last_not_of(L" \t\r\n") - first + 1);
}
std::string FormEscape(std::string_view value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
std::string SignedForm(const std::map<std::string, std::string>& params, const std::string& secret) {
    std::string signature, body;
    for (const auto& [key, value] : params) {
        if (key == "format" || key == "callback" || key == "api_sig") continue;
        signature += key + value;
        if (!body.empty()) body += '&';
        body += FormEscape(key) + '=' + FormEscape(value);
    }
    signature += secret;
    Md5 md5; md5.Add(signature.data(), signature.size());
    std::string hex;
    for (uint8_t byte : md5.Finish()) { char b[3]; snprintf(b, sizeof(b), "%02x", byte); hex += b; }
    return body + "&api_sig=" + hex + "&format=json";
}
unsigned SubmissionStatus(unsigned http, const std::string& body) {
    web::JsonValue reply;
    if (!web::ParseJson(body, reply)) return http == 200 ? 503 : http;
    unsigned error = (unsigned)strtoul(reply["error"].text.c_str(), nullptr, 10);
    if (error == 9 || error == 4 || error == 10 || error == 13 || error == 26) return 401;
    if (error == 29) return 429;
    if (error == 11 || error == 16) return 503;
    if (error) return 400;
    if (http == 200 && reply["scrobbles"].kind == web::JsonValue::Kind::Null &&
        reply["nowplaying"].kind == web::JsonValue::Kind::Null) return 503;
    return http;
}
std::wstring AuthorizationUrl(Service s, const std::wstring& key, const std::wstring& token) {
    return (s == Service::LastFm ? L"https://www.last.fm/api/auth/?" : L"https://libre.fm/api/auth/?") +
        FromUtf8("api_key=" + FormEscape(ToUtf8(Trim(key))) + "&token=" + FormEscape(ToUtf8(Trim(token))));
}
namespace {

// Monotonic clock for retry scheduling, immune to wall-clock changes.
using Clock = std::chrono::steady_clock;

const wchar_t kUserAgent[] = L"cordinal/0.1.9";
const char kClientVersion[] = "0.1.9";

#ifdef _WIN32
const wchar_t kSep[] = L"\\";
#else
const wchar_t kSep[] = L"/";
#endif

constexpr size_t kMaxBatch = 100;
constexpr size_t kMaxQueueFileBytes = 64u << 20;
constexpr DWORD kRetryDefaultSeconds = 60;
constexpr DWORD kRetryMaxSeconds = 3600;
constexpr DWORD kBackoffFirstMs = 30 * 1000;
constexpr DWORD kBackoffMaxMs = 30 * 60 * 1000;

// ---------------------------------------------------------------------------
// JSON

void AppendEscaped(std::string& out, std::string_view text) {
    // A badly tagged file can hold invalid UTF-8, which would make the whole request invalid
    // JSON; the round trip turns bad sequences into U+FFFD.
    std::string s = ToUtf8(FromUtf8(text));
    out += '"';
    for (unsigned char c : s) {
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += (char)c;  // UTF-8 bytes pass through unchanged
        }
    }
    out += '"';
}

std::wstring FileStem(Service s) {
    return s == Service::ListenBrainz ? L"listenbrainz" : s == Service::LastFm ? L"lastfm" : L"librefm";
}
std::wstring CredentialUser(const std::wstring& token) {
    web::JsonValue json;
    return web::ParseJson(ToUtf8(token), json) ? FromUtf8(json.String("user")) : L"";
}
std::string AccountFingerprint(const std::wstring& token, const std::wstring& user) {
    std::string identity = user.empty() ? "token:" + ToUtf8(token) : "user:" + ToUtf8(user);
    Md5 hash; hash.Add(identity.data(), identity.size());
    std::string owner;
    for (uint8_t b : hash.Finish()) { char hex[3]; snprintf(hex, sizeof(hex), "%02x", b); owner += hex; }
    return owner;
}
std::atomic<bool> g_paused{false}, g_sendNow{true};
HttpForTest g_httpForTest = nullptr;

struct Client {
    Service service = Service::ListenBrainz;
    // Guarded by g_mu, except where noted.
    std::mutex g_mu;
    std::condition_variable g_cv;
    std::thread g_worker;                    // joinable between Start and Stop
    bool g_stop = false;
    std::wstring g_dir;                      // set by Start, unchanged while running
    HWND g_notify = nullptr;
    UINT g_msg = 0;
    std::wstring g_token;                    // empty = off
    bool g_rejected = false;                 // the server refused the token
    size_t g_oneByOne = 0;                   // front listens to send singly after a refused batch
    std::vector<std::string> g_queue;        // mirrors the queue file, oldest first
    std::string queueOwner;                 // account fingerprint; never send a queue to a different account
    std::string g_nowPlaying;
    bool g_nowPlayingPending = false;
    Clock::time_point g_due;                 // next time the worker may send
    DWORD g_backoffMs = 0;
    Status g_status;
    std::atomic<bool> cancel{false};
#ifdef _WIN32
    HINTERNET session = nullptr;
#endif
    std::wstring authKey, authSecret, authToken;
    std::atomic<unsigned> generation{0};
    int authJob = 0;  // 1: validate LB, 2: request browser token, 3: exchange approved token
    std::wstring validateToken;
    std::wstring validatedToken, validatedUser;
    bool g_sendingDisabled = false;

    // Wall-clock seconds since the epoch, for listen timestamps.
    uint32_t NowUnix() {
        return (uint32_t)time(nullptr);
    }

    // Linux posts to the one UI thread's queue, whose "window" is null; Windows needs the notify window.
#ifdef _WIN32
    bool CanNotify() { return g_notify != nullptr; }
#else
    bool CanNotify() { return g_msg != 0; }
#endif

    void PostStatus() {  // g_mu held
        if (CanNotify()) PostMessageW(g_notify, g_msg, (unsigned)service * 16 + kStatusChanged, 0);
    }

    // Each service has one sender, a separate durable queue and a cancellable native HTTPS session.
    using Response = web::HttpReply;
    Response Http(const char* method, const char* path, const std::wstring& token, const std::string& body) {
        std::string host = service == Service::ListenBrainz ? "api.listenbrainz.org" :
                           service == Service::LastFm ? "ws.audioscrobbler.com" : "libre.fm";
        std::string headers = service == Service::ListenBrainz ?
            "Authorization: Token " + ToUtf8(token) + "\r\nContent-Type: application/json\r\n" :
            "Content-Type: application/x-www-form-urlencoded\r\n";
        if (token.find_first_of(L"\r\n") != token.npos && service == Service::ListenBrainz) return {};
        if (g_httpForTest) return g_httpForTest(service, "https://" + host + path, method, body, headers);
#ifdef _WIN32
        if (!session || cancel) return {};
        return web::HttpFetch(session, "https://" + host + path, method, body, headers, 10000);
#else
        return web::HttpFetch(&cancel, "https://" + host + path, method, body, headers, 10000);
#endif
    }
    Response SendListens(const std::wstring& token, const char* type, const std::string& payload) {
        if (service == Service::ListenBrainz) {
            std::string body = std::string("{\"listen_type\":\"") + type + "\",\"payload\":" + payload + "}";
            Response r = Http("POST", "/1/submit-listens", token, body);
            web::JsonValue reply;
            if (r.status == 200 && (!web::ParseJson(r.body, reply) || reply.String("status") != "ok")) r.status = 503;
            return r;
        }
        web::JsonValue credentials, listens;
        if (!web::ParseJson(ToUtf8(token), credentials) || !web::ParseJson(payload, listens)) return {};
        bool now = std::string_view(type) == "playing_now";
        std::map<std::string, std::string> params{{"api_key", credentials.String("key")},
            {"sk", credentials.String("session")}, {"method", now ? "track.updateNowPlaying" : "track.scrobble"}};
        for (size_t i = 0; i < listens.items.size(); ++i) {
            const auto& listen = listens.items[i];
            const auto& meta = listen["track_metadata"];
            std::string suffix = now ? "" : "[" + std::to_string(i) + "]";
            params["artist" + suffix] = meta.String("artist_name");
            params["track" + suffix] = meta.String("track_name");
            if (!now) params["timestamp" + suffix] = listen["listened_at"].text;
            if (!meta.String("release_name").empty()) params["album" + suffix] = meta.String("release_name");
            const auto& info = meta["additional_info"];
            if (!info.String("release_artist_name").empty()) params["albumArtist" + suffix] = info.String("release_artist_name");
            if (!info["duration_ms"].text.empty()) params["duration" + suffix] =
                std::to_string(std::strtoul(info["duration_ms"].text.c_str(), nullptr, 10) / 1000);
            if (!info["tracknumber"].text.empty()) params["trackNumber" + suffix] = info["tracknumber"].text;
        }
        Response r = Http("POST", "/2.0/", L"", SignedForm(params, credentials.String("secret")));
        r.status = SubmissionStatus(r.status, r.body);
        return r;
    }

    // ---------------------------------------------------------------------------
    // Files

    std::wstring QueuePath() {
        return g_dir + kSep + FileStem(service) + L"-queue.txt";
    }

    std::wstring TokenPath() {
        return g_dir + kSep + FileStem(service) + L".token";
    }

    // Reads a file into `out` up to maxBytes; false when missing, unreadable or too large.
    bool ReadWholeFile(const std::wstring& path, std::string& out, size_t maxBytes) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(h, &size) && size.QuadPart <= (LONGLONG)maxBytes;
        if (ok) {
            out.resize((size_t)size.QuadPart);
            DWORD got = 0;
            ok = out.empty() || (ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
        }
        CloseHandle(h);
        return ok;
    }

    // Writes to a temp file and swaps it in, so a crash never leaves a half-written file.
    bool WriteWholeFile(const std::wstring& path, const void* data, size_t size) {
        std::wstring temp = path + L".tmp";
        HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = (size == 0 || (WriteFile(h, data, (DWORD)size, &written, nullptr) && written == size));
        ok = FlushFileBuffers(h) && ok;
        CloseHandle(h);
        if (!ok) {
            DeleteFileW(temp.c_str());
            return false;
        }
        return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    // Adds one line to the end of the queue file and flushes it. g_mu held.
    bool AppendLine(const std::string& line) {
        HANDLE h = CreateFileW(QueuePath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        std::string text = line + "\n";
        DWORD written = 0;
        bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
        ok = FlushFileBuffers(h) && ok;
        CloseHandle(h);
        return ok;
    }

    // Rewrites the queue file from memory. g_mu held.
    bool RewriteQueue() {
        std::string text = queueOwner.empty() ? "" : "#account=" + queueOwner + "\n";
        for (const std::string& line : g_queue) {
            text += line;
            text += '\n';
        }
        return WriteWholeFile(QueuePath(), text.data(), text.size());
    }

    // Reads the queue file. Lines that are not whole JSON objects (a torn write) are dropped.
    void LoadQueue() {  // g_mu held
        g_queue.clear();
        queueOwner.clear();
        std::string bytes;
        if (!ReadWholeFile(QueuePath(), bytes, kMaxQueueFileBytes)) return;
        size_t start = 0;
        while (start < bytes.size()) {
            size_t end = bytes.find('\n', start);
            if (end == std::string::npos) end = bytes.size();
            std::string line = bytes.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("#account=")) queueOwner = line.substr(9);
            if (line.size() > 2 && line.front() == '{' && line.back() == '}') g_queue.push_back(std::move(line));
            start = end + 1;
        }
        // A torn last line would fuse with the next appended listen, so the file is rewritten clean.
        if (!bytes.empty() && bytes.back() != '\n') RewriteQueue();
    }

#ifdef _WIN32
    // The token is DPAPI-encrypted: only this Windows account can read it back.
    bool SaveToken(const std::wstring& token) {
        std::string utf8 = ToUtf8(token);
        DATA_BLOB in{(DWORD)utf8.size(), reinterpret_cast<BYTE*>(utf8.data())};
        DATA_BLOB out{};
        if (!CryptProtectData(&in, L"cordinal scrobbling credentials", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                              &out))
            return false;
        bool ok = WriteWholeFile(TokenPath(), out.pbData, out.cbData);
        LocalFree(out.pbData);
        return ok;
    }

    // Reads and DPAPI-decrypts the stored token; empty when absent or unreadable.
    std::wstring LoadToken() {
        std::string bytes;
        if (!ReadWholeFile(TokenPath(), bytes, 4096)) return {};
        DATA_BLOB in{(DWORD)bytes.size(), reinterpret_cast<BYTE*>(bytes.data())};
        DATA_BLOB out{};
        if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
        std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
        LocalFree(out.pbData);
        return Trim(FromUtf8(plain));
    }
#else
    // The token sits in a file only this user can read (mode 0600), beside the queue.
    bool SaveToken(const std::wstring& token) {
        const std::string path = ToUtf8(TokenPath()), temp = path + ".tmp", utf8 = ToUtf8(token);
        int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return false;
        bool ok = write(fd, utf8.data(), utf8.size()) == (ssize_t)utf8.size() && fsync(fd) == 0;
        close(fd);
        if (ok) ok = rename(temp.c_str(), path.c_str()) == 0;
        if (!ok) unlink(temp.c_str());
        return ok;
    }

    std::wstring LoadToken() {
        std::string bytes;
        if (!ReadWholeFile(TokenPath(), bytes, 4096)) return {};
        return Trim(FromUtf8(bytes));
    }
#endif

    // ---------------------------------------------------------------------------
    // Worker

    // Applies the server's answer to the batch of `sent` listens at the front of the queue. g_mu held.
    void Finish(const Response& r, size_t sent) {
        if (r.status == 200) {
            g_oneByOne -= std::min(g_oneByOne, sent);
            g_queue.erase(g_queue.begin(), g_queue.begin() + sent);
            g_status.lastError = RewriteQueue() ? L"" : L"Could not update the queue file";
            g_status.lastSent = NowUnix();
            g_backoffMs = 0;
            g_due = Clock::now();
        } else if (r.status == 401) {
            g_rejected = true;
            g_status.lastError = L"Token rejected";
        } else if (r.status == 429) {
            long long seconds = std::wcstoll(FromUtf8(r.headers.contains("retry-after") ? r.headers.at("retry-after") :
                r.headers.contains("x-ratelimit-reset-in") ? r.headers.at("x-ratelimit-reset-in") : "").c_str(), nullptr, 10);
            if (seconds <= 0) seconds = kRetryDefaultSeconds;
            seconds = std::min<long long>(seconds, kRetryMaxSeconds);
            g_due = Clock::now() + std::chrono::seconds(seconds);
            g_status.lastError = L"Rate limited, retrying";
        } else if (r.status == 0 || r.status >= 500) {
            g_backoffMs = g_backoffMs == 0 ? kBackoffFirstMs : std::min(g_backoffMs * 2, kBackoffMaxMs);
            g_due = Clock::now() + std::chrono::milliseconds(g_backoffMs);
            g_status.lastError =
                r.status == 0 ? L"Network error, retrying" : L"Server error " + std::to_wstring(r.status) + L", retrying";
        } else if (sent > 1) {
            // A refused batch (400 and friends) may hold a single bad listen: send them one at a time,
            // so only what the server really refuses is dropped.
            g_oneByOne = sent;
        } else {
            // The server will never take this listen. Dropping it keeps the queue from jamming.
            g_oneByOne -= std::min<size_t>(g_oneByOne, 1);
            g_queue.erase(g_queue.begin(), g_queue.begin() + sent);
            RewriteQueue();
            g_status.lastError = L"Listens rejected by the server: HTTP " + std::to_wstring(r.status);
        }
        g_status.queued = g_queue.size();
        PostStatus();
    }

    // The single sender thread: playing-now first, then queued listens once the retry deadline passes.
    void WorkerLoop() {
        std::unique_lock lock(g_mu);
        while (!g_stop) {
            if (authJob) {
                int job = authJob; authJob = 0;
                unsigned version = generation;
                auto key = authKey, secret = authSecret, approved = authToken, checkToken = validateToken;
                lock.unlock();
                WPARAM result = 2;
                std::wstring value;
                Response r;
                if (job == 1) {
                    r = Http("GET", "/1/validate-token", checkToken, {});
                    web::JsonValue reply;
                    if (r.status == 200 && web::ParseJson(r.body, reply)) {
                        result = reply["valid"].text == "true" ? 1 : 0;
                        if (result == 1) value = FromUtf8(reply.String("user_name"));
                    } else if (r.status == 401 || r.status == 403) result = 0;
                } else {
                    std::map<std::string, std::string> params{{"api_key", ToUtf8(key)},
                        {"method", job == 2 ? "auth.getToken" : "auth.getSession"}};
                    if (job == 3) params["token"] = ToUtf8(approved);
                    r = Http("POST", "/2.0/", L"", SignedForm(params, ToUtf8(secret)));
                    web::JsonValue reply;
                    if (r.status && web::ParseJson(r.body, reply)) {
                        result = 0;
                        if (r.status == 200 && job == 2 && !reply.String("token").empty()) {
                            result = 4; value = FromUtf8(reply.String("token"));
                        } else if (r.status == 200 && job == 3 && !reply["session"].String("key").empty()) {
                            result = 5;
                            std::string stored = "{\"key\":"; AppendEscaped(stored, ToUtf8(key));
                            stored += ",\"secret\":"; AppendEscaped(stored, ToUtf8(secret));
                            stored += ",\"session\":"; AppendEscaped(stored, reply["session"].String("key"));
                            stored += ",\"user\":"; AppendEscaped(stored, reply["session"].String("name"));
                            stored += "}"; value = FromUtf8(stored);
                        }
                    }
                }
                lock.lock();
                if (g_stop) break;
                if (version == generation) {
                    if (result == 1) { validatedToken = checkToken; validatedUser = value; }
                    auto* answer = value.empty() ? nullptr : new std::wstring(value);
                    if (!CanNotify() || !PostMessageW(g_notify, g_msg, ((WPARAM)version << 8) | (unsigned)service * 16 | result,
                                                      (LPARAM)answer))
                        delete answer;
                }
                continue;
            }
            const bool canSend = !g_token.empty() && !g_rejected && !g_sendingDisabled && !g_paused;
            if (canSend && g_nowPlayingPending) {
                std::string json = std::move(g_nowPlaying);
                g_nowPlaying.clear();
                g_nowPlayingPending = false;
                std::wstring token = g_token;
                lock.unlock();
                Response r = SendListens(token, "playing_now", "[" + json + "]");
                lock.lock();
                if (g_stop) break;
                if (g_token != token) continue;
                if (r.status == 401) {
                    g_rejected = true;
                    g_status.lastError = L"Token rejected";
                    PostStatus();
                }
                continue;
            }
            if (canSend && !g_queue.empty() && Clock::now() >= g_due) {
                size_t n = g_oneByOne ? 1 : std::min(service == Service::ListenBrainz ? kMaxBatch : size_t(50), g_queue.size());
                std::string payload = "[";
                for (size_t i = 0; i < n; ++i) {
                    if (i) payload += ',';
                    payload += g_queue[i];
                }
                payload += ']';
                std::wstring token = g_token;
                lock.unlock();
                Response r = SendListens(token, n > 1 ? "import" : "single", payload);
                lock.lock();
                if (g_stop) break;
                if (g_token != token) continue;
                Finish(r, n);
                continue;
            }
            if (canSend && !g_queue.empty())
                g_cv.wait_until(lock, g_due);
            else
                g_cv.wait(lock);
        }
    }

    // Loads the token and queue and starts the worker; a no-op if it is already running.
    void Start(const std::wstring& dataDir, HWND notify, UINT msg) {
        if (g_worker.joinable()) return;
        g_dir = dataDir;
        g_notify = notify;
        g_msg = msg;
        SHCreateDirectoryExW(nullptr, dataDir.c_str(), nullptr);
        std::wstring token = LoadToken();
        {
            std::lock_guard lock(g_mu);
            g_stop = false;
            g_token = token;
            g_rejected = false;
            g_nowPlaying.clear();
            g_nowPlayingPending = false;
            g_backoffMs = 0;
            g_oneByOne = 0;
            g_due = Clock::now();
            g_status = Status{};
            g_status.user = CredentialUser(token);
            LoadQueue();
            if (queueOwner.empty() && !g_queue.empty() && !token.empty()) {
                // Bind queues from before account fingerprints existed to their current credentials.
                queueOwner = AccountFingerprint(token, g_status.user);
                RewriteQueue();
            }
            g_status.queued = g_queue.size();
        }
        cancel = false;
#ifdef _WIN32
        session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
#endif
        g_worker = std::thread([this] { WorkerLoop(); });
    }

    // Cancels requests in flight, then joins the sender.
    void Stop() {
        {
            std::lock_guard lock(g_mu);
            g_stop = true;
            ++generation; authJob = 0;
            cancel = true;
        }
#ifdef _WIN32
        if (session) WinHttpCloseHandle(session);
#endif
        g_cv.notify_all();
        if (g_worker.joinable()) g_worker.join();
#ifdef _WIN32
        session = nullptr;
#endif
    }

    // Trims and stores a new token (or clears it, turning sending off); the queue is left alone.
    bool SetToken(const std::wstring& raw) {
        if (g_dir.empty()) return false;
        std::wstring token = Trim(raw);
        std::lock_guard lock(g_mu);
        std::wstring user = service != Service::ListenBrainz ? CredentialUser(token) :
            token == validatedToken ? validatedUser : token == g_token ? g_status.user : L"";
        std::string owner = AccountFingerprint(token, user);
        if (!token.empty() && token != g_token && !g_queue.empty() && !queueOwner.empty() && queueOwner != owner) {
            g_status.lastError = L"Waiting listens belong to another account. Reconnect that account and send them first";
            PostStatus(); return false;
        }
        bool saved = true;
        if (token.empty())
            saved = DeleteFileW(TokenPath().c_str()) || GetFileAttributesW(TokenPath().c_str()) == INVALID_FILE_ATTRIBUTES;
        else
            saved = SaveToken(token);
        if (!saved) { g_status.lastError = L"Could not save the connection"; PostStatus(); return false; }
        if (!token.empty()) { queueOwner = owner; RewriteQueue(); }
        ++generation; authJob = 0;
        g_token = token;
        g_status.user = token.empty() ? L"" : user;
        g_rejected = false;
        g_backoffMs = 0;
        g_due = Clock::now();
        if (token.empty()) {
            g_nowPlaying.clear();
            g_nowPlayingPending = false;
            g_status.lastError.clear();
        } else {
            g_status.lastError = saved ? L"" : L"Could not save the token";
        }
        g_cv.notify_all();
        PostStatus();
        return true;
    }

    // True while a token is set, i.e. scrobbling is on.
    bool Enabled() {
        std::lock_guard lock(g_mu);
        return !g_token.empty();
    }

    // Replaces the pending now-playing notice; only the newest is ever sent.
    void PlayingNow(const Listen& l) {
        std::string json = ListenJson(l, false);
        std::lock_guard lock(g_mu);
        if (json.empty() || g_token.empty() || g_paused || !g_sendNow) return;
        g_nowPlaying = std::move(json);
        g_nowPlayingPending = true;
        g_cv.notify_all();
    }

    void Submit(const Listen& l) {
        std::string json = ListenJson(l, true);
        std::lock_guard lock(g_mu);
        if (json.empty() || l.listenedAt == 0 || g_paused ||
            (service != Service::ListenBrainz && l.durationMs <= 30000) || g_token.empty() || !g_worker.joinable()) return;
        // Written first: a listen that is only in memory is lost if the app closes.
        if (!AppendLine(json)) { g_status.lastError = L"Could not write the queue file"; PostStatus(); return; }
        g_queue.push_back(std::move(json));
        g_status.queued = g_queue.size();
        g_cv.notify_all();
        PostStatus();
    }

    void Validate(const std::wstring& token) {
        std::lock_guard lock(g_mu);
        ++generation; authJob = 1; validateToken = Trim(token); g_cv.notify_all();
    }
    void Authorize(const std::wstring& key, const std::wstring& secret, const std::wstring& token) {
        std::lock_guard lock(g_mu);
        ++generation; authKey = Trim(key); authSecret = Trim(secret); authToken = Trim(token);
        authJob = token.empty() ? 2 : 3;
        g_cv.notify_all();
    }
    void CancelAuthorization() {
        std::lock_guard lock(g_mu);
        ++generation; authJob = 0;
        authKey.clear(); authSecret.clear(); authToken.clear(); validateToken.clear();
        validatedToken.clear(); validatedUser.clear();
    }

    // A copy of the status, safe to read from the UI thread.
    Status GetStatus() {
        std::lock_guard lock(g_mu);
        return g_status;
    }

    void SetSendingDisabledForTest(bool disabled) {
        std::lock_guard lock(g_mu);
        g_sendingDisabled = disabled;
        g_cv.notify_all();
    }

};
Client clients[3];
Client& For(Service service) { return clients[(unsigned)service < 3 ? (unsigned)service : 0]; }
}  // namespace

std::string ListenJson(const Listen& l, bool withTimestamp) {
    if (l.artist.empty() || l.title.empty()) return {};
    std::string info;
    if (l.durationMs) info += "\"duration_ms\":" + std::to_string(l.durationMs) + ",";
    if (l.trackNumber) info += "\"tracknumber\":" + std::to_string(l.trackNumber) + ",";
    if (!l.releaseArtist.empty()) {
        info += "\"release_artist_name\":";
        AppendEscaped(info, l.releaseArtist);
        info += ",";
    }
    info += std::string("\"media_player\":\"cordinal\",\"submission_client\":\"cordinal\","
                        "\"submission_client_version\":\"") +
            kClientVersion + "\"";

    std::string out = "{";
    if (withTimestamp && l.listenedAt) out += "\"listened_at\":" + std::to_string(l.listenedAt) + ",";
    out += "\"track_metadata\":{\"artist_name\":";
    AppendEscaped(out, l.artist);
    out += ",\"track_name\":";
    AppendEscaped(out, l.title);
    if (!l.release.empty()) {
        out += ",\"release_name\":";
        AppendEscaped(out, l.release);
    }
    out += ",\"additional_info\":{" + info + "}}}";
    return out;
}


void Start(const std::wstring& dir, HWND notify, UINT msg) {
    for (unsigned i = 0; i < 3; ++i) { clients[i].service = (Service)i; clients[i].Start(dir, notify, msg); }
}
void Stop() { for (auto& c : clients) c.Stop(); }
bool SetToken(const std::wstring& token, Service s) { return For(s).SetToken(token); }
bool Enabled(Service s) { return For(s).Enabled(); }
void PlayingNow(const Listen& l) { for (auto& c : clients) c.PlayingNow(l); }
void Submit(const Listen& l) { for (auto& c : clients) c.Submit(l); }
void Validate(const std::wstring& token) { For(Service::ListenBrainz).Validate(token); }
void Authorize(Service s, const std::wstring& key, const std::wstring& secret, const std::wstring& token) {
    if (s != Service::ListenBrainz) For(s).Authorize(key, secret, token);
}
void CancelAuthorization(Service s) { For(s).CancelAuthorization(); }
bool CurrentResult(WPARAM message) {
    if ((message & 15) == kStatusChanged) return true;
    auto& c = For((Service)((message >> 4) & 3));
    return (message >> 8) == c.generation.load();
}
Status GetStatus(Service s) { return For(s).GetStatus(); }
void SetSendingDisabledForTest(bool disabled) { for (auto& c : clients) c.SetSendingDisabledForTest(disabled); }
void SetHttpForTest(HttpForTest request) { g_httpForTest = request; }
bool Paused() { return g_paused; }
void SetPaused(bool paused) {
    g_paused = paused;
    for (auto& c : clients) { std::lock_guard lock(c.g_mu); c.g_nowPlayingPending = false; c.g_cv.notify_all(); }
}
bool SendNowPlaying() { return g_sendNow; }
void SetSendNowPlaying(bool on) {
    g_sendNow = on;
    if (!on) for (auto& c : clients) { std::lock_guard lock(c.g_mu); c.g_nowPlayingPending = false; }
}
}  // namespace listenbrainz
