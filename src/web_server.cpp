// Opt-in HTTP server and device pairing. Connections, streams and listening reports are bounded;
// workers see only owned snapshots, and signal the frontend without calling into its state. Every
// port also speaks HTTPS once there is a certificate: a TLS handshake is told from HTTP by its first byte.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include "web_internal.h"
#include "web_tls.h"
#include "library.h"
#include "sort.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace web {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kBad = INVALID_SOCKET;
void Close(Socket s) { closesocket(s); }
void Shutdown(Socket s) { shutdown(s, SD_BOTH); }
#else
using Socket = int;
constexpr Socket kBad = -1;
void Close(Socket s) { close(s); }
void Shutdown(Socket s) { shutdown(s, SHUT_RDWR); }
#endif
bool WouldBlock() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
using Address = std::array<uint8_t, 16>;  // IPv6; IPv4 is mapped as ::ffff:a.b.c.d
struct Prefix {
    Address bytes{};
    int bits = 0;
};
constexpr uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
Address ToAddress(const sockaddr* a) {
    Address out{};
    if (a->sa_family == AF_INET) {
        memcpy(out.data(), kMapped, 12);
        memcpy(out.data() + 12, &((const sockaddr_in*)a)->sin_addr, 4);
    } else if (a->sa_family == AF_INET6) memcpy(out.data(), &((const sockaddr_in6*)a)->sin6_addr, 16);
    return out;
}
bool ParseAddress(const std::string& text, Address& out) {
    sockaddr_in6 v6{};
    sockaddr_in v4{};
    if (inet_pton(AF_INET6, text.c_str(), &v6.sin6_addr) == 1) { v6.sin6_family = AF_INET6; out = ToAddress((const sockaddr*)&v6); return true; }
    if (inet_pton(AF_INET, text.c_str(), &v4.sin_addr) == 1) { v4.sin_family = AF_INET; out = ToAddress((const sockaddr*)&v4); return true; }
    return false;
}
bool Within(const Address& a, const Prefix& p) {
    for (int bit = 0; bit < p.bits; ++bit)
        if (((a[bit >> 3] ^ p.bytes[bit >> 3]) >> (7 - (bit & 7))) & 1) return false;
    return true;
}
bool Loopback(const Address& a) {
    static constexpr Address one{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return (!memcmp(a.data(), kMapped, 12) && a[12] == 127) || a == one;
}
// At home: loopback, the private and link-local ranges, unique local IPv6, 100.64/10 (which
// Tailscale-style VPNs hand out) and every prefix on this computer's own links.
bool Home(const Address& a, const std::vector<Prefix>& links) {
    if (!memcmp(a.data(), kMapped, 12)) {
        const uint8_t* v = a.data() + 12;
        if (v[0] == 127 || v[0] == 10 || (v[0] == 172 && (v[1] & 0xF0) == 16) || (v[0] == 192 && v[1] == 168) ||
            (v[0] == 169 && v[1] == 254) || (v[0] == 100 && (v[1] & 0xC0) == 64)) return true;
    } else if (Loopback(a) || (a[0] == 0xfe && (a[1] & 0xC0) == 0x80) || (a[0] & 0xFE) == 0xfc) return true;
    return std::any_of(links.begin(), links.end(), [&](const Prefix& p) { return Within(a, p); });
}
struct Connection {
    Socket socket = kBad;
    std::atomic<bool> cancel{false};
    std::string device;
    bool streaming = false, away = false, secure = false;
    bool dispatched = false;  // handed to a worker; until then the listener thread watches it
    Address peer{};
    uint64_t since = 0;
    std::string input;  // received request bytes, decrypted when the connection is HTTPS
    std::unique_ptr<TlsSession> tls;
    std::string encrypted;  // received TLS bytes not decrypted yet
};
bool Ready(Socket socket, bool write, unsigned ms) {
    fd_set fds; FD_ZERO(&fds); FD_SET(socket, &fds);
    timeval time{(long)(ms / 1000), (long)(ms % 1000) * 1000};
    return select((int)(socket + 1), write ? nullptr : &fds, write ? &fds : nullptr, nullptr, &time) > 0;
}
bool SendRaw(Connection& c, const char* bytes, size_t size) {
    uint64_t until = ClockMs() + 5000;
    while (size && !c.cancel) {
        if (!Ready(c.socket, true, 250)) {
            // Browsers stop reading when their media buffer is full, especially while paused.
            // Keep those streams resumable; cancellation still wakes within a short poll.
            if (!c.streaming && ClockMs() >= until) return false;
            continue;
        }
        int n = send(c.socket, bytes, (int)std::min<size_t>(size, 65536),
#ifdef _WIN32
                     0
#else
                     MSG_NOSIGNAL
#endif
        );
        if (n < 0 && WouldBlock()) continue;
        if (n <= 0) return false;
        bytes += n; size -= (size_t)n;
    }
    return size == 0;
}
bool Send(Connection& c, const char* bytes, size_t size) {
    if (c.tls) {
        std::string out;
        while (size) {
            size_t n = std::min<size_t>(size, 65536);
            out.clear();
            if (!c.tls->Encrypt({bytes, n}, out) || !SendRaw(c, out.data(), out.size())) return false;
            bytes += n; size -= n;
        }
        return true;
    }
    return SendRaw(c, bytes, size);
}
// Waits up to `ms` for more of the request. False when the peer closed, failed or stayed silent.
bool Receive(Connection& c, unsigned ms) {
    if (c.cancel || !Ready(c.socket, false, ms)) return false;
    char buffer[16384];
    int n = recv(c.socket, buffer, sizeof(buffer), 0);
    if (n < 0 && WouldBlock()) return true;
    if (n <= 0) return false;
    if (c.tls) {
        c.encrypted.append(buffer, (size_t)n);
        std::string reply;
        int result = c.tls->Decrypt(c.encrypted, c.input, reply);
        if (!reply.empty() && !SendRaw(c, reply.data(), reply.size())) return false;
        return result > 0;
    }
    c.input.append(buffer, (size_t)n);
    return true;
}
// The bytes received so far begin a TLS handshake; finish it within ten seconds (phone networks).
bool StartTls(Connection& c, std::shared_ptr<TlsIdentity> identity) {
    c.tls = std::make_unique<TlsSession>(std::move(identity));
    c.encrypted.swap(c.input);
    uint64_t deadline = ClockMs() + 10000;
    for (;;) {
        std::string reply;
        int result = c.tls->Handshake(c.encrypted, reply);
        if (!reply.empty() && !SendRaw(c, reply.data(), reply.size())) return false;
        if (result < 0) return false;
        if (result > 0) break;
        if (c.cancel || ClockMs() >= deadline || !Ready(c.socket, false, 5000)) return false;
        char buffer[16384];
        int n = recv(c.socket, buffer, sizeof(buffer), 0);
        if (n < 0 && WouldBlock()) continue;
        if (n <= 0) return false;
        c.encrypted.append(buffer, (size_t)n);
    }
    std::string reply;
    if (!c.encrypted.empty() && c.tls->Decrypt(c.encrypted, c.input, reply) < 0) return false;
    return reply.empty() || SendRaw(c, reply.data(), reply.size());
}
bool Response(Connection& c, int status, const std::string& mime, uint64_t length,
              const std::string& extra = {}) {
    const char* reason = status == 200 ? "OK" : status == 206 ? "Partial Content" : status == 204 ? "No Content" :
        status == 307 ? "Temporary Redirect" :
        status == 400 ? "Bad Request" : status == 401 ? "Unauthorized" : status == 403 ? "Forbidden" :
        status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : status == 409 ? "Conflict" :
        status == 416 ? "Range Not Satisfiable" : status == 429 ? "Too Many Requests" : "Service Unavailable";
    std::string header = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + mime +
        "\r\nContent-Length: " + std::to_string(length) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
        "Cross-Origin-Resource-Policy: same-origin\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
        "img-src 'self' data: blob:; media-src 'self'; font-src 'self'; connect-src 'self'; "
        "frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n" + extra + "\r\n";
    return Send(c, header.data(), header.size());
}
void Body(Connection& c, const Request& r, int status, std::string_view bytes,
          const std::string& mime = "application/json; charset=utf-8", const std::string& extra = {}) {
    if (Response(c, status, mime, bytes.size(), extra) && r.method != "HEAD") Send(c, bytes.data(), bytes.size());
}
void Error(Connection& c, const Request& r, int status, std::string_view message) {
    Body(c, r, status, "{\"error\":" + Json(message) + '}');
}
std::string Value(const Request& r, const char* key) {
    auto it = r.query.find(key); return it == r.query.end() ? "" : it->second;
}
std::string Cookie(const Request& r, const std::string& name) {
    auto it = r.headers.find("cookie"); if (it == r.headers.end()) return {};
    std::string_view text = it->second;
    while (!text.empty()) {
        while (text.starts_with(' ')) text.remove_prefix(1);
        size_t end = text.find(';'); auto part = text.substr(0, end);
        if (part.starts_with(name + '=')) return std::string(part.substr(name.size() + 1));
        if (end == text.npos) break;
        text.remove_prefix(end + 1);
    }
    return {};
}
bool Fields(const Request& r, Request& form) {
    std::string data = "GET /?" + r.body + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    size_t size = 0; return ParseRequest(data, form, size);
}
uint64_t Integer(std::string_view text) {
    uint64_t value = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : 0;
}
bool IsHex(const std::string& text, size_t length) {
    return text.size() == length && text.find_first_not_of("0123456789abcdef") == text.npos;
}
std::string CleanName(std::string name) {
    if (name.empty()) name = "Browser";
    std::erase_if(name, [](unsigned char c) { return c < 32 || c == 127; });
    name = ToUtf8(FromUtf8(name));
    size_t length = std::min<size_t>(120, name.size());
    while (length < name.size() && length && ((unsigned char)name[length] & 0xC0) == 0x80) --length;
    return name.substr(0, length);
}
// "host" or "host:port" as a browser sends it, with the scheme's default port made explicit.
std::string WithPort(const std::string& host, uint16_t port) {
    size_t bracket = host.rfind(']'), colon = host.rfind(':');
    return colon != host.npos && (bracket == host.npos || colon > bracket) ? host : host + ':' + std::to_string(port);
}
std::string WithoutPort(const std::string& host, uint16_t port) {
    std::string suffix = ':' + std::to_string(port);
    return host.ends_with(suffix) ? host.substr(0, host.size() - suffix.size()) : host;
}
}  // namespace
bool ReadRecords(const std::wstring& path, std::string& bytes) {
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") || !file) return false;
    // Kept-music grants are the largest records: 20,000 of them stay well inside this.
    std::vector<char> buffer((8u << 20) + 1);
    size_t n = fread(buffer.data(), 1, buffer.size(), file);
    bool ok = !ferror(file) && n < buffer.size(); fclose(file);
    if (!ok) return false;
#ifdef _WIN32
    DATA_BLOB in{(DWORD)n, (BYTE*)buffer.data()}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    bytes.assign((const char*)out.pbData, out.cbData); LocalFree(out.pbData);
#else
    struct stat st{};
    if (stat(compat::Narrow(path.c_str()).c_str(), &st) || st.st_uid != getuid() || (st.st_mode & 077)) return false;
    bytes.assign(buffer.data(), n);
#endif
    return true;
}
bool SaveRecords(const std::wstring& path, std::string bytes) {
    if (path.empty()) return true;
#ifdef _WIN32
    DATA_BLOB in{(DWORD)bytes.size(), (BYTE*)bytes.data()}, out{};
    if (!CryptProtectData(&in, L"cordinal web server", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) return false;
    bytes.assign((const char*)out.pbData, out.cbData); LocalFree(out.pbData);
    std::wstring temp = path + L"." + FromUtf8(RandomHex(8)) + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size() &&
        FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) ok = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ?
        MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != 0 :
        ReplaceFileW(path.c_str(), temp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != 0;
    if (!ok) DeleteFileW(temp.c_str());
#else
    std::string target = compat::Narrow(path.c_str()), temp = target + "." + RandomHex(8) + ".tmp";
    int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t at = 0;
    while (at < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + at, bytes.size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        at += (size_t)n;
    }
    bool ok = at == bytes.size() && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok) ok = rename(temp.c_str(), target.c_str()) == 0;
    if (!ok) unlink(temp.c_str());
#endif
    return ok;
}
namespace {
struct Network {
    std::set<std::string> hosts;  // "address:port" for every listening port
    std::vector<Prefix> links;
};
// This computer's names for each port, and the prefixes of the links it is on.
Network LocalNetwork(const std::vector<uint16_t>& ports, bool loopbackOnly) {
    Network network;
    std::set<std::string> names{"localhost", "127.0.0.1"};
    auto add = [&](const sockaddr* address, unsigned prefix) {
        if (!address || (address->sa_family != AF_INET && address->sa_family != AF_INET6)) return;
        bool v4 = address->sa_family == AF_INET;
        if (prefix && prefix <= (v4 ? 32u : 128u)) network.links.push_back({ToAddress(address), (int)(v4 ? 96 + prefix : prefix)});
        char host[NI_MAXHOST]{};
        int size = v4 ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        if (!getnameinfo(address, size, host, sizeof(host), nullptr, 0, NI_NUMERICHOST)) {
            std::string name = Lower(host);
            // Link-local IPv6 scope identifiers require per-client interface names; show usable addresses instead.
            if (name.find('%') != name.npos) return;
            names.insert(v4 ? name : "[" + name + "]");
        }
    };
    if (!loopbackOnly) {
#ifdef _WIN32
        ULONG size = 16384; std::vector<char> bytes(size);
        ULONG result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER, nullptr, (IP_ADAPTER_ADDRESSES*)bytes.data(), &size);
        if (result == ERROR_BUFFER_OVERFLOW) {
            bytes.resize(size); result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST |
                GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, (IP_ADAPTER_ADDRESSES*)bytes.data(), &size);
        }
        if (result == NO_ERROR) for (auto* a = (IP_ADAPTER_ADDRESSES*)bytes.data(); a; a = a->Next)
            if (a->OperStatus == IfOperStatusUp) for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
                add(u->Address.lpSockaddr, u->OnLinkPrefixLength);
#else
        ifaddrs* addresses = nullptr;
        if (!getifaddrs(&addresses)) {
            for (auto* a = addresses; a; a = a->ifa_next) {
                unsigned bits = 0;
                if (a->ifa_netmask && a->ifa_addr && a->ifa_netmask->sa_family == a->ifa_addr->sa_family) {
                    Address mask = ToAddress(a->ifa_netmask);
                    for (size_t i = a->ifa_addr->sa_family == AF_INET ? 12 : 0; i < 16; ++i)
                        for (uint8_t m = mask[i]; m; m <<= 1) ++bits;
                }
                add(a->ifa_addr, bits);
            }
            freeifaddrs(addresses);
        }
#endif
        char hostname[256]{};
        if (!gethostname(hostname, sizeof(hostname))) names.insert(Lower(hostname));
    }
    for (uint16_t port : ports) for (const auto& name : names) network.hosts.insert(name + ':' + std::to_string(port));
    return network;
}
}

