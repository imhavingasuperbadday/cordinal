// Away from home: reaching the web server from other networks. The router is asked to
// forward the ports (UPnP), the internet address comes from the router or a STUN server, and
// Let's Encrypt certifies that address, answered through the server's own HTTP challenge.
#pragma once
#include "web_server.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace web {

constexpr const char* kLetsEncrypt = "https://acme-v02.api.letsencrypt.org/directory";

// One HTTP or HTTPS request. On Windows `session` (a HINTERNET) may be shared so another thread can
// cancel it by closing it, and null opens a private session; on Linux it is a const std::atomic<bool>*
// that cancels the request when set, or null. Header names in the reply are lowercase.
struct HttpReply {
    unsigned status = 0;  // 0 when the request didn't complete
    std::string body;
    std::map<std::string, std::string> headers;
};
HttpReply HttpFetch(void* session, const std::string& url, const char* method, const std::string& body = {},
                    const std::string& headers = {}, unsigned timeoutMs = 10000);

// A small JSON reader for ACME replies: objects, arrays, strings, numbers and literals.
struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
    std::string text;  // a string's value, or a number's or literal's spelling
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;
    const JsonValue& operator[](std::string_view key) const;
    const std::string& String(std::string_view key) const { return (*this)[key].text; }
};
bool ParseJson(std::string_view text, JsonValue& value);
std::string Base64Url(std::string_view bytes);

// ACME (RFC 8555) with an ES256 account key kept private (ReadRecords) in `accountPath`.
struct AcmeRequest {
    std::string directory = kLetsEncrypt;
    std::wstring accountPath, keyName;   // keyName: the key the certificate is issued for (web_tls.h)
    std::vector<std::string> names;      // IP addresses or DNS names
    std::string profile = "shortlived";  // asked for when the directory offers it (required for IPs)
    // Publishes the http-01 answers (token, key authorization) on port 80; called with none to clear.
    std::function<void(const std::vector<std::pair<std::string, std::string>>&)> serve;
    void* session = nullptr;  // HttpFetch session, closed by another thread to cancel
    const std::atomic<bool>* cancel = nullptr;
};
struct AcmeResult {
    std::vector<std::string> chain;  // DER certificates, the server's first; empty on failure
    std::string problem;             // the ACME error type without its prefix ("connection", ...)
    std::string error;               // what went wrong, in words
    uint64_t retryAfter = 0;         // seconds the CA asked us to wait, when it said so
};
AcmeResult RequestCertificate(const AcmeRequest& request);

// UPnP Internet Gateway Device control. `local` is this computer's address toward the router.
struct Gateway {
    std::string control, service, local;
};
std::vector<std::string> DiscoverGateways(const std::string& local, unsigned ms);  // SSDP LOCATION urls
bool ReadGateway(void* session, const std::string& location, Gateway& gateway);
std::string GatewayAddress(void* session, const Gateway& gateway);
// Returns 0 or the UPnP error code; a router that only keeps permanent mappings gets one with lease 0.
int MapPort(void* session, const Gateway& gateway, uint16_t external, uint16_t internal, unsigned lease);
bool UnmapPort(void* session, const Gateway& gateway, uint16_t external);
std::string SsdpLocation(std::string_view reply);  // the LOCATION header of an M-SEARCH reply
// STUN (RFC 8489): the public address a STUN server sees for this network, "" when none answers.
std::string StunAddress(const std::string& host, uint16_t port, unsigned ms);
bool ParseStun(std::string_view reply, const uint8_t transaction[12], std::string& address);
// This computer's IPv4 address on its route to the internet, and that route's gateway.
bool DefaultRoute(std::string& local, std::string& gateway);
bool PublicIPv4(const std::string& address);

struct RemoteConfig {
    uint16_t httpsPort = 443, httpPort = 80;  // the ports browsers and Let's Encrypt use from outside
    std::wstring dataDir;                     // the account and certificate files live here
    std::wstring keyName;                     // the key the certificate is issued for (web_tls.h)
    std::string directory = kLetsEncrypt;
    std::vector<std::pair<std::string, uint16_t>> stun{{"stun.cloudflare.com", (uint16_t)3478},
                                                       {"stun.l.google.com", (uint16_t)19302}};
    std::function<void()> notify;
    std::string address;      // checks: use this public address instead of looking it up
    bool router = true;       // checks: false skips UPnP
    bool loopback = true;     // checks: false skips trying the internet address from home
};
struct RemoteStatus {
    enum class State { Off, Working, Ready, Forward, Shared, Failed } state = State::Off;
    std::string url;      // https://address/ once a certificate is in place
    std::string address;  // the internet address, when known
    std::string local;    // this computer's address at home, for forwarding instructions
    bool router = false;  // the router opened the ports itself
    // Whether the router takes its own internet address back home (NAT loopback), so a phone at
    // home can use the same secure address; unknown until the address is ready.
    bool loopbackKnown = false, loopback = false;
    uint64_t expires = 0; // the certificate's expiry, Unix seconds
    std::string message;  // what is happening or what to do, in words
};
// Keeps the server reachable away from home: ports, address, certificate and renewals, on its own
// thread. Stop removes the router mappings it made and the server's away settings.
class Remote {
public:
    explicit Remote(Server& server);
    ~Remote();
    Remote(const Remote&) = delete;
    Remote& operator=(const Remote&) = delete;
    void Start(const RemoteConfig& config);
    void Stop();
    void Retry();
    bool Running() const;
    RemoteStatus GetStatus() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace web
