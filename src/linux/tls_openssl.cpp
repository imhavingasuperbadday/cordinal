// HTTPS for the web server on Linux, with OpenSSL: what web_tls.h declares (keys, certificate requests,
// certificates, server sessions over memory buffers and the ACME account key) and web_remote.h's
// HttpFetch, the client that talks to Let's Encrypt and the router. A key name is the path of an
// owner-only file (web::SaveRecords) holding the key as PEM; the account key's blob is its PKCS#8 DER.
#include "web_tls.h"
#include "web_remote.h"
#include "web_internal.h"
#include "radio_http.h"
#include "radio_hls.h"
#include "library.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/core_names.h>
#include <openssl/x509v3.h>

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

namespace web {
namespace {

struct Deleter {
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
    void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); }
    void operator()(X509* p) const { X509_free(p); }
    void operator()(X509_REQ* p) const { X509_REQ_free(p); }
    void operator()(X509_EXTENSION* p) const { X509_EXTENSION_free(p); }
    void operator()(GENERAL_NAMES* p) const { GENERAL_NAMES_free(p); }
    void operator()(BIO* p) const { BIO_free(p); }
    void operator()(BIGNUM* p) const { BN_free(p); }
    void operator()(ECDSA_SIG* p) const { ECDSA_SIG_free(p); }
    void operator()(SSL_CTX* p) const { SSL_CTX_free(p); }
    void operator()(SSL* p) const { SSL_free(p); }
};
template <class T> using Own = std::unique_ptr<T, Deleter>;

std::string Path(const std::wstring& name) { return compat::Narrow(name.c_str()); }

// The key `name`; a missing one is made and saved when `create` is set.
Own<EVP_PKEY> ReadKey(const std::wstring& name, bool create, std::string& error) {
    std::string pem;
    if (!ReadRecords(name, pem)) { error = "Couldn't read the server's private key."; return nullptr; }
    if (!pem.empty()) {
        Own<BIO> bio(BIO_new_mem_buf(pem.data(), (int)pem.size()));
        Own<EVP_PKEY> key(bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr);
        if (!key) error = "The server's private key can't be read.";
        return key;
    }
    if (!create) { error = "The server has no private key."; return nullptr; }
    Own<EVP_PKEY> key(EVP_EC_gen("P-256"));
    Own<BIO> bio(BIO_new(BIO_s_mem()));
    char* data = nullptr;
    long size = 0;
    if (!key || !bio || !PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) ||
        (size = BIO_get_mem_data(bio.get(), &data)) <= 0 || !SaveRecords(name, std::string(data, (size_t)size))) {
        error = "Couldn't make and save the server's private key.";
        return nullptr;
    }
    return key;
}

std::string Der(X509* x) {
    int size = i2d_X509(x, nullptr);
    std::string out(size > 0 ? (size_t)size : 0, '\0');
    unsigned char* p = (unsigned char*)out.data();
    if (size <= 0 || i2d_X509(x, &p) != size) return {};
    return out;
}
Own<X509> FromDer(const std::string& der) {
    const unsigned char* p = (const unsigned char*)der.data();
    return Own<X509>(d2i_X509(nullptr, &p, (long)der.size()));
}

// The subjectAltName extension: IPv4 and IPv6 literals become IP entries, anything else a DNS name.
Own<X509_EXTENSION> AltNames(const std::vector<std::string>& names) {
    Own<GENERAL_NAMES> list(GENERAL_NAMES_new());
    if (!list) return nullptr;
    for (const std::string& name : names) {
        GENERAL_NAME* entry = GENERAL_NAME_new();
        unsigned char ip[16];
        int size = inet_pton(AF_INET, name.c_str(), ip) == 1 ? 4 : inet_pton(AF_INET6, name.c_str(), ip) == 1 ? 16 : 0;
        ASN1_STRING* value = size ? ASN1_OCTET_STRING_new() : ASN1_IA5STRING_new();
        if (!entry || !value || !ASN1_STRING_set(value, size ? (const void*)ip : name.data(), size ? size : (int)name.size())) {
            GENERAL_NAME_free(entry);
            ASN1_STRING_free(value);
            return nullptr;
        }
        GENERAL_NAME_set0_value(entry, size ? GEN_IPADD : GEN_DNS, value);
        sk_GENERAL_NAME_push(list.get(), entry);
    }
    return Own<X509_EXTENSION>(X509V3_EXT_i2d(NID_subject_alt_name, 0, list.get()));
}
std::vector<std::string> AltNamesOf(X509* x) {
    std::vector<std::string> names;
    Own<GENERAL_NAMES> list((GENERAL_NAMES*)X509_get_ext_d2i(x, NID_subject_alt_name, nullptr, nullptr));
    for (int i = 0; list && i < sk_GENERAL_NAME_num(list.get()); ++i) {
        const GENERAL_NAME* entry = sk_GENERAL_NAME_value(list.get(), i);
        char text[INET6_ADDRSTRLEN]{};
        if (entry->type == GEN_DNS)
            names.emplace_back((const char*)ASN1_STRING_get0_data(entry->d.dNSName), (size_t)ASN1_STRING_length(entry->d.dNSName));
        else if (entry->type == GEN_IPADD && (ASN1_STRING_length(entry->d.iPAddress) == 4 || ASN1_STRING_length(entry->d.iPAddress) == 16) &&
                 inet_ntop(ASN1_STRING_length(entry->d.iPAddress) == 4 ? AF_INET : AF_INET6, ASN1_STRING_get0_data(entry->d.iPAddress),
                           text, sizeof(text)))
            names.push_back(text);
    }
    return names;
}
uint64_t Unix(const ASN1_TIME* time) {
    tm t{};
    return time && ASN1_TIME_to_tm(time, &t) ? (uint64_t)std::max<time_t>(0, timegm(&t)) : 0;
}

