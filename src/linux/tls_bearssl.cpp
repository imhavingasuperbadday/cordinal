// HTTPS for the web server on Linux, with BearSSL (src/third_party/bearssl) compiled in: what web_tls.h
// declares (keys, certificate requests, certificates, server sessions over memory buffers and the ACME
// account key) and web_remote.h's HttpFetch, the client behind every HTTPS request the player makes.
// Certificates are checked against the system's roots (or SSL_CERT_FILE) and must name the host.
// A key name is the path of an owner-only file (web::SaveRecords) holding the key as PKCS#8 PEM; the
// account key's blob is its RFC 5915 DER. Both read keys OpenSSL wrote for earlier releases.
#include "web_tls.h"
#include "web_remote.h"
#include "web_internal.h"
#include "radio_http.h"
#include "radio_hls.h"
#include "library.h"
#include "linux/tls_der.h"

#include <bearssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <type_traits>

namespace web {
namespace {

bool Random(void* out, size_t size) {
    for (size_t at = 0; at < size;) {
        ssize_t n = getrandom((char*)out + at, size - at, 0);
        if (n > 0) at += (size_t)n;
        else if (n < 0 && errno != EINTR) return false;
    }
    return true;
}

// A P-256 key pair: the secret scalar and the uncompressed public point.
struct EcKey {
    unsigned char secret[32]{};
    unsigned char point[65]{};
    ~EcKey() { explicit_bzero(secret, sizeof(secret)); }
    br_ec_private_key Private() const { return {BR_EC_secp256r1, (unsigned char*)secret, sizeof(secret)}; }
    br_ec_public_key Public() const { return {BR_EC_secp256r1, (unsigned char*)point, sizeof(point)}; }
    bool Complete() {  // fills in the point; false for a scalar that isn't a valid key
        br_ec_private_key sk = Private();
        br_ec_public_key pk;
        return br_ec_compute_pub(br_ec_get_default(), &pk, point, &sk) == sizeof(point);
    }
};

bool Generate(EcKey& key) {
    unsigned char seed[48];
    if (!Random(seed, sizeof(seed))) return false;
    br_hmac_drbg_context rng;
    br_hmac_drbg_init(&rng, &br_sha256_vtable, seed, sizeof(seed));
    br_ec_private_key sk;
    unsigned char buffer[BR_EC_KBUF_PRIV_MAX_SIZE];
    bool ok = br_ec_keygen(&rng.vtable, br_ec_get_default(), &sk, buffer, BR_EC_secp256r1) == sizeof(key.secret);
    if (ok) memcpy(key.secret, buffer, sizeof(key.secret));
    explicit_bzero(seed, sizeof(seed));
    explicit_bzero(buffer, sizeof(buffer));
    explicit_bzero(&rng, sizeof(rng));
    return ok && key.Complete();
}

// A PKCS#8 or RFC 5915 private key, which must be on P-256.
bool Decode(std::string_view der, EcKey& key) {
    br_skey_decoder_context decoder;
    br_skey_decoder_init(&decoder);
    br_skey_decoder_push(&decoder, der.data(), der.size());
    const br_ec_private_key* sk = br_skey_decoder_last_error(&decoder) ? nullptr : br_skey_decoder_get_ec(&decoder);
    bool ok = false;
    if (sk && sk->curve == BR_EC_secp256r1) {
        const unsigned char* x = sk->x;
        size_t size = sk->xlen;
        while (size && !*x) { ++x; --size; }
        if (size && size <= sizeof(key.secret)) {
            memset(key.secret, 0, sizeof(key.secret));
            memcpy(key.secret + sizeof(key.secret) - size, x, size);
            ok = key.Complete();
        }
    }
    explicit_bzero(&decoder, sizeof(decoder));
    return ok;
}

std::string Encode(const EcKey& key, bool pkcs8) {
    br_ec_private_key sk = key.Private();
    br_ec_public_key pk = key.Public();
    size_t size = pkcs8 ? br_encode_ec_pkcs8_der(nullptr, &sk, &pk) : br_encode_ec_raw_der(nullptr, &sk, &pk);
    std::string der(size, '\0');
    if (!size || (pkcs8 ? br_encode_ec_pkcs8_der(der.data(), &sk, &pk) : br_encode_ec_raw_der(der.data(), &sk, &pk)) != size) return {};
    return der;
}

std::string Hash(std::string_view data) {
    br_sha256_context context;
    br_sha256_init(&context);
    br_sha256_update(&context, data.data(), data.size());
    std::string hash(br_sha256_SIZE, '\0');
    br_sha256_out(&context, hash.data());
    return hash;
}

// ECDSA over SHA-256 (deterministic, RFC 6979): r and s side by side, or the DER Ecdsa-Sig-Value.
std::string SignSha256(const EcKey& key, std::string_view data, bool raw) {
    std::string hash = Hash(data);
    br_ec_private_key sk = key.Private();
    unsigned char signature[80];
    size_t size = (raw ? br_ecdsa_sign_raw_get_default() : br_ecdsa_sign_asn1_get_default())(
        br_ec_get_default(), &br_sha256_vtable, hash.data(), &sk, signature);
    return std::string((const char*)signature, size);
}

std::string Path(const std::wstring& name) { return compat::Narrow(name.c_str()); }

// The key `name`; a missing one is made and saved when `create` is set.
bool ReadKey(const std::wstring& name, bool create, EcKey& key, std::string& error) {
    std::string pem;
    if (!ReadRecords(name, pem)) { error = "Couldn't read the server's private key."; return false; }
    if (!pem.empty()) {
        auto ders = der::Pem(pem, "PRIVATE KEY");
        if (ders.empty()) ders = der::Pem(pem, "EC PRIVATE KEY");
        bool ok = !ders.empty() && Decode(ders[0], key);
        for (auto& d : ders) explicit_bzero(d.data(), d.size());
        explicit_bzero(pem.data(), pem.size());
        if (!ok) error = "The server's private key can't be read.";
        return ok;
    }
    if (!create) { error = "The server has no private key."; return false; }
    std::string der;
    if (!Generate(key) || (der = Encode(key, true)).empty() || !SaveRecords(name, der::ToPem(der, "PRIVATE KEY"))) {
        error = "Couldn't make and save the server's private key.";
        return false;
    }
    explicit_bzero(der.data(), der.size());
    return true;
}

// The certificate's public key; false unless it is the P-256 point `point`.
bool CertifiesKey(const std::string& certificate, const unsigned char (&point)[65]) {
    br_x509_decoder_context decoder;
    br_x509_decoder_init(&decoder, nullptr, nullptr);
    br_x509_decoder_push(&decoder, certificate.data(), certificate.size());
    const br_x509_pkey* key = br_x509_decoder_last_error(&decoder) ? nullptr : br_x509_decoder_get_pkey(&decoder);
    return key && key->key_type == BR_KEYTYPE_EC && key->key.ec.curve == BR_EC_secp256r1 && key->key.ec.qlen == sizeof(point) &&
           !memcmp(key->key.ec.q, point, sizeof(point));
}

// TLS 1.2, as both sides speak it: forward-secret suites only, AEAD first.
constexpr uint16_t kServerSuites[] = {
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384, BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA, BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA};
constexpr uint16_t kClientSuites[] = {
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256, BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256, BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384, BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384,
    BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA, BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
    BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA, BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA};

// Moves the engine's outgoing records onto `out`.
void Drain(br_ssl_engine_context* engine, std::string& out) {
    size_t size = 0;
    while (unsigned char* bytes = br_ssl_engine_sendrec_buf(engine, &size)) {
        if (!size) break;
        out.append((const char*)bytes, size);
        br_ssl_engine_sendrec_ack(engine, size);
    }
}
// Hands the engine as much of `input` (from `used`) as it will take for the record it is reading.
void Feed(br_ssl_engine_context* engine, const std::string& input, size_t& used) {
    size_t size = 0;
    unsigned char* room = br_ssl_engine_recvrec_buf(engine, &size);
    size = std::min(size, input.size() - used);
    if (!room || !size) return;
    memcpy(room, input.data() + used, size);
    used += size;
    br_ssl_engine_recvrec_ack(engine, size);
}

}  // namespace

// The server's side of an identity: its key, and the chain as BearSSL holds it (pointing into chain_).
struct ServerCredentials {
    EcKey key;
    br_ec_private_key secret{};
    std::vector<br_x509_certificate> chain;
};
// One server connection's engine and its record buffers.
struct ServerEngine {
    br_ssl_server_context server;
    unsigned char buffer[BR_SSL_BUFSIZE_BIDI];
};

bool EnsureTlsKey(const std::wstring& name, std::string& error) {
    EcKey key;
    return ReadKey(name, true, key, error);
}
bool DeleteTlsKey(const std::wstring& name) { return !unlink(Path(name).c_str()); }

std::string CertificateRequest(const std::wstring& keyName, const std::vector<std::string>& names, std::string& error) {
    EcKey key;
    if (!ReadKey(keyName, false, key, error)) return {};
    std::string body = der::RequestBody({(const char*)key.point, sizeof(key.point)}, names);
    std::string signature = SignSha256(key, body, false);
    if (signature.empty()) { error = "Couldn't sign the certificate request."; return {}; }
    return der::SignedEcdsaSha256(body, signature);
}

std::string SelfSignedCertificate(const std::wstring& keyName, const std::vector<std::string>& names, int days) {
    std::string error;
    EcKey key;
    unsigned char serial[8];
    if (!ReadKey(keyName, false, key, error) || !Random(serial, sizeof(serial))) return {};
    serial[0] = (unsigned char)((serial[0] & 0x7f) | 0x40);  // positive, and no padding byte needed
    const uint64_t now = (uint64_t)time(nullptr);
    std::string body = der::SelfSignedBody({(const char*)key.point, sizeof(key.point)}, names, {(const char*)serial, sizeof(serial)},
                                           now - 3600, now + (uint64_t)std::max(days, 0) * 86400);
    std::string signature = SignSha256(key, body, false);
    return signature.empty() ? std::string{} : der::SignedEcdsaSha256(body, signature);
}

std::vector<std::string> PemCertificates(std::string_view pem) { return der::Pem(pem, "CERTIFICATE"); }

bool CertificateInfo(const std::string& der, uint64_t& notBefore, uint64_t& notAfter, std::vector<std::string>& names) {
    der::Certificate certificate;
    if (!der::ReadCertificate(der, certificate)) return false;
    notBefore = certificate.notBefore;
    notAfter = certificate.notAfter;
    names = std::move(certificate.names);
    return true;
}

std::string Sha256(std::string_view data) { return Hash(data); }

std::shared_ptr<TlsIdentity> TlsIdentity::Create(const std::vector<std::string>& chain, const std::wstring& keyName,
                                                 std::string& error) {
    if (chain.empty()) { error = "There is no certificate."; return {}; }
    der::Certificate leaf;
    if (!der::ReadCertificate(chain[0], leaf)) { error = "The certificate can't be read."; return {}; }
    auto credentials = std::make_unique<ServerCredentials>();
    if (!ReadKey(keyName, false, credentials->key, error)) return {};
    if (!CertifiesKey(chain[0], credentials->key.point)) { error = "The certificate can't be joined to its key."; return {}; }
    std::shared_ptr<TlsIdentity> id(new TlsIdentity());
    id->chain_ = chain;  // BearSSL's chain points into these strings, which are never changed again
    // The intermediates go out with the leaf, so browsers can build the path to their roots.
    for (const std::string& certificate : id->chain_)
        credentials->chain.push_back({(unsigned char*)certificate.data(), certificate.size()});
    credentials->secret = credentials->key.Private();
    id->context_ = credentials.release();
    id->notAfter_ = leaf.notAfter;
    id->names_ = std::move(leaf.names);
    return id;
}
TlsIdentity::~TlsIdentity() { delete (ServerCredentials*)context_; }

TlsSession::TlsSession(std::shared_ptr<TlsIdentity> identity) : identity_(std::move(identity)) {
    auto* credentials = (ServerCredentials*)identity_->Context();
    auto engine = std::make_unique<ServerEngine>();
    br_ssl_server_init_full_ec(&engine->server, credentials->chain.data(), credentials->chain.size(), BR_KEYTYPE_EC,
                               &credentials->secret);
    br_ssl_engine_context* e = &engine->server.eng;
    br_ssl_engine_set_versions(e, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(e, kServerSuites, sizeof(kServerSuites) / sizeof(kServerSuites[0]));
    br_ssl_engine_add_flags(e, BR_OPT_ENFORCE_SERVER_PREFERENCES | BR_OPT_NO_RENEGOTIATION);
    br_ssl_engine_set_buffer(e, engine->buffer, sizeof(engine->buffer), 1);
    if (br_ssl_server_reset(&engine->server)) engine_ = engine.release();
}
TlsSession::~TlsSession() { delete (ServerEngine*)engine_; }

int TlsSession::Handshake(std::string& input, std::string& output) {
    auto* engine = (ServerEngine*)engine_;
    if (!engine) return -1;
    if (established_) return 1;
    br_ssl_engine_context* e = &engine->server.eng;
    // BearSSL takes one record at a time, so what follows the handshake stays in `input` for the caller.
    size_t used = 0;
    int result = 0;
    for (;;) {
        unsigned state = br_ssl_engine_current_state(e);
        if (state & BR_SSL_CLOSED) { result = -1; break; }
        if (state & BR_SSL_SENDREC) { Drain(e, output); continue; }
        if (state & BR_SSL_SENDAPP) { established_ = true; result = 1; break; }
        size_t before = used;
        if (state & BR_SSL_RECVREC) Feed(e, input, used);
        if (used == before) break;  // waiting for more bytes
    }
    input.erase(0, used);
    return result;
}
int TlsSession::Decrypt(std::string& input, std::string& plain, std::string& output) {
    auto* engine = (ServerEngine*)engine_;
    if (!engine) return -1;
    if (!established_) {
        int result = Handshake(input, output);
        if (result <= 0) return result < 0 ? -1 : 1;
    }
    br_ssl_engine_context* e = &engine->server.eng;
    size_t used = 0;
    int status = 1;
    for (;;) {
        unsigned state = br_ssl_engine_current_state(e);
        if (state & BR_SSL_SENDREC) { Drain(e, output); continue; }  // a close_notify in reply, say
        size_t size = 0;
        if (unsigned char* bytes = (state & BR_SSL_RECVAPP) ? br_ssl_engine_recvapp_buf(e, &size) : nullptr) {
            plain.append((const char*)bytes, size);
            br_ssl_engine_recvapp_ack(e, size);
            continue;
        }
        if (state & BR_SSL_CLOSED) { status = br_ssl_engine_last_error(e) == BR_ERR_OK ? 0 : -1; break; }
        size_t before = used;
        if (state & BR_SSL_RECVREC) Feed(e, input, used);
        if (used == before) break;
    }
    input.erase(0, used);
    return status;
}
bool TlsSession::Encrypt(std::string_view data, std::string& output) {
    auto* engine = (ServerEngine*)engine_;
    if (!engine || !established_) return false;
    br_ssl_engine_context* e = &engine->server.eng;
    while (!data.empty()) {
        unsigned state = br_ssl_engine_current_state(e);
        if (state & BR_SSL_CLOSED) return false;
        if (state & BR_SSL_SENDREC) { Drain(e, output); continue; }
        size_t size = 0;
        unsigned char* room = (state & BR_SSL_SENDAPP) ? br_ssl_engine_sendapp_buf(e, &size) : nullptr;
        if (!room || !size) return false;
        size = std::min(size, data.size());
        memcpy(room, data.data(), size);
        br_ssl_engine_sendapp_ack(e, size);
        data.remove_prefix(size);
    }
    br_ssl_engine_flush(e, 0);
    Drain(e, output);
    return !(br_ssl_engine_current_state(e) & BR_SSL_CLOSED);
}

AccountKey::~AccountKey() { Release(); }
void AccountKey::Release() {
    delete (EcKey*)key_;
    key_ = nullptr;
}
bool AccountKey::Create() {
    Release();
    auto key = std::make_unique<EcKey>();
    if (!Generate(*key)) return false;
    key_ = key.release();
    return true;
}
bool AccountKey::Import(std::string_view blob) {
    Release();
    auto key = std::make_unique<EcKey>();
    if (!Decode(blob, *key)) return false;
    key_ = key.release();
    return true;
}
std::string AccountKey::Export() const { return key_ ? Encode(*(const EcKey*)key_, false) : std::string{}; }
std::string AccountKey::Jwk() const {
    if (!key_) return {};
    const unsigned char* point = ((const EcKey*)key_)->point;
    return "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"" + Base64Url({(const char*)point + 1, 32}) + "\",\"y\":\"" +
        Base64Url({(const char*)point + 33, 32}) + "\"}";
}
std::string AccountKey::Sign(std::string_view data) const {
    std::string signature = key_ ? SignSha256(*(const EcKey*)key_, data, true) : std::string{};
    return signature.size() == 64 ? signature : std::string{};
}

namespace {
// The roots HttpFetch trusts: SSL_CERT_FILE when it is set, otherwise the distribution's bundle
// (or, failing that, the certificates in SSL_CERT_DIR or /etc/ssl/certs). Read once, on first use.
struct TrustStore {
    std::deque<std::string> bytes;  // what the anchors point into; a deque never moves its elements
    std::vector<br_x509_trust_anchor> anchors;
    void Add(const std::string& certificate) {
        std::string name;
        br_x509_decoder_context decoder;
        br_x509_decoder_init(&decoder, [](void* to, const void* data, size_t size) { ((std::string*)to)->append((const char*)data, size); },
                             &name);
        br_x509_decoder_push(&decoder, certificate.data(), certificate.size());
        const br_x509_pkey* key = br_x509_decoder_last_error(&decoder) ? nullptr : br_x509_decoder_get_pkey(&decoder);
        if (!key || name.empty()) return;
        auto keep = [&](const unsigned char* data, size_t size) {
            return (unsigned char*)bytes.emplace_back((const char*)data, size).data();
        };
        br_x509_trust_anchor anchor{};
        anchor.dn.data = (unsigned char*)bytes.emplace_back(std::move(name)).data();
        anchor.dn.len = bytes.back().size();
        anchor.flags = br_x509_decoder_isCA(&decoder) ? BR_X509_TA_CA : 0;
        anchor.pkey.key_type = key->key_type;
        if (key->key_type == BR_KEYTYPE_RSA) {
            anchor.pkey.key.rsa.n = keep(key->key.rsa.n, key->key.rsa.nlen);
            anchor.pkey.key.rsa.nlen = key->key.rsa.nlen;
            anchor.pkey.key.rsa.e = keep(key->key.rsa.e, key->key.rsa.elen);
            anchor.pkey.key.rsa.elen = key->key.rsa.elen;
        } else if (key->key_type == BR_KEYTYPE_EC) {
            anchor.pkey.key.ec.curve = key->key.ec.curve;
            anchor.pkey.key.ec.q = keep(key->key.ec.q, key->key.ec.qlen);
            anchor.pkey.key.ec.qlen = key->key.ec.qlen;
        } else {
            return;
        }
        anchors.push_back(anchor);
    }
    static bool Read(const std::string& path, std::string& text) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        char buffer[65536];
        for (size_t n; (n = fread(buffer, 1, sizeof(buffer), f)) > 0 && text.size() < (64u << 20);) text.append(buffer, n);
        fclose(f);
        return true;
    }
    TrustStore() {
        std::string text;
        if (const char* file = getenv("SSL_CERT_FILE"); file && *file) {
            Read(file, text);
        } else {
            // Debian, Ubuntu, Arch and Gentoo; Fedora and RHEL; openSUSE; Alpine and the BSD layout.
            for (const char* path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                                     "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/ca-bundle.pem", "/var/lib/ca-certificates/ca-bundle.pem",
                                     "/etc/ssl/cert.pem"})
                if (Read(path, text) && text.find("-----BEGIN CERTIFICATE-----") != text.npos) break;
            if (text.find("-----BEGIN CERTIFICATE-----") == text.npos) {
                const char* dir = getenv("SSL_CERT_DIR");
                std::error_code ec;
                for (const auto& entry : std::filesystem::directory_iterator(dir && *dir ? dir : "/etc/ssl/certs", ec)) {
                    auto extension = entry.path().extension();
                    if ((extension == ".pem" || extension == ".crt") && entry.is_regular_file(ec)) Read(entry.path(), text);
                }
            }
        }
        for (const std::string& certificate : der::Pem(text, "CERTIFICATE")) Add(certificate);
    }
};
const TrustStore& Trust() {
    static const TrustStore store;
    return store;
}

// BearSSL's certificate checker with one more rule: a host given as an IP address must be one of
// the leaf certificate's iPAddress names (BearSSL itself matches DNS names only).
struct Checker {
    const br_x509_class* vtable;  // first, as BearSSL calls through it
    br_x509_minimal_context minimal;
    unsigned char ip[16];
    size_t ipSize;
    unsigned index;      // the certificate being read; the leaf is 0
    std::string* leaf;   // the leaf's DER
    static Checker& Of(const br_x509_class* const* c) { return *(Checker*)c; }
    static const br_x509_class** Inner(const br_x509_class* const* c) { return &Of(c).minimal.vtable; }
    static void StartChain(const br_x509_class** c, const char* name) {
        Of(c).index = 0;
        Of(c).leaf->clear();
        (*Inner(c))->start_chain(Inner(c), name);
    }
    static void StartCert(const br_x509_class** c, uint32_t size) { (*Inner(c))->start_cert(Inner(c), size); }
    static void Append(const br_x509_class** c, const unsigned char* data, size_t size) {
        if (Of(c).index == 0 && Of(c).leaf->size() < (1u << 16)) Of(c).leaf->append((const char*)data, size);
        (*Inner(c))->append(Inner(c), data, size);
    }
    static void EndCert(const br_x509_class** c) {
        ++Of(c).index;
        (*Inner(c))->end_cert(Inner(c));
    }
    static unsigned EndChain(const br_x509_class** c) {
        unsigned result = (*Inner(c))->end_chain(Inner(c));
        if (result != BR_ERR_OK || !Of(c).ipSize) return result;
        der::Certificate leaf;
        if (!der::ReadCertificate(*Of(c).leaf, leaf)) return BR_ERR_X509_BAD_SERVER_NAME;
        for (const std::string& address : leaf.addresses)
            if (address.size() == Of(c).ipSize && !memcmp(address.data(), Of(c).ip, Of(c).ipSize)) return BR_ERR_OK;
        return BR_ERR_X509_BAD_SERVER_NAME;
    }
    static const br_x509_pkey* Key(const br_x509_class* const* c, unsigned* usages) {
        return (*Inner(c))->get_pkey(Inner(c), usages);
    }
};
static_assert(std::is_standard_layout_v<Checker>);
const br_x509_class kChecker = {sizeof(Checker), Checker::StartChain, Checker::StartCert, Checker::Append,
                                Checker::EndCert, Checker::EndChain, Checker::Key};

// One client connection's engine, checker and record buffers.
struct ClientTls {
    br_ssl_client_context client;
    Checker checker;
    std::string leaf;
    unsigned char buffer[BR_SSL_BUFSIZE_BIDI];
};

// One connection's bytes, plain or through TLS, with a deadline and the caller's cancel flag.
struct Stream {
    int fd = -1;
    std::unique_ptr<ClientTls> tls;
    uint64_t deadline = 0;
    const std::atomic<bool>* cancel = nullptr;
    ~Stream() { if (fd >= 0) close(fd); }
    // Waits for the socket in short steps, so a cancel is seen within a tenth of a second.
    bool Wait(bool write) {
        for (;;) {
            if ((cancel && *cancel) || ClockMs() >= deadline) return false;
            pollfd p{fd, (short)(write ? POLLOUT : POLLIN), 0};
            int n = poll(&p, 1, (int)std::min<uint64_t>(100, deadline - ClockMs()));
            if (n > 0) return true;
            if (n < 0 && errno != EINTR) return false;
        }
    }
    bool Connect(const addrinfo* a) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) return false;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        if (connect(fd, a->ai_addr, a->ai_addrlen) && errno != EINPROGRESS) return false;
        int error = 0;
        socklen_t size = sizeof(error);
        return Wait(true) && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) && !error;
    }
    // Moves records between the engine and the socket until it can take (BR_SSL_SENDAPP) or give
    // (BR_SSL_RECVAPP) application data. 1: ready; 0: the server closed; -1: failed or timed out.
    // A connection closed without close_notify counts as closed: HttpFetch checks the length itself.
    int Run(unsigned target) {
        br_ssl_engine_context* e = &tls->client.eng;
        for (;;) {
            unsigned state = br_ssl_engine_current_state(e);
            if (state & BR_SSL_CLOSED) return br_ssl_engine_last_error(e) == BR_ERR_OK ? 0 : -1;
            size_t size = 0;
            if (state & BR_SSL_SENDREC) {
                unsigned char* bytes = br_ssl_engine_sendrec_buf(e, &size);
                long n = send(fd, bytes, size, MSG_NOSIGNAL);
                if (n > 0) { br_ssl_engine_sendrec_ack(e, (size_t)n); continue; }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno == EAGAIN && Wait(true)) continue;
                return -1;
            }
            if (state & target) return 1;
            if (state & BR_SSL_RECVREC) {
                unsigned char* room = br_ssl_engine_recvrec_buf(e, &size);
                long n = recv(fd, room, size, 0);
                if (n > 0) { br_ssl_engine_recvrec_ack(e, (size_t)n); continue; }
                if (n == 0) return 0;
                if (errno == EINTR) continue;
                if (errno == EAGAIN && Wait(false)) continue;
                return -1;
            }
            return -1;  // neither side can move: nothing more will happen on this connection
        }
    }
    // TLS to `host`, whose certificate must name it (an IP address or a DNS name).
    bool Secure(const std::string& host) {
        const TrustStore& trust = Trust();
        if (trust.anchors.empty()) return false;
        tls = std::make_unique<ClientTls>();
        br_ssl_client_init_full(&tls->client, &tls->checker.minimal, trust.anchors.data(), trust.anchors.size());
        br_ssl_engine_context* e = &tls->client.eng;
        br_ssl_engine_set_versions(e, BR_TLS12, BR_TLS12);
        br_ssl_engine_set_suites(e, kClientSuites, sizeof(kClientSuites) / sizeof(kClientSuites[0]));
        br_ssl_engine_add_flags(e, BR_OPT_NO_RENEGOTIATION);
        br_ssl_engine_set_buffer(e, tls->buffer, sizeof(tls->buffer), 1);
        tls->checker.vtable = &kChecker;
        tls->checker.leaf = &tls->leaf;
        tls->checker.index = 0;
        tls->checker.ipSize = inet_pton(AF_INET, host.c_str(), tls->checker.ip) == 1 ? 4
                            : inet_pton(AF_INET6, host.c_str(), tls->checker.ip) == 1 ? 16 : 0;
        br_ssl_engine_set_x509(e, &tls->checker.vtable);
        // An address is checked by the checker above and is never sent as the server name (RFC 6066).
        if (!br_ssl_client_reset(&tls->client, tls->checker.ipSize ? nullptr : host.c_str(), 0)) return false;
        return Run(BR_SSL_SENDAPP | BR_SSL_RECVAPP) == 1;
    }
    bool Write(std::string_view data) {
        if (!tls) {
            while (!data.empty()) {
                long n = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
                if (n > 0) { data.remove_prefix((size_t)n); continue; }
                bool again = n < 0 && (errno == EAGAIN || errno == EINTR);
                if (!again || !Wait(true)) return false;
            }
            return true;
        }
        br_ssl_engine_context* e = &tls->client.eng;
        while (!data.empty()) {
            if (Run(BR_SSL_SENDAPP) != 1) return false;
            size_t size = 0;
            unsigned char* room = br_ssl_engine_sendapp_buf(e, &size);
            size = std::min(size, data.size());
            memcpy(room, data.data(), size);
            br_ssl_engine_sendapp_ack(e, size);
            data.remove_prefix(size);
        }
        br_ssl_engine_flush(e, 0);
        return Run(BR_SSL_SENDAPP) == 1;  // records go out before the engine can take more
    }
    // Some bytes onto `out`: 1 read, 0 the end, -1 failed or timed out.
    int Read(std::string& out) {
        if (!tls) {
            char buffer[16384];
            for (;;) {
                long n = recv(fd, buffer, sizeof(buffer), 0);
                if (n > 0) { out.append(buffer, (size_t)n); return 1; }
                if (n == 0) return 0;
                bool again = errno == EAGAIN || errno == EINTR;
                if (!again || !Wait(false)) return -1;
            }
        }
        int result = Run(BR_SSL_RECVAPP);
        if (result != 1) return result;
        size_t size = 0;
        unsigned char* bytes = br_ssl_engine_recvapp_buf(&tls->client.eng, &size);
        out.append((const char*)bytes, size);
        br_ssl_engine_recvapp_ack(&tls->client.eng, size);
        return 1;
    }
};

