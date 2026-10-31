// Real loopback HTTP and HTTPS checks plus deterministic byte-range/decoder checks. This server never
// opens the user's library or LAN; preview mode serves only the explicitly supplied fixture folder.
#include "selftest_common.h"
#include "qr.h"
#include "web_internal.h"
#include "web_library.h"
#include "web_remote.h"
#include "web_tls.h"
#include <bcrypt.h>
#include <winhttp.h>

#include <mutex>
#include <set>
#include <thread>

namespace {
struct Reply { DWORD status = 0; std::string body, cookie, headers; };
// `secure` speaks HTTPS and accepts the test's self-signed certificate.
Reply Request(unsigned port, const std::wstring& target, const char* body = nullptr,
              const std::string& cookie = {}, const std::wstring& extra = {}, bool head = false, bool secure = false) {
    Reply reply;
    HINTERNET session = WinHttpOpen(L"cordinal self-test", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return reply;
    WinHttpSetTimeouts(session, 2000, 2000, 2000, 5000);
    HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", (INTERNET_PORT)port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, head ? L"HEAD" : body ? L"POST" : L"GET",
        target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (request) {
        DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
        DWORD lax = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        if (secure) WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &lax, sizeof(lax));
        std::wstring headers = extra;
        if (!cookie.empty()) headers += L"Cookie: " + FromUtf8(cookie) + L"\r\n";
        if (body) headers += L"Content-Type: application/x-www-form-urlencoded\r\n";
        // Replacing rather than adding lets a check send its own Host, as a reverse proxy does.
        if (!headers.empty()) WinHttpAddRequestHeaders(request, headers.c_str(), (DWORD)headers.size(),
                                                       WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (void*)body,
                              body ? (DWORD)strlen(body) : 0, body ? (DWORD)strlen(body) : 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD size = sizeof(reply.status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                &reply.status, &size, nullptr);
            size = 0; WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, nullptr, &size, nullptr);
            std::wstring raw(size / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, nullptr, raw.data(), &size, nullptr)) {
                reply.headers = ToUtf8(raw);
                size_t cookieAt = reply.headers.find("Set-Cookie: ");
                if (cookieAt != reply.headers.npos) {
                    cookieAt += 12; reply.cookie = reply.headers.substr(cookieAt, reply.headers.find(';', cookieAt) - cookieAt);
                }
            }
            for (;;) {
                char bytes[8192]; DWORD read = 0;
                if (!WinHttpReadData(request, bytes, sizeof(bytes), &read) || !read) break;
                reply.body.append(bytes, read);
                if (reply.body.size() > 4u * 1024 * 1024) break;
            }
        }
        WinHttpCloseHandle(request);
    }
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return reply;
}
std::string WaveBytes(unsigned rate, unsigned seconds = 2) {
    std::string bytes(44 + (size_t)rate * seconds * 4, '\0');
    auto put16 = [&](size_t at, uint16_t n) { bytes[at] = (char)n; bytes[at + 1] = (char)(n >> 8); };
    auto put32 = [&](size_t at, uint32_t n) { put16(at, (uint16_t)n); put16(at + 2, (uint16_t)(n >> 16)); };
    memcpy(bytes.data(), "RIFF", 4); put32(4, (uint32_t)bytes.size() - 8); memcpy(bytes.data() + 8, "WAVEfmt ", 8);
    put32(16, 16); put16(20, 1); put16(22, 2); put32(24, rate); put32(28, rate * 4); put16(32, 4); put16(34, 16);
    memcpy(bytes.data() + 36, "data", 4); put32(40, (uint32_t)bytes.size() - 44);
    for (size_t i = 44; i < bytes.size(); i += 2) put16(i, (uint16_t)((i * 113) % 60000 - 30000));
    return bytes;
}
std::string ReadMedia(web::Media media, uint64_t at, size_t size) {
    std::string bytes;
    if (at <= media.size) size = (size_t)std::min<uint64_t>(size, media.size - at);
    if (!media.error.empty() || !media.send || !media.send(at, size, [&](const char* p, size_t n) {
        bytes.append(p, n); return true;
    })) return {};
    return bytes;
}
size_t AdtsFrames(std::string_view bytes) {
    size_t frames = 0, at = 0;
    while (at < bytes.size()) {
        if (bytes.size() - at < 7) return 0;
        const auto* p = (const uint8_t*)bytes.data() + at;
        size_t length = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
        if (p[0] != 255 || (p[1] & 0xf6) != 0xf0 || p[2] >> 6 != 1 ||
            ((p[2] >> 2) & 15) != 3 || (((p[2] & 1) << 2) | (p[3] >> 6)) != 2 ||
            length < 7 || length > bytes.size() - at) return 0;
        at += length; ++frames;
    }
    return frames;
}
web::Track TestTrack(const std::wstring& path) {
    web::Track t;
    t.key = "fixture-track"; t.id = 0; t.path = path; t.title = "Sinus Ä \"雪\"";
    t.artist = "Toby Fox; Geoffrey Day"; t.albumArtist = "Toby Fox"; t.album = "Fixture album";
    t.duration = 2; t.library = true; t.original = true; t.codec = "PCM"; t.mime = "audio/wav";
    return t;
}
class LongTrack : public Decoder {
    uint64_t at_ = 0;
public:
    size_t Read(float* out, size_t frames) override {
        frames = (size_t)std::min<uint64_t>(frames, 48000ull * 36000 - at_);
        for (size_t i = 0; i < frames * 2; ++i) out[i] = .125f;
        at_ += frames; return frames;
    }
    bool Seek(double seconds) override { at_ = (uint64_t)std::llround(seconds * 48000); return at_ <= 48000ull * 36000; }
    double Duration() const override { return 36000; }
    unsigned Rate() const override { return 48000; }
};

// A loopback HTTP server standing in for the router or the certificate authority in one check;
// `handle` turns each request into a whole response.
class FakeHttp {
public:
    explicit FakeHttp(std::function<std::string(const web::Request&)> handle) : handle_(std::move(handle)) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof(address);
        if (listener_ == INVALID_SOCKET || bind(listener_, (sockaddr*)&address, sizeof(address)) || listen(listener_, 8) ||
            getsockname(listener_, (sockaddr*)&address, &size)) return;
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { Run(); });
    }
    ~FakeHttp() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
    }
    std::string Url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
private:
    void Run() {
        while (!stop_) {
            fd_set ready; FD_ZERO(&ready); FD_SET(listener_, &ready);
            timeval wait{0, 50000};
            if (select(0, &ready, nullptr, nullptr, &wait) <= 0) continue;
            SOCKET s = accept(listener_, nullptr, nullptr);
            if (s == INVALID_SOCKET) continue;
            DWORD timeout = 2000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            std::string bytes;
            web::Request r;
            size_t required = 0;
            for (char buffer[4096];;) {
                int n = recv(s, buffer, sizeof(buffer), 0);
                if (n <= 0 || !web::ParseRequest(bytes.append(buffer, (size_t)n), r, required)) break;
                if (required <= bytes.size()) {
                    std::string reply = handle_(r);
                    send(s, reply.data(), (int)reply.size(), 0);
                    break;
                }
            }
            shutdown(s, 1);
            closesocket(s);
        }
    }
    std::function<std::string(const web::Request&)> handle_;
    SOCKET listener_ = INVALID_SOCKET;
    unsigned port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};
std::string HttpResponse(int status, const std::string& body, const std::string& headers = {}) {
    return "HTTP/1.1 " + std::to_string(status) + " Test\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n" + headers + "\r\n" + body;
}
std::string FromBase64Url(std::string_view text) {
    std::string out;
    uint32_t pending = 0;
    int bits = 0;
    for (char c : text) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
            c == '-' ? 62 : c == '_' ? 63 : -1;
        if (v < 0) break;
        pending = pending << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)(pending >> bits & 255); }
    }
    return out;
}
std::string TestSha256(std::string_view data) {
    std::string hash(32, '\0');
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), (PUCHAR)hash.data(), 32);
    return hash;
}
std::string PemOf(const std::vector<std::string>& chain) {
    std::string out;
    for (const auto& der : chain) {
        DWORD size = 0;
        CryptBinaryToStringA((const BYTE*)der.data(), (DWORD)der.size(), CRYPT_STRING_BASE64HEADER, nullptr, &size);
        std::string text(size, '\0');
        CryptBinaryToStringA((const BYTE*)der.data(), (DWORD)der.size(), CRYPT_STRING_BASE64HEADER, text.data(), &size);
        out += text.c_str();
    }
    return out;
}
// A certificate authority speaking ACME the way Let's Encrypt does, strictly: every POST must carry a
// valid ES256 signature over a fresh nonce and its own URL, and the http-01 answer is fetched from
// the cordinal server before anything is issued. It signs what it issues with a test key of its own.
struct FakeAcme {
    std::mutex mutex;
    std::string base, x, y, problem;
    std::set<std::string> nonces;
    int nonce = 0, accounts = 0;
    unsigned validationPort = 0;
    bool validated = false, unreachable = false, failed = false;
    std::vector<std::string> identifiers, chain;
    std::string profile;
    std::wstring caKey;
    std::string caDer;

