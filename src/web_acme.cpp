// Let's Encrypt for the web server (web_remote.h): a JSON reader, ES256 signing with an account key
// kept private (web_tls.h), and one certificate order from account to downloaded chain (RFC 8555).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include "web_remote.h"
#include "web_internal.h"
#include "web_tls.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace web {
namespace {
struct Parser {
    std::string_view s;
    size_t at = 0;
    int depth = 0;
    void Space() { while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) ++at; }
    bool Take(char c) { Space(); if (at < s.size() && s[at] == c) { ++at; return true; } return false; }
    bool Text(std::string& out) {
        if (!Take('"')) return false;
        while (at < s.size()) {
            char c = s[at++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (at >= s.size()) return false;
            char e = s[at++];
            if (e == 'u') {
                auto hex = [&](uint32_t& v) {
                    if (at + 4 > s.size()) return false;
                    v = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s[at++];
                        int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                        if (d < 0) return false;
                        v = v * 16 + (uint32_t)d;
                    }
                    return true;
                };
                uint32_t cp = 0, low = 0;
                if (!hex(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && s.substr(at, 2) == "\\u") {
                    at += 2;
                    if (!hex(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 63)); }
                else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
                else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 63)); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
            } else out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
        }
        return false;
    }
    bool Value(JsonValue& v) {
        Space();
        if (at >= s.size() || ++depth > 32) return false;
        bool ok = false;
        char c = s[at];
        if (c == '{') {
            v.kind = JsonValue::Kind::Object; ++at;
            if (Take('}')) ok = true;
            else for (;;) {
                std::string key;
                JsonValue member;
                if (!Text(key) || !Take(':') || !Value(member)) break;
                v.members.emplace_back(std::move(key), std::move(member));
                if (Take('}')) { ok = true; break; }
                if (!Take(',')) break;
            }
        } else if (c == '[') {
            v.kind = JsonValue::Kind::Array; ++at;
            if (Take(']')) ok = true;
            else for (;;) {
                JsonValue item;
                if (!Value(item)) break;
                v.items.push_back(std::move(item));
                if (Take(']')) { ok = true; break; }
                if (!Take(',')) break;
            }
        } else if (c == '"') {
            v.kind = JsonValue::Kind::String; ok = Text(v.text);
        } else {
            size_t start = at;
            while (at < s.size() && s[at] && strchr("+-.0123456789eEtrufalsn", s[at])) ++at;
            v.text = std::string(s.substr(start, at - start));
            v.kind = v.text == "true" || v.text == "false" ? JsonValue::Kind::Bool : v.text == "null" ? JsonValue::Kind::Null :
                JsonValue::Kind::Number;
            ok = !v.text.empty() && (v.kind != JsonValue::Kind::Number || strchr("-0123456789", v.text[0]));
        }
        --depth;
        return ok;
    }
};

struct Client {
    const AcmeRequest& request;
    AccountKey key;
    std::string newNonce, nonce, kid;
    bool Cancelled() const { return request.cancel && *request.cancel; }
    void Wait(unsigned ms) const { for (unsigned waited = 0; waited < ms && !Cancelled(); waited += 100) std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    HttpReply Post(const std::string& url, const std::string& payload, bool asGet = false, const std::string& accept = {}) {
        HttpReply reply;
        for (int attempt = 0; attempt < 3 && !Cancelled(); ++attempt) {
            if (nonce.empty()) nonce = HttpFetch(request.session, newNonce, "HEAD").headers["replay-nonce"];
            if (nonce.empty()) return reply;
            std::string header = "{\"alg\":\"ES256\"," + (kid.empty() ? "\"jwk\":" + key.Jwk() : "\"kid\":" + Json(kid)) +
                ",\"nonce\":" + Json(nonce) + ",\"url\":" + Json(url) + '}';
            std::string signed64 = Base64Url(header) + '.' + (asGet ? std::string{} : Base64Url(payload));
            std::string signature = key.Sign(signed64);
            if (signature.empty()) return reply;
            size_t dot = signed64.find('.');
            std::string body = "{\"protected\":\"" + signed64.substr(0, dot) + "\",\"payload\":\"" + signed64.substr(dot + 1) +
                "\",\"signature\":\"" + Base64Url(signature) + "\"}";
            reply = HttpFetch(request.session, url, "POST", body, "Content-Type: application/jose+json\r\n" + accept, 15000);
            nonce = reply.headers["replay-nonce"];
            JsonValue problem;
            if (reply.status == 400 && ParseJson(reply.body, problem) && problem.String("type").ends_with(":badNonce")) continue;
            break;
        }
        return reply;
    }
};
unsigned RetryAfter(const HttpReply& reply, unsigned fallback) {
    auto it = reply.headers.find("retry-after");
    unsigned seconds = it == reply.headers.end() ? 0 : (unsigned)strtoul(it->second.c_str(), nullptr, 10);
    return seconds ? std::min(seconds, 30u) * 1000 : fallback;
}
}  // namespace