// Moves what OpenSSL wrote into `bio` onto `out`.
void Drain(BIO* bio, std::string& out) {
    char buffer[16384];
    for (int n; (n = BIO_read(bio, buffer, sizeof(buffer))) > 0;) out.append(buffer, (size_t)n);
}

}  // namespace

bool EnsureTlsKey(const std::wstring& name, std::string& error) { return ReadKey(name, true, error) != nullptr; }
bool DeleteTlsKey(const std::wstring& name) { return !unlink(Path(name).c_str()); }

std::string CertificateRequest(const std::wstring& keyName, const std::vector<std::string>& names, std::string& error) {
    Own<EVP_PKEY> key = ReadKey(keyName, false, error);
    if (!key) return {};
    Own<X509_REQ> request(X509_REQ_new());
    Own<X509_EXTENSION> alt = AltNames(names);
    STACK_OF(X509_EXTENSION)* extensions = sk_X509_EXTENSION_new_null();
    bool ok = request && alt && extensions && X509_REQ_set_version(request.get(), 0) && X509_REQ_set_pubkey(request.get(), key.get()) &&
        sk_X509_EXTENSION_push(extensions, alt.get()) && X509_REQ_add_extensions(request.get(), extensions) &&
        X509_REQ_sign(request.get(), key.get(), EVP_sha256()) > 0;
    sk_X509_EXTENSION_free(extensions);  // the extension itself belongs to `alt`
    int size = ok ? i2d_X509_REQ(request.get(), nullptr) : 0;
    std::string der(size > 0 ? (size_t)size : 0, '\0');
    unsigned char* p = (unsigned char*)der.data();
    if (size <= 0 || i2d_X509_REQ(request.get(), &p) != size) {
        error = "Couldn't sign the certificate request.";
        return {};
    }
    return der;
}

std::string SelfSignedCertificate(const std::wstring& keyName, const std::vector<std::string>& names, int days) {
    std::string error;
    Own<EVP_PKEY> key = ReadKey(keyName, false, error);
    Own<X509> cert(X509_new());
    Own<X509_EXTENSION> alt = AltNames(names);
    Own<X509_EXTENSION> usage(X509V3_EXT_conf_nid(nullptr, nullptr, NID_ext_key_usage, "serverAuth"));
    uint32_t serial = 0;
    if (!key || !cert || !alt || !usage || getrandom(&serial, sizeof(serial), 0) != sizeof(serial)) return {};
    X509_NAME* name = X509_get_subject_name(cert.get());
    bool ok = X509_set_version(cert.get(), 2) && ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), (long)(serial >> 1) + 1) &&
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600) && X509_gmtime_adj(X509_getm_notAfter(cert.get()), (long)days * 86400) &&
        X509_set_pubkey(cert.get(), key.get()) &&
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*)"cordinal", -1, -1, 0) &&
        X509_set_issuer_name(cert.get(), name) && X509_add_ext(cert.get(), alt.get(), -1) && X509_add_ext(cert.get(), usage.get(), -1) &&
        X509_sign(cert.get(), key.get(), EVP_sha256()) > 0;
    return ok ? Der(cert.get()) : std::string{};
}

