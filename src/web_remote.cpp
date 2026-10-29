// The away-from-home manager (web_remote.h): one thread keeps the router's ports, the internet
// address and the Let's Encrypt certificate current, and hands them to the server. It checks
// every twenty minutes, renews at half the certificate's life and backs off after failures.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#else
#include <arpa/inet.h>
#endif
#include "web_remote.h"
#include "web_internal.h"
#include "web_tls.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>

namespace web {
namespace {
#ifdef _WIN32
constexpr const wchar_t* kSlash = L"\\";
#else
constexpr const wchar_t* kSlash = L"/";
#endif
uint64_t Now() { return (uint64_t)time(nullptr); }
std::string Pem(const std::vector<std::string>& chain) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (const auto& der : chain) {
        std::string text;
        for (size_t i = 0; i < der.size(); i += 3) {
            size_t n = std::min<size_t>(3, der.size() - i);
            uint32_t v = 0;
            for (size_t k = 0; k < 3; ++k) v = v << 8 | (k < n ? (uint8_t)der[i + k] : 0);
            for (size_t k = 0; k < 4; ++k) text += k <= n ? table[(v >> (18 - 6 * k)) & 63] : '=';
        }
        out += "-----BEGIN CERTIFICATE-----\r\n";
        for (size_t at = 0; at < text.size(); at += 64) out += text.substr(at, 64) + "\r\n";
        out += "-----END CERTIFICATE-----\r\n";
    }
    return out;
}
void Pause(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
std::string Host(const std::string& address, uint16_t port) {
    return (address.find(':') != address.npos ? "[" + address + "]" : address) + ':' + std::to_string(port);
}
}  // namespace

struct Remote::Impl {
    using State = RemoteStatus::State;
    Server& server;
    RemoteConfig config;
    mutable std::mutex mutex;
    std::condition_variable wake;
    RemoteStatus status;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<void*> session{nullptr};  // HINTERNET; Stop closes it to cancel a request in flight
    bool retry = false;
    // The rest belongs to the thread.
    Gateway gateway;
    std::vector<uint16_t> mapped;
    std::shared_ptr<TlsIdentity> identity;
    uint64_t issued = 0, expires = 0, backoff = 0;
    unsigned failures = 0;
    uint16_t httpsLocal = 0, httpLocal = 0;
    std::string address, local;
    std::vector<std::pair<std::string, std::string>> challenges;