    std::string Thumbprint() const {
        return web::Base64Url(TestSha256("{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"" + x + "\",\"y\":\"" + y + "\"}"));
    }
    bool Verify(const web::Request& r, std::string& payload, bool& withKey) {
        web::JsonValue jws, header;
        if (!web::ParseJson(r.body, jws) || !web::ParseJson(FromBase64Url(jws.String("protected")), header) ||
            header.String("alg") != "ES256" || header.String("url") != base + r.path || !nonces.erase(header.String("nonce")))
            return false;
        withKey = header["jwk"].kind == web::JsonValue::Kind::Object;
        std::string kx = withKey ? header["jwk"].String("x") : x, ky = withKey ? header["jwk"].String("y") : y;
        if (!withKey && header.String("kid") != base + "/acct/1") return false;
        std::string blob(sizeof(BCRYPT_ECCKEY_BLOB), '\0');
        auto* head = (BCRYPT_ECCKEY_BLOB*)blob.data();
        head->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC; head->cbKey = 32;
        blob += FromBase64Url(kx) + FromBase64Url(ky);
        BCRYPT_KEY_HANDLE key = nullptr;
        std::string hash = TestSha256(jws.String("protected") + '.' + jws.String("payload")),
            signature = FromBase64Url(jws.String("signature"));
        bool ok = blob.size() == sizeof(BCRYPT_ECCKEY_BLOB) + 64 && signature.size() == 64 &&
            BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                               (PUCHAR)blob.data(), (ULONG)blob.size(), 0)) &&
            BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr, (PUCHAR)hash.data(), 32, (PUCHAR)signature.data(), 64, 0));
        if (key) BCryptDestroyKey(key);
        payload = FromBase64Url(jws.String("payload"));
        if (ok && withKey) { x = kx; y = ky; }
        return ok;
    }
    std::string Issue(const std::string& csr) {
        CERT_REQUEST_INFO* request = nullptr;
        CERT_EXTENSIONS* extensions = nullptr;
        DWORD size = 0;
        if (!CryptDecodeObjectEx(X509_ASN_ENCODING, X509_CERT_REQUEST_TO_BE_SIGNED, (const BYTE*)csr.data(), (DWORD)csr.size(),
                                 CRYPT_DECODE_ALLOC_FLAG, nullptr, &request, &size)) return {};
        CRYPT_DATA_BLOB whole{(DWORD)csr.size(), (BYTE*)csr.data()};
        bool signedByKey = CryptVerifyCertificateSignatureEx(0, X509_ASN_ENCODING, CRYPT_VERIFY_CERT_SIGN_SUBJECT_BLOB, &whole,
            CRYPT_VERIFY_CERT_SIGN_ISSUER_PUBKEY, &request->SubjectPublicKeyInfo, 0, nullptr);
        for (DWORD i = 0; i < request->cAttribute; ++i)
            if (!strcmp(request->rgAttribute[i].pszObjId, szOID_RSA_certExtensions) && request->rgAttribute[i].cValue)
                CryptDecodeObjectEx(X509_ASN_ENCODING, X509_EXTENSIONS, request->rgAttribute[i].rgValue[0].pbData,
                    request->rgAttribute[i].rgValue[0].cbData, CRYPT_DECODE_ALLOC_FLAG, nullptr, &extensions, &size);
        PCCERT_CONTEXT ca = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)caDer.data(), (DWORD)caDer.size());
        CRYPT_KEY_PROV_INFO info{caKey.data(), const_cast<wchar_t*>(MS_KEY_STORAGE_PROVIDER), 0, 0, 0, nullptr, 0};
        HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
        DWORD spec = 0;
        BOOL release = FALSE;
        std::string der;
        if (signedByKey && extensions && ca && CertSetCertificateContextProperty(ca, CERT_KEY_PROV_INFO_PROP_ID, 0, &info) &&
            CryptAcquireCertificatePrivateKey(ca, CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_CACHE_FLAG, nullptr,
                                              &key, &spec, &release)) {
            BYTE serial[16];
            BCryptGenRandom(nullptr, serial, sizeof(serial), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            serial[15] &= 0x7F;
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            auto at = [&](int64_t hours) {
                uint64_t v = ((uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime) + (uint64_t)(hours * 36000000000ll);
                return FILETIME{(DWORD)v, (DWORD)(v >> 32)};
            };
            CERT_INFO cert{};
            cert.dwVersion = CERT_V3;
            cert.SerialNumber = {sizeof(serial), serial};
            cert.SignatureAlgorithm = {(LPSTR)szOID_ECDSA_SHA256, {}};
            cert.Issuer = ca->pCertInfo->Subject;
            cert.NotBefore = at(-1); cert.NotAfter = at(159);
            cert.Subject = request->Subject;
            cert.SubjectPublicKeyInfo = request->SubjectPublicKeyInfo;
            cert.cExtension = extensions->cExtension; cert.rgExtension = extensions->rgExtension;
            CRYPT_ALGORITHM_IDENTIFIER algorithm{(LPSTR)szOID_ECDSA_SHA256, {}};
            if (CryptSignAndEncodeCertificate(key, 0, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &cert, &algorithm, nullptr,
                                              nullptr, &size)) {
                der.resize(size);
                if (CryptSignAndEncodeCertificate(key, 0, X509_ASN_ENCODING, X509_CERT_TO_BE_SIGNED, &cert, &algorithm, nullptr,
                                                  (BYTE*)der.data(), &size)) der.resize(size);
                else der.clear();
            }
        }
        if (ca) CertFreeCertificateContext(ca);
        LocalFree(extensions);
        LocalFree(request);
        return der;
    }
    std::string Handle(const web::Request& r) {
        std::lock_guard lock(mutex);
        std::string fresh = "n" + std::to_string(++nonce);
        nonces.insert(fresh);
        std::string headers = "Replay-Nonce: " + fresh + "\r\n";
        auto json = [&](int status, const std::string& body, const std::string& extra = {}) {
            return HttpResponse(status, body, headers + "Content-Type: application/json\r\n" + extra);
        };
        if (r.path == "/dir")
            return json(200, "{\"newNonce\":\"" + base + "/nonce\",\"newAccount\":\"" + base + "/acct\",\"newOrder\":\"" + base +
                "/order\",\"meta\":{\"profiles\":{\"classic\":\"90 days\",\"shortlived\":\"6 days\"}}}");
        if (r.path == "/nonce") return HttpResponse(200, "", headers);
        std::string payload;
        bool withKey = false;
        if (r.method != "POST" || !Verify(r, payload, withKey)) {
            if (problem.empty()) problem = "bad request to " + r.path;
            return json(400, "{\"type\":\"urn:ietf:params:acme:error:malformed\",\"detail\":\"bad JWS\"}");
        }
        const std::string order = "{\"status\":\"valid\",\"certificate\":\"" + base + "/cert/1\"}";
        if (r.path == "/acct") {
            if (!withKey) problem = "account request without its key";
            ++accounts;
            return json(accounts == 1 ? 201 : 200, "{\"status\":\"valid\"}", "Location: " + base + "/acct/1\r\n");
        }
        if (r.path == "/order") {
            web::JsonValue o;
            web::ParseJson(payload, o);
            identifiers.clear();
            for (const auto& id : o["identifiers"].items) identifiers.push_back(id.String("type") + ':' + id.String("value"));
            profile = o.String("profile");
            validated = failed = false;
            return json(201, "{\"status\":\"pending\",\"authorizations\":[\"" + base + "/authz/1\"],\"finalize\":\"" + base +
                "/finalize/1\"}", "Location: " + base + "/order/1\r\n");
        }
        if (r.path == "/authz/1") {
            std::string status = validated ? "valid" : failed ? "invalid" : "pending";
            return json(200, "{\"status\":\"" + status + "\",\"challenges\":[{\"type\":\"dns-01\",\"url\":\"" + base +
                "/chall/2\",\"token\":\"other\"},{\"type\":\"http-01\",\"url\":\"" + base + "/chall/1\",\"token\":\"tok-1\"" +
                (status == "invalid" ? ",\"error\":{\"type\":\"urn:ietf:params:acme:error:connection\",\"detail\":\"Timeout\"}" : "") + "}]}");
        }
        if (r.path == "/chall/1") {
            // Fetches the answer from the cordinal server, as the CA does from port 80.
            if (!unreachable) {
                auto answer = Request(validationPort, L"/.well-known/acme-challenge/tok-1");
                validated = answer.status == 200 && answer.body == "tok-1." + Thumbprint();
            }
            failed = !validated;
            return json(200, "{\"type\":\"http-01\",\"status\":\"processing\"}");
        }
        if (r.path == "/finalize/1") {
            web::JsonValue f;
            if (!validated || !web::ParseJson(payload, f)) return json(403, "{\"type\":\"urn:ietf:params:acme:error:orderNotReady\"}");
            std::string leaf = Issue(FromBase64Url(f.String("csr")));
            if (leaf.empty()) { problem = "the certificate request was not signed by its key"; return json(400, "{}"); }
            chain = {leaf, caDer};
            return json(200, order);
        }
        if (r.path == "/order/1") return json(200, order);
        if (r.path == "/cert/1") return HttpResponse(200, PemOf(chain), headers + "Content-Type: application/pem-certificate-chain\r\n");
        return json(404, "{}");
    }
};
// The router side of UPnP: a device description and the WANIPConnection control point.
struct FakeRouter {
    std::mutex mutex;
    std::vector<std::string> actions;
    std::string added;
    std::string Handle(const web::Request& r) {
        if (r.method == "GET" && r.path == "/rootDesc.xml")
            return HttpResponse(200, "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device>"
                "<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType><deviceList><device><deviceList>"
                "<device><serviceList><service><serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>"
                "<controlURL>/ctl/CmnIfCfg</controlURL></service><service><serviceType>urn:schemas-upnp-org:service:"
                "WANIPConnection:1</serviceType><serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId><controlURL>"
                "/ctl/IPConn</controlURL></service></serviceList></device></deviceList></device></deviceList></device></root>");
        std::lock_guard lock(mutex);
        auto soap = r.headers.find("soapaction");
        std::string action = soap == r.headers.end() || r.path != "/ctl/IPConn" ? "" : soap->second;
        action = action.substr(action.find('#') + 1);
        if (!action.empty() && action.back() == '"') action.pop_back();
        actions.push_back(action);
        auto reply = [&](const std::string& inner) {
            return HttpResponse(200, "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
                "<s:Body><u:" + action + "Response xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">" + inner +
                "</u:" + action + "Response></s:Body></s:Envelope>");
        };
        if (action == "GetExternalIPAddress") return reply("<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>");
        if (action == "AddPortMapping" && r.body.find("<NewLeaseDuration>0</NewLeaseDuration>") == std::string::npos)
            return HttpResponse(500, "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><s:Fault>"
                "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>725</errorCode><errorDescription>"
                "OnlyPermanentLeasesSupported</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>");
        if (action == "AddPortMapping") { added = r.body; return reply({}); }
        if (action == "DeletePortMapping") return reply({});
        return HttpResponse(500, "<errorCode>401</errorCode>");
    }
};
unsigned FreePort() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(address);
    unsigned port = !bind(s, (sockaddr*)&address, sizeof(address)) && !getsockname(s, (sockaddr*)&address, &size) ?
        ntohs(address.sin_port) : 0;
    closesocket(s);
    return port;
}