const JsonValue& JsonValue::operator[](std::string_view key) const {
    static const JsonValue none;
    for (const auto& [name, value] : members) if (name == key) return value;
    return none;
}
bool ParseJson(std::string_view text, JsonValue& value) {
    Parser parser{text};
    value = {};
    if (!parser.Value(value)) return false;
    parser.Space();
    return parser.at == text.size();
}
std::string Base64Url(std::string_view bytes) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    uint32_t pending = 0;
    int bits = 0;
    for (unsigned char c : bytes) {
        pending = pending << 8 | c; bits += 8;
        while (bits >= 6) { bits -= 6; out += table[(pending >> bits) & 63]; }
    }
    if (bits) out += table[(pending << (6 - bits)) & 63];
    return out;
}

AcmeResult RequestCertificate(const AcmeRequest& request) {
    AcmeResult result;
    Client client{request};
    // Fails with `what`, adding the CA's own explanation when the reply carries an ACME problem.
    auto fail = [&](const std::string& what, const HttpReply* reply = nullptr, const JsonValue* problem = nullptr) {
        JsonValue parsed;
        if (!problem && reply && ParseJson(reply->body, parsed)) problem = &parsed;
        std::string type = problem ? problem->String("type") : "", detail = problem ? problem->String("detail") : "";
        result.problem = type.substr(type.rfind(':') + 1);
        result.error = what + (detail.empty() ? "" : " Let's Encrypt said: " + detail);
        if (reply) result.retryAfter = RetryAfter(*reply, 0) / 1000;
        if (client.Cancelled()) result.error = "Stopped.";
        result.chain.clear();
        return result;
    };
    auto directory = HttpFetch(request.session, request.directory, "GET", {}, {}, 15000);
    JsonValue d;
    if (directory.status != 200 || !ParseJson(directory.body, d)) return fail("Couldn't reach Let's Encrypt.");
    client.newNonce = d.String("newNonce");
    const bool profiles = d["meta"]["profiles"][request.profile].kind != JsonValue::Kind::Null;

    std::string saved;
    if (!(ReadRecords(request.accountPath, saved) && saved.starts_with("CACME1\n") && client.key.Import(std::string_view(saved).substr(7)))) {
        std::string blob;
        if (!client.key.Create() || (blob = client.key.Export()).empty() ||
            !SaveRecords(request.accountPath, "CACME1\n" + blob)) return fail("Couldn't create the Let's Encrypt account key.");
    }
    // Asking for the account again is how ACME finds the existing one for this key.
    auto account = client.Post(d.String("newAccount"), "{\"termsOfServiceAgreed\":true}");
    client.kid = account.headers["location"];
    if ((account.status != 200 && account.status != 201) || client.kid.empty())
        return fail("Let's Encrypt didn't accept the account.", &account);

    std::string identifiers;
    for (const auto& name : request.names) {
        in6_addr bytes{};
        bool ip = inet_pton(AF_INET, name.c_str(), &bytes) == 1 || inet_pton(AF_INET6, name.c_str(), &bytes) == 1;
        identifiers += std::string(identifiers.empty() ? "" : ",") + "{\"type\":\"" + (ip ? "ip" : "dns") + "\",\"value\":" + Json(name) + '}';
    }
    auto order = client.Post(d.String("newOrder"), "{\"identifiers\":[" + identifiers + "]" +
        (profiles ? ",\"profile\":" + Json(request.profile) : std::string{}) + '}');
    JsonValue o;
    if ((order.status != 201 && order.status != 200) || !ParseJson(order.body, o))
        return fail("Let's Encrypt refused the certificate order.", &order);
    const std::string orderUrl = order.headers["location"], finalize = o.String("finalize");

    // Each address is checked over plain HTTP: the CA fetches a token from port 80 and expects it
    // followed by the account key's thumbprint.
    const std::string thumbprint = Base64Url(Sha256(client.key.Jwk()));
    std::vector<std::pair<std::string, std::string>> answers;
    std::vector<std::string> waiting, challenges;
    for (const auto& authorization : o["authorizations"].items) {
        auto reply = client.Post(authorization.text, {}, true);
        JsonValue z;
        if (reply.status != 200 || !ParseJson(reply.body, z)) return fail("Couldn't read the address check.", &reply);
        if (z.String("status") == "valid") continue;
        const JsonValue* http = nullptr;
        for (const auto& challenge : z["challenges"].items) if (challenge.String("type") == "http-01") http = &challenge;
        if (!http) return fail("Let's Encrypt offered no way to check this address over HTTP.");
        answers.emplace_back(http->String("token"), http->String("token") + '.' + thumbprint);
        waiting.push_back(authorization.text);
        challenges.push_back(http->String("url"));
    }
    struct Clear {
        const AcmeRequest& request;
        ~Clear() { if (request.serve) request.serve({}); }
    } clear{request};
    if (request.serve) request.serve(answers);
    for (const auto& url : challenges) {
        auto reply = client.Post(url, "{}");
        if (reply.status != 200) return fail("Let's Encrypt didn't start checking this address.", &reply);
    }
    for (const auto& url : waiting) {
        for (int round = 0;; ++round) {
            client.Wait(round ? 2000 : 300);
            auto reply = client.Post(url, {}, true);
            JsonValue z;
            if (client.Cancelled() || reply.status != 200 || !ParseJson(reply.body, z)) return fail("Couldn't follow the address check.", &reply);
            std::string status = z.String("status");
            if (status == "valid") break;
            if (status != "pending" && status != "processing") {
                for (const auto& challenge : z["challenges"].items)
                    if (challenge["error"].kind == JsonValue::Kind::Object)
                        return fail("Let's Encrypt couldn't reach this computer.", nullptr, &challenge["error"]);
                return fail("Let's Encrypt couldn't confirm this address.");
            }
            if (round >= 30) return fail("Let's Encrypt took too long to check this address.");
        }
    }

    std::string error, csr = CertificateRequest(request.keyName, request.names, error);
    if (csr.empty()) return fail(error);
    auto finished = client.Post(finalize, "{\"csr\":\"" + Base64Url(csr) + "\"}");
    JsonValue f;
    if (finished.status != 200 || !ParseJson(finished.body, f)) return fail("Let's Encrypt refused the certificate request.", &finished);
    for (int round = 0; f.String("status") != "valid"; ++round) {
        if (f.String("status") == "invalid" || round >= 30 || client.Cancelled())
            return fail("Let's Encrypt couldn't issue the certificate.", nullptr, &f["error"]);
        client.Wait(RetryAfter(finished, 2000));
        finished = client.Post(orderUrl, {}, true);
        if (finished.status != 200 || !ParseJson(finished.body, f)) return fail("Couldn't follow the certificate order.", &finished);
    }
    auto chain = client.Post(f.String("certificate"), {}, true, "Accept: application/pem-certificate-chain\r\n");
    result.chain = chain.status == 200 ? PemCertificates(chain.body) : std::vector<std::string>{};
    if (result.chain.empty()) return fail("Couldn't download the certificate.", &chain);
    return result;
}
}  // namespace web