std::vector<std::string> PemCertificates(std::string_view pem) {
    std::vector<std::string> out;
    Own<BIO> bio(BIO_new_mem_buf(pem.data(), (int)pem.size()));
    while (bio) {
        Own<X509> cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
        if (!cert) break;
        std::string der = Der(cert.get());
        if (!der.empty()) out.push_back(std::move(der));
    }
    ERR_clear_error();  // the end of the bundle reads as an error
    return out;
}

bool CertificateInfo(const std::string& der, uint64_t& notBefore, uint64_t& notAfter, std::vector<std::string>& names) {
    Own<X509> cert = FromDer(der);
    if (!cert) return false;
    notBefore = Unix(X509_get0_notBefore(cert.get()));
    notAfter = Unix(X509_get0_notAfter(cert.get()));
    names = AltNamesOf(cert.get());
    return true;
}

std::string Sha256(std::string_view data) {
    std::string hash(32, '\0');
    unsigned size = 0;
    if (!EVP_Digest(data.data(), data.size(), (unsigned char*)hash.data(), &size, EVP_sha256(), nullptr) || size != 32) return {};
    return hash;
}

std::shared_ptr<TlsIdentity> TlsIdentity::Create(const std::vector<std::string>& chain, const std::wstring& keyName,
                                                 std::string& error) {
    if (chain.empty()) { error = "There is no certificate."; return {}; }
    Own<X509> leaf = FromDer(chain[0]);
    if (!leaf) { error = "The certificate can't be read."; return {}; }
    Own<EVP_PKEY> key = ReadKey(keyName, false, error);
    if (!key) return {};
    Own<SSL_CTX> context(SSL_CTX_new(TLS_server_method()));
    if (!context || !SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) || !SSL_CTX_use_certificate(context.get(), leaf.get()) ||
        !SSL_CTX_use_PrivateKey(context.get(), key.get()) || !SSL_CTX_check_private_key(context.get())) {
        ERR_clear_error();
        error = "The certificate can't be joined to its key.";
        return {};
    }
    // The intermediates go out with the leaf, so browsers can build the path to their roots.
    for (size_t i = 1; i < chain.size(); ++i)
        if (Own<X509> extra = FromDer(chain[i])) SSL_CTX_add1_chain_cert(context.get(), extra.get());
    SSL_CTX_set_options(context.get(), SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE);
    std::shared_ptr<TlsIdentity> id(new TlsIdentity());
    id->context_ = context.release();
    id->notAfter_ = Unix(X509_get0_notAfter(leaf.get()));
    id->names_ = AltNamesOf(leaf.get());
    id->chain_ = chain;
    return id;
}
TlsIdentity::~TlsIdentity() { SSL_CTX_free((SSL_CTX*)context_); }

TlsSession::TlsSession(std::shared_ptr<TlsIdentity> identity) : identity_(std::move(identity)) {
    SSL* ssl = SSL_new((SSL_CTX*)identity_->Context());
    BIO* in = BIO_new(BIO_s_mem());
    BIO* out = BIO_new(BIO_s_mem());
    if (!ssl || !in || !out) {
        SSL_free(ssl);
        BIO_free(in);
        BIO_free(out);
        return;
    }
    BIO_set_mem_eof_return(in, -1);  // an empty input means "more to come", not the end
    SSL_set_bio(ssl, in, out);       // the session owns both
    SSL_set_accept_state(ssl);
    ssl_ = ssl; in_ = in; out_ = out;
}
TlsSession::~TlsSession() { SSL_free((SSL*)ssl_); }