void TestWebAway(const std::wstring& scratch) {
    using web::JsonValue;
    using web::Server;
    using web::Config;
    {
        testlog::Group group(L"away-from-home building blocks");
        JsonValue v;
        Check(web::ParseJson("{\"a\":[1,true,null,{\"b\":\"\\u00e9\\ud83c\\udfb5\\n\"}],\"c\":-2.5e3}", v) &&
              v["a"].items.size() == 4 && v["a"].items[1].kind == JsonValue::Kind::Bool &&
              v["a"].items[3].String("b") == "\xc3\xa9\xf0\x9f\x8e\xb5\n" && v["c"].text == "-2.5e3" &&
              v["missing"]["deeper"].kind == JsonValue::Kind::Null &&
              !web::ParseJson("{\"a\":1,}", v) && !web::ParseJson("[1] 2", v) && !web::ParseJson("\"open", v),
              L"the JSON reader takes nested values and escapes, and refuses trailing junk");
        Check(web::Base64Url("{}") == "e30" && web::Base64Url("\xfb\xff") == "-_8" && web::Base64Url("") == "",
              L"base64url uses the URL alphabet without padding");
        // RFC 5769 section 2.2: a binding response carrying 192.0.2.1 port 32853 for this transaction.
        const uint8_t id[12] = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
        const uint8_t reply[] = {0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86,
            0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
            0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43, 0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99,
            0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7, 0x80, 0x28, 0x00, 0x04,
            0xc0, 0x7d, 0x4c, 0x96};
        std::string address, other;
        uint8_t wrong[12] = {};
        Check(web::ParseStun({(const char*)reply, sizeof(reply)}, id, address) && address == "192.0.2.1" &&
              !web::ParseStun({(const char*)reply, sizeof(reply)}, wrong, other) &&
              !web::ParseStun({(const char*)reply, 30}, id, other),
              L"STUN replies give the public address and are matched to their own request");
        Check(web::PublicIPv4("73.12.200.41") && !web::PublicIPv4("10.0.0.67") && !web::PublicIPv4("100.72.1.2") &&
              !web::PublicIPv4("192.168.1.1") && !web::PublicIPv4("172.20.0.1") && !web::PublicIPv4("not an address"),
              L"private and shared addresses are not taken for the internet address");
        Check(web::SsdpLocation("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nlocation: http://10.0.0.1:49152/rootDesc.xml\r\n"
                                "ST: upnp:rootdevice\r\n\r\n") == "http://10.0.0.1:49152/rootDesc.xml",
              L"router discovery replies are read for their description address");
        FakeRouter router;
        FakeHttp http([&](const web::Request& r) { return router.Handle(r); });
        web::Gateway gateway;
        gateway.local = "10.0.0.67";
        Check(web::ReadGateway(nullptr, http.Url("/rootDesc.xml"), gateway) && gateway.control == http.Url("/ctl/IPConn") &&
              gateway.service == "urn:schemas-upnp-org:service:WANIPConnection:1" &&
              web::GatewayAddress(nullptr, gateway) == "203.0.113.7",
              L"the router's connection service is found and reports the internet address");
        Check(web::MapPort(nullptr, gateway, 443, 8787, 3600) == 0 &&
              router.added.find("<NewExternalPort>443</NewExternalPort>") != std::string::npos &&
              router.added.find("<NewInternalPort>8787</NewInternalPort>") != std::string::npos &&
              router.added.find("<NewInternalClient>10.0.0.67</NewInternalClient>") != std::string::npos &&
              web::UnmapPort(nullptr, gateway, 443) &&
              router.actions == std::vector<std::string>{"GetExternalIPAddress", "AddPortMapping", "AddPortMapping", "DeletePortMapping"},
              L"a router that keeps only permanent mappings gets one, and it is removed again");
    }
    {
        testlog::Group group(L"certificates from an ACME authority");
        Server server;
        Config config; config.port = 0; config.devicesPath = scratch + L"\\web-devices-acme.bin";
        DeleteFileW(config.devicesPath.c_str());
        Ready(server.Start(config), L"start a loopback-only server to answer the authority");
        const unsigned port = server.GetStatus().port;
        const std::wstring key = L"cordinal self-test " + FromUtf8(web::RandomHex(8)), caKey = key + L" CA";
        struct Keys {
            const std::wstring &a, &b;
            ~Keys() { web::DeleteTlsKey(a); web::DeleteTlsKey(b); }
        } keys{key, caKey};
        FakeAcme ca;
        ca.caKey = caKey;
        ca.caDer = web::SelfSignedCertificate(caKey, {"authority.test"}, 2);
        ca.validationPort = port;
        FakeHttp http([&](const web::Request& r) { return ca.Handle(r); });
        ca.base = http.Url("");
        std::string error;
        Ready(!ca.caDer.empty() && web::EnsureTlsKey(key, error), L"make the authority's and the server's test keys");
        const std::wstring account = scratch + L"\\web-account-test.bin", stored = scratch + L"\\web-certificate.bin";
        DeleteFileW(account.c_str());
        DeleteFileW(stored.c_str());
        web::Away away;
        web::AcmeRequest request;
        request.directory = http.Url("/dir");
        request.accountPath = account;
        request.keyName = key;
        request.names = {"127.0.0.1"};
        request.serve = [&](const std::vector<std::pair<std::string, std::string>>& answers) {
            away.challenges = answers;
            server.SetAway(away);
        };
        auto result = web::RequestCertificate(request);
        std::vector<std::string> names;
        uint64_t from = 0, until = 0;
        Check(result.chain.size() == 2 && web::CertificateInfo(result.chain[0], from, until, names) &&
              names == std::vector<std::string>{"127.0.0.1"} && until > from && ca.problem.empty() &&
              ca.identifiers == std::vector<std::string>{"ip:127.0.0.1"} && ca.profile == "shortlived",
              L"a signed account, order, http-01 answer and request yield the certificate chain %hs%hs",
              result.error.c_str(), ca.problem.c_str());
        Check(Request(port, L"/.well-known/acme-challenge/tok-1").status == 404,
              L"challenge answers are withdrawn once the order is done");
        auto identity = result.chain.empty() ? nullptr : web::TlsIdentity::Create(result.chain, key, error);
        away.identity = identity;
        away.url = "https://127.0.0.1:" + std::to_string(port) + "/";
        server.SetAway(away);
        Check(identity && Request(port, L"/", nullptr, {}, {}, false, true).status == 200,
              L"the issued chain serves HTTPS with the key it was requested for");
        auto again = web::RequestCertificate(request);
        Check(again.chain.size() == 2 && ca.accounts == 2 && ca.problem.empty(),
              L"renewal signs with the saved account key and finds the same account");
        ca.unreachable = true;
        auto unreachable = web::RequestCertificate(request);
        Check(unreachable.chain.empty() && unreachable.problem == "connection" &&
              unreachable.error.find("Timeout") != std::string::npos,
              L"an address the authority can't reach fails with its reason");
        ca.unreachable = false;
        server.SetAway({});

        // The manager end to end: address, ports, certificate, and the certificate kept for next time.
        web::Remote remote(server);
        web::RemoteConfig rc;
        rc.httpsPort = (uint16_t)FreePort(); rc.httpPort = (uint16_t)FreePort();
        rc.dataDir = scratch; rc.keyName = key; rc.directory = http.Url("/dir");
        rc.stun.clear(); rc.router = false; rc.loopback = false; rc.address = "203.0.113.7";
        remote.Start(rc);
        Pump(15000, [&] { return remote.GetStatus().state == web::RemoteStatus::State::Ready; });
        auto rs = remote.GetStatus();
        const std::string url = "https://203.0.113.7:" + std::to_string(rc.httpsPort) + "/";
        Check(rs.state == web::RemoteStatus::State::Ready && rs.url == url && server.GetStatus().away == url,
              L"the manager certifies the internet address and hands it to the server: %hs", rs.message.c_str());
        Pump(1000, [&] { return server.GetStatus().ports.size() == 3; });
        Check(Request(rc.httpsPort, L"/", nullptr, {}, {}, false, true).status == 200,
              L"the server answers HTTPS on the outside port too");
        remote.Stop();
        Pump(1000, [&] { return server.GetStatus().ports.size() == 1; });
        Check(server.GetStatus().away.empty() && server.GetStatus().ports.size() == 1,
              L"stopping removes the secure address and closes the extra ports");
        rc.directory = "http://127.0.0.1:1/unreachable";
        remote.Start(rc);
        Pump(15000, [&] { return remote.GetStatus().state == web::RemoteStatus::State::Ready; });
        Check(remote.GetStatus().state == web::RemoteStatus::State::Ready && server.GetStatus().away == url,
              L"a saved certificate is used again without asking the authority");
        remote.Stop();
        server.Stop();
        DeleteFileW(account.c_str());
        DeleteFileW(stored.c_str());
        DeleteFileW((scratch + L"\\web-account.bin").c_str());
        DeleteFileW(config.devicesPath.c_str());
    }
}
}