struct Server::Impl {
    struct Paired {
        std::string id, name, token;
        uint64_t seen = 0;  // ClockMs of its last request this session
        bool away = false;
    };
    struct Link {
        uint64_t until = 0;
        std::string name;  // set when another paired browser asked for it: the new one is named after it
    };
    struct Listener {
        Socket socket = kBad;
        uint16_t port = 0;
    };
    struct Listening {
        std::string device;
        std::wstring path;
        uint64_t sequence = 0, tick = 0, seen = 0;
        double heard = 0, reported = 0, budget = 0;
        bool playing = true;
    };
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{true};
    std::atomic<unsigned> streams{0}, peaks{0};
    AacCache audioCache;
    std::thread listener;
    std::vector<std::thread> workers;
    std::deque<std::shared_ptr<Connection>> pending;
    std::vector<std::shared_ptr<Connection>> connections;
    std::vector<Listener> listeners;  // the main port first; only the listener thread changes it once started
    Config config;
    Status status;
    std::string cookieName;
    std::set<std::string> hosts;
    std::vector<Prefix> links;
    std::vector<Paired> paired;
    std::map<std::string, Link> pairingLinks;
    Away away;
    // A track a browser downloaded to keep: plays it reports later are credited only from then on.
    // The path is kept too, because track keys change when cordinal restarts.
    struct Grant {
        std::string device, key;
        std::wstring path;
        uint64_t time = 0;  // unix seconds
    };
    std::vector<Grant> grants;
    std::deque<std::string> playedIds;  // offline plays already counted, newest last
    std::map<std::string, Listening> listening;
    std::map<std::string, uint64_t> finished;
    std::vector<ListenEvent> listens;
    std::map<std::string, std::string> waveforms;
    std::shared_ptr<const Snapshot> snapshot = std::make_shared<Snapshot>();
    std::shared_ptr<Snapshot> preparing;
    ThemeMode theme = ThemeMode::Light;
    bool systemDark = false, sockets = false, rebuild = false, loopbackAway = false;
    uint64_t pairingUntil = 0, nextRefresh = 0;
    uint64_t publishSequence = 0;
    unsigned attempts = 0;

