// ListenBrainz submission: builds the JSON, queues finished listens on disk and sends them from
// a worker thread. The UI thread only queues work and reads status; nothing here blocks it on the
// network. Shared state is guarded by g_mu except where a comment says otherwise.
#include "listenbrainz.h"

#include "library.h"

#include <windows.h>
#include <shlobj.h>
#ifdef _WIN32
#include <wincrypt.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
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
namespace {

// Monotonic clock for retry scheduling, immune to wall-clock changes.
using Clock = std::chrono::steady_clock;

const wchar_t kHost[] = L"api.listenbrainz.org";
const wchar_t kUserAgent[] = L"cordinal/0.1.0";
const char kClientVersion[] = "0.1.0";
const wchar_t kQueueName[] = L"listenbrainz-queue.txt";
#ifdef _WIN32
const wchar_t kSep[] = L"\\";
#else
const wchar_t kSep[] = L"/";
#endif
const wchar_t kTokenName[] = L"listenbrainz.token";
constexpr size_t kMaxBatch = 100;
constexpr size_t kMaxResponseBytes = 1 << 20;
constexpr size_t kMaxQueueFileBytes = 64u << 20;
constexpr DWORD kRetryDefaultSeconds = 60;
constexpr DWORD kRetryMaxSeconds = 3600;
constexpr DWORD kBackoffFirstMs = 30 * 1000;
constexpr DWORD kBackoffMaxMs = 30 * 60 * 1000;

// Guarded by g_mu, except where noted.
std::mutex g_mu;
std::condition_variable g_cv;
std::thread g_worker;                    // joinable between Start and Stop
std::vector<std::thread> g_validators;   // UI thread only; Stop joins them
bool g_stop = false;
std::wstring g_dir;                      // set by Start, unchanged while running
HWND g_notify = nullptr;
UINT g_msg = 0;
std::wstring g_token;                    // empty = off
bool g_rejected = false;                 // the server refused the token
size_t g_oneByOne = 0;                   // front listens to send singly after a refused batch
std::vector<std::string> g_queue;        // mirrors the queue file, oldest first
std::string g_nowPlaying;
bool g_nowPlayingPending = false;
Clock::time_point g_due;                 // next time the worker may send
DWORD g_backoffMs = 0;
Status g_status;
#ifdef _WIN32
std::vector<HINTERNET> g_sessions;       // open WinHTTP sessions; Stop closes them to cancel
#else
std::vector<pid_t> g_children;           // running curl processes; Stop kills them to cancel
#endif
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
    if (CanNotify()) PostMessageW(g_notify, g_msg, kStatusChanged, 0);
}

// Strips leading and trailing ASCII whitespace (tokens are often pasted with stray space).
std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

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