// A chunked body decoded; false when it is cut short or malformed.
bool Unchunk(std::string_view in, std::string& out) {
    out.clear();
    for (size_t at = 0;;) {
        size_t line = in.find("\r\n", at);
        if (line == in.npos) return false;
        size_t size = strtoul(std::string(in.substr(at, line - at)).c_str(), nullptr, 16);
        at = line + 2;
        if (!size) return true;
        if (at + size + 2 > in.size()) return false;
        out.append(in.substr(at, size));
        at += size + 2;
    }
}
}  // namespace

HttpReply HttpFetch(void* session, const std::string& url, const char* method, const std::string& body,
                    const std::string& headers, unsigned timeoutMs, size_t maxBody) {
    HttpReply reply;
    bool https = url.starts_with("https://");
    if (!https && !url.starts_with("http://")) return reply;
    size_t start = https ? 8 : 7, slash = url.find('/', start);
    std::string authority = url.substr(start, slash == url.npos ? url.npos : slash - start);
    std::string target = slash == url.npos ? "/" : url.substr(slash), host = authority, port = https ? "443" : "80";
    if (size_t hash = target.find('#'); hash != target.npos) target.resize(hash);
    if (authority.starts_with('[')) {
        size_t close = authority.find(']');
        if (close == authority.npos) return reply;
        host = authority.substr(1, close - 1);
        if (authority.size() > close + 2 && authority[close + 1] == ':') port = authority.substr(close + 2);
    } else if (size_t colon = authority.rfind(':'); colon != authority.npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (host.empty()) return reply;

    Stream stream;
    stream.deadline = ClockMs() + timeoutMs;
    stream.cancel = (const std::atomic<bool>*)session;
    addrinfo hints{}, *list = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &list) || !list) return reply;
    bool connected = false;
    for (const addrinfo* a = list; a && !connected; a = a->ai_next) {
        if (stream.fd >= 0) { close(stream.fd); stream.fd = -1; }
        connected = stream.Connect(a);
    }
    freeaddrinfo(list);
    if (!connected || (https && !stream.Secure(host))) return reply;

    const bool head = !strcmp(method, "HEAD");
    // A caller that names itself (the metadata services ask for a contact) replaces the bare name.
    const bool named = Lower(headers).find("user-agent:") != std::string::npos;
    std::string request = std::string(method) + ' ' + target + " HTTP/1.1\r\nHost: " + authority + "\r\n" +
        (named ? "" : "User-Agent: cordinal\r\n") + "Accept-Encoding: identity\r\nConnection: close\r\n" + headers;
    if (!body.empty() || !strcmp(method, "POST")) request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n" + body;
    if (!stream.Write(request)) return reply;

    // Read until the server closes, or until the body it announced is complete.
    std::string raw;
    size_t headerEnd = std::string::npos, length = std::string::npos;
    bool chunked = false;
    for (;;) {
        int got = stream.Read(raw);
        if (got < 0) return reply;
        if (headerEnd == std::string::npos && (headerEnd = raw.find("\r\n\r\n")) != std::string::npos) {
            std::string_view lines(raw.data(), headerEnd);
            size_t at = lines.find("\r\n");
            for (; at != lines.npos && at + 2 < lines.size();) {
                size_t end = std::min(lines.find("\r\n", at + 2), lines.size()), colon = lines.find(':', at + 2);
                if (colon < end) {
                    std::string value(lines.substr(colon + 1, end - colon - 1));
                    while (!value.empty() && value.front() == ' ') value.erase(0, 1);
                    while (!value.empty() && (unsigned char)value.back() <= ' ') value.pop_back();
                    reply.headers[Lower(std::string(lines.substr(at + 2, colon - at - 2)))] = value;
                }
                at = end;
            }
            unsigned status = raw.size() > 12 && raw.starts_with("HTTP/1.") ? (unsigned)atoi(raw.c_str() + 9) : 0;
            if (!status) return reply;
            chunked = Lower(reply.headers["transfer-encoding"]).find("chunked") != std::string::npos;
            if (auto it = reply.headers.find("content-length"); it != reply.headers.end() && !chunked)
                length = strtoul(it->second.c_str(), nullptr, 10);
            if (head || status == 204 || status == 304 || status / 100 == 1) length = 0;
            reply.status = status;
        }
        if (headerEnd != std::string::npos) {
            size_t have = raw.size() - headerEnd - 4;
            if (have > maxBody) { reply.status = 0; return reply; }
            if (length != std::string::npos && have >= length) break;
            if (chunked && raw.ends_with("\r\n0\r\n\r\n")) break;
        }
        if (got == 0) break;
    }
    if (headerEnd == std::string::npos) { reply.status = 0; return reply; }
    std::string_view rest(raw.data() + headerEnd + 4, raw.size() - headerEnd - 4);
    if (chunked) {
        if (!Unchunk(rest, reply.body)) reply.status = 0;
    } else if (length != std::string::npos) {
        if (rest.size() < length) reply.status = 0;
        else reply.body.assign(rest.substr(0, length));
    } else {
        reply.body.assign(rest);
    }
    return reply;
}