void TestWeb(const std::wstring& scratch) {
    using web::ByteRange;
    using web::ParseRange;
    using web::ParseRequest;
    using web::Json;
    using web::WaveMedia;
    using web::OriginalMedia;
    using web::Server;
    using web::Config;
    using web::Snapshot;
    std::atomic<bool> cancel{false};
    {
        testlog::Group group(L"owned browser library snapshots");
        Library library;
        auto add = [&](const wchar_t* name, uint16_t flags) {
            auto id = library.Add(scratch + L"\\" + name);
            library[id].flags = flags | ::Track::kLoaded;
            library[id].title = library.Intern(ToUtf8(name));
            library[id].artist = library.Intern("Snapshot artist");
            return id;
        };
        auto visible = add(L"visible.wav", ::Track::kInLibrary);
        auto inbox = add(L"inbox.wav", ::Track::kInLibrary | ::Track::kInbox);
        auto hidden = add(L"hidden.wav", ::Track::kInLibrary | ::Track::kExcluded);
        auto original = add(L"original.wav", ::Track::kInLibrary | ::Track::kOriginal);
        auto missing = add(L"missing.wav", ::Track::kInLibrary | ::Track::kMissing);
        std::vector<::Playlist> lists{{L"Saved 雪", {hidden, visible, hidden, missing}, PlaylistKind::Static},
            {L"Artist mix", {}, PlaylistKind::Auto}, {L"Desktop queue", {inbox}, PlaylistKind::NowPlaying}};
        AutoPlaylist rule; rule.root.rules = {{RuleField::Artist, RuleOp::Contains, "Snapshot", ""}};
        lists[1].rules = SerializeAutoPlaylist(rule);
        web::LibraryBuilder builder;
        auto capture = [&] {
            std::shared_ptr<Snapshot> s;
            for (unsigned i = 0; i < 100 && !s; ++i) s = builder.Tick(library, lists);
            return s;
        };
        auto s = capture();
        Check(s && s->prepare, L"UI batches publish an owned capture for worker preparation");
        if (s && s->prepare) {
            library[visible].artist = library.Intern("Changed after capture");
            s->prepare(*s); s->prepare = {};
            auto find = [&](TrackId id) { return std::find_if(s->tracks.begin(), s->tracks.end(),
                [&](const auto& t) { return t.id == id; }); };
            auto v = find(visible), h = find(hidden);
            Check(s->tracks.size() == 2 && v != s->tracks.end() && h != s->tracks.end() &&
                  v->library && !h->library && find(inbox) == s->tracks.end() && find(original) == s->tracks.end(),
                  L"only visible music and explicit saved-playlist entries are exposed");
            Check(s->playlists.size() == 2 && s->playlists[0].tracks.size() == 3 &&
                  s->playlists[0].tracks.front() == s->playlists[0].tracks.back() &&
                  s->playlists[1].tracks.size() == 1 && v != s->tracks.end() && v->artist == "Snapshot artist",
                  L"saved order, duplicates and automatic rules use captured metadata");
            auto key = v != s->tracks.end() ? v->key : ""; builder.Reset();
            auto fresh = capture();
            if (fresh && fresh->prepare) fresh->prepare(*fresh);
            Check(fresh && std::any_of(fresh->tracks.begin(), fresh->tracks.end(),
                    [&](const auto& t) { return t.id == visible && t.key != key; }),
                  L"replacement and compaction generations invalidate old opaque handles");
        }
        Snapshot catalog;
        auto a = TestTrack(L"a.wav"), b = a;
        a.album.clear(); a.albumArtist.clear(); a.artist.clear(); a.key = "a";
        b.key = "b"; catalog.tracks = {a, b}; catalog.byKey = {{"a", 0}, {"b", 1}};
        catalog.playlists.push_back({"duplicates", "Duplicates", {"a", "b", "a", "b", "a"}});
        web::Request query; query.query = {{"playlist", "duplicates"}, {"offset", "4"}, {"limit", "1"}};
        auto page = web::Catalog(catalog, query);
        Check(page.find("\"id\":\"a\"") != page.npos && page.find("\"id\":\"b\"") == page.npos,
              L"duplicate-heavy saved playlists paginate beyond the unique track count");
        query.query = {{"album", ""}, {"artist", ""}, {"date", ""}};
        Check(web::Catalog(catalog, query).find("\"total\":1") != std::string::npos,
              L"unknown album and artist groups select only their own tracks");
    }
    {
        testlog::Group group(L"HTTP parsing and ranges");
        ByteRange range;
        Check(ParseRange("bytes=4-9", 20, range) && range.first == 4 && range.size == 6 && range.partial,
              L"closed byte ranges include both endpoints");
        Check(ParseRange("bytes=-100", 20, range) && range.first == 0 && range.size == 20 &&
              ParseRange("bytes=19-", 20, range) && range.first == 19 && range.size == 1,
              L"suffix and open-ended ranges clamp to the representation");
        Check(!ParseRange("bytes=20-", 20, range) && !ParseRange("bytes=9-4", 20, range) &&
              !ParseRange("bytes=0-1,4-5", 20, range) && !ParseRange("bytes=-0", 20, range) &&
              !ParseRange("bytes=18446744073709551616-", 20, range), L"invalid and overflowing ranges are refused");
        web::Request r; size_t needed = 0;
        Check(ParseRequest("GET /api/v1/catalog?q=%E9%9B%AA HTTP/1.1\r\nHost: localhost\r\n\r\n", r, needed) &&
              r.query["q"] == "雪", L"query text is decoded as UTF-8");
        Check(!ParseRequest("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\na", r, needed) &&
              !ParseRequest("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", r, needed) &&
              !ParseRequest("GET /%2e%2e/x HTTP/1.1\r\nHost: a\r\n\r\n", r, needed) &&
              !ParseRequest("GET /?q=%00 HTTP/1.1\r\nHost: a\r\n\r\n", r, needed),
              L"ambiguous framing, encoded paths and embedded NULs are refused");
        Check(Json("a\n\"\\雪") == "\"a\\u000a\\\"\\\\雪\"", L"JSON preserves Unicode and escapes tag control characters");
    }
    std::wstring path = scratch + L"\\web-audio.wav";
    auto original = WaveBytes(44100);
    Ready(WriteTestBytes(path, original), L"create the isolated server audio fixture");
    auto track = TestTrack(path);
    {
        testlog::Group group(L"browser data saver");
        auto compressed = web::AacMedia(track, 0, cancel);
        Check(compressed.error.empty() && compressed.mime == "audio/aac" && compressed.size > 1000 &&
              compressed.size < original.size() / 8,
              L"Windows data saver prepares a bounded AAC representation with an exact length");
        if (compressed.error.empty()) {
            auto aac = ReadMedia(std::move(compressed), 0, SIZE_MAX);
            size_t frames = AdtsFrames(aac);
            Check(frames >= 94 && frames <= 98 && aac.size() > 1000 && aac.size() < original.size() / 8,
                  L"data saver emits complete stereo 48 kHz AAC-LC frames at a fraction of PCM size");
            web::AacCache cache;
            auto cached = ReadMedia(web::AacMedia(track, 0, cancel, 96, &cache), 0, SIZE_MAX);
            auto fragment = ReadMedia(web::AacMedia(track, 0, cancel, 96, &cache), 1003, 4321);
            Check(cached == aac && fragment == aac.substr(1003, 4321),
                  L"compressed byte ranges preserve the exact encoded representation across requests");
            std::atomic<bool> separateCancel{false};
            cancel = true;
            auto resumed = ReadMedia(web::AacMedia(track, 0, separateCancel, 96, &cache), 1003, 4321);
            Check(resumed == fragment, L"cached audio resumption uses the new connection's cancellation state");
            cancel = false;
            auto balanced = ReadMedia(web::AacMedia(track, 0, cancel, 128), 0, SIZE_MAX);
            Check(AdtsFrames(balanced) >= 94 && AdtsFrames(balanced) <= 98 && balanced.size() > aac.size() * 1.15 &&
                  balanced.size() < original.size() / 6,
                  L"balanced mode encodes the full track at a distinct higher compressed bitrate");
            const auto encodedPath = scratch + L"\\web-data-saver.aac";
            Ready(WriteTestBytes(encodedPath, aac), L"save the isolated compressed stream for decoder verification");
            std::wstring error;
            auto decoder = OpenTrackDecoder(encodedPath, 48000, error);
            size_t decoded = 0; float peak = 0; bool finite = true;
            std::array<float, 4096 * 2> samples;
            while (decoder && decoded < 3 * 48000) {
                size_t n = decoder->Read(samples.data(), samples.size() / 2);
                if (!n) break;
                decoded += n;
                for (size_t i = 0; i < n * 2; ++i) {
                    finite = finite && std::isfinite(samples[i]); peak = std::max(peak, std::abs(samples[i]));
                }
            }
            Check(decoder && decoded >= 2 * 48000 && decoded < 2.1 * 48000 && finite && peak > .05f,
                  L"the independent native decoder reads audible finite PCM for the complete AAC stream");
            auto later = ReadMedia(web::AacMedia(track, 1, cancel), 0, SIZE_MAX);
            Check(AdtsFrames(later) >= 47 && AdtsFrames(later) <= 51 && later.size() < aac.size() * .7,
                  L"compressed seeking emits just the requested remaining audio");
            auto changing = WaveBytes(48000, 4);
            for (size_t i = 0; i < 4 * 48000; ++i) {
                double hz = i < 2 * 48000 ? 220 : 880;
                int16_t value = (int16_t)std::lround(12000 * std::sin(2 * 3.141592653589793 * hz * i / 48000));
                for (unsigned channel = 0; channel < 2; ++channel) {
                    changing[44 + i * 4 + channel * 2] = (char)value;
                    changing[45 + i * 4 + channel * 2] = (char)(value >> 8);
                }
            }
            auto marked = track; marked.path = scratch + L"\\web-changing-tones.wav";
            Ready(WriteTestBytes(marked.path, changing), L"create a source with audibly different first and second halves");
            const auto markedPath = scratch + L"\\web-resumed-tone.aac";
            auto secondHalf = ReadMedia(web::AacMedia(marked, 2, cancel, 96, &cache), 0, SIZE_MAX);
            Ready(WriteTestBytes(markedPath, secondHalf), L"save the resumed compressed audio for content verification");
            unsigned rate = 0;
            auto resumedPcm = DecodeAll(markedPath, rate);
            auto power = [&](double hz) {
                double real = 0, imaginary = 0;
                for (size_t i = 4096; i < std::min<size_t>(resumedPcm.size() / 2, 28096); ++i) {
                    double phase = 2 * 3.141592653589793 * hz * i / rate;
                    real += resumedPcm[i * 2] * std::cos(phase);
                    imaginary += resumedPcm[i * 2] * std::sin(phase);
                }
                return real * real + imaginary * imaginary;
            };
            Check(rate == 48000 && resumedPcm.size() >= 2 * 48000 * 2 && power(880) > 100 * power(220),
                  L"a compressed resume plays the requested second half rather than replaying the beginning");
            auto firstHalf = ReadMedia(web::AacMedia(marked, 0, cancel, 96, &cache), 0, SIZE_MAX);
            std::fill(changing.begin() + 44, changing.end(), '\0');
            Ready(WriteTestBytes(marked.path, changing), L"replace only the isolated source audio to check cache invalidation");
            auto replaced = ReadMedia(web::AacMedia(marked, 0, cancel, 96, &cache), 0, SIZE_MAX);
            Check(!replaced.empty() && replaced != firstHalf,
                  L"changing the source file invalidates the compressed representation even at the same path and size");
            auto lowRate = track; lowRate.path = scratch + L"\\web-data-saver-8k.wav";
            Ready(WriteTestBytes(lowRate.path, WaveBytes(8000)), L"create an isolated low-rate streaming source");
            auto upsampled = ReadMedia(web::AacMedia(lowRate, 0, cancel), 0, SIZE_MAX);
            Check(AdtsFrames(upsampled) >= 94 && AdtsFrames(upsampled) <= 98,
                  L"large resampler output from low-rate sources is encoded completely with bounded input blocks");
            auto audioCue = scratch + L"\\web-audio.cue";
            Ready(WriteTestBytes(audioCue, "FILE \"web-audio.wav\" WAVE\n TRACK 01 AUDIO\n INDEX 01 00:00:00\n TRACK 02 AUDIO\n INDEX 01 00:01:00\n"),
                  L"create an isolated CUE boundary for compressed streaming");
            auto subtrack = track; subtrack.path = SubtrackPath(path, 2); subtrack.original = false;
            WIN32_FILE_ATTRIBUTE_DATA cueInfo{};
            Ready(GetFileAttributesExW(audioCue.c_str(), GetFileExInfoStandard, &cueInfo),
                  L"read the CUE sheet timestamp as the scanner does");
            subtrack.fileSize = original.size();
            subtrack.fileTime = ((uint64_t)cueInfo.ftLastWriteTime.dwHighDateTime << 32) |
                cueInfo.ftLastWriteTime.dwLowDateTime;
            auto slice = ReadMedia(web::AacMedia(subtrack, 0, cancel), 0, SIZE_MAX);
            Check(AdtsFrames(slice) >= 47 && AdtsFrames(slice) <= 51,
                  L"data saver respects CUE track boundaries instead of streaming the whole album");
            DeleteFileW(audioCue.c_str());
            auto stopped = web::AacMedia(track, 0, cancel);
            unsigned packets = 0;
            bool completed = stopped.send && stopped.send(0, (size_t)stopped.size, [&](const char*, size_t) {
                ++packets; cancel = true; return true;
            });
            Check(!completed && packets == 1, L"disconnect cancellation stops the compressed encoder after its current packet");
            cancel = false;
        }
        cancel = true;
        Check(!web::AacMedia(track, 0, cancel).error.empty(), L"cancelled data saver never starts encoding");
        cancel = false;
        Check(!web::AacMedia(track, 3, cancel).error.empty() && !web::AacMedia(track, NAN, cancel).error.empty(),
              L"data saver rejects invalid seek positions before starting a response");
        Check(!web::AacMedia(track, 0, cancel, 0).error.empty() && !web::AacMedia(track, 0, cancel, 4096).error.empty(),
              L"unsupported compressed bitrates are refused before allocating an encoder");
    }
    {
        testlog::Group group(L"browser PCM delivery");
        auto fullMedia = WaveMedia(track, 0, cancel);
        auto pcm = ReadMedia(std::move(fullMedia), 0, 44 + 48000 * 2 * 4);
        Check(pcm.size() == 44 + 48000 * 2 * 4 && pcm.substr(0, 4) == "RIFF" && pcm.substr(8, 8) == "WAVEfmt ",
              L"fallback is a complete stereo PCM16 48 kHz WAV");
        bool equal = !pcm.empty();
        for (size_t at : {0u, 39u, 43u, 44u, 45u, 1003u, 153779u}) {
            auto bytes = ReadMedia(WaveMedia(track, 0, cancel), at, 1234);
            equal = equal && bytes == pcm.substr(at, 1234);
        }
        Check(equal, L"unaligned WAV ranges reproduce full-stream bytes across headers and resampler phases");
        Check(ReadMedia(OriginalMedia(track), 0, original.size()) == original,
              L"original streams preserve every source byte");
        std::string cue = "FILE \"web-audio.wav\" WAVE\n TRACK 01 AUDIO\n INDEX 01 00:00:00\n TRACK 02 AUDIO\n INDEX 01 00:01:00\n";
        WriteTestBytes(scratch + L"\\web-audio.cue", cue);
        auto subtrack = track; subtrack.path = SubtrackPath(path, 2); subtrack.original = false;
        auto slice = WaveMedia(subtrack, 0, cancel);
        Check(slice.error.empty() && slice.size == 44 + 48000 * 4 &&
              !ReadMedia(std::move(slice), 44, 1024).empty(), L"CUE fallback contains only the selected track");
        Check(!OriginalMedia(subtrack).error.empty(), L"a CUE URL cannot expose the whole underlying album");
        cancel = true;
        auto stopped = ReadMedia(WaveMedia(track, 0, cancel), 44, 4000);
        Check(stopped.empty(), L"cancelled fallback stops before producing PCM"); cancel = false;
        Check(!WaveMedia(track, 3, cancel).error.empty() && !WaveMedia(track, NAN, cancel).error.empty(),
              L"invalid decoded positions fail before any response body");
        auto longTrack = WaveMedia(std::make_shared<LongTrack>(), 9 * 3600, cancel);
        Check(longTrack.error.empty() && longTrack.size == 44 + 3600ull * 48000 * 4 &&
              !ReadMedia(std::move(longTrack), 44, 512).empty(),
              L"long decoded tracks use bounded WAV windows and seek without decoding preceding hours");
        DeleteFileW((scratch + L"\\web-audio.cue").c_str());
    }
    {
        testlog::Group group(L"paired browser server");
        Server server;
        Config config; config.port = 0; config.devicesPath = scratch + L"\\web-devices-test.bin";
        config.offlinePath = scratch + L"\\web-offline-test.bin";
        DeleteFileW(config.devicesPath.c_str());
        DeleteFileW(config.offlinePath.c_str());
        Ready(server.Start(config), L"start a loopback-only server on an available port");
        auto status = server.GetStatus();
        if (!status.running) return;
        unsigned port = status.port;
        auto snapshot = std::make_shared<Snapshot>(); snapshot->revision = 1; snapshot->tracks.push_back(track);
        for (int i = 1; i <= 12; ++i) {
            auto t = track; t.key = "page-" + std::to_string(i); t.title = "Z page " + std::to_string(i);
            snapshot->tracks.push_back(std::move(t));
        }
        snapshot->playlists.push_back({"saved", "Saved 雪", {"page-2", track.key, "page-2"}});
        server.Publish(snapshot);
        Check(Request(port, L"/api/v1/catalog").status == 401 && Request(port, L"/api/v1/audio?id=fixture-track").status == 401 &&
              Request(port, L"/api/v1/audio?id=fixture-track&format=aac").status == 401,
              L"library metadata and audio require pairing");
        auto playerPage = Request(port, L"/");
        Check(playerPage.status == 200 && Request(port, L"/app.js").status == 200,
              L"the pairing interface is available without credentials");
        size_t audioAt = playerPage.body.find("<audio ");
        auto audioTag = audioAt == std::string::npos ? std::string{} :
            playerPage.body.substr(audioAt, playerPage.body.find('>', audioAt) - audioAt);
        Check(audioAt != std::string::npos && playerPage.body.find("<audio ", audioAt + 1) == std::string::npos &&
              audioTag.find("preload=\"none\"") != std::string::npos && audioTag.find("src=") == std::string::npos,
              L"the page has one source-free audio element and tells the browser not to preload music");
        auto themes = Request(port, L"/api/v1/themes");
        Check(themes.status == 200 && themes.body.find("DELTARUNE") != themes.body.npos &&
              themes.body.find("Midnight") != themes.body.npos && themes.body.find("Terminal") != themes.body.npos,
              L"all native themes are available on every server");
        Check(server.Pair(), L"explicit desktop pairing creates a temporary code");
        auto paired = Request(port, L"/api/v1/pair", ("code=" + server.GetStatus().pairingCode + "&name=Test+phone").c_str());
        std::string cookie = paired.cookie;
        Check(paired.status == 200 && !cookie.empty() && server.GetStatus().pairingCode.empty() &&
              server.GetStatus().devices.size() == 1, L"pairing consumes its code and stores one browser");
        Reply catalog;
        Pump(1000, [&] { catalog = Request(port, L"/api/v1/catalog?limit=1", nullptr, cookie); return catalog.body.find("Sinus") != catalog.body.npos; });
        Check(catalog.status == 200 && catalog.body.find("雪") != catalog.body.npos &&
              catalog.body.find("web-audio.wav") == catalog.body.npos, L"catalog returns Unicode metadata without filesystem paths");
        auto page = Request(port, L"/api/v1/catalog?offset=1&limit=2", nullptr, cookie);
        Check(page.status == 200 && page.body.find("\"total\":13") != page.body.npos &&
              page.body.find("fixture-track") == page.body.npos &&
              Request(port, L"/api/v1/catalog?offset=999999", nullptr, cookie).body.find("\"items\":[]") != page.body.npos,
              L"pagination bounds results and safely handles offsets past the end");
        auto saved = Request(port, L"/api/v1/catalog?playlist=saved", nullptr, cookie);
        size_t first = saved.body.find("page-2"), middle = saved.body.find("fixture-track"), last = saved.body.rfind("page-2");
        Check(first < middle && middle < last && saved.body.find("\"total\":3") != saved.body.npos &&
              Request(port, L"/api/v1/catalog?artist=Geoffrey+Day", nullptr, cookie).body.find("\"total\":13") != page.body.npos,
              L"saved playlists retain duplicates and contributing artists have a working view");
        Check(Request(port, L"/api/v1/albums", nullptr, cookie).body.find("Fixture album") != std::string::npos &&
              Request(port, L"/api/v1/artists", nullptr, cookie).body.find("Geoffrey Day") != std::string::npos &&
              Request(port, L"/api/v1/playlists", nullptr, cookie).body.find("Saved 雪") != std::string::npos &&
              Request(port, L"/api/v1/library?album=Fixture+album", nullptr, cookie).body.find("\"total\":13") != std::string::npos,
              L"versioned album, artist and playlist routes expose the same owned catalog");
        server.SetTheme(ThemeMode::Terminal, true);
        Check(Request(port, L"/api/v1/themes").body.find("\"current\":7") != std::string::npos,
              L"host theme changes update the shared theme metadata while running");
        Check(Request(port, L"/api/v1/catalog", nullptr, cookie, L"Origin: http://example.invalid\r\n").status == 403,
              L"cross-origin requests cannot use paired credentials");
        auto bytes = Request(port, L"/api/v1/audio?id=fixture-track", nullptr, cookie, L"Range: bytes=40-99\r\n");
        Check(bytes.status == 206 && bytes.body == original.substr(40, 60), L"real HTTP original ranges are byte-identical");
        auto head = Request(port, L"/api/v1/audio?id=fixture-track&format=wav", nullptr, cookie, {}, true);
        Check(head.status == 200 && head.body.empty() && head.headers.find("Content-Length: 384044") != head.headers.npos,
              L"HEAD reports decoded length without sending audio");
        auto compressed = Request(port, L"/api/v1/audio?id=fixture-track&format=aac", nullptr, cookie);
        Check(compressed.status == 200 && AdtsFrames(compressed.body) >= 94 &&
              compressed.headers.find("Content-Type: audio/aac") != compressed.headers.npos &&
              compressed.headers.find("Content-Length: " + std::to_string(compressed.body.size()) + "\r\n") != compressed.headers.npos &&
              compressed.headers.find("Accept-Ranges: bytes") != compressed.headers.npos,
              L"HTTP compressed audio has an exact length and advertises byte range support");
        auto probe = Request(port, L"/api/v1/audio?id=fixture-track&format=aac", nullptr, cookie, L"Range: bytes=0-1\r\n");
        Check(probe.status == 206 && probe.body == compressed.body.substr(0, 2) &&
              probe.headers.find("Content-Range: bytes 0-1/" + std::to_string(compressed.body.size()) + "\r\n") != probe.headers.npos,
              L"the iPhone's initial two-byte probe receives only those bytes and the correct total length");
        size_t midpoint = compressed.body.size() / 2;
        auto range = L"Range: bytes=" + std::to_wstring(midpoint) + L"-\r\n";
        auto resumed = Request(port, L"/api/v1/audio?id=fixture-track&format=aac", nullptr, cookie, range);
        Check(resumed.status == 206 && resumed.body == compressed.body.substr(midpoint) &&
              resumed.headers.find("Content-Range: bytes " + std::to_string(midpoint) + '-') != resumed.headers.npos,
              L"a halfway compressed reconnection returns the remaining bytes without repeating the start of the song");
        auto suffix = Request(port, L"/api/v1/audio?id=fixture-track&format=aac", nullptr, cookie, L"Range: bytes=-123\r\n");
        Check(suffix.status == 206 && suffix.body == compressed.body.substr(compressed.body.size() - 123),
              L"compressed suffix probes follow the same rules as source audio");
        auto compressedHead = Request(port, L"/api/v1/audio?id=fixture-track&format=aac", nullptr, cookie, {}, true);
        Check(compressedHead.status == 200 && compressedHead.body.empty() &&
              compressedHead.headers.find("Content-Length: " + std::to_string(compressed.body.size()) + "\r\n") != compressedHead.headers.npos,
              L"compressed HEAD reports the same representation length without sending audio");
        auto later = Request(port, L"/api/v1/audio?id=fixture-track&format=aac&start=1", nullptr, cookie);
        Check(later.status == 200 && AdtsFrames(later.body) >= 47 && AdtsFrames(later.body) <= 51 &&
              Request(port, L"/api/v1/audio?id=fixture-track&format=aac&start=3", nullptr, cookie).status == 409 &&
              Request(port, L"/api/v1/audio?id=fixture-track&format=unknown", nullptr, cookie).status == 400,
              L"HTTP compressed seeks are bounded and invalid requests never fall back to original audio");
        auto balanced = Request(port, L"/api/v1/audio?id=fixture-track&format=aac&bitrate=128", nullptr, cookie);
        Check(balanced.status == 200 && AdtsFrames(balanced.body) >= 94 && balanced.body.size() > compressed.body.size() * 1.15 &&
              Request(port, L"/api/v1/audio?id=fixture-track&format=aac&bitrate=0", nullptr, cookie).status == 400 &&
              Request(port, L"/api/v1/audio?id=fixture-track&format=aac&bitrate=128oops", nullptr, cookie).status == 400,
              L"HTTP offers both compressed modes and refuses malformed or unsupported bitrate requests");
        Check(Request(port, L"/api/v1/state", nullptr, cookie).body.find("\"dataSaver\":true") != std::string::npos,
              L"the browser can discover the host's compressed streaming option");
        Check(Request(port, L"/api/v1/audio?id=fixture-track&format=aac", nullptr, cookie,
                      L"Range: bytes=9999999-\r\n").status == 416 &&
              Request(port, L"/api/v1/audio?id=fixture-track", nullptr, cookie,
                      L"Range: bytes=9999999-\r\n").status == 416,
              L"out-of-bounds HTTP ranges return 416 for compressed and source audio");
        auto listen = [&](const char* event, unsigned seq, double heard) {
            std::string body = "session=00112233445566778899aabbccddeeff&id=fixture-track&event=" + std::string(event) +
                "&seq=" + std::to_string(seq) + "&heard=" + std::to_string(heard);
            return Request(port, L"/api/v1/listen", body.c_str(), cookie);
        };
        Check(server.TakeListens().empty(), L"fetching and buffering audio never produces a listen");
        listen("start", 1, 0); Pump(1100); listen("progress", 2, 1.1); listen("progress", 2, 1.1);
        listen("seek", 3, 1.1); listen("pause", 4, 1.1); Pump(100); listen("resume", 5, 1.1); listen("ended", 6, 1.1);
        auto listens = server.TakeListens();
        listen("ended", 6, 1.1); listen("start", 1, 0);
        Check(listens.size() == 1 && listens[0].seconds >= 1.05 && listens[0].seconds <= 1.15 &&
              !listens[0].manual && server.TakeListens().empty(),
              L"progress counts once, excludes seek/pause and cannot replay a completed session");
        auto worker = Request(port, L"/sw.js");
        Check(worker.status == 200 && worker.body.find("cordinal-kept") != std::string::npos && worker.body.find("206") != std::string::npos &&
              worker.headers.find("Cache-Control: no-cache") != std::string::npos,
              L"the service worker is served, uncached, and answers kept music by byte range");
        // Offline plays: only of a track this browser downloaded to keep, ending between then and now, once each.
        const uint64_t now = (uint64_t)std::time(nullptr);
        auto play = [&](char id, const std::string& key, uint64_t end, const char* seconds) {
            return std::string(32, id) + "_" + key + "_" + std::to_string(end) + "_" + seconds + "_0";
        };
        auto played = [&](const std::string& plays) { return Request(port, L"/api/v1/played", ("plays=" + plays).c_str(), cookie).body; };
        const std::string unkept = played(play('a', "fixture-track", now, "1.5"));
        const bool downloaded = Request(port, L"/api/v1/audio?id=fixture-track&keep=1", nullptr, cookie).status == 200;
        const std::string batch = played(play('b', "fixture-track", now, "1.5") + "*" + play('b', "fixture-track", now, "1.5") + "*" +
                                         play('c', "fixture-track", now - 3600, "1.5") + "*" + play('d', "fixture-track", now + 3600, "1.5") + "*" +
                                         play('e', "page-3", now, "1.5") + "*" + play('f', "fixture-track", now, "99999") + "*garbage");
        auto offline = server.TakeListens();
        Check(unkept.find("\"accepted\":0") != std::string::npos && downloaded && batch.find("\"accepted\":1,\"refused\":6") != std::string::npos &&
                  offline.size() == 1 && offline[0].time == now && offline[0].path == track.path && offline[0].seconds == 1.5,
              L"offline plays count once, only for kept tracks, between keeping and now (%hs, %zu listens)", batch.c_str(), offline.size());
        testlog::Cases ordering;
        for (unsigned shape = 0; shape < 8; ++shape) {
            auto input = std::make_shared<Snapshot>(); input->revision = 100 + shape;
            uint32_t random = TestSeed();
            const unsigned count = shape == 6 ? 0 : shape == 7 ? 1 : 72;
            for (unsigned i = 0; i < count; ++i) {
                random = random * 1664525u + 1013904223u;
                unsigned key = shape == 3 ? 0 : (random >> 16) % 9;
                auto t = track;
                t.key = "order-" + std::to_string(i); t.artist = "Payload 雪 " + std::to_string(i);
                t.albumArtist = "Artist " + std::to_string(key / 4);
                t.album = "Album " + std::to_string(key / 3); t.date = "2026";
                t.disc = key / 6; t.number = key;
                t.title = std::string(80, 'T') + " 雪 " + std::to_string(key);
                input->tracks.push_back(std::move(t));
            }
            // The generated keys increase in tuple order; equal keys keep their capture order.
            auto reference = [&](const std::vector<web::Track>& tracks) {
                std::vector<web::Track> result;
                for (unsigned key = 0; key < 9; ++key)
                    for (const auto& t : tracks)
                        if (t.number == key) result.push_back(t);
                return result;
            };
            if (shape == 1 || shape == 2 || shape == 4 || shape == 5)
                input->tracks = reference(input->tracks);
            if (shape == 2) std::reverse(input->tracks.begin(), input->tracks.end());
            if (shape == 4) std::swap(input->tracks.front(), input->tracks.back());
            if (shape == 5) std::swap(input->tracks[count - 2], input->tracks[count / 4]);
            auto expected = *input;
            expected.tracks = reference(input->tracks);
            web::Request query; query.query = {{"limit", "128"}};
            const auto wanted = web::Catalog(expected, query);
            server.Publish(std::move(input));
            Reply actual;
            Pump(1000, [&] {
                actual = Request(port, L"/api/v1/catalog?limit=128", nullptr, cookie);
                return actual.body.find("\"revision\":" + std::to_string(expected.revision)) != actual.body.npos;
            });
            bool same = actual.status == 200 && actual.body == wanted;
            for (const auto& t : expected.tracks)
                same = same && Request(port, L"/api/v1/track?id=" + FromUtf8(t.key), nullptr, cookie).body == web::TrackJson(t);
            ordering.Add(same, L"shape " + std::to_wstring(shape));
        }
        Check(ordering.Passed(), L"published web sorts preserve known order, ties, payloads and key lookups: seed=%u%ls",
              TestSeed(), ordering.failed.c_str());
        auto replaced = std::make_shared<Snapshot>(); replaced->revision = 2; server.Publish(replaced);
        Pump(1000, [&] { return Request(port, L"/api/v1/track?id=fixture-track", nullptr, cookie).status == 404; });
        Check(Request(port, L"/api/v1/track?id=fixture-track", nullptr, cookie).status == 404,
              L"removed identifiers cannot select another track");
        server.Stop();
        Check(server.Start(config) && server.GetStatus().devices.size() == 1, L"paired browsers survive server restarts");
        port = server.GetStatus().port; auto device = server.GetStatus().devices.front().id;
        {
            // The kept track is gone from the library view and the server restarted (on a new port,
            // so a new cookie name); the phone's play still counts.
            const std::string old = cookie;
            cookie = "cordinal_" + std::to_string(port) + old.substr(old.find('='));
            const std::string afterRestart = played(play('7', "fixture-track", now, "1.5") + "*" + play('b', "fixture-track", now, "1.5"));
            auto credited = server.TakeListens();
            Check(afterRestart.find("\"accepted\":1,\"refused\":1") != std::string::npos && credited.size() == 1 && credited[0].path == track.path,
                  L"offline grants and counted plays survive a server restart (%hs)", afterRestart.c_str());
            cookie = old;
        }
        server.Revoke(device);
        Check(Request(port, L"/api/v1/catalog", nullptr, cookie).status == 401 && server.GetStatus().devices.empty(),
              L"revocation immediately invalidates remembered credentials");
        server.Pair();
        for (int i = 0; i < 5; ++i) Request(port, L"/api/v1/pair", "code=wrong");
        Check(server.GetStatus().pairingCode.empty() && Request(port, L"/api/v1/pair", "code=wrong").status == 429,
              L"pairing codes are disabled after five failed attempts");
        std::string code;
        Check(!web::CheckFixedCode(" 4829-1735 ", code) && code == "48291735" && !web::CheckFixedCode("", code) && code.empty() &&
              web::CheckFixedCode("48291", code) && web::CheckFixedCode("4829173548291", code) && web::CheckFixedCode("48a917", code) &&
              web::CheckFixedCode("777777", code) && web::CheckFixedCode("901234", code) && web::CheckFixedCode("654321", code),
              L"fixed codes are 6 to 12 digits and refuse a repeated digit or a straight run");
        server.SetFixedCode("482917");
        Check(server.GetStatus().pairingCode == "482917" && !server.GetStatus().pairingSeconds,
              L"a fixed code applies to the running server at once and does not expire");
        auto fixed = Request(port, L"/api/v1/pair", "code=482917&name=Fixed+phone");
        Check(fixed.status == 200 && !fixed.cookie.empty() && server.GetStatus().pairingCode == "482917",
              L"a fixed code pairs a browser and stays open for the next one");
        server.Stop(); config.fixedCode = "482917";
        Check(server.Start(config) && server.GetStatus().pairingCode == "482917", L"the fixed code is kept across a stop and start");
        port = server.GetStatus().port;
        // The ephemeral test port changes on restart, and the cookie is named after the port.
        std::string again = "cordinal_" + std::to_string(port) + fixed.cookie.substr(fixed.cookie.find('='));
        Check(Request(port, L"/api/v1/catalog", nullptr, again).status == 200, L"browsers paired with a fixed code stay paired");
        for (int i = 0; i < 5; ++i) Request(port, L"/api/v1/pair", "code=111111");
        Check(server.GetStatus().pairingCode.empty() && server.GetStatus().fixedCode &&
              Request(port, L"/api/v1/pair", "code=482917").status == 429,
              L"five wrong tries lock a fixed code too");
        Check(server.Pair() && server.GetStatus().pairingCode == "482917", L"Pair device reopens a locked fixed code");
        server.SetFixedCode("");
        Check(server.GetStatus().pairingCode.empty() && !server.GetStatus().fixedCode,
              L"clearing the fixed code returns to temporary codes");
        server.Stop();
        Check(!server.GetStatus().running, L"stopping the server joins its workers");
        DeleteFileW(config.devicesPath.c_str());
    }
    {
        testlog::Group group(L"QR codes for pairing links");
        Check(qr::CheckBytes({32, 91, 11, 120, 209, 114, 220, 77, 67, 64, 236, 17, 236, 17, 236, 17}, 10) ==
              std::vector<uint8_t>{196, 35, 39, 119, 235, 215, 231, 226, 93, 23},
              L"Reed-Solomon check bytes match the standard's worked example");
        // Golden matrices: these symbols were read back by an independent decoder (jsQR) when the
        // encoder was written; a change that alters them needs the same outside check again.
        auto hash = [](const qr::Code& code) {
            uint64_t h = 1469598103934665603ull;
            for (auto m : code.modules) h = (h ^ m) * 1099511628211ull;
            return h;
        };
        auto link = qr::Encode("https://73.12.200.41/#pair=0123456789abcdef0123456789abcdef");
        auto longer = qr::Encode("The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog. "
                                 "0123456789 The quick brown fox jumps over the lazy dog!!");
        Check(link.size == 33 && hash(link) == 0x16c1fb6366a87468ull && longer.size == 49 &&
              hash(longer) == 0x85803ed93018be29ull, L"pairing links and longer text encode to the verified symbols");
        Check(qr::Encode(std::string(2331, 'x')).size == 177 && qr::Encode(std::string(2332, 'x')).size == 0,
              L"the largest version holds 2331 bytes and longer text is refused");
    }
    {
        testlog::Group group(L"secure away-from-home access");
        Server server;
        Config config; config.port = 0; config.devicesPath = scratch + L"\\web-devices-away.bin";
        config.names = {"music.example"};
        DeleteFileW(config.devicesPath.c_str());
        Ready(server.Start(config), L"start a loopback-only server for HTTPS checks");
        const unsigned port = server.GetStatus().port;
        const std::string origin = "127.0.0.1:" + std::to_string(port);
        auto snapshot = std::make_shared<Snapshot>(); snapshot->revision = 1; snapshot->tracks.push_back(track);
        server.Publish(snapshot);
        // A key name of its own, removed again, so the check never touches a real server key.
        const std::wstring key = L"cordinal self-test " + FromUtf8(web::RandomHex(8));
        struct KeyGuard { const std::wstring& name; ~KeyGuard() { web::DeleteTlsKey(name); } } keyGuard{key};
        std::string error, der = web::SelfSignedCertificate(key, {"127.0.0.1", "::1", "music.example"}, 2);
        auto identity = der.empty() ? nullptr : web::TlsIdentity::Create({der}, key, error);
        Check(identity && identity->Names() == std::vector<std::string>{"127.0.0.1", "::1", "music.example"},
              L"a persisted key and its certificate make SChannel credentials %hs", error.c_str());
        web::Away away;
        away.identity = identity; away.url = "https://" + origin + "/"; away.hosts = {origin};
        away.challenges = {{"token-123", "token-123.thumbprint"}};
        server.SetAway(away);
        auto page = Request(port, L"/", nullptr, {}, {}, false, true);
        Check(page.status == 200 && page.body.starts_with("<!doctype html>") && Request(port, L"/").status == 200,
              L"one port answers HTTPS and plain HTTP alike");
        Check(Request(port, L"/.well-known/acme-challenge/token-123").body == "token-123.thumbprint" &&
              Request(port, L"/.well-known/acme-challenge/other").status == 404,
              L"certificate challenges are answered over plain HTTP, and only known ones");
        server.TreatLoopbackAsAwayForTest(true);
        auto moved = Request(port, L"/api/v1/catalog?q=x");
        Check(moved.status == 307 && moved.headers.find("Location: https://" + origin + "/api/v1/catalog?q=x") != std::string::npos,
              L"plain HTTP from away is sent to the secure address before any credentials are read");
        server.Pair();
        Check(Request(port, L"/api/v1/pair", ("code=" + server.GetStatus().pairingCode).c_str(), {}, {}, false, true).status == 403,
              L"away browsers can't pair with the short code");
        auto link = server.PairingLink();
        auto linked = Request(port, L"/api/v1/pair", ("link=" + link + "&name=Phone").c_str(), {}, {}, false, true);
        Check(linked.status == 200 && linked.cookie.starts_with("__Host-cordinal=") &&
              linked.headers.find("; Secure") != std::string::npos,
              L"a pairing link pairs from away over HTTPS with a secure host-only cookie");
        Check(Request(port, L"/api/v1/pair", ("link=" + link).c_str(), {}, {}, false, true).status == 401,
              L"a pairing link works once");
        Reply catalog;
        Pump(1000, [&] {
            catalog = Request(port, L"/api/v1/catalog", nullptr, linked.cookie, {}, false, true);
            return catalog.body.find("Sinus") != std::string::npos;
        });
        auto status = server.GetStatus();
        Check(catalog.status == 200 && !status.devices.empty() && status.devices.back().away && status.devices.back().seen,
              L"the paired browser reads the library over HTTPS and shows as away");
        auto audio = Request(port, L"/api/v1/audio?id=fixture-track", nullptr, linked.cookie, L"Range: bytes=40-99\r\n", false, true);
        Check(audio.status == 206 && audio.body == original.substr(40, 60), L"audio ranges stream byte-identical over HTTPS");
        auto compressed = Request(port, L"/api/v1/audio?id=fixture-track&format=aac", nullptr, linked.cookie, {}, false, true);
        Check(compressed.status == 200 && AdtsFrames(compressed.body) >= 94 &&
              compressed.headers.find("Content-Length: " + std::to_string(compressed.body.size()) + "\r\n") != compressed.headers.npos,
              L"compressed audio reports and sends its exact length over HTTPS");
        auto resumed = Request(port, L"/api/v1/audio?id=fixture-track&format=aac", nullptr, linked.cookie,
                               L"Range: bytes=1003-4321\r\n", false, true);
        Check(resumed.status == 206 && resumed.body == compressed.body.substr(1003, 3319),
              L"HTTPS compressed resumption returns the exact requested bytes rather than the beginning");
        server.TreatLoopbackAsAwayForTest(false);
        server.Pair();
        auto home = Request(port, L"/api/v1/pair", ("code=" + server.GetStatus().pairingCode + "&name=Tablet").c_str());
        auto offer = Request(port, L"/api/v1/away", "", home.cookie);
        size_t at = offer.body.find("#pair=");
        std::string token = at == std::string::npos ? "" : offer.body.substr(at + 6, 32);
        Check(home.status == 200 && offer.status == 200 && offer.body.find("https://" + origin + "/#pair=") != std::string::npos &&
              Request(port, L"/api/v1/state", nullptr, home.cookie).body.find("\"away\":\"https://" + origin + "/\"") != std::string::npos,
              L"a browser paired at home can fetch a one-time link to the secure address");
        auto awayTablet = Request(port, L"/api/v1/pair", ("link=" + token).c_str(), {}, {}, false, true);
        status = server.GetStatus();
        Check(awayTablet.status == 200 && !status.devices.empty() && status.devices.back().name == "Tablet (away)",
              L"that link pairs the secure address under the same name");
        Check(Request(port, L"/api/v1/catalog", nullptr, {}, L"Origin: https://" + FromUtf8(origin) + L"\r\n", false, true).status == 401 &&
              Request(port, L"/api/v1/catalog", nullptr, {}, L"Origin: http://" + FromUtf8(origin) + L"\r\n", false, true).status == 403,
              L"HTTPS requests accept only their own https origin");
        // A reverse proxy on this computer: its name is allowed, its HTTPS counts, and its client decides
        // home or away, so a proxy that publishes to the internet can't open code pairing to everyone.
        server.Pair();
        const std::wstring proxy = L"Host: music.example\r\nOrigin: https://music.example\r\nX-Forwarded-Proto: https\r\n";
        auto fromInternet = Request(port, L"/api/v1/pair", ("code=" + server.GetStatus().pairingCode).c_str(), {},
                                    proxy + L"X-Forwarded-For: 203.0.113.9, 10.0.0.1\r\n");
        auto fromTailnet = Request(port, L"/api/v1/pair", ("code=" + server.GetStatus().pairingCode + "&name=Proxied").c_str(), {},
                                   proxy + L"X-Forwarded-For: 100.101.102.103\r\n");
        Check(fromInternet.status == 403 && fromTailnet.status == 200 && fromTailnet.cookie.starts_with("__Host-cordinal=") &&
              Request(port, L"/", nullptr, {}, L"Host: other.example\r\n").status == 403,
              L"a local reverse proxy's name and HTTPS are honoured, and its internet clients count as away");
        server.Stop();
        DeleteFileW(config.devicesPath.c_str());
    }
    TestWebAway(scratch);
    DeleteFileW(path.c_str());
}

