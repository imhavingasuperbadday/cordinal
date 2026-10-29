// SChannel server sessions, certificate handling and the ACME account key for the web server's HTTPS
// (web_tls.h).
// Keys are persisted CNG keys: SChannel signs the handshake in LSASS, which opens the key by name.
#include <winsock2.h>
#include <ws2tcpip.h>
#include "web_tls.h"
#include "web_remote.h"
#define SCHANNEL_USE_BLACKLISTS
#include <subauth.h>
#include <wincrypt.h>
#include <schannel.h>
#include <ncrypt.h>
#include <bcrypt.h>

#include "library.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace web {
namespace {
PSecurityFunctionTableW Sspi() {
    static PSecurityFunctionTableW table = [] {
        HMODULE module = LoadLibraryExW(L"secur32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto init = module ? (INIT_SECURITY_INTERFACE_W)(void*)GetProcAddress(module, "InitSecurityInterfaceW") : nullptr;
        return init ? init() : nullptr;
    }();
    return table;
}
struct NCryptApi {
    decltype(&NCryptOpenStorageProvider) openProvider;
    decltype(&NCryptOpenKey) openKey;
    decltype(&NCryptCreatePersistedKey) create;
    decltype(&NCryptFinalizeKey) finalize;
    decltype(&NCryptDeleteKey) remove;
    decltype(&NCryptFreeObject) release;
};
const NCryptApi* NCrypt() {
    static const NCryptApi api = [] {
        NCryptApi a{};
        if (HMODULE m = LoadLibraryExW(L"ncrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            a.openProvider = (decltype(a.openProvider))(void*)GetProcAddress(m, "NCryptOpenStorageProvider");
            a.openKey = (decltype(a.openKey))(void*)GetProcAddress(m, "NCryptOpenKey");
            a.create = (decltype(a.create))(void*)GetProcAddress(m, "NCryptCreatePersistedKey");
            a.finalize = (decltype(a.finalize))(void*)GetProcAddress(m, "NCryptFinalizeKey");
            a.remove = (decltype(a.remove))(void*)GetProcAddress(m, "NCryptDeleteKey");
            a.release = (decltype(a.release))(void*)GetProcAddress(m, "NCryptFreeObject");
        }
        return a;
    }();
    return api.openProvider && api.openKey && api.create && api.finalize && api.remove && api.release ? &api : nullptr;
}
struct Key {
    NCRYPT_KEY_HANDLE handle = 0;
    ~Key() { if (handle) NCrypt()->release(handle); }
};
bool OpenKey(const std::wstring& name, Key& key, std::string& error) {
    const NCryptApi* api = NCrypt();
    NCRYPT_PROV_HANDLE provider = 0;
    if (!api || api->openProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) {
        error = "Windows key storage is unavailable."; return false;
    }
    SECURITY_STATUS s = api->openKey(provider, &key.handle, name.c_str(), 0, 0);
    if (s == NTE_BAD_KEYSET) {
        s = api->create(provider, &key.handle, BCRYPT_ECDSA_P256_ALGORITHM, name.c_str(), 0, 0);
        if (s == ERROR_SUCCESS) s = api->finalize(key.handle, 0);
    }
    api->release(provider);
    if (s == ERROR_SUCCESS) return true;
    if (key.handle) { api->release(key.handle); key.handle = 0; }
    error = "Couldn't open the server's private key in Windows key storage.";
    return false;
}

uint64_t Unix(const FILETIME& time) {
    uint64_t value = (uint64_t)time.dwHighDateTime << 32 | time.dwLowDateTime;
    return value > 116444736000000000ull ? (value - 116444736000000000ull) / 10000000 : 0;
}
std::string Encode(LPCSTR type, const void* value) {
    DWORD size = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, type, value, 0, nullptr, nullptr, &size)) return {};
    std::string out(size, '\0');
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, type, value, 0, nullptr, (BYTE*)out.data(), &size)) return {};
    out.resize(size);
    return out;
}
std::string SignAndEncode(NCRYPT_KEY_HANDLE key, LPCSTR type, const void* value) {
    CRYPT_ALGORITHM_IDENTIFIER algorithm{(LPSTR)szOID_ECDSA_SHA256, {}};
    DWORD size = 0;
    if (!CryptSignAndEncodeCertificate(key, 0, X509_ASN_ENCODING, type, value, &algorithm, nullptr, nullptr, &size))
        return {};
    std::string out(size, '\0');
    if (!CryptSignAndEncodeCertificate(key, 0, X509_ASN_ENCODING, type, value, &algorithm, nullptr, (BYTE*)out.data(),
                                       &size)) return {};
    out.resize(size);
    return out;
}
// The subjectAltName extension value: IPv4 and IPv6 literals become IP entries, anything else a DNS name.
std::string AltNameValue(const std::vector<std::string>& names) {
    std::vector<CERT_ALT_NAME_ENTRY> entries(names.size());
    std::vector<std::array<BYTE, 16>> addresses(names.size());
    std::vector<std::wstring> dns(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        auto& e = entries[i];
        if (InetPtonA(AF_INET, names[i].c_str(), addresses[i].data()) == 1) {
            e.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS; e.IPAddress = {4, addresses[i].data()};
        } else if (InetPtonA(AF_INET6, names[i].c_str(), addresses[i].data()) == 1) {
            e.dwAltNameChoice = CERT_ALT_NAME_IP_ADDRESS; e.IPAddress = {16, addresses[i].data()};
        } else {
            dns[i] = FromUtf8(names[i]); e.dwAltNameChoice = CERT_ALT_NAME_DNS_NAME; e.pwszDNSName = dns[i].data();
        }
    }
    CERT_ALT_NAME_INFO info{(DWORD)entries.size(), entries.data()};
    return Encode(X509_ALTERNATE_NAME, &info);
}
std::vector<std::string> AltNames(PCCERT_CONTEXT cert) {
    std::vector<std::string> names;
    PCERT_EXTENSION e = CertFindExtension(szOID_SUBJECT_ALT_NAME2, cert->pCertInfo->cExtension, cert->pCertInfo->rgExtension);
    CERT_ALT_NAME_INFO* info = nullptr;
    DWORD size = 0;
    if (!e || !CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, e->Value.pbData, e->Value.cbData,
                                   CRYPT_DECODE_ALLOC_FLAG, nullptr, &info, &size)) return names;
    for (DWORD i = 0; i < info->cAltEntry; ++i) {
        const auto& entry = info->rgAltEntry[i];
        char text[INET6_ADDRSTRLEN]{};
        if (entry.dwAltNameChoice == CERT_ALT_NAME_DNS_NAME) names.push_back(ToUtf8(entry.pwszDNSName));
        else if (entry.dwAltNameChoice == CERT_ALT_NAME_IP_ADDRESS && (entry.IPAddress.cbData == 4 || entry.IPAddress.cbData == 16) &&
                 InetNtopA(entry.IPAddress.cbData == 4 ? AF_INET : AF_INET6, entry.IPAddress.pbData, text, sizeof(text)))
            names.push_back(text);
    }
    LocalFree(info);
    return names;
}
std::string Code(SECURITY_STATUS status) {
    char text[16];
    snprintf(text, sizeof(text), "0x%08lX", (unsigned long)status);
    return text;
}
constexpr ULONG kAcceptFlags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
    ASC_REQ_EXTENDED_ERROR | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;
}  // namespace