// A live response cannot use HttpFetch, which deliberately reads a bounded complete body. Keep
// only partial headers or a partial chunk here; encoded audio is handed straight to the caller.
bool RadioHttpStream(const std::string& initial, const std::atomic<bool>& cancel,
                     const std::function<bool(unsigned, const radio::Headers&)>& headers,
                     const std::function<bool(std::string_view)>& data,
                     const std::string& requestHeaders, std::string* effectiveUrl) {
    std::string url = initial;
    for (unsigned redirects = 0; redirects <= 5 && !cancel; ++redirects) {
        bool secure = url.starts_with("https://");
        if ((!secure && !url.starts_with("http://")) || url.size() > 8192 ||
            std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) return false;
        size_t start = secure ? 8 : 7, slash = url.find_first_of("/?#", start);
        std::string authority = url.substr(start, slash == url.npos ? url.npos : slash - start);
        std::string target = slash == url.npos ? "/" : url.substr(slash);
        if (target.starts_with('?')) target.insert(target.begin(), '/');
        if (size_t hash = target.find('#'); hash != target.npos) target.resize(hash);
        if (target.empty()) target = "/";
        std::string host = authority, port = secure ? "443" : "80";
        if (authority.empty() || authority.find('@') != authority.npos) return false;
        if (authority.starts_with('[')) {
            size_t close = authority.find(']');
            if (close == authority.npos) return false;
            host = authority.substr(1, close - 1);
            if (authority.size() > close + 1) {
                if (authority[close + 1] != ':') return false;
                port = authority.substr(close + 2);
            }
        } else if (size_t colon = authority.rfind(':'); colon != authority.npos) {
            host = authority.substr(0, colon); port = authority.substr(colon + 1);
        }
        if (host.empty() || port.empty()) return false;
        Stream stream; stream.deadline = ClockMs() + 8000; stream.cancel = &cancel;
        addrinfo hints{}, *list = nullptr; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_ADDRCONFIG;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &list) || !list) return false;
        bool connected = false;
        for (const auto* a = list; a && !connected && !cancel; a = a->ai_next) {
            if (stream.fd >= 0) { close(stream.fd); stream.fd = -1; }
            connected = stream.Connect(a);
        }
        freeaddrinfo(list);
        if (!connected || (secure && !stream.Secure(host))) return false;
        std::string request = "GET " + target + " HTTP/1.1\r\nHost: " + authority +
            "\r\nUser-Agent: cordinal/0.1.12\r\nIcy-MetaData: 1\r\nAccept-Encoding: identity\r\nConnection: close\r\n" + requestHeaders + "\r\n";
        if (!stream.Write(request)) return false;
        std::string raw; size_t end = std::string::npos;
        while ((end = raw.find("\r\n\r\n")) == raw.npos) {
            if (raw.size() > 65536 || stream.Read(raw) != 1) return false;
        }
        size_t first = raw.find("\r\n");
        if (first == raw.npos) return false;
        unsigned status = 0;
        if (raw.starts_with("HTTP/1.")) status = (unsigned)atoi(raw.c_str() + 9);
        else if (raw.starts_with("ICY ")) status = (unsigned)atoi(raw.c_str() + 4);
        if (!status) return false;
        radio::Headers fields;
        for (size_t at = first + 2; at < end;) {
            size_t next = raw.find("\r\n", at), colon = raw.find(':', at);
            if (next == raw.npos || next > end) next = end;
            if (colon < next) {
                std::string value = raw.substr(colon + 1, next - colon - 1);
                while (!value.empty() && (unsigned char)value.front() <= ' ') value.erase(0, 1);
                while (!value.empty() && (unsigned char)value.back() <= ' ') value.pop_back();
                fields[Lower(raw.substr(at, colon - at))] = std::move(value);
            }
            at = next + 2;
        }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            auto found = fields.find("location");
            if (found == fields.end()) return false;
            const std::string& location = found->second;
            if (location.starts_with("http://") || location.starts_with("https://")) url = location;
            else if (location.starts_with("//")) url = (secure ? "https:" : "http:") + location;
            else {
                std::string origin = (secure ? "https://" : "http://") + authority;
                if (location.starts_with('/')) url = origin + location;
                else {
                    size_t last = target.rfind('/');
                    url = origin + target.substr(0, last == target.npos ? 0 : last + 1) + location;
                }
            }
            continue;
        }
        if (effectiveUrl) *effectiveUrl = url;
        if (!headers(status, fields)) return false;
        bool chunked = fields.contains("transfer-encoding") && Lower(fields["transfer-encoding"]).find("chunked") != std::string::npos;
        raw.erase(0, end + 4);
        uint64_t chunkLeft = 0;
        bool sizeLine = true, trailing = false;
        for (;;) {
            if (cancel) return false;
            if (!chunked) {
                if (!raw.empty()) { if (!data(raw)) return true; raw.clear(); }
            } else {
                for (;;) {
                    if (sizeLine) {
                        size_t line = raw.find("\r\n");
                        if (line == raw.npos) { if (raw.size() > 128) return false; break; }
                        std::string_view number(raw.data(), line);
                        if (size_t ext = number.find(';'); ext != number.npos) number = number.substr(0, ext);
                        auto parsed = std::from_chars(number.data(), number.data() + number.size(), chunkLeft, 16);
                        if (number.empty() || parsed.ec != std::errc() || parsed.ptr != number.data() + number.size() ||
                            chunkLeft > (16u << 20)) return false;
                        raw.erase(0, line + 2); sizeLine = false;
                        if (!chunkLeft) return true;
                    }
                    if (chunkLeft) {
                        size_t n = (size_t)std::min<uint64_t>(chunkLeft, raw.size());
                        if (!n) break;
                        if (!data(std::string_view(raw.data(), n))) return true;
                        raw.erase(0, n); chunkLeft -= n;
                        if (!chunkLeft) trailing = true;
                    }
                    if (trailing) {
                        if (raw.size() < 2) break;
                        if (!raw.starts_with("\r\n")) return false;
                        raw.erase(0, 2); trailing = false; sizeLine = true;
                    }
                }
            }
            stream.deadline = ClockMs() + 8000;
            int got = stream.Read(raw);
            if (got != 1) return got == 0 && !chunked;
        }
    }
    return false;
}
}  // namespace web