// Appends one code point as UTF-8 (used for \u escapes and surrogate pairs).
void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Position of the value for `key` in a JSON object (the server's replies are flat), or npos.
size_t JsonValueAt(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return std::string::npos;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return std::string::npos;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    return pos < json.size() ? pos : std::string::npos;
}

bool JsonBool(const std::string& json, const char* key) {
    size_t pos = JsonValueAt(json, key);
    return pos != std::string::npos && json.compare(pos, 4, "true") == 0;
}

// The string value for `key`, with JSON escapes decoded; empty when absent.
std::string JsonString(const std::string& json, const char* key) {
    size_t pos = JsonValueAt(json, key);
    if (pos == std::string::npos || json[pos] != '"') return {};
    std::string out;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"') break;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= json.size()) break;
        switch (json[i]) {
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 >= json.size()) return out;
                uint32_t cp = (uint32_t)std::strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < json.size() && json[i + 1] == '\\' && json[i + 2] == 'u') {
                    uint32_t low = (uint32_t)std::strtoul(json.substr(i + 3, 4).c_str(), nullptr, 16);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                AppendUtf8(out, cp);
                break;
            }
            default: out += json[i]; break;  // \" \\ and \/
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// HTTP

struct Response {
    bool network = false;  // no HTTP response (offline, timeout, or Stop() cancelled it)
    DWORD status = 0;
    std::string body;
    std::wstring retryAfter;  // X-RateLimit-Reset-In, seconds
};

#ifdef _WIN32
// Sessions are tracked so Stop() can close them and cancel whatever is in flight.
bool Track(HINTERNET session) {
    std::lock_guard lock(g_mu);
    if (g_stop) return false;
    g_sessions.push_back(session);
    return true;
}

// Closes the session unless Stop() already took it.
void Untrack(HINTERNET session) {
    bool owned = false;
    {
        std::lock_guard lock(g_mu);
        auto it = std::find(g_sessions.begin(), g_sessions.end(), session);
        if (it != g_sessions.end()) {
            g_sessions.erase(it);
            owned = true;
        }
    }
    if (owned) WinHttpCloseHandle(session);
}

// One blocking request on the calling thread (the worker, or a validator thread). Sessions are
// tracked so Stop() can cancel them; closing the session tears down the request too.
Response Http(const wchar_t* method, const wchar_t* path, const std::wstring& token, const std::string* body) {
    Response r;
    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        r.network = true;
        return r;
    }
    if (!Track(session)) {
        WinHttpCloseHandle(session);
        r.network = true;
        return r;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, method, path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    bool ok = false;
    if (request) {
        std::wstring headers = L"Authorization: Token " + token + L"\r\nContent-Type: application/json\r\n";
        DWORD length = body ? (DWORD)body->size() : 0;
        LPVOID data = body ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA;
        ok = WinHttpSendRequest(request, headers.c_str(), (DWORD)-1, data, length, length, 0) &&
             WinHttpReceiveResponse(request, nullptr);
    }
    if (!ok) {
        r.network = true;
    } else {
        DWORD size = sizeof(r.status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &r.status, &size, WINHTTP_NO_HEADER_INDEX);
        wchar_t retry[32] = {};
        DWORD bytes = sizeof(retry) - sizeof(wchar_t);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"X-RateLimit-Reset-In", retry, &bytes,
                                WINHTTP_NO_HEADER_INDEX))
            r.retryAfter = retry;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            if (r.body.size() + available > kMaxResponseBytes) break;
            size_t old = r.body.size();
            r.body.resize(old + available);
            DWORD got = 0;
            if (!WinHttpReadData(request, r.body.data() + old, available, &got)) {
                r.body.resize(old);
                break;
            }
            r.body.resize(old + got);
            if (got == 0) break;
        }
    }
    Untrack(session);  // closing the session closes the connection and request too
    return r;
}

#else

// Linux has no system HTTPS client library to link, so each request runs `curl` (present on every
// desktop install): the request's settings, token included, go over its stdin rather than argv, where
// other users could read them. Children are tracked so Stop() can kill the ones in flight.
bool Track(pid_t pid) {
    std::lock_guard lock(g_mu);
    if (g_stop) return false;
    g_children.push_back(pid);
    return true;
}