bool EnsureTlsKey(const std::wstring& name, std::string& error) {
    Key key;
    return OpenKey(name, key, error);
}
bool DeleteTlsKey(const std::wstring& name) {
    const NCryptApi* api = NCrypt();
    NCRYPT_PROV_HANDLE provider = 0;
    if (!api || api->openProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return false;
    NCRYPT_KEY_HANDLE key = 0;
    bool ok = api->openKey(provider, &key, name.c_str(), 0, 0) == ERROR_SUCCESS && api->remove(key, 0) == ERROR_SUCCESS;
    api->release(provider);
    return ok;
}
std::string CertificateRequest(const std::wstring& keyName, const std::vector<std::string>& names, std::string& error) {
    Key key;
    if (!OpenKey(keyName, key, error)) return {};
    DWORD size = 0;
    std::vector<BYTE> publicKey;
    if (CryptExportPublicKeyInfoEx(key.handle, 0, X509_ASN_ENCODING, nullptr, 0, nullptr, nullptr, &size)) {
        publicKey.resize(size);
        if (!CryptExportPublicKeyInfoEx(key.handle, 0, X509_ASN_ENCODING, nullptr, 0, nullptr,
                                        (CERT_PUBLIC_KEY_INFO*)publicKey.data(), &size)) publicKey.clear();
    }
    std::string alt = AltNameValue(names);
    if (publicKey.empty() || alt.empty()) { error = "Couldn't prepare the certificate request."; return {}; }
    CERT_EXTENSION extension{(LPSTR)szOID_SUBJECT_ALT_NAME2, FALSE, {(DWORD)alt.size(), (BYTE*)alt.data()}};
    CERT_EXTENSIONS extensions{1, &extension};
    std::string requested = Encode(X509_EXTENSIONS, &extensions);
    CRYPT_ATTR_BLOB value{(DWORD)requested.size(), (BYTE*)requested.data()};
    CRYPT_ATTRIBUTE attribute{(LPSTR)szOID_RSA_certExtensions, 1, &value};
    CERT_NAME_INFO empty{};
    std::string subject = Encode(X509_NAME, &empty);
    CERT_REQUEST_INFO request{};
    request.dwVersion = CERT_REQUEST_V1;
    request.Subject = {(DWORD)subject.size(), (BYTE*)subject.data()};
    request.SubjectPublicKeyInfo = *(CERT_PUBLIC_KEY_INFO*)publicKey.data();
    request.cAttribute = 1;
    request.rgAttribute = &attribute;
    std::string der = SignAndEncode(key.handle, X509_CERT_REQUEST_TO_BE_SIGNED, &request);
    if (der.empty()) error = "Couldn't sign the certificate request.";
    return der;
}
std::string SelfSignedCertificate(const std::wstring& keyName, const std::vector<std::string>& names, int days) {
    Key key;
    std::string error;
    if (!OpenKey(keyName, key, error)) return {};
    char cn[] = "cordinal";
    CERT_RDN_ATTR attribute{(LPSTR)szOID_COMMON_NAME, CERT_RDN_PRINTABLE_STRING, {8, (BYTE*)cn}};
    CERT_RDN rdn{1, &attribute};
    CERT_NAME_INFO name{1, &rdn};
    std::string subject = Encode(X509_NAME, &name), alt = AltNameValue(names);
    LPSTR usages[] = {(LPSTR)szOID_PKIX_KP_SERVER_AUTH};
    CERT_ENHKEY_USAGE usage{1, usages};
    std::string server = Encode(X509_ENHANCED_KEY_USAGE, &usage);
    CERT_EXTENSION list[2] = {{(LPSTR)szOID_SUBJECT_ALT_NAME2, FALSE, {(DWORD)alt.size(), (BYTE*)alt.data()}},
                              {(LPSTR)szOID_ENHANCED_KEY_USAGE, FALSE, {(DWORD)server.size(), (BYTE*)server.data()}}};
    CERT_EXTENSIONS extensions{2, list};
    CERT_NAME_BLOB subjectBlob{(DWORD)subject.size(), (BYTE*)subject.data()};
    CRYPT_ALGORITHM_IDENTIFIER algorithm{(LPSTR)szOID_ECDSA_SHA256, {}};
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    auto shift = [&](int64_t seconds) {
        uint64_t v = ((uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime) + (uint64_t)(seconds * 10000000);
        FILETIME f{(DWORD)v, (DWORD)(v >> 32)};
        SYSTEMTIME s{};
        FileTimeToSystemTime(&f, &s);
        return s;
    };
    SYSTEMTIME start = shift(-3600), end = shift((int64_t)days * 86400);
    PCCERT_CONTEXT cert = subject.empty() || alt.empty() ? nullptr :
        CertCreateSelfSignCertificate(key.handle, &subjectBlob, 0, nullptr, &algorithm, &start, &end, &extensions);
    if (!cert) return {};
    std::string der((const char*)cert->pbCertEncoded, cert->cbCertEncoded);
    CertFreeCertificateContext(cert);
    return der;
}
std::vector<std::string> PemCertificates(std::string_view pem) {
    std::vector<std::string> out;
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";
    for (size_t at = 0; (at = pem.find(begin, at)) != pem.npos;) {
        size_t stop = pem.find(end, at);
        if (stop == pem.npos) break;
        auto body = pem.substr(at + begin.size(), stop - at - begin.size());
        DWORD size = 0;
        if (CryptStringToBinaryA(body.data(), (DWORD)body.size(), CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) {
            std::string der(size, '\0');
            if (CryptStringToBinaryA(body.data(), (DWORD)body.size(), CRYPT_STRING_BASE64, (BYTE*)der.data(), &size,
                                     nullptr, nullptr)) { der.resize(size); out.push_back(std::move(der)); }
        }
        at = stop + end.size();
    }
    return out;
}
bool CertificateInfo(const std::string& der, uint64_t& notBefore, uint64_t& notAfter, std::vector<std::string>& names) {
    PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE*)der.data(), (DWORD)der.size());
    if (!cert) return false;
    notBefore = Unix(cert->pCertInfo->NotBefore); notAfter = Unix(cert->pCertInfo->NotAfter);
    names = AltNames(cert);
    CertFreeCertificateContext(cert);
    return true;
}

std::shared_ptr<TlsIdentity> TlsIdentity::Create(const std::vector<std::string>& chain, const std::wstring& keyName,
                                                 std::string& error) {
    PSecurityFunctionTableW sspi = Sspi();
    if (!sspi) { error = "Windows TLS is unavailable."; return {}; }
    if (chain.empty()) { error = "There is no certificate."; return {}; }
    std::shared_ptr<TlsIdentity> id(new TlsIdentity());
    id->store_ = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    PCCERT_CONTEXT leaf = nullptr;
    if (!id->store_ || !CertAddEncodedCertificateToStore((HCERTSTORE)id->store_, X509_ASN_ENCODING,
            (const BYTE*)chain[0].data(), (DWORD)chain[0].size(), CERT_STORE_ADD_ALWAYS, &leaf)) {
        error = "The certificate can't be read."; return {};
    }
    id->leaf_ = leaf;
    for (size_t i = 1; i < chain.size(); ++i)
        CertAddEncodedCertificateToStore((HCERTSTORE)id->store_, X509_ASN_ENCODING, (const BYTE*)chain[i].data(),
                                         (DWORD)chain[i].size(), CERT_STORE_ADD_ALWAYS, nullptr);
    CRYPT_KEY_PROV_INFO provider{const_cast<wchar_t*>(keyName.c_str()), const_cast<wchar_t*>(MS_KEY_STORAGE_PROVIDER),
                                 0, 0, 0, nullptr, 0};
    if (!CertSetCertificateContextProperty(leaf, CERT_KEY_PROV_INFO_PROP_ID, 0, &provider)) {
        error = "The certificate can't be joined to its key."; return {};
    }
    TLS_PARAMETERS parameters{};
    parameters.grbitDisabledProtocols = SP_PROT_SSL2_SERVER | SP_PROT_SSL3_SERVER | SP_PROT_TLS1_0_SERVER |
        SP_PROT_TLS1_1_SERVER;
    SCH_CREDENTIALS credentials{};
    credentials.dwVersion = SCH_CREDENTIALS_VERSION;
    credentials.cCreds = 1;
    credentials.paCred = &leaf;
    credentials.dwFlags = SCH_USE_STRONG_CRYPTO;
    credentials.cTlsParameters = 1;
    credentials.pTlsParameters = &parameters;
    TimeStamp expiry{};
    SECURITY_STATUS s = sspi->AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_INBOUND,
        nullptr, &credentials, nullptr, nullptr, &id->credentials_, &expiry);
    if (s != SEC_E_OK) { error = "Windows couldn't use the server certificate (" + Code(s) + ")."; return {}; }
    id->acquired_ = true;
    id->notAfter_ = Unix(leaf->pCertInfo->NotAfter);
    id->names_ = AltNames(leaf);
    id->chain_ = chain;
    return id;
}
TlsIdentity::~TlsIdentity() {
    if (acquired_) Sspi()->FreeCredentialsHandle(&credentials_);
    if (leaf_) CertFreeCertificateContext((PCCERT_CONTEXT)leaf_);
    if (store_) CertCloseStore((HCERTSTORE)store_, 0);
}