int TlsSession::Handshake(std::string& input, std::string& output) {
    SSL* ssl = (SSL*)ssl_;
    if (!ssl) return -1;
    if (established_) return 1;
    if (!input.empty()) BIO_write((BIO*)in_, input.data(), (int)input.size());
    input.clear();
    int result = SSL_do_handshake(ssl);
    Drain((BIO*)out_, output);
    if (result == 1) {
        // OpenSSL reads one record at a time, so what follows the handshake is still in the buffer: hand
        // it back, as the caller expects the request's bytes in `input`.
        established_ = true;
        Drain((BIO*)in_, input);
        return 1;
    }
    int error = SSL_get_error(ssl, result);
    ERR_clear_error();
    return error == SSL_ERROR_WANT_READ ? 0 : -1;
}
int TlsSession::Decrypt(std::string& input, std::string& plain, std::string& output) {
    SSL* ssl = (SSL*)ssl_;
    if (!ssl) return -1;
    if (!established_) {
        int result = Handshake(input, output);
        if (result <= 0) return result < 0 ? -1 : 1;
    }
    if (!input.empty()) BIO_write((BIO*)in_, input.data(), (int)input.size());
    input.clear();
    int status = 1;
    char buffer[16384];
    for (;;) {
        int n = SSL_read(ssl, buffer, sizeof(buffer));
        if (n > 0) { plain.append(buffer, (size_t)n); continue; }
        int error = SSL_get_error(ssl, n);
        if (error == SSL_ERROR_ZERO_RETURN) status = 0;
        else if (error != SSL_ERROR_WANT_READ) status = -1;
        break;
    }
    ERR_clear_error();
    Drain((BIO*)out_, output);  // a key update or a session ticket
    return status;
}
bool TlsSession::Encrypt(std::string_view data, std::string& output) {
    SSL* ssl = (SSL*)ssl_;
    if (!ssl || !established_) return false;
    while (!data.empty()) {
        int n = SSL_write(ssl, data.data(), (int)std::min<size_t>(data.size(), 16384));
        if (n <= 0) { ERR_clear_error(); return false; }
        data.remove_prefix((size_t)n);
    }
    Drain((BIO*)out_, output);
    return true;
}

AccountKey::~AccountKey() { Release(); }
void AccountKey::Release() {
    EVP_PKEY_free((EVP_PKEY*)key_);
    key_ = nullptr;
}
bool AccountKey::Create() {
    Release();
    key_ = EVP_EC_gen("P-256");
    return key_ != nullptr;
}
bool AccountKey::Import(std::string_view blob) {
    Release();
    const unsigned char* p = (const unsigned char*)blob.data();
    EVP_PKEY* key = d2i_AutoPrivateKey(nullptr, &p, (long)blob.size());
    if (key && EVP_PKEY_is_a(key, "EC")) key_ = key;
    else EVP_PKEY_free(key);
    ERR_clear_error();
    return key_ != nullptr;
}
std::string AccountKey::Export() const {
    int size = key_ ? i2d_PrivateKey((EVP_PKEY*)key_, nullptr) : 0;
    std::string blob(size > 0 ? (size_t)size : 0, '\0');
    unsigned char* p = (unsigned char*)blob.data();
    if (size <= 0 || i2d_PrivateKey((EVP_PKEY*)key_, &p) != size) return {};
    return blob;
}
std::string AccountKey::Jwk() const {
    std::string coordinates[2];
    const char* names[2] = {OSSL_PKEY_PARAM_EC_PUB_X, OSSL_PKEY_PARAM_EC_PUB_Y};
    for (int i = 0; i < 2; ++i) {
        BIGNUM* raw = nullptr;
        if (!key_ || !EVP_PKEY_get_bn_param((EVP_PKEY*)key_, names[i], &raw)) return {};
        Own<BIGNUM> value(raw);
        coordinates[i].resize(32);
        if (BN_bn2binpad(value.get(), (unsigned char*)coordinates[i].data(), 32) != 32) return {};
    }
    return "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"" + Base64Url(coordinates[0]) + "\",\"y\":\"" + Base64Url(coordinates[1]) + "\"}";
}
std::string AccountKey::Sign(std::string_view data) const {
    Own<EVP_MD_CTX> context(EVP_MD_CTX_new());
    size_t size = 0;
    if (!key_ || !context || EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, (EVP_PKEY*)key_) != 1 ||
        EVP_DigestSign(context.get(), nullptr, &size, (const unsigned char*)data.data(), data.size()) != 1) return {};
    std::string der(size, '\0');
    if (EVP_DigestSign(context.get(), (unsigned char*)der.data(), &size, (const unsigned char*)data.data(), data.size()) != 1) return {};
    // OpenSSL gives the DER sequence of r and s; JWS wants them as two 32-byte numbers.
    const unsigned char* p = (const unsigned char*)der.data();
    Own<ECDSA_SIG> signature(d2i_ECDSA_SIG(nullptr, &p, (long)size));
    const BIGNUM *r = nullptr, *s = nullptr;
    if (!signature) return {};
    ECDSA_SIG_get0(signature.get(), &r, &s);
    std::string out(64, '\0');
    if (BN_bn2binpad(r, (unsigned char*)out.data(), 32) != 32 || BN_bn2binpad(s, (unsigned char*)out.data() + 32, 32) != 32) return {};
    return out;
}

