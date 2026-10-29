// HTTPS for the web server: TLS server sessions driven over buffers, the server's certificates with
// their private key, and the ACME account key. On Windows (web_tls.cpp) SChannel and CNG: the key
// lives in the user's CNG key store where SChannel can use it, and secur32.dll and ncrypt.dll load
// only when the server first needs them. On Linux (linux/tls_openssl.cpp) OpenSSL: a key name is
// the path of an owner-only file holding the key.
#pragma once
#ifdef _WIN32
#include <windows.h>
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <sspi.h>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace web {

// The persisted ECDSA P-256 key `name`, created when missing. Returns false with `error` set.
bool EnsureTlsKey(const std::wstring& name, std::string& error);
bool DeleteTlsKey(const std::wstring& name);
// A PKCS#10 request (DER) signed by the key, naming IP addresses and DNS names; the subject is empty,
// as certificates for addresses carry their names only in the subject alternative names.
std::string CertificateRequest(const std::wstring& keyName, const std::vector<std::string>& names, std::string& error);
// A self-signed certificate (DER) for the key, valid for `days`; used by checks and local tests.
std::string SelfSignedCertificate(const std::wstring& keyName, const std::vector<std::string>& names, int days);
// The DER certificates of a PEM bundle, in order.
std::vector<std::string> PemCertificates(std::string_view pem);
// Expiry (Unix seconds) and the IP/DNS names of a DER certificate. False when it doesn't parse.
bool CertificateInfo(const std::string& der, uint64_t& notBefore, uint64_t& notAfter, std::vector<std::string>& names);
std::string Sha256(std::string_view data);

// A certificate chain (the server's first) bound to its key, with the TLS credentials made from them.
// Immutable: connections keep the identity they started with while a renewal replaces it.
class TlsIdentity {
public:
    static std::shared_ptr<TlsIdentity> Create(const std::vector<std::string>& chain, const std::wstring& keyName,
                                               std::string& error);
    ~TlsIdentity();
    TlsIdentity(const TlsIdentity&) = delete;
    TlsIdentity& operator=(const TlsIdentity&) = delete;
#ifdef _WIN32
    CredHandle* Credentials() { return &credentials_; }
#else
    void* Context() const { return context_; }  // the SSL_CTX
#endif
    uint64_t NotAfter() const { return notAfter_; }
    const std::vector<std::string>& Names() const { return names_; }
    const std::vector<std::string>& Chain() const { return chain_; }
private:
    TlsIdentity() = default;
#ifdef _WIN32
    CredHandle credentials_{};
    bool acquired_ = false;
    void* store_ = nullptr;  // HCERTSTORE holding the chain, so SChannel can send the intermediates
    const void* leaf_ = nullptr;
#else
    void* context_ = nullptr;
#endif
    uint64_t notAfter_ = 0;
    std::vector<std::string> names_, chain_;
};

// One server-side TLS connection. The caller moves bytes between these buffers and the socket.
class TlsSession {
public:
    explicit TlsSession(std::shared_ptr<TlsIdentity> identity);
    ~TlsSession();
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;
    // Consumes handshake bytes from `input` (keeping what follows the handshake) and appends replies to
    // `output`. 1: established; 0: needs more input; -1: failed (send `output`, then close).
    int Handshake(std::string& input, std::string& output);
    // Decrypts the complete records in `input` onto `plain`, keeping a partial record in `input`.
    // 1: fine; 0: the peer closed the session; -1: failed. Post-handshake replies go to `output`.
    int Decrypt(std::string& input, std::string& plain, std::string& output);
    bool Encrypt(std::string_view data, std::string& output);
private:
    std::shared_ptr<TlsIdentity> identity_;
    bool established_ = false;
#ifdef _WIN32
    int Continue(std::string& input, std::string& output);
    CtxtHandle context_{};
    bool hasContext_ = false;
    unsigned header_ = 0, trailer_ = 0, maximum_ = 0;
#else
    void* ssl_ = nullptr;  // the SSL, reading and writing through memory BIOs
    void* in_ = nullptr;
    void* out_ = nullptr;
#endif
};

// The ACME account's ES256 key, in memory, saved by the caller as an opaque private blob (protected
// with DPAPI on Windows, in an owner-only file on Linux).
class AccountKey {
public:
    AccountKey() = default;
    ~AccountKey();
    AccountKey(const AccountKey&) = delete;
    AccountKey& operator=(const AccountKey&) = delete;
    bool Create();                       // a new key, replacing any held
    bool Import(std::string_view blob);  // false leaves no key
    std::string Export() const;          // the private blob Import takes
    // The public key as a JWK; RFC 7638 orders the members, so this text is also what the thumbprint hashes.
    std::string Jwk() const;
    std::string Sign(std::string_view data) const;  // ES256: r and s side by side, as JWS wants them
private:
    void Release();
    void* key_ = nullptr;
};

}  // namespace web
