// The DER, PEM and X.509 pieces the Linux HTTPS code needs, written for cordinal: reading a
// certificate's dates and names, and the bytes of a certificate request and a self-signed
// certificate for a P-256 key. Signing and every check of a signature are BearSSL's (tls_bearssl.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace web::der {

// One element: tag, then its contents. Definite lengths only, as DER requires.
std::string Element(uint8_t tag, std::string_view contents);
// Reads the element at the front of `in` and removes it. False on anything that isn't DER.
bool Next(std::string_view& in, uint8_t& tag, std::string_view& contents, std::string_view* whole = nullptr);

// Unix seconds from a UTCTime or GeneralizedTime ending in Z, and back (UTCTime before 2050).
bool Time(uint8_t tag, std::string_view text, uint64_t& seconds);
std::string Time(uint64_t seconds);

// What CertificateInfo reports: the validity window and the subject alternative names, IP
// addresses as text. `addresses` holds the same IP entries as their 4 or 16 raw bytes.
struct Certificate {
    uint64_t notBefore = 0, notAfter = 0;
    std::vector<std::string> names, addresses;
};
bool ReadCertificate(std::string_view der, Certificate& out);

// SubjectPublicKeyInfo for an uncompressed P-256 point (65 bytes).
std::string P256PublicKey(std::string_view point);
// GeneralNames: IPv4 and IPv6 literals as iPAddress, anything else as dNSName.
std::string AltNames(const std::vector<std::string>& names);
// The unsigned body of a PKCS#10 request (empty subject, the names requested as an extension), and
// of a v3 certificate named CN=cordinal for itself, for serverAuth.
std::string RequestBody(std::string_view point, const std::vector<std::string>& names);
std::string SelfSignedBody(std::string_view point, const std::vector<std::string>& names, std::string_view serial,
                           uint64_t notBefore, uint64_t notAfter);
// A body joined to its ecdsa-with-SHA256 signature (DER Ecdsa-Sig-Value), as both forms end.
std::string SignedEcdsaSha256(std::string_view body, std::string_view signature);

std::string Base64(std::string_view bytes);
bool FromBase64(std::string_view text, std::string& out);  // skips whitespace
// The DER of every "-----BEGIN label-----" block in `text`, in order; a block that doesn't decode is skipped.
std::vector<std::string> Pem(std::string_view text, std::string_view label);
std::string ToPem(std::string_view der, std::string_view label);

}  // namespace web::der