    void Notify() { if (config.notify) config.notify(); }
    // A fixed code has no expiry, so the five-try limit is all that stands between it and guessing.
    void OpenFixed() {
        if (config.fixedCode.empty() || !status.running) return;
        status.pairingCode = config.fixedCode; pairingUntil = UINT64_MAX; attempts = 0;
    }
    bool Save() {
        std::string text = "CWEB1\n";
        for (const auto& p : paired) text += p.id + '\n' + p.token + '\n' + p.name + '\n';
        return SaveRecords(config.devicesPath, std::move(text));
    }
    bool Load() {
        std::string text;
        if (!ReadRecords(config.devicesPath, text)) return false;
        if (text.empty()) return true;
        if (!text.starts_with("CWEB1\n")) return false;
        size_t at = 6;
        while (at < text.size()) {
            std::string fields[3];
            for (auto& field : fields) {
                size_t end = text.find('\n', at); if (end == text.npos) return false;
                field = text.substr(at, end - at); at = end + 1;
            }
            if (!IsHex(fields[0], 16) || !IsHex(fields[1], 64) || fields[2].size() > 120 || paired.size() >= 64)
                return false;
            paired.push_back({fields[0], fields[2], fields[1]});
        }
        return true;
    }
    // Mutex held, like the two below.
    void SaveOffline() {
        if (config.offlinePath.empty()) return;
        std::string text = "COFF1\n";
        for (const auto& g : grants) text += "G\t" + g.device + '\t' + g.key + '\t' + std::to_string(g.time) + '\t' + ToUtf8(g.path) + '\n';
        for (const auto& id : playedIds) text += "P\t" + id + '\n';
        SaveRecords(config.offlinePath, std::move(text));
    }
    void LoadOffline() {
        grants.clear(); playedIds.clear();
        std::string text;
        if (config.offlinePath.empty() || !ReadRecords(config.offlinePath, text) || !text.starts_with("COFF1\n")) return;
        for (size_t at = 6; at < text.size();) {
            size_t end = text.find('\n', at); if (end == text.npos) break;
            std::vector<std::string> f;
            for (size_t i = at; i <= end;) { size_t tab = std::min(text.find('\t', i), end); f.push_back(text.substr(i, tab - i)); i = tab + 1; }
            at = end + 1;
            if (f.size() == 5 && f[0] == "G" && IsHex(f[1], 16)) grants.push_back({f[1], f[2], FromUtf8(f[4]), Integer(f[3])});
            else if (f.size() == 2 && f[0] == "P" && IsHex(f[1], 32)) playedIds.push_back(f[1]);
        }
    }
    void Forget(const std::string& device) {
        size_t before = grants.size();
        std::erase_if(grants, [&](const Grant& g) { return device.empty() || g.device == device; });
        if (grants.size() != before) SaveOffline();
    }
    // Stores a new browser and returns 200, or the status and message that refuse it. Mutex held.
    int AddDevice(const std::string& name, std::string& token, std::string& message) {
        if (paired.size() >= 64) { message = "Remove a paired device in cordinal first."; return 409; }
        Paired p{RandomHex(8), name, RandomHex(32)};
        if (p.id.empty() || p.token.empty()) { message = "Couldn't create a device key."; return 503; }
        paired.push_back(p);
        if (!Save()) { paired.pop_back(); message = "Couldn't save this device. Pairing records were preserved."; return 503; }
        token = p.token;
        return 200;
    }
    // Mutex held. A link works once, for ten minutes; only the newest eight are kept.
    std::string NewLink(const std::string& name) {
        uint64_t now = ClockMs();
        std::erase_if(pairingLinks, [&](const auto& l) { return now >= l.second.until; });
        while (pairingLinks.size() >= 8) {
            pairingLinks.erase(std::min_element(pairingLinks.begin(), pairingLinks.end(), [](const auto& a, const auto& b) {
                return a.second.until < b.second.until;
            }));
        }
        std::string token = RandomHex(16);
        if (!token.empty()) pairingLinks[token] = {now + 600000, name};
        return token;
    }
    std::string SetCookie(bool https, const std::string& token, int maxAge) const {
        return "Set-Cookie: " + (https ? std::string("__Host-cordinal") : cookieName) + '=' + token +
            (https ? "; Secure" : "") + "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" + std::to_string(maxAge) + "\r\n";
    }
    std::string Manifest() const {
        const auto& t = ThemeInfo(theme);
        const auto& palette = t.palette ? *t.palette : systemDark ? DarkPalette() : LightPalette();
        char color[8];
        snprintf(color, sizeof(color), "#%06x", palette.bg);
        return std::string("{\"name\":\"cordinal\",\"short_name\":\"cordinal\",\"start_url\":\"/\",\"scope\":\"/\","
            "\"display\":\"standalone\",\"background_color\":\"") + color + "\",\"theme_color\":\"" + color + "\"" +
            (config.icon.empty() ? "" : ",\"icons\":[{\"src\":\"/icon.png\",\"sizes\":\"256x256\",\"type\":\"image/png\"}]") + '}';
    }
    void Finish(std::map<std::string, Listening>::iterator it, bool manual) {
        if (it->second.heard >= 0.5) listens.push_back({it->second.path, it->second.heard, manual});
        finished[it->first] = ClockMs();
        if (finished.size() > 4096) {
            auto oldest = std::min_element(finished.begin(), finished.end(), [](const auto& a, const auto& b) {
                return a.second < b.second;
            });
            finished.erase(oldest);
        }
        listening.erase(it);
    }
    Socket Listen(uint16_t port, bool loopbackOnly) {
        bool v6 = !loopbackOnly;
        Socket s = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kBad && v6) { v6 = false; s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); }
        if (s == kBad) return kBad;
        int yes = 1;