TlsSession::TlsSession(std::shared_ptr<TlsIdentity> identity) : identity_(std::move(identity)) {
    SecInvalidateHandle(&context_);
}
TlsSession::~TlsSession() {
    if (hasContext_) Sspi()->DeleteSecurityContext(&context_);
}
int TlsSession::Continue(std::string& input, std::string& output) {
    PSecurityFunctionTableW sspi = Sspi();
    SecBuffer in[2]{{(ULONG)input.size(), SECBUFFER_TOKEN, input.empty() ? nullptr : input.data()},
                    {0, SECBUFFER_EMPTY, nullptr}};
    SecBuffer out[2]{{0, SECBUFFER_TOKEN, nullptr}, {0, SECBUFFER_ALERT, nullptr}};
    SecBufferDesc inDesc{SECBUFFER_VERSION, 2, in}, outDesc{SECBUFFER_VERSION, 2, out};
    ULONG attributes = 0;
    SECURITY_STATUS s = sspi->AcceptSecurityContext(identity_->Credentials(), hasContext_ ? &context_ : nullptr, &inDesc,
        kAcceptFlags, 0, &context_, &outDesc, &attributes, nullptr);
    if (SecIsValidHandle(&context_)) hasContext_ = true;
    for (auto& b : out) if (b.pvBuffer) {
        if (b.BufferType == SECBUFFER_TOKEN && b.cbBuffer) output.append((const char*)b.pvBuffer, b.cbBuffer);
        sspi->FreeContextBuffer(b.pvBuffer);
    }
    if (s == SEC_E_INCOMPLETE_MESSAGE) return 0;
    if (s != SEC_E_OK && s != SEC_I_CONTINUE_NEEDED) return -1;
    input.erase(0, input.size() - (in[1].BufferType == SECBUFFER_EXTRA ? in[1].cbBuffer : 0));
    if (s == SEC_I_CONTINUE_NEEDED) return 0;
    SecPkgContext_StreamSizes sizes{};
    if (sspi->QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) return -1;
    header_ = sizes.cbHeader; trailer_ = sizes.cbTrailer; maximum_ = sizes.cbMaximumMessage;
    established_ = true;
    return 1;
}
int TlsSession::Handshake(std::string& input, std::string& output) {
    while (!established_) {
        if (input.empty()) return 0;
        size_t before = input.size();
        int result = Continue(input, output);
        if (result < 0) return -1;
        if (result == 0 && input.size() == before) return 0;  // the next message is still arriving
    }
    return 1;
}
int TlsSession::Decrypt(std::string& input, std::string& plain, std::string& output) {
    PSecurityFunctionTableW sspi = Sspi();
    while (!input.empty()) {
        if (!established_) return Handshake(input, output) < 0 ? -1 : 1;
        SecBuffer b[4]{{(ULONG)input.size(), SECBUFFER_DATA, input.data()}, {0, SECBUFFER_EMPTY, nullptr},
                       {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc desc{SECBUFFER_VERSION, 4, b};
        SECURITY_STATUS s = sspi->DecryptMessage(&context_, &desc, 0, nullptr);
        if (s == SEC_E_INCOMPLETE_MESSAGE) return 1;
        if (s == SEC_I_CONTEXT_EXPIRED) return 0;
        if (s != SEC_E_OK && s != SEC_I_RENEGOTIATE) return -1;
        std::string rest;
        for (const auto& x : b) {
            if (x.BufferType == SECBUFFER_DATA && x.cbBuffer) plain.append((const char*)x.pvBuffer, x.cbBuffer);
            else if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer) rest.assign((const char*)x.pvBuffer, x.cbBuffer);
        }
        input.swap(rest);
        if (s == SEC_I_RENEGOTIATE) {
            // TLS 1.3 post-handshake messages (a key update, say) go back through the handshake call.
            established_ = false;
            int result = Continue(input, output);
            if (result < 0) return -1;
            if (result == 0) return 1;
        }
    }
    return 1;
}
bool TlsSession::Encrypt(std::string_view data, std::string& output) {
    PSecurityFunctionTableW sspi = Sspi();
    while (!data.empty()) {
        size_t n = std::min<size_t>(data.size(), maximum_), at = output.size();
        output.resize(at + header_ + n + trailer_);
        char* p = output.data() + at;
        memcpy(p + header_, data.data(), n);
        SecBuffer b[4]{{header_, SECBUFFER_STREAM_HEADER, p}, {(ULONG)n, SECBUFFER_DATA, p + header_},
                       {trailer_, SECBUFFER_STREAM_TRAILER, p + header_ + n}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc desc{SECBUFFER_VERSION, 4, b};
        if (sspi->EncryptMessage(&context_, 0, &desc, 0) != SEC_E_OK) { output.resize(at); return false; }
        output.resize(at + b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer);
        data.remove_prefix(n);
    }
    return true;
}

std::string Sha256(std::string_view data) {
    std::string hash(32, '\0');
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(),
                                   (PUCHAR)hash.data(), 32))) return {};
    return hash;
}