// A companion-only preview for browser/phone QA against generated fixtures. The control file is
// test output (not a player log) and is removed on exit; it never contains real library credentials.
int RunWebPreview(const std::vector<std::wstring>& args) {
    if (args.size() < 3) return 2;
    std::vector<ScanFile> files;
    if (!ListAudioFiles(args[1], files)) return 3;
    auto snapshot = std::make_shared<web::Snapshot>(); snapshot->revision = 1;
    for (const auto& file : files) {
        TagData tags; if (!ReadTags(file.path, tags)) continue;
        auto t = TestTrack(file.path); t.key = "fixture-" + std::to_string(snapshot->tracks.size());
        t.title = tags.title.empty() ? ToUtf8(StemOf(file.path)) : tags.title;
        t.artist = tags.artist; t.albumArtist = tags.albumArtist.empty() ? tags.artist : tags.albumArtist;
        t.album = tags.album; t.duration = tags.duration; t.codec = ToUtf8(CodecName(tags.codec));
        t.mime = web::Mime(file.path, t.codec); t.original = !t.mime.empty(); t.number = tags.track;
        snapshot->tracks.push_back(std::move(t));
    }
    web::Playlist originals{"originals", "Original streams", {}}, converted{"converted", "WAV fallback", {}};
    const size_t count = snapshot->tracks.size();
    for (size_t i = 0; i < count; ++i) {
        const auto& t = snapshot->tracks[i];
        auto ext = ExtOf(t.path);
        if (ext == L"mp3" || ext == L"m4a" || ext == L"wav") originals.tracks.push_back(t.key);
        if (converted.tracks.size() < 3) {
            auto copy = t; copy.key += "-converted"; copy.title += " (WAV fallback)";
            copy.original = false; copy.library = false; converted.tracks.push_back(copy.key);
            snapshot->tracks.push_back(std::move(copy));
        }
    }
    snapshot->playlists = {std::move(originals), std::move(converted)};
    web::Server server; web::Config config;
    config.port = args.size() > 3 ? (uint16_t)_wtoi(args[3].c_str()) : 0;
    HRSRC font = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_FONT_DELTARUNE), RT_RCDATA);
    config.font = {(const char*)LockResource(LoadResource(nullptr, font)), SizeofResource(nullptr, font)};
    if (!server.Start(config) || !server.Pair()) return 4;
    server.Publish(snapshot);
    auto ready = [&] {
        auto status = server.GetStatus();
        return WriteTestBytes(args[2], "{\"port\":" + std::to_string(status.port) + ",\"code\":" +
            web::Json(status.pairingCode) + '}');
    };
    if (!ready()) return 5;
    uint64_t until = GetTickCount64() + 60 * 60 * 1000;
    while (GetTickCount64() < until && GetFileAttributesW((args[2] + L".stop").c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (DeleteFileW((args[2] + L".pair").c_str())) { server.Pair(); ready(); }
        Sleep(100);
    }
    server.Stop(); DeleteFileW(args[2].c_str()); DeleteFileW((args[2] + L".stop").c_str());
    return 0;
}