#ifdef _WIN32
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&yes, sizeof(yes));
#else
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
        int bound;
        if (v6) {
            int no = 0; setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&no, sizeof(no));
            sockaddr_in6 address{}; address.sin6_family = AF_INET6; address.sin6_port = htons(port);
            bound = bind(s, (sockaddr*)&address, sizeof(address));
        } else {
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
            bound = bind(s, (sockaddr*)&address, sizeof(address));
        }
        if (bound || listen(s, 16)) { Close(s); return kBad; }
        return s;
    }
    // Recomputes the names browsers may use and the home prefixes; called without the mutex.
    void Refresh() {
        std::vector<uint16_t> ports;
        for (const auto& l : listeners) ports.push_back(l.port);
        auto network = LocalNetwork(ports, config.port == 0);
        std::vector<std::string> addresses;
        std::string suffix = ':' + std::to_string(ports.front());
        for (const auto& host : network.hosts) if (host.ends_with(suffix)) addresses.push_back("http://" + host + '/');
        for (const auto& name : config.names) {
            network.hosts.insert(WithPort(Lower(name), 443));
            network.hosts.insert(WithPort(Lower(name), 80));
        }
        std::lock_guard lock(mutex);
        hosts = std::move(network.hosts);
        hosts.insert(away.hosts.begin(), away.hosts.end());
        links = std::move(network.links);
        status.addresses = std::move(addresses);
        status.ports = std::move(ports);
        nextRefresh = ClockMs() + 30000;
    }
    // Opens the extra ports SetAway asked for and closes the ones it no longer wants. Listener thread.
    void Reconcile() {
        std::vector<uint16_t> wanted;
        {
            std::lock_guard lock(mutex);
            if (!rebuild && ClockMs() < nextRefresh) return;
            wanted = away.ports; rebuild = false;
        }
        std::erase(wanted, listeners.front().port);
        for (size_t i = 1; i < listeners.size();) {
            if (std::find(wanted.begin(), wanted.end(), listeners[i].port) != wanted.end()) { ++i; continue; }
            Close(listeners[i].socket); listeners.erase(listeners.begin() + (ptrdiff_t)i);
        }
        std::string error;
        for (uint16_t port : wanted) {
            if (std::any_of(listeners.begin(), listeners.end(), [&](const Listener& l) { return l.port == port; })) continue;
            Socket s = Listen(port, config.port == 0);
            if (s != kBad) listeners.push_back({s, port});
            else error = "Port " + std::to_string(port) + " is in use by another program, so cordinal can't listen on it.";
        }
        { std::lock_guard lock(mutex); status.portError = error; }
        Refresh();
    }
    void Worker() {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
        setpriority(PRIO_PROCESS, 0, 10);
#endif
        for (;;) {
            std::shared_ptr<Connection> c;
            std::shared_ptr<Snapshot> next;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return stop || !pending.empty() || preparing; });
                if (stop && pending.empty()) return;
                if (preparing) next.swap(preparing);
                else { c = pending.front(); pending.pop_front(); }
            }
            if (next) {
                try {
                    if (next->prepare) { auto prepare = std::move(next->prepare); prepare(*next); }
                    auto less = [](const Track& a, const Track& b) {
                        return std::tie(a.albumArtist, a.album, a.date, a.disc, a.number, a.title) <
                               std::tie(b.albumArtist, b.album, b.date, b.disc, b.number, b.title);
                    };
                    // Reorder owning tracks through the shared index permutation instead of merge buffers.
                    // Compare keys once per pair; already ordered captures need no allocation or swaps.
                    if (!std::is_sorted(next->tracks.begin(), next->tracks.end(), less))
                        sort_detail::StableSort(next->tracks.data(), next->tracks.size(), &next->tracks,
                            [](void* context, size_t a, size_t b) {
                                const auto& tracks = *static_cast<std::vector<Track>*>(context);
                                const auto &x = tracks[a], &y = tracks[b];
                                auto order = std::tie(x.albumArtist, x.album, x.date, x.disc, x.number, x.title) <=>
                                             std::tie(y.albumArtist, y.album, y.date, y.disc, y.number, y.title);
                                return (order > 0) - (order < 0);
                            },
                            [](void* data, size_t a, size_t b) {
                                auto* tracks = static_cast<Track*>(data);
                                std::swap(tracks[a], tracks[b]);
                            });
                    for (size_t i = 0; i < next->tracks.size(); ++i) next->byKey.emplace(next->tracks[i].key, i);
                } catch (...) {
                    { std::lock_guard lock(mutex); status.error = "Couldn't prepare the web library. Playback is still available."; }
                    Notify(); continue;
                }
                std::lock_guard lock(mutex);
                if (!stop && next->serial >= snapshot->serial) snapshot = std::move(next);
                continue;
            }
            if (!c->cancel) {
                try { Handle(*c); } catch (...) { c->cancel = true; }
            }
            {
                std::lock_guard lock(mutex);
                std::erase(connections, c);
                Shutdown(c->socket); Close(c->socket); c->socket = kBad;
            }
        }
    }
    void AcceptOne(const Listener& from) {
        sockaddr_storage peer{};
#ifdef _WIN32
        int size = sizeof(peer);
#else
        socklen_t size = sizeof(peer);
#endif
        Socket accepted = accept(from.socket, (sockaddr*)&peer, &size);
        if (accepted == kBad) return;
#ifdef _WIN32
        u_long nonblocking = 1; ioctlsocket(accepted, FIONBIO, &nonblocking);
#else
        fcntl(accepted, F_SETFL, fcntl(accepted, F_GETFL) | O_NONBLOCK);
#endif
        auto c = std::make_shared<Connection>();
        c->socket = accepted; c->since = ClockMs(); c->peer = ToAddress((const sockaddr*)&peer);
        std::lock_guard lock(mutex);
        c->away = (loopbackAway && Loopback(c->peer)) || !Home(c->peer, links);
        // One address can't take every connection, so a scanner can't lock the household out.
        size_t same = (size_t)std::count_if(connections.begin(), connections.end(), [&](const auto& o) { return o->peer == c->peer; });
        if (connections.size() >= 24 || same >= 12 || stop) Close(accepted);
        else connections.push_back(std::move(c));
    }
    void Accept() {
        while (!stop) {
            Reconcile();
            // Wait for new connections, and for the first bytes of accepted ones: an idle connection
            // (a browser's speculative one) stays here instead of holding a worker.
            fd_set ready; FD_ZERO(&ready);
            Socket top = 0;
            auto watch = [&](Socket s) { FD_SET(s, &ready); top = std::max(top, s); };
            for (const auto& l : listeners) watch(l.socket);
            {
                std::lock_guard lock(mutex);
                for (const auto& c : connections) if (!c->dispatched && !c->cancel) watch(c->socket);
            }
            timeval wait{0, 200000};
            if (select((int)(top + 1), &ready, nullptr, nullptr, &wait) > 0)
                for (const auto& l : listeners) if (FD_ISSET(l.socket, &ready)) AcceptOne(l);
            bool notify = false;
            {
                std::lock_guard lock(mutex);
                uint64_t now = ClockMs();
                for (auto it = connections.begin(); it != connections.end();) {
                    auto& c = *it;
                    if (!c->dispatched) {
                        if (c->cancel || now - c->since > 15000) {
                            Shutdown(c->socket); Close(c->socket); it = connections.erase(it); continue;
                        }
                        if (Ready(c->socket, false, 0)) { c->dispatched = true; pending.push_back(c); cv.notify_one(); }
                    } else if (!c->cancel && Ready(c->socket, false, 0)) {
                        char byte;
                        if (recv(c->socket, &byte, 1, MSG_PEEK) == 0) { c->cancel = true; Shutdown(c->socket); }
                    }
                    ++it;
                }
                for (auto it = listening.begin(); it != listening.end();) {
                    if (now - it->second.seen > 120000) { auto old = it++; Finish(old, false); notify = true; }
                    else ++it;
                }
            }
            if (notify) Notify();
        }
    }
    void Handle(Connection& c) {
        uint64_t deadline = ClockMs() + 5000;
        while (c.input.empty()) if (ClockMs() >= deadline || !Receive(c, 1000)) return;
        if ((uint8_t)c.input[0] == 0x16) {  // a TLS handshake record: the browser asked for HTTPS
            std::shared_ptr<TlsIdentity> identity;
            { std::lock_guard lock(mutex); identity = away.identity; }
            if (!identity || !StartTls(c, std::move(identity))) return;
            c.secure = true;
            deadline = ClockMs() + 5000;
        }
        Request r; size_t required = 0;
        for (;;) {
            if (!ParseRequest(c.input, r, required)) { Error(c, r, 400, "Malformed request."); return; }
            if (required <= c.input.size()) break;
            if (c.cancel || ClockMs() >= deadline || !Receive(c, 1000)) return;
        }
        if (r.method != "GET" && r.method != "HEAD" && r.method != "POST") {
            Error(c, r, 405, "Use GET, HEAD or POST."); return;
        }
        // Let's Encrypt fetches its challenge over plain HTTP by address, so this comes before the
        // host check; the answers are public by design.
        if (!c.secure && r.method == "GET" && r.path.starts_with("/.well-known/acme-challenge/")) {
            std::string answer, token = r.path.substr(28);
            { std::lock_guard lock(mutex); for (const auto& [t, value] : away.challenges) if (t == token) answer = value; }
            if (answer.empty()) Error(c, r, 404, "Unknown challenge.");
            else Body(c, r, 200, answer, "text/plain");
            return;
        }
        bool https = c.secure;
        // A reverse proxy on this computer (Tailscale Serve, Caddy) may have taken the HTTPS for us,
        // and says who its client is: one that publishes to the internet must not make it look local.
        if (!c.away && Loopback(c.peer)) {
            if (Lower(r.headers["x-forwarded-proto"]) == "https") https = true;
            if (auto forwarded = r.headers.find("x-forwarded-for"); forwarded != r.headers.end()) {
                std::string client = forwarded->second.substr(0, forwarded->second.find(','));
                client.erase(0, client.find_first_not_of(' '));
                client.erase(client.find_last_not_of(' ') + 1);
                Address a{};
                bool parsed = ParseAddress(client, a);
                std::lock_guard lock(mutex);
                c.away = !parsed || (loopbackAway && Loopback(a)) || !Home(a, links);
            }
        }
        const uint16_t defaultPort = https ? 443 : 80;
        const std::string sent = Lower(r.headers["host"]), host = WithPort(sent, defaultPort);
        const std::string scheme = https ? "https://" : "http://";
        bool allowed;
        { std::lock_guard lock(mutex); allowed = hosts.count(host) != 0; }
        auto origin = r.headers.find("origin");
        if (!allowed || (origin != r.headers.end() && origin->second != scheme + sent &&
                         origin->second != scheme + WithoutPort(host, defaultPort)) ||
            (r.headers.count("sec-fetch-site") && r.headers["sec-fetch-site"] != "same-origin" &&
             r.headers["sec-fetch-site"] != "none")) {
            Error(c, r, 403, "Open the server address directly."); return;
        }
        if (c.away && !https) {
            // Plain HTTP would expose the browser's key on the way; send away browsers to HTTPS.
            std::string url;
            { std::lock_guard lock(mutex); if (away.identity) url = away.url; }
            if (url.empty()) Error(c, r, 403, "Away from home, open the secure address that cordinal shows.");
            else Body(c, r, 307, "", "text/plain; charset=utf-8", "Location: " + url + r.target.substr(1) + "\r\n");
            return;
        }
        if (r.path == "/api/v1/themes" && r.method != "POST") {
            std::string data;
            { std::lock_guard lock(mutex); data = Themes(theme, systemDark); }
            Body(c, r, 200, data); return;
        }
        if (r.method != "POST") {
            if (r.path == "/") { Body(c, r, 200, Page(), "text/html; charset=utf-8"); return; }
            if (r.path == "/app.js") { Body(c, r, 200, Script(), "text/javascript; charset=utf-8"); return; }
            if (r.path == "/style.css") { Body(c, r, 200, Style(), "text/css; charset=utf-8"); return; }
            // Browsers register it only over HTTPS; it keeps the page and kept music working offline.
            if (r.path == "/sw.js") { Body(c, r, 200, ServiceWorker(), "text/javascript; charset=utf-8", "Cache-Control: no-cache\r\n"); return; }
            if (r.path == "/deltarune.ttf" && !config.font.empty()) {
                Body(c, r, 200, config.font, "font/ttf"); return;
            }
            if (r.path == "/icon.png" && !config.icon.empty()) { Body(c, r, 200, config.icon, "image/png"); return; }
            if (r.path == "/manifest.webmanifest") {
                std::string data;
                { std::lock_guard lock(mutex); data = Manifest(); }
                Body(c, r, 200, data, "application/manifest+json"); return;
            }
        }
        if (r.path == "/api/v1/pair" && r.method == "POST") {
            Request form;
            if (!Fields(r, form)) { Error(c, r, 400, "Invalid pairing form."); return; }
            std::string link = Value(form, "link"), name = CleanName(Value(form, "name")), token, message;
            int code = 200;
            {
                std::lock_guard lock(mutex);
                uint64_t now = ClockMs();
                std::erase_if(pairingLinks, [&](const auto& l) { return now >= l.second.until; });
                if (!link.empty()) {
                    auto it = pairingLinks.find(link);
                    if (it == pairingLinks.end()) {
                        code = 401; message = "This pairing link has expired. Choose Pair device in cordinal for a new one.";
                    } else {
                        if (!it->second.name.empty()) name = it->second.name;
                        code = AddDevice(name, token, message);
                        if (code == 200) pairingLinks.erase(it);
                    }
                } else if (c.away) {
                    // Six digits are easy to try from the whole internet; away pairing needs the long link.
                    code = 403; message = "Away from home, pair by scanning the code in cordinal's Web server window.";
                } else if (now >= pairingUntil || status.pairingCode.empty() || attempts >= 5) {
                    code = 429; message = "Generate a new pairing code in cordinal.";
                } else if (Value(form, "code") != status.pairingCode) {
                    ++attempts; code = 401; message = "The pairing code did not match.";
                } else {
                    code = AddDevice(name, token, message);
                    // A fixed code stays open for the next device; a temporary one is spent.
                    if (code == 200 && config.fixedCode.empty()) { status.pairingCode.clear(); pairingUntil = 0; }
                }
            }
            if (code == 200) Body(c, r, 200, "{\"paired\":true}", "application/json", SetCookie(https, token, 31536000));
            else Error(c, r, code, message);
            Notify(); return;
        }
        std::shared_ptr<const Snapshot> data;
        {
            std::lock_guard lock(mutex);
            std::string token = Cookie(r, https ? "__Host-cordinal" : cookieName);
            for (auto& p : paired) if (!token.empty() && p.token == token) {
                c.device = p.id; p.seen = ClockMs(); p.away = c.away; break;
            }
            data = snapshot;
        }
        if (c.device.empty()) { Error(c, r, 401, "Pair this browser with cordinal."); return; }
        if (r.path == "/api/v1/logout" && r.method == "POST") {
            {
                std::lock_guard lock(mutex);
                auto original = paired;
                std::erase_if(paired, [&](const auto& d) { return d.id == c.device; });
                if (!Save()) { paired = std::move(original); Error(c, r, 503, "Couldn't remove the paired device."); return; }
                Forget(c.device);
                for (const auto& connection : connections)
                    if (connection.get() != &c && connection->device == c.device) {
                        connection->cancel = true; Shutdown(connection->socket);
                    }
                for (auto it = listening.begin(); it != listening.end();) {
                    if (it->second.device == c.device) { auto old = it++; Finish(old, false); } else ++it;
                }
            }
            Body(c, r, 200, "{}", "application/json", SetCookie(https, "", 0)); Notify(); return;
        }
        if (r.path == "/api/v1/away" && r.method == "POST") {
            // A browser paired at home asks for a one-time link to the secure address, so moving it
            // to away-from-home listening needs no trip back to the computer.
            std::string url, token;
            {
                std::lock_guard lock(mutex);
                if (away.identity && !away.url.empty()) {
                    std::string name;
                    for (const auto& p : paired) if (p.id == c.device) name = p.name;
                    token = NewLink(CleanName(name + " (away)"));
                    url = away.url;
                }
            }
            if (url.empty() || token.empty()) Error(c, r, 409, "Away from home isn't set up in cordinal yet.");
            else Body(c, r, 200, "{\"url\":" + Json(url + "#pair=" + token) + '}');
            return;
        }
        if (r.method != "POST") {
            const char* view = r.path == "/api/v1/library" ? "tracks" : r.path == "/api/v1/albums" ? "albums" :
                r.path == "/api/v1/artists" ? "artists" : r.path == "/api/v1/playlists" ? "playlists" : nullptr;
            if (view) r.query["view"] = view;
            if (view || r.path == "/api/v1/catalog") { Body(c, r, 200, Catalog(*data, r)); return; }
        }
        if (r.path == "/api/v1/state" && r.method != "POST") {
            std::string url;
            { std::lock_guard lock(mutex); if (away.identity) url = away.url; }
            Body(c, r, 200, "{\"revision\":" + std::to_string(data->revision) + ",\"ready\":" +
                (data->serial ? "true" : "false") + ",\"away\":" + (url.empty() ? "null" : Json(url)) +
                ",\"secure\":" + (https ? "true" : "false") + ",\"dataSaver\":true}");
            return;
        }
        std::string key = Value(r, "id");
        auto found = data->byKey.find(key);
        const Track* track = found == data->byKey.end() ? nullptr : &data->tracks[found->second];
        if (r.path == "/api/v1/listen" && r.method == "POST") {
            Listen(c, r, *data); return;
        }
        if (r.path == "/api/v1/played" && r.method == "POST") {
            Played(c, r, *data); return;
        }
        if (!track) { Error(c, r, 404, "This track is no longer available. Refresh the library."); return; }
        if (r.method == "POST") { Error(c, r, 405, "This operation is read-only."); return; }
        if (r.path == "/api/v1/track") { Body(c, r, 200, TrackJson(*track)); return; }
        if (r.path == "/api/v1/art") {
            std::string mime, image = Artwork(*track, mime);
            if (image.empty()) Error(c, r, 404, "No artwork.");
            else Body(c, r, 200, image, mime);
            return;
        }
        if (r.path == "/api/v1/peaks") {
            std::string cacheKey = track->key + ':' + std::to_string(track->fileTime) + ':' + std::to_string(track->fileSize);
            std::string result;
            { std::lock_guard lock(mutex); auto it = waveforms.find(cacheKey); if (it != waveforms.end()) result = it->second; }
            if (result.empty()) {
                if (peaks.fetch_add(1) != 0) { --peaks; Error(c, r, 503, "A waveform is being prepared."); return; }
                struct PeakGuard { std::atomic<unsigned>& n; ~PeakGuard() { --n; } } guard{peaks};
                result = Waveform(*track, c.cancel);
                if (result.empty()) { Error(c, r, 404, "No waveform is available."); return; }
                std::lock_guard lock(mutex);
                if (waveforms.size() >= 8) waveforms.erase(waveforms.begin());
                waveforms[cacheKey] = result;
            }
            Body(c, r, 200, result); return;
        }
        if (r.path != "/api/v1/audio") { Error(c, r, 404, "Unknown operation."); return; }
        if (streams.fetch_add(1) >= 4) { --streams; Error(c, r, 503, "Four streams are already active. Try again shortly."); return; }
        struct StreamGuard { std::atomic<unsigned>& n; ~StreamGuard() { --n; } } guard{streams};
        c.streaming = r.method != "HEAD";
        std::string startText = Value(r, "start"); char* end = nullptr;
        double start = startText.empty() ? 0 : strtod(startText.c_str(), &end);
        if (!std::isfinite(start) || start < 0 || (!startText.empty() && end != startText.c_str() + startText.size())) {
            Error(c, r, 400, "Invalid playback position."); return;
        }
        const auto format = Value(r, "format");
        if (!format.empty() && format != "original" && format != "wav" && format != "aac") {
            Error(c, r, 400, "Unknown streaming quality."); return;
        }
        const auto bitrate = Value(r, "bitrate");
        if (format == "aac" && !bitrate.empty() && bitrate != "96" && bitrate != "128") {
            Error(c, r, 400, "Unknown compressed streaming quality."); return;
        }
        Media media = format == "aac" ? AacMedia(*track, start, c.cancel, bitrate == "128" ? 128 : 96, &audioCache) :
            format == "wav" ? WaveMedia(*track, start, c.cancel) : OriginalMedia(*track);
        if (!media.error.empty()) { Error(c, r, 409, media.error); return; }
        if (Value(r, "keep") == "1" && r.method == "GET" && start == 0) {
            // Kept for offline listening: from now on this browser may report plays of it.
            std::lock_guard lock(mutex);
            const uint64_t now = (uint64_t)std::time(nullptr);
            // The first download counts: plays from then on stay creditable when it is kept again.
            if (std::none_of(grants.begin(), grants.end(), [&](const Grant& g) { return g.device == c.device && g.key == track->key; })) {
                if (grants.size() >= 20000) grants.erase(grants.begin());
                grants.push_back({c.device, track->key, track->path, now});
                SaveOffline();
            }
        }
        ByteRange range;
        if (!ParseRange(r.headers["range"], media.size, range)) {
            Body(c, r, 416, "", media.mime, "Content-Range: bytes */" + std::to_string(media.size) + "\r\n"); return;
        }
        std::string extra = "Accept-Ranges: bytes\r\n";
        if (range.partial) extra += "Content-Range: bytes " + std::to_string(range.first) + '-' +
            std::to_string(range.first + range.size - 1) + '/' + std::to_string(media.size) + "\r\n";
        if (Response(c, range.partial ? 206 : 200, media.mime, range.size, extra) && r.method != "HEAD")
            media.send(range.first, (size_t)range.size, [&](const char* bytes, size_t n) { return Send(c, bytes, n); });
    }
    // Plays a browser made of kept tracks while it couldn't reach cordinal, sent when it can again:
    // "id_key_end_seconds_manual" records joined by '*'. Each is counted once, only for a track
    // this browser downloaded to keep, ending between that download and now, and no longer than
    // the time since. The rest are refused without failing the batch.
    void Played(Connection& c, const Request& r, const Snapshot& data) {
        Request form;
        if (!Fields(r, form)) { Error(c, r, 400, "Invalid offline plays."); return; }
        const std::string all = Value(form, "plays");
        size_t accepted = 0, refused = 0;
        {
            std::lock_guard lock(mutex);
            const uint64_t now = (uint64_t)std::time(nullptr);
            for (size_t at = 0; at < all.size();) {
                size_t end = all.find('*', at); if (end == all.npos) end = all.size();
                std::vector<std::string> f;
                for (size_t i = at; i <= end;) { size_t sep = std::min(all.find('_', i), end); f.push_back(all.substr(i, sep - i)); i = sep + 1; }
                at = end + 1;
                char* last = nullptr;
                const double seconds = f.size() == 5 ? strtod(f[3].c_str(), &last) : -1;
                const uint64_t ended = f.size() == 5 ? Integer(f[2]) : 0;
                auto g = f.size() != 5 ? grants.end() : std::find_if(grants.begin(), grants.end(), [&](const Grant& g) {
                    return g.device == c.device && g.key == f[1];
                });
                auto known = data.byKey.find(f.size() == 5 ? f[1] : std::string());
                const double length = known != data.byKey.end() ? data.tracks[known->second].duration : 86400;
                bool ok = g != grants.end() && IsHex(f[0], 32) && last == f[3].c_str() + f[3].size() && std::isfinite(seconds) &&
                          seconds >= 0.5 && seconds <= length + 10 && ended <= now + 120 && ended + 120 >= g->time + (uint64_t)seconds &&
                          (f[4] == "0" || f[4] == "1") && std::find(playedIds.begin(), playedIds.end(), f[0]) == playedIds.end();
                if (!ok) { ++refused; continue; }
                ++accepted;
                playedIds.push_back(f[0]);
                if (playedIds.size() > 4096) playedIds.pop_front();
                listens.push_back({g->path, seconds, f[4] == "1", ended});
            }
            if (accepted) SaveOffline();
        }
        Body(c, r, 200, "{\"accepted\":" + std::to_string(accepted) + ",\"refused\":" + std::to_string(refused) + '}');
        if (accepted) Notify();
    }
    void Listen(Connection& c, const Request& r, const Snapshot& data) {
        Request form;
        if (!Fields(r, form)) { Error(c, r, 400, "Invalid listening event."); return; }
        std::string session = Value(form, "session"), event = Value(form, "event"), id = Value(form, "id");
        uint64_t sequence = Integer(Value(form, "seq"));
        std::string heardText = Value(form, "heard"); char* end = nullptr;
        double heard = strtod(heardText.c_str(), &end);
        if (!IsHex(session, 32) || !sequence || heardText.empty() || end != heardText.c_str() + heardText.size() ||
            !std::isfinite(heard) || heard < 0 || heard > 1e9 ||
            (event != "start" && event != "progress" && event != "pause" && event != "resume" &&
             event != "seek" && event != "ended" && event != "skip")) {
            Error(c, r, 400, "Invalid listening event."); return;
        }
        int code = 200;
        {
            std::lock_guard lock(mutex);
            std::string key = c.device + session;
            auto it = listening.find(key);
            uint64_t now = ClockMs();
            if (event == "start") {
                auto t = std::find_if(data.tracks.begin(), data.tracks.end(), [&](const Track& t) { return t.key == id; });
                if (finished.count(key)) code = 409;
                else if (t == data.tracks.end()) code = 404;
                else if (it == listening.end() && listening.size() < 128)
                    listening[key] = {c.device, t->path, sequence, now, now, 0, 0, 0.25, true};
                else if (it == listening.end()) code = 503;
            } else if (it != listening.end() && sequence > it->second.sequence) {
                auto& s = it->second;
                if (s.playing) s.budget += (now - s.tick) / 1000.0;
                s.tick = s.seen = now; s.sequence = sequence;
                double delta = std::max(0.0, heard - s.reported);
                s.heard += std::min(delta, s.budget);
                s.budget = std::max(0.0, s.budget - delta);
                s.reported = std::max(s.reported, heard);
                if (event == "pause") s.playing = false;
                if (event == "resume") s.playing = true;
                if (event == "ended" || event == "skip") Finish(it, event == "skip");
            } else if (it == listening.end() && !finished.count(key)) code = 409;
        }
        if (code == 200) Body(c, r, 200, "{}"); else Error(c, r, code, "The listening session could not be started.");
        Notify();
    }
};