// The account key is in-process (BCrypt); its blob is BCrypt's private ECC blob.
AccountKey::~AccountKey() { Release(); }
void AccountKey::Release() {
    if (key_) BCryptDestroyKey((BCRYPT_KEY_HANDLE)key_);
    key_ = nullptr;
}
bool AccountKey::Create() {
    Release();
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, &key, 256, 0))) return false;
    key_ = key;
    if (BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0))) return true;
    Release();
    return false;
}
bool AccountKey::Import(std::string_view blob) {
    Release();
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key,
                                            (PUCHAR)blob.data(), (ULONG)blob.size(), 0))) return false;
    key_ = key;
    return true;
}
namespace {
std::string ExportKey(void* key, LPCWSTR type) {
    ULONG size = 0;
    if (!key || !BCRYPT_SUCCESS(BCryptExportKey((BCRYPT_KEY_HANDLE)key, nullptr, type, nullptr, 0, &size, 0))) return {};
    std::string blob(size, '\0');
    if (!BCRYPT_SUCCESS(BCryptExportKey((BCRYPT_KEY_HANDLE)key, nullptr, type, (PUCHAR)blob.data(), size, &size, 0))) return {};
    blob.resize(size);
    return blob;
}
}  // namespace
std::string AccountKey::Export() const { return ExportKey(key_, BCRYPT_ECCPRIVATE_BLOB); }
std::string AccountKey::Jwk() const {
    std::string blob = ExportKey(key_, BCRYPT_ECCPUBLIC_BLOB);
    if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB) + 64) return {};
    return "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"" + Base64Url(blob.substr(sizeof(BCRYPT_ECCKEY_BLOB), 32)) +
        "\",\"y\":\"" + Base64Url(blob.substr(sizeof(BCRYPT_ECCKEY_BLOB) + 32, 32)) + "\"}";
}
std::string AccountKey::Sign(std::string_view data) const {  // BCrypt gives r and s side by side
    std::string hash = Sha256(data), signature(64, '\0');
    ULONG size = 0;
    if (!key_ || hash.empty() || !BCRYPT_SUCCESS(BCryptSignHash((BCRYPT_KEY_HANDLE)key_, nullptr, (PUCHAR)hash.data(), 32,
                                                              (PUCHAR)signature.data(), 64, &size, 0)) || size != 64) return {};
    return signature;
}
}  // namespace web