    explicit Impl(Server& s) : server(s) {}
    void* Session() const { return session.load(); }
    bool Covered() const {
        return identity && !address.empty() &&
            std::find(identity->Names().begin(), identity->Names().end(), address) != identity->Names().end();
    }
    std::string Url() const {
        std::string host = address.find(':') != address.npos ? "[" + address + "]" : address;
        return "https://" + host + (config.httpsPort == 443 ? "" : ':' + std::to_string(config.httpsPort)) + '/';
    }
    // The router step in words, for when the ports have to be forwarded by hand.
    std::string Forwarding() const {
        if (httpsLocal == config.httpsPort && httpLocal == config.httpPort)
            return "forward TCP ports " + std::to_string(config.httpPort) + " and " + std::to_string(config.httpsPort) +
                " to " + local;
        return "forward TCP port " + std::to_string(config.httpsPort) + " to " + local + " port " + std::to_string(httpsLocal) +
            ", and port " + std::to_string(config.httpPort) + " to " + local + " port " + std::to_string(httpLocal);
    }
    void Set(State state, std::string message) {
        {
            std::lock_guard lock(mutex);
            status.state = state; status.message = std::move(message);
            status.address = address; status.local = local;
            status.url = Covered() ? Url() : std::string{};
            status.expires = Covered() ? expires : 0;
        }
        if (config.notify) config.notify();
    }
    void Publish() {
        Away away;
        away.ports = {config.httpsPort, config.httpPort};
        if (!address.empty()) {
            away.hosts = {Host(address, config.httpsPort), Host(address, config.httpPort)};
            away.url = Url();
        }
        if (Covered()) away.identity = identity;
        away.challenges = challenges;
        server.SetAway(std::move(away));
    }
    void Ready() {
        Publish();
        std::string note;
        if (config.loopback) {
            bool back = HttpFetch(Session(), Url() + "manifest.webmanifest", "GET", {}, {}, 4000).status == 200;
            { std::lock_guard lock(mutex); status.loopbackKnown = true; status.loopback = back; }
            if (!back) note = " Your router doesn't connect this address back home, so at home pair with the code, or "
                "scan with the phone's Wi-Fi off.";
        }
        Set(State::Ready, "Ready. Away from home, open " + Url() + " or scan a pairing code. The certificate renews "
            "itself." + note);
    }
    void LoadCertificate() {
        std::string pem, error;
        if (!ReadRecords(config.dataDir + kSlash + L"web-certificate.bin", pem) || pem.empty()) return;
        auto chain = PemCertificates(pem);
        uint64_t from = 0, until = 0;
        std::vector<std::string> names;
        if (chain.empty() || !CertificateInfo(chain.front(), from, until, names) || names.empty() || until <= Now() + 3600) return;
        if (auto loaded = TlsIdentity::Create(chain, config.keyName, error)) {
            identity = loaded; issued = from; expires = until;
            if (address.empty()) address = names.front();  // serve at once; the check confirms it
        }
    }
    void Fail(State state, std::string message, uint64_t wait = 0) {
        ++failures;
        uint64_t delay = std::min<uint64_t>(6 * 3600, 600ull << std::min(2 * (failures - 1), 10u));
        backoff = Now() + std::max(delay, wait);
        Set(state, std::move(message));
    }
    void Check() {
        std::string route;
        DefaultRoute(local, route);
        if (!Covered()) Set(State::Working, "Finding this network's internet address...");
        // The router can open the ports and say the address; a STUN server can only say the address.
        std::string found = config.address, seen;
        bool upnp = false;
        if (found.empty() && config.router && !local.empty()) {
            if (gateway.control.empty() || gateway.local != local) {
                gateway = {};
                for (const auto& location : DiscoverGateways(local, 1500)) {
                    Gateway candidate;
                    candidate.local = local;
                    if (ReadGateway(Session(), location, candidate)) { gateway = candidate; break; }
                }
            }
            if (!gateway.control.empty()) {
                found = GatewayAddress(Session(), gateway);
                upnp = !found.empty();
                if (!upnp) gateway = {};
            }
        }
        if (found.empty() || !PublicIPv4(found))
            for (const auto& [host, port] : config.stun) if (stop || !(seen = StunAddress(host, port, 1500)).empty()) break;
        if (stop) return;
        // A router whose own outside address is 100.64.0.0/10 sits behind the provider's shared NAT;
        // any other private one has a second router of the household's in front of it.
        in_addr outside{};
        bool shared = inet_pton(AF_INET, found.c_str(), &outside) == 1 && ((const uint8_t*)&outside)[0] == 100 &&
            (((const uint8_t*)&outside)[1] & 0xC0) == 64;
        bool twoRouters = false;
        if (found.empty()) found = seen;
        else if (upnp && !shared && !PublicIPv4(found) && PublicIPv4(seen)) { twoRouters = true; found = seen; }
        if (found.empty()) {
            Set(State::Failed, "Couldn't find this network's internet address. Check the internet connection, then choose Check again.");
            return;
        }
        if (!PublicIPv4(found)) {
            Set(State::Shared, "Your internet provider shares this network's address (" + found + ") with other homes, so "
                "nothing outside can reach this computer directly. Ask the provider for a public IPv4 address, or use a "
                "VPN such as Tailscale and open the address it gives this computer.");
            return;
        }
        address = found;
        // The server listens on the outside ports itself when it can, so a plain forward works too.
        Publish();
        std::vector<uint16_t> ports;
        auto has = [&](uint16_t port) { return std::find(ports.begin(), ports.end(), port) != ports.end(); };
        for (int i = 0; i < 10 && !stop; ++i) {
            auto s = server.GetStatus();
            ports = s.ports;
            if ((has(config.httpsPort) && has(config.httpPort)) || !s.portError.empty()) break;
            Pause(100);
        }
        uint16_t main = ports.empty() ? 0 : ports.front();
        httpsLocal = has(config.httpsPort) ? config.httpsPort : main;
        httpLocal = has(config.httpPort) ? config.httpPort : main;
        bool opened = false, taken = false;
        int refused = 0;  // the router's error for a port it wouldn't open
        if (upnp && main) {
            for (auto [port, inside] : {std::pair{config.httpsPort, httpsLocal}, std::pair{config.httpPort, httpLocal}}) {
                int code = MapPort(Session(), gateway, port, inside, 3600);
                bool ours = std::find(mapped.begin(), mapped.end(), port) != mapped.end();
                if (code == 718 && ours) code = 0;  // some routers call renewing our own mapping a conflict
                if (!code && !ours) mapped.push_back(port);
                if (code == 718) taken = true;
                else if (code) refused = code;
            }
            opened = !taken && !refused;
        }
        { std::lock_guard lock(mutex); status.router = opened; }
        if (Covered() && Now() < issued + (expires - issued) / 2) { Ready(); return; }
        if (Now() < backoff) { Publish(); return; }  // the last failure's message stays until the next try
        Set(State::Working, "Getting a certificate from Let's Encrypt for " + address + "...");
        std::string error;
        if (!EnsureTlsKey(config.keyName, error)) { Fail(State::Failed, error); return; }
        AcmeRequest request;
        request.directory = config.directory;
        request.accountPath = config.dataDir + kSlash + L"web-account.bin";
        request.keyName = config.keyName;
        request.names = {address};
        request.serve = [&](const std::vector<std::pair<std::string, std::string>>& answers) { challenges = answers; Publish(); };
        request.session = Session();
        request.cancel = &stop;
        auto result = RequestCertificate(request);
        if (stop) return;
        if (result.chain.empty()) {
            const std::string& p = result.problem;
            bool unreachable = p == "connection" || p == "unauthorized" || p == "incorrectResponse" || p == "tls" ||
                p == "dns" || result.error.starts_with("Let's Encrypt couldn't reach");
            if (p == "rateLimited")
                Fail(State::Failed, "Let's Encrypt asked cordinal to wait before asking again. It will try again by itself later.",
                     result.retryAfter);
            else if (unreachable && taken)
                Fail(State::Forward, "Your router already forwards port " + std::to_string(config.httpsPort) + " or " +
                     std::to_string(config.httpPort) + " to another device. Remove that rule in the router's settings, then "
                     "choose Check again.");
            else if (unreachable && twoRouters)
                Fail(State::Forward, "There are two routers between this computer and the internet. On the one that connects "
                     "to the internet, forward TCP ports " + std::to_string(config.httpPort) + " and " +
                     std::to_string(config.httpsPort) + " to the other router, then choose Check again.");
            else if (unreachable && opened)
                Fail(State::Failed, "Your router opened the ports, but Let's Encrypt still couldn't reach this computer at " +
                     address + ". Your internet provider may block incoming connections.");
            else if (unreachable && refused)
                Fail(State::Forward, "Your router didn't open the ports automatically (UPnP error " + std::to_string(refused) +
                     "). In its settings, " + Forwarding() + ", then choose Check again.");
            else if (unreachable)
                Fail(State::Forward, "Let's Encrypt couldn't reach this computer at " + address + ". In your router's settings, "
                     "turn on UPnP, or " + Forwarding() + ". Then choose Check again.");
            else Fail(State::Failed, result.error);
            return;
        }
        auto fresh = TlsIdentity::Create(result.chain, config.keyName, error);
        uint64_t from = 0, until = 0;
        std::vector<std::string> names;
        if (!fresh || !CertificateInfo(result.chain.front(), from, until, names)) {
            Fail(State::Failed, error.empty() ? "The new certificate can't be read." : error);
            return;
        }
        identity = fresh; issued = from; expires = until; failures = 0; backoff = 0;
        SaveRecords(config.dataDir + kSlash + L"web-certificate.bin", Pem(result.chain));
        Ready();
    }
    void Run() {
#ifdef _WIN32
        WSADATA wsa{};
        bool sockets = !WSAStartup(MAKEWORD(2, 2), &wsa);
        session = WinHttpOpen(L"cordinal", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#else
        session = (void*)&stop;  // Linux's HttpFetch takes the session as the flag that cancels it
#endif
        LoadCertificate();
        Publish();
        while (!stop) {
            Check();
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::minutes(20), [&] { return stop || retry; });
            if (retry) { retry = false; backoff = 0; failures = 0; }
        }
        // Leave the router as it was. A fresh session: Stop closed the working one.
        if (!mapped.empty() && !gateway.control.empty()) {
#ifdef _WIN32
            HINTERNET cleanup = WinHttpOpen(L"cordinal", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                            WINHTTP_NO_PROXY_BYPASS, 0);
            for (uint16_t port : mapped) UnmapPort(cleanup, gateway, port);
            if (cleanup) WinHttpCloseHandle(cleanup);
#else
            for (uint16_t port : mapped) UnmapPort(nullptr, gateway, port);
#endif
        }
        server.SetAway({});
#ifdef _WIN32
        if (void* s = session.exchange(nullptr)) WinHttpCloseHandle((HINTERNET)s);
        if (sockets) WSACleanup();
#else
        session = nullptr;
#endif
    }
};

Remote::Remote(Server& server) : p_(std::make_unique<Impl>(server)) {}
Remote::~Remote() { Stop(); }
void Remote::Start(const RemoteConfig& config) {
    Stop();
    auto& p = *p_;
    p.config = config; p.stop = false; p.retry = false;
    p.gateway = {}; p.mapped.clear(); p.identity.reset(); p.issued = p.expires = p.backoff = 0; p.failures = 0;
    p.address.clear(); p.challenges.clear();
    { std::lock_guard lock(p.mutex); p.status = {}; p.status.state = RemoteStatus::State::Working; p.status.message = "Starting..."; }
    p.thread = std::thread([&p] { p.Run(); });
}
void Remote::Stop() {
    auto& p = *p_;
    if (!p.thread.joinable()) return;
    { std::lock_guard lock(p.mutex); p.stop = true; }
    p.wake.notify_all();
#ifdef _WIN32
    if (void* s = p.session.exchange(nullptr)) WinHttpCloseHandle((HINTERNET)s);
#endif
    p.thread.join();
    std::lock_guard lock(p.mutex);
    p.status = {};
}
void Remote::Retry() {
    { std::lock_guard lock(p_->mutex); p_->retry = true; }
    p_->wake.notify_all();
}
bool Remote::Running() const { return p_->thread.joinable(); }
RemoteStatus Remote::GetStatus() const {
    std::lock_guard lock(p_->mutex);
    return p_->status;
}
}  // namespace web