namespace {
// The client side for HttpFetch: certificates are checked against the system's roots (or SSL_CERT_FILE).
SSL_CTX* ClientContext() {
    static SSL_CTX* context = [] {
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        if (c && (!SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION) || !SSL_CTX_set_default_verify_paths(c))) {
            SSL_CTX_free(c);
            c = nullptr;
        }
        if (c) SSL_CTX_set_options(c, SSL_OP_IGNORE_UNEXPECTED_EOF);
        return c;
    }();
    return context;
}

// One connection's bytes, plain or through TLS, with a deadline and the caller's cancel flag.
struct Stream {
    int fd = -1;
    Own<SSL> ssl;
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
    // TLS to `host`, whose certificate must name it (an IP address or a DNS name).
    bool Secure(const std::string& host) {
        SSL_CTX* context = ClientContext();
        if (!context || !(ssl = Own<SSL>(SSL_new(context))) || !SSL_set_fd(ssl.get(), fd)) return false;
        unsigned char ip[16];
        bool literal = inet_pton(AF_INET, host.c_str(), ip) == 1 || inet_pton(AF_INET6, host.c_str(), ip) == 1;
        X509_VERIFY_PARAM* check = SSL_get0_param(ssl.get());
        if (literal ? !X509_VERIFY_PARAM_set1_ip_asc(check, host.c_str())
                    : !SSL_set_tlsext_host_name(ssl.get(), host.c_str()) || !X509_VERIFY_PARAM_set1_host(check, host.c_str(), 0))
            return false;
        SSL_set_verify(ssl.get(), SSL_VERIFY_PEER, nullptr);
        for (;;) {
            int result = SSL_connect(ssl.get());
            if (result == 1) return true;
            int error = SSL_get_error(ssl.get(), result);
            ERR_clear_error();
            if ((error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) || !Wait(error == SSL_ERROR_WANT_WRITE)) return false;
        }
    }
    bool Write(std::string_view data) {
        while (!data.empty()) {
            long n = ssl ? SSL_write(ssl.get(), data.data(), (int)std::min<size_t>(data.size(), 16384))
                         : send(fd, data.data(), data.size(), MSG_NOSIGNAL);
            if (n > 0) { data.remove_prefix((size_t)n); continue; }
            int error = ssl ? SSL_get_error(ssl.get(), (int)n) : 0;
            ERR_clear_error();
            bool again = ssl ? error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE : n < 0 && (errno == EAGAIN || errno == EINTR);
            if (!again || !Wait(!ssl || error == SSL_ERROR_WANT_WRITE)) return false;
        }
        return true;
    }
    // Some bytes onto `out`: 1 read, 0 the end, -1 failed or timed out.
    int Read(std::string& out) {
        char buffer[16384];
        for (;;) {
            long n = ssl ? SSL_read(ssl.get(), buffer, sizeof(buffer)) : recv(fd, buffer, sizeof(buffer), 0);
            if (n > 0) { out.append(buffer, (size_t)n); return 1; }
            int error = ssl ? SSL_get_error(ssl.get(), (int)n) : 0;
            ERR_clear_error();
            if (ssl ? error == SSL_ERROR_ZERO_RETURN : n == 0) return 0;
            bool again = ssl ? error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE : errno == EAGAIN || errno == EINTR;
            if (!again || !Wait(ssl && error == SSL_ERROR_WANT_WRITE)) return -1;
        }
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
                    const std::string& headers, unsigned timeoutMs) {
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
    std::string request = std::string(method) + ' ' + target + " HTTP/1.1\r\nHost: " + authority + "\r\nUser-Agent: cordinal\r\n"
        "Accept-Encoding: identity\r\nConnection: close\r\n" + headers;
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
            if (have > (1u << 20)) { reply.status = 0; return reply; }
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
            "\r\nUser-Agent: cordinal/2.3\r\nIcy-MetaData: 1\r\nAccept-Encoding: identity\r\nConnection: close\r\n" + requestHeaders + "\r\n";
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
bool DecryptRadioAes(std::string& bytes, const std::string& key, std::array<uint8_t, 16> iv) {
    if (key.size() != 16 || bytes.empty() || bytes.size() % 16) return false;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    std::string out(bytes.size() + 16, 0); int count = 0, tail = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, (const unsigned char*)key.data(), iv.data()) == 1 &&
        EVP_DecryptUpdate(ctx, (unsigned char*)out.data(), &count, (const unsigned char*)bytes.data(), (int)bytes.size()) == 1 &&
        EVP_DecryptFinal_ex(ctx, (unsigned char*)out.data() + count, &tail) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (ok) { out.resize(count + tail); bytes = std::move(out); }
    return ok;
}
}  // namespace radio