namespace radio {
bool HttpStream(const std::string& url, const std::atomic<bool>& cancel,
                const std::function<bool(unsigned, const Headers&)>& headers,
                const std::function<bool(std::string_view)>& data,
                const std::string& requestHeaders, std::string* effectiveUrl) {
    return web::RadioHttpStream(url, cancel, headers, data, requestHeaders, effectiveUrl);
}
// AES-128-CBC with PKCS#7 padding, as HLS (RFC 8216) encrypts segments.
bool DecryptRadioAes(std::string& bytes, const std::string& key, std::array<uint8_t, 16> iv) {
    if (key.size() != 16 || bytes.empty() || bytes.size() % 16) return false;
    br_aes_gen_cbcdec_keys context;
    const br_block_cbcdec_class* aes = br_aes_x86ni_cbcdec_get_vtable();
    if (!aes) aes = &br_aes_ct_cbcdec_vtable;
    aes->init(&context.vtable, key.data(), key.size());
    std::string out = bytes;
    aes->run(&context.vtable, iv.data(), out.data(), out.size());
    explicit_bzero(&context, sizeof(context));
    const size_t pad = (uint8_t)out.back();
    if (!pad || pad > 16) return false;
    for (size_t i = 1; i <= pad; ++i)
        if ((uint8_t)out[out.size() - i] != pad) return false;
    out.resize(out.size() - pad);
    bytes = std::move(out);
    return true;
}
}  // namespace radio