Server::Server() : p_(std::make_unique<Impl>()) {}
Server::~Server() { Stop(); }
bool Server::Start(const Config& config) {
    Stop();
    auto& p = *p_;
    p.config = config; p.status = {}; p.paired.clear(); p.attempts = 0; p.pairingUntil = 0;
    p.pairingLinks.clear(); p.rebuild = true;
    auto fail = [&](const char* message) { Stop(); p.status.error = message; return false; };
    if (!p.Load()) return fail("Couldn't read paired devices. The existing file was preserved.");
    p.LoadOffline();
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return fail("Couldn't start Windows networking.");
    p.sockets = true;
#endif
    Socket main = p.Listen(config.port, config.port == 0);
    if (main == kBad) return fail("Couldn't listen on this port. Choose another port or stop the other server.");
    sockaddr_storage address{};
#ifdef _WIN32
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    getsockname(main, (sockaddr*)&address, &size);
    p.status.port = ntohs(address.ss_family == AF_INET6 ? ((sockaddr_in6*)&address)->sin6_port : ((sockaddr_in*)&address)->sin_port);
    p.listeners = {{main, p.status.port}};
    p.Refresh();
    p.cookieName = "cordinal_" + std::to_string(p.status.port);
    p.status.running = true; p.stop = false;
    p.OpenFixed();
    try {
        for (int i = 0; i < 8; ++i) p.workers.emplace_back([&p] { p.Worker(); });
        p.listener = std::thread([&p] { p.Accept(); });
    } catch (...) { Stop(); return fail("Couldn't start the server workers."); }
    p.Notify(); return true;
}
bool Server::LoadDevices(const std::wstring& path) {
    auto& p = *p_; std::lock_guard lock(p.mutex);
    if (p.status.running) return false;
    auto original = std::move(p.paired);
    p.paired.clear(); p.config.devicesPath = path;
    if (p.Load()) return true;
    p.paired = std::move(original); p.status.error = "Couldn't read paired devices. The existing file was preserved.";
    return false;
}
void Server::Stop() {
    auto& p = *p_;
    p.stop = true;
    {
        std::lock_guard lock(p.mutex);
        for (auto& c : p.connections) { c->cancel = true; Shutdown(c->socket); }
    }
    p.cv.notify_all();
    if (p.listener.joinable()) p.listener.join();
    for (auto& t : p.workers) if (t.joinable()) t.join();
    p.workers.clear(); p.pending.clear();
    { std::lock_guard lock(p.audioCache.mutex); p.audioCache.key.clear(); p.audioCache.bytes.reset(); }
    for (const auto& l : p.listeners) Close(l.socket);
    p.listeners.clear();
    {
        std::lock_guard lock(p.mutex);
        for (auto& c : p.connections) if (c->socket != kBad) { Close(c->socket); c->socket = kBad; }
        p.connections.clear();
    }
#ifdef _WIN32
    if (p.sockets) { WSACleanup(); p.sockets = false; }
#endif
    {
        std::lock_guard lock(p.mutex);
        while (!p.listening.empty()) p.Finish(p.listening.begin(), false);
        p.status.running = false; p.status.pairingCode.clear(); p.pairingUntil = 0; p.pairingLinks.clear();
        p.status.ports.clear(); p.status.portError.clear();
        p.waveforms.clear(); p.preparing.reset(); p.snapshot = std::make_shared<Snapshot>();
    }
}
Status Server::GetStatus() const {
    auto& p = *p_; std::lock_guard lock(p.mutex);
    Status status = p.status; status.streams = p.streams;
    uint64_t now = ClockMs();
    bool open = p.pairingUntil > now && p.attempts < 5;
    status.fixedCode = !p.config.fixedCode.empty();
    status.pairingSeconds = open && p.pairingUntil != UINT64_MAX ? (p.pairingUntil - now + 999) / 1000 : 0;
    if (!open) status.pairingCode.clear();
    if (p.status.running && p.away.identity) status.away = p.away.url;
    for (const auto& d : p.paired) status.devices.push_back({d.id, d.name, d.seen ? (now - d.seen) / 1000 + 1 : 0, d.away});
    return status;
}
bool Server::Pair() {
    auto& p = *p_; std::lock_guard lock(p.mutex);
    if (!p.status.running) return false;
    if (!p.config.fixedCode.empty()) { p.OpenFixed(); return true; }
    auto random = RandomHex(4); if (random.empty()) return false;
    char code[7]; snprintf(code, sizeof(code), "%06lu", strtoul(random.c_str(), nullptr, 16) % 1000000ul);
    p.status.pairingCode = code; p.pairingUntil = ClockMs() + 300000; p.attempts = 0;
    return true;
}
std::string Server::PairingLink() {
    auto& p = *p_; std::lock_guard lock(p.mutex);
    return p.status.running ? p.NewLink({}) : std::string{};
}
void Server::SetFixedCode(const std::string& code) {
    auto& p = *p_; std::lock_guard lock(p.mutex);
    if (code == p.config.fixedCode) return;
    bool wasFixed = !p.config.fixedCode.empty() && p.status.pairingCode == p.config.fixedCode;
    p.config.fixedCode = code;
    if (!code.empty()) p.OpenFixed();
    else if (wasFixed) { p.status.pairingCode.clear(); p.pairingUntil = 0; }
}
const char* CheckFixedCode(std::string_view text, std::string& code) {
    code.clear();
    for (char c : text) {
        if (c == ' ' || c == '-') continue;
        if (c < '0' || c > '9') return "Use only digits for the fixed code.";
        code += c;
    }
    if (code.empty()) return nullptr;
    if (code.size() < 6 || code.size() > 12) return "Use 6 to 12 digits for the fixed code.";
    // These are the codes a guesser tries first, and a fixed code stays open while the server runs.
    bool same = true, up = true, down = true;
    for (size_t i = 1; i < code.size(); ++i) {
        same = same && code[i] == code[0];
        up = up && code[i] == '0' + (code[i - 1] - '0' + 1) % 10;
        down = down && code[i] == '0' + (code[i - 1] - '0' + 9) % 10;
    }
    if (same || up || down) return "Choose a code that is harder to guess than a repeated digit or a run like 123456.";
    return nullptr;
}
void Server::Revoke(const std::string& device) {
    auto& p = *p_; std::lock_guard lock(p.mutex);
    auto original = p.paired;
    std::erase_if(p.paired, [&](const auto& d) { return device.empty() || d.id == device; });
    if (!p.Save()) { p.paired = std::move(original); p.status.error = "Couldn't save device removal. Access was preserved."; return; }
    p.Forget(device);
    for (const auto& c : p.connections) if (device.empty() || c->device == device) { c->cancel = true; Shutdown(c->socket); }
    for (auto it = p.listening.begin(); it != p.listening.end();) {
        if (device.empty() || it->second.device == device) { auto old = it++; p.Finish(old, false); } else ++it;
    }
}
void Server::Publish(std::shared_ptr<Snapshot> snapshot) {
    { std::lock_guard lock(p_->mutex); snapshot->serial = ++p_->publishSequence; p_->preparing = std::move(snapshot); }
    p_->cv.notify_one();
}
void Server::SetTheme(ThemeMode theme, bool systemDark) {
    std::lock_guard lock(p_->mutex); p_->theme = theme; p_->systemDark = systemDark;
}
void Server::SetAway(Away away) {
    auto& p = *p_; std::lock_guard lock(p.mutex);
    p.hosts.insert(away.hosts.begin(), away.hosts.end());  // the rest of the refresh follows on the listener thread
    p.away = std::move(away); p.rebuild = true;
}
void Server::TreatLoopbackAsAwayForTest(bool away) {
    std::lock_guard lock(p_->mutex); p_->loopbackAway = away;
}
std::vector<ListenEvent> Server::TakeListens() {
    std::lock_guard lock(p_->mutex); std::vector<ListenEvent> result; result.swap(p_->listens); return result;
}
}  // namespace web