// What away-from-home listening would find on this network: the route, any UPnP router and the
// address it reports, the address STUN servers see, and whether ports 443 and 80 are free here.
// Real requests, made only when this is run: multicast on the home network and one STUN query each.
int RunAwayProbe() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 3;
    std::string local, gateway;
    web::DefaultRoute(local, gateway);
    fwprintf(stdout, L"route: this computer is %hs, its gateway %hs\n", local.c_str(), gateway.c_str());
    auto locations = web::DiscoverGateways(local, 2500);
    if (locations.empty()) fwprintf(stdout, L"router: no UPnP gateway answered (UPnP is off, or this router has none)\n");
    for (const auto& location : locations) {
        web::Gateway g;
        g.local = local;
        if (!web::ReadGateway(nullptr, location, g)) {
            fwprintf(stdout, L"router: %hs offers no internet connection service\n", location.c_str());
            continue;
        }
        std::string external = web::GatewayAddress(nullptr, g);
        fwprintf(stdout, L"router: %hs reports the internet address %hs (%ls)\n", g.control.c_str(),
                 external.empty() ? "nothing" : external.c_str(), web::PublicIPv4(external) ? L"public" : L"not public");
    }
    for (const auto& [host, port] : web::RemoteConfig{}.stun) {
        std::string address = web::StunAddress(host, port, 2000);
        fwprintf(stdout, L"stun %hs:%u: %hs%ls\n", host.c_str(), port, address.empty() ? "no answer" : address.c_str(),
                 address.empty() ? L"" : web::PublicIPv4(address) ? L" (public)" : L" (not public)");
    }
    for (unsigned port : {443u, 80u}) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons((u_short)port);
        bool free = s != INVALID_SOCKET && !bind(s, (sockaddr*)&address, sizeof(address));
        if (s != INVALID_SOCKET) closesocket(s);
        fwprintf(stdout, L"port %u on this computer: %ls\n", port, free ? L"free" : L"in use by another program");
    }
    WSACleanup();
    return 0;
}