void Untrack(pid_t pid) {
    std::lock_guard lock(g_mu);
    std::erase(g_children, pid);
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Splits curl's `-i` output (header blocks, then the body) into the final response's status, the
// rate-limit header and the body. Interim 1xx blocks are skipped.
void ParseResponse(const std::string& raw, Response& r) {
    size_t pos = 0;
    while (raw.compare(pos, 5, "HTTP/") == 0) {
        size_t end = raw.find("\r\n\r\n", pos);
        if (end == std::string::npos) return;
        std::string head = raw.substr(pos, end - pos);
        size_t sp = head.find(' ');
        r.status = sp == std::string::npos ? 0 : (DWORD)std::strtoul(head.c_str() + sp + 1, nullptr, 10);
        pos = end + 4;
        if (r.status >= 200) {
            static const char kHeader[] = "\r\nx-ratelimit-reset-in:";
            size_t h = Lower(head).find(kHeader);
            if (h != std::string::npos) {
                h += sizeof(kHeader) - 1;
                while (h < head.size() && head[h] == ' ') ++h;
                size_t e = head.find("\r\n", h);
                r.retryAfter = FromUtf8(head.substr(h, e == std::string::npos ? std::string::npos : e - h));
            }
            r.body = raw.substr(pos);
            return;
        }
    }
}

Response Http(const wchar_t* method, const wchar_t* path, const std::wstring& token, const std::string* body) {
    Response r;
    r.network = true;
    const std::string utf8Token = ToUtf8(token);
    // A pasted token is a UUID; anything that could break out of a curl config line is not one.
    for (unsigned char c : utf8Token)
        if (c <= 0x20 || c == '"' || c == '\\' || c >= 0x7f) {
            r.network = false;
            r.status = 401;
            return r;
        }
    std::string bodyFile;
    if (body) {  // curl reads the body from a private file: the config on stdin has no room for it
        std::string pattern = ToUtf8(g_dir) + "/listenbrainz-body-XXXXXX";
        int fd = mkstemp(pattern.data());
        if (fd < 0) return r;
        size_t done = 0;
        while (done < body->size()) {
            ssize_t n = write(fd, body->data() + done, body->size() - done);
            if (n <= 0) break;
            done += (size_t)n;
        }
        close(fd);
        bodyFile = pattern;
        if (done != body->size()) {
            unlink(bodyFile.c_str());
            return r;
        }
    }
    std::string config = "url = \"https://" + ToUtf8(kHost) + ToUtf8(path) + "\"\nrequest = \"" + ToUtf8(method) +
                         "\"\nheader = \"Authorization: Token " + utf8Token +
                         "\"\nheader = \"Content-Type: application/json\"\n";
    if (body) config += "data-binary = \"@" + bodyFile + "\"\n";

    auto cleanup = [&] {
        if (!bodyFile.empty()) unlink(bodyFile.c_str());
    };
    int in[2], out[2];
    if (pipe(in) != 0) {
        cleanup();
        return r;
    }
    if (pipe(out) != 0) {
        close(in[0]);
        close(in[1]);
        cleanup();
        return r;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    for (int fd : {in[0], in[1], out[0], out[1]}) posix_spawn_file_actions_addclose(&actions, fd);
    const std::string agent = ToUtf8(kUserAgent);
    const char* argv[] = {"curl", "-sS", "-i", "--connect-timeout", "10", "--max-time", "45", "-A", agent.c_str(), "-K", "-", nullptr};
    pid_t pid = 0;
    int spawned = posix_spawnp(&pid, "curl", &actions, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in[0]);
    close(out[1]);
    if (spawned != 0 || !Track(pid)) {
        if (spawned == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
        close(in[1]);
        close(out[0]);
        cleanup();
        return r;
    }
    signal(SIGPIPE, SIG_IGN);  // curl may exit before reading its settings
    for (size_t done = 0; done < config.size();) {
        ssize_t n = write(in[1], config.data() + done, config.size() - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    close(in[1]);
    std::string raw;
    char buf[4096];
    for (ssize_t n; (n = read(out[0], buf, sizeof(buf))) > 0 && raw.size() <= kMaxResponseBytes;) raw.append(buf, (size_t)n);
    close(out[0]);
    Untrack(pid);  // before reaping it, so Stop() never signals a pid that has been reused
    waitpid(pid, nullptr, 0);
    cleanup();
    r.network = false;
    ParseResponse(raw, r);
    if (r.status == 0) r.network = true;  // curl failed before an answer: offline, no curl, or cancelled
    return r;
}
#endif

// Wraps a payload array in the submit-listens envelope.
Response SendListens(const std::wstring& token, const char* listenType, const std::string& payload) {
    std::string body = std::string("{\"listen_type\":\"") + listenType + "\",\"payload\":" + payload + "}";
    return Http(L"POST", L"/1/submit-listens", token, &body);
}

// ---------------------------------------------------------------------------
// Files

std::wstring QueuePath() {
    return g_dir + kSep + kQueueName;
}

std::wstring TokenPath() {
    return g_dir + kSep + kTokenName;
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
    std::string text;
    for (const std::string& line : g_queue) {
        text += line;
        text += '\n';
    }
    return WriteWholeFile(QueuePath(), text.data(), text.size());
}

// Reads the queue file. Lines that are not whole JSON objects (a torn write) are dropped.
void LoadQueue() {  // g_mu held
    g_queue.clear();
    std::string bytes;
    if (!ReadWholeFile(QueuePath(), bytes, kMaxQueueFileBytes)) return;
    size_t start = 0;
    while (start < bytes.size()) {
        size_t end = bytes.find('\n', start);
        if (end == std::string::npos) end = bytes.size();
        std::string line = bytes.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
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
    if (!CryptProtectData(&in, L"cordinal ListenBrainz token", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
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
        long long seconds = std::wcstoll(r.retryAfter.c_str(), nullptr, 10);
        if (seconds <= 0) seconds = kRetryDefaultSeconds;
        seconds = std::min<long long>(seconds, kRetryMaxSeconds);
        g_due = Clock::now() + std::chrono::seconds(seconds);
        g_status.lastError = L"Rate limited, retrying";
    } else if (r.network || r.status == 0 || r.status >= 500) {
        g_backoffMs = g_backoffMs == 0 ? kBackoffFirstMs : std::min(g_backoffMs * 2, kBackoffMaxMs);
        g_due = Clock::now() + std::chrono::milliseconds(g_backoffMs);
        g_status.lastError =
            r.network ? L"Network error, retrying" : L"Server error " + std::to_wstring(r.status) + L", retrying";
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
        const bool canSend = !g_token.empty() && !g_rejected && !g_sendingDisabled;
        if (canSend && g_nowPlayingPending) {
            std::string json = std::move(g_nowPlaying);
            g_nowPlaying.clear();
            g_nowPlayingPending = false;
            std::wstring token = g_token;
            lock.unlock();
            Response r = SendListens(token, "playing_now", "[" + json + "]");
            lock.lock();
            if (g_stop) break;
            if (r.status == 401) {
                g_rejected = true;
                g_status.lastError = L"Token rejected";
                PostStatus();
            }
            continue;
        }
        if (canSend && !g_queue.empty() && Clock::now() >= g_due) {
            size_t n = g_oneByOne ? 1 : std::min(kMaxBatch, g_queue.size());
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
            Finish(r, n);
            continue;
        }
        if (canSend && !g_queue.empty())
            g_cv.wait_until(lock, g_due);
        else
            g_cv.wait(lock);
    }
}

// Runs one Validate() request and posts the answer. Each one gets its own thread.
void ValidateWorker(std::wstring token) {
    Response r = Http(L"GET", L"/1/validate-token", token, nullptr);
    WPARAM result = 2;
    std::wstring* user = nullptr;
    if (r.status == 200) {
        if (JsonBool(r.body, "valid")) {
            result = 1;
            user = new std::wstring(FromUtf8(JsonString(r.body, "user_name")));
        } else {
            result = 0;
        }
    } else if (r.status == 401 || r.status == 403) {
        result = 0;
    }
    std::lock_guard lock(g_mu);
    // Copy the name before posting: the UI owns and frees lParam as soon as it runs.
    if (user) g_status.user = *user;
    if (g_stop || !CanNotify() || !PostMessageW(g_notify, g_msg, result, (LPARAM)user)) {
        delete user;  // nothing received it, so nothing will delete it
        return;
    }
}

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
        LoadQueue();
        g_status.queued = g_queue.size();
    }
    g_worker = std::thread(WorkerLoop);
}

// Cancels requests in flight by closing their sessions, then joins the worker and validators.
void Stop() {
#ifdef _WIN32
    std::vector<HINTERNET> sessions;
    {
        std::lock_guard lock(g_mu);
        g_stop = true;
        sessions.swap(g_sessions);
    }
    for (HINTERNET session : sessions) WinHttpCloseHandle(session);
#else
    {
        std::lock_guard lock(g_mu);
        g_stop = true;
        for (pid_t pid : g_children) kill(pid, SIGKILL);  // the request's thread reaps it
    }
#endif
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    for (std::thread& t : g_validators)
        if (t.joinable()) t.join();
    g_validators.clear();
}

// Trims and stores a new token (or clears it, turning sending off); the queue is left alone.
void SetToken(const std::wstring& raw) {
    if (g_dir.empty()) return;
    std::wstring token = Trim(raw);
    bool saved = true;
    if (token.empty())
        DeleteFileW(TokenPath().c_str());
    else
        saved = SaveToken(token);
    std::lock_guard lock(g_mu);
    g_token = token;
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
    if (json.empty() || g_token.empty()) return;
    g_nowPlaying = std::move(json);
    g_nowPlayingPending = true;
    g_cv.notify_all();
}

void Submit(const Listen& l) {
    std::string json = ListenJson(l, true);
    std::lock_guard lock(g_mu);
    if (json.empty() || l.listenedAt == 0 || g_token.empty() || !g_worker.joinable()) return;
    // Written first: a listen that is only in memory is lost if the app closes.
    if (!AppendLine(json)) g_status.lastError = L"Could not write the queue file";
    g_queue.push_back(std::move(json));
    g_status.queued = g_queue.size();
    g_cv.notify_all();
    PostStatus();
}

// Starts one token check on its own thread; the result arrives as a message.
void Validate(const std::wstring& token) {
    g_validators.emplace_back(ValidateWorker, Trim(token));
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

}  // namespace listenbrainz