// Called by the existing isolated native-app benchmark: real muted desktop output and four
// independent decoding clients, while the UI publishes snapshots and services browse requests.
bool MeasureWebRuntime(FILE* log) {
    auto cpu = [] {
        FILETIME create{}, exit{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
        return (((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
            (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime);
    };
    LARGE_INTEGER frequency{}, a{}, b{}; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&a);
    for (unsigned i = 0; i < 1000; ++i) { web::Server disabled; }
    QueryPerformanceCounter(&b);
    fwprintf(log, L"disabled_construct_destroy_us=%.3f disabled_workers=0 disabled_sockets=0\n",
        1000.0 * (b.QuadPart - a.QuadPart) / frequency.QuadPart);
    web::Server server; web::Config config; config.port = 0;
    if (!server.Start(config) || !server.Pair()) return false;
    web::LibraryBuilder builder;
    std::shared_ptr<web::Snapshot> snapshot;
    for (int i = 0; i < 100 && !snapshot; ++i) snapshot = builder.Tick(g_lib, app::g.playlists);
    if (!snapshot || snapshot->tracks.empty()) return false;
    auto track = snapshot->tracks.front().key; server.Publish(snapshot);
    auto status = server.GetStatus();
    auto paired = Request(status.port, L"/api/v1/pair", ("code=" + status.pairingCode + "&name=Benchmark").c_str());
    if (paired.status != 200) return false;
    auto cookie = paired.cookie;
    Pump(2000, [&] { return Request(status.port, L"/api/v1/track?id=" + FromUtf8(track), nullptr, cookie).status == 200; });
    app::g.engine.SetVolume(0); app::PlayTrack(0, 0, false);
    Pump(1000, [&] { return app::g.engine.Position() > .1; });
    std::atomic<bool> done{false}; std::atomic<unsigned> responses{0}, failures{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < 4; ++i) clients.emplace_back([&] {
        while (!done) {
            auto response = Request(status.port, L"/api/v1/audio?id=" + FromUtf8(track) + L"&format=wav", nullptr, cookie);
            if (response.status == 200) ++responses; else if (response.status != 503) ++failures;
        }
    });
    auto start = GetTickCount64(), previous = start, nextBrowse = start, beginCpu = cpu();
    unsigned peakStreams = 0, browseCount = 0; uint64_t maxGap = 0;
    double initialPosition = app::g.engine.Position();
    std::vector<double> times;
    while (GetTickCount64() - start < 5000) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        uint64_t now = GetTickCount64(); maxGap = std::max(maxGap, now - previous); previous = now;
        peakStreams = std::max(peakStreams, server.GetStatus().streams);
        if (auto update = builder.Tick(g_lib, app::g.playlists)) server.Publish(std::move(update));
        if (now >= nextBrowse) {
            QueryPerformanceCounter(&a);
            auto result = Request(status.port, L"/api/v1/catalog?limit=100", nullptr, cookie);
            QueryPerformanceCounter(&b);
            times.push_back(1000.0 * (b.QuadPart - a.QuadPart) / frequency.QuadPart);
            if (result.status == 200) ++browseCount; else ++failures;
            nextBrowse = now + 100;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
    }
    done = true;
    double advanced = app::g.engine.Position() - initialPosition;
    for (auto& client : clients) client.join();
    auto beforeStop = GetTickCount64(); server.Stop();
    std::sort(times.begin(), times.end());
    PROCESS_MEMORY_COUNTERS_EX memory{};
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&memory, sizeof(memory));
    fwprintf(log, L"remote_responses=%u failures=%u peak_streams=%u browse_count=%u browse_median_ms=%.3f browse_p95_ms=%.3f ui_max_gap_ms=%llu local_advanced_s=%.3f cpu_ms=%.3f private_bytes=%zu stop_ms=%llu\n",
        responses.load(), failures.load(), peakStreams, browseCount, times.empty() ? 0 : times[times.size() / 2],
        times.empty() ? 0 : times[std::min(times.size() - 1, times.size() * 95 / 100)],
        (unsigned long long)maxGap, advanced, (cpu() - beginCpu) / 10000.0, memory.PrivateUsage,
        (unsigned long long)(GetTickCount64() - beforeStop));
    return !failures && responses >= 4 && browseCount > 10 && advanced > 4;
}
