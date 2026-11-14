// DER, PEM and the X.509 structures cordinal reads and writes on Linux (tls_der.h). RFC 5280 for
// certificates, RFC 2986 for requests, RFC 5480 for P-256 keys and RFC 7468 for PEM.
#include "tls_der.h"

#include <arpa/inet.h>

#include <cstdio>
#include <cstring>

namespace web::der {
namespace {

constexpr uint8_t kSequence = 0x30, kSet = 0x31, kInteger = 0x02, kBits = 0x03, kOctets = 0x04, kOid = 0x06;
constexpr std::string_view kEcPublicKey{"\x2a\x86\x48\xce\x3d\x02\x01", 7};
constexpr std::string_view kP256{"\x2a\x86\x48\xce\x3d\x03\x01\x07", 8};
constexpr std::string_view kEcdsaSha256{"\x2a\x86\x48\xce\x3d\x04\x03\x02", 8};
constexpr std::string_view kExtensionRequest{"\x2a\x86\x48\x86\xf7\x0d\x01\x09\x0e", 9};
constexpr std::string_view kAltName{"\x55\x1d\x11", 3};
constexpr std::string_view kExtendedUsage{"\x55\x1d\x25", 3};
constexpr std::string_view kServerAuth{"\x2b\x06\x01\x05\x05\x07\x03\x01", 8};
constexpr std::string_view kCommonName{"\x55\x04\x03", 3};

std::string Bits(std::string_view bytes) { return Element(kBits, std::string(1, '\0') + std::string(bytes)); }
std::string Algorithm() { return Element(kSequence, Element(kOid, kEcdsaSha256)); }
std::string OwnName() {
    return Element(kSequence, Element(kSet, Element(kSequence, Element(kOid, kCommonName) + Element(0x13, "cordinal"))));
}
std::string Extension(std::string_view oid, std::string_view value) {
    return Element(kSequence, Element(kOid, oid) + Element(kOctets, value));
}

// Howard Hinnant's days_from_civil and civil_from_days, for the proleptic Gregorian calendar.
int64_t Days(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void Civil(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = (int64_t)yoe + era * 400 + (m <= 2);
}

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string Element(uint8_t tag, std::string_view contents) {
    std::string out(1, (char)tag);
    size_t n = contents.size();
    if (n < 0x80) {
        out += (char)n;
    } else {
        char length[sizeof(size_t)];
        int count = 0;
        for (size_t v = n; v; v >>= 8) length[count++] = (char)(v & 0xff);
        out += (char)(0x80 | count);
        while (count) out += length[--count];
    }
    out.append(contents);
    return out;
}

bool Next(std::string_view& in, uint8_t& tag, std::string_view& contents, std::string_view* whole) {
    if (in.size() < 2) return false;
    tag = (uint8_t)in[0];
    if ((tag & 0x1f) == 0x1f) return false;  // high tag numbers appear nowhere cordinal reads
    size_t length = (uint8_t)in[1], head = 2;
    if (length & 0x80) {
        size_t count = length & 0x7f;
        if (!count || count > 4 || in.size() < 2 + count || !(uint8_t)in[2]) return false;
        length = 0;
        for (size_t i = 0; i < count; ++i) length = length << 8 | (uint8_t)in[2 + i];
        if (length < 0x80) return false;  // DER uses the short form for these
        head += count;
    }
    if (length > in.size() - head) return false;
    contents = in.substr(head, length);
    if (whole) *whole = in.substr(0, head + length);
    in.remove_prefix(head + length);
    return true;
}

bool Time(uint8_t tag, std::string_view text, uint64_t& seconds) {
    const size_t digits = tag == 0x17 ? 12 : tag == 0x18 ? 14 : 0;
    if (!digits || text.size() != digits + 1 || text.back() != 'Z') return false;
    unsigned v[14];
    for (size_t i = 0; i < digits; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        v[i] = (unsigned)(text[i] - '0');
    }
    const unsigned* p = v;
    int64_t year;
    if (tag == 0x17) {
        year = v[0] * 10 + v[1];
        year += year < 50 ? 2000 : 1900;  // RFC 5280 4.1.2.5.1
        p += 2;
    } else {
        year = v[0] * 1000 + v[1] * 100 + v[2] * 10 + v[3];
        p += 4;
    }
    unsigned month = p[0] * 10 + p[1], day = p[2] * 10 + p[3], hour = p[4] * 10 + p[5], minute = p[6] * 10 + p[7],
             second = p[8] * 10 + p[9];
    static const unsigned kMonthDays[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12 || day < 1 || day > kMonthDays[month - 1] || hour > 23 || minute > 59 || second > 59) return false;
    if (month == 2 && day == 29 && !(year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) return false;
    int64_t value = Days(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
    seconds = value > 0 ? (uint64_t)value : 0;
    return true;
}

std::string Time(uint64_t seconds) {
    int64_t year;
    unsigned month, day;
    Civil((int64_t)(seconds / 86400), year, month, day);
    unsigned rest = (unsigned)(seconds % 86400);
    char text[32];
    const bool utc = year >= 1950 && year < 2050;
    if (utc) snprintf(text, sizeof(text), "%02u%02u%02u%02u%02u%02uZ", (unsigned)(year % 100), month, day, rest / 3600, rest / 60 % 60, rest % 60);
    else snprintf(text, sizeof(text), "%04u%02u%02u%02u%02u%02uZ", (unsigned)year, month, day, rest / 3600, rest / 60 % 60, rest % 60);
    return Element(utc ? 0x17 : 0x18, text);
}

bool ReadCertificate(std::string_view der, Certificate& out) {
    out = {};
    std::string_view in = der, certificate, body, part;
    uint8_t tag;
    if (!Next(in, tag, certificate) || tag != kSequence || !Next(certificate, tag, body) || tag != kSequence) return false;
    if (!Next(body, tag, part)) return false;
    if (tag == 0xa0 && !Next(body, tag, part)) return false;  // the version
    if (tag != kInteger) return false;                        // the serial number
    std::string_view algorithm, issuer, validity, subject, key, when;
    if (!Next(body, tag, algorithm) || tag != kSequence || !Next(body, tag, issuer) || tag != kSequence ||
        !Next(body, tag, validity) || tag != kSequence) return false;
    if (!Next(validity, tag, when) || !Time(tag, when, out.notBefore) || !Next(validity, tag, when) || !Time(tag, when, out.notAfter))
        return false;
    if (!Next(body, tag, subject) || tag != kSequence || !Next(body, tag, key) || tag != kSequence) return false;
    while (!body.empty()) {
        if (!Next(body, tag, part)) return false;
        if (tag != 0xa3) continue;  // the issuer's and subject's unique identifiers
        std::string_view list;
        if (!Next(part, tag, list) || tag != kSequence) return false;
        while (!list.empty()) {
            std::string_view extension, oid, value;
            if (!Next(list, tag, extension) || tag != kSequence || !Next(extension, tag, oid) || tag != kOid ||
                !Next(extension, tag, value)) return false;
            if (tag == 0x01 && !Next(extension, tag, value)) return false;  // critical
            if (tag != kOctets) return false;
            if (oid != kAltName) continue;
            std::string_view names, name;
            if (!Next(value, tag, names) || tag != kSequence) return false;
            while (!names.empty()) {
                if (!Next(names, tag, name)) return false;
                if (tag == 0x82) {
                    out.names.emplace_back(name);
                } else if (tag == 0x87 && (name.size() == 4 || name.size() == 16)) {
                    char text[INET6_ADDRSTRLEN]{};
                    if (!inet_ntop(name.size() == 4 ? AF_INET : AF_INET6, name.data(), text, sizeof(text))) continue;
                    out.names.push_back(text);
                    out.addresses.emplace_back(name);
                }
            }
        }
    }
    return true;
}

std::string P256PublicKey(std::string_view point) {
    return Element(kSequence, Element(kSequence, Element(kOid, kEcPublicKey) + Element(kOid, kP256)) + Bits(point));
}

std::string AltNames(const std::vector<std::string>& names) {
    std::string list;
    for (const std::string& name : names) {
        unsigned char ip[16];
        if (inet_pton(AF_INET, name.c_str(), ip) == 1) list += Element(0x87, {(const char*)ip, 4});
        else if (inet_pton(AF_INET6, name.c_str(), ip) == 1) list += Element(0x87, {(const char*)ip, 16});
        else list += Element(0x82, name);
    }
    return Element(kSequence, list);
}

std::string RequestBody(std::string_view point, const std::vector<std::string>& names) {
    std::string extensions = Element(kSequence, Extension(kAltName, AltNames(names)));
    std::string attribute = Element(kSequence, Element(kOid, kExtensionRequest) + Element(kSet, extensions));
    return Element(kSequence, Element(kInteger, std::string(1, '\0')) + Element(kSequence, {}) + P256PublicKey(point) +
                                  Element(0xa0, attribute));
}

std::string SelfSignedBody(std::string_view point, const std::vector<std::string>& names, std::string_view serial,
                           uint64_t notBefore, uint64_t notAfter) {
    std::string extensions = Extension(kAltName, AltNames(names)) +
        Extension(kExtendedUsage, Element(kSequence, Element(kOid, kServerAuth)));
    return Element(kSequence, Element(0xa0, Element(kInteger, "\x02")) + Element(kInteger, serial) + Algorithm() + OwnName() +
                                  Element(kSequence, Time(notBefore) + Time(notAfter)) + OwnName() + P256PublicKey(point) +
                                  Element(0xa3, Element(kSequence, extensions)));
}

std::string SignedEcdsaSha256(std::string_view body, std::string_view signature) {
    return Element(kSequence, std::string(body) + Algorithm() + Bits(signature));
}

std::string Base64(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t v = (uint32_t)(uint8_t)bytes[i] << 16;
        if (i + 1 < bytes.size()) v |= (uint32_t)(uint8_t)bytes[i + 1] << 8;
        if (i + 2 < bytes.size()) v |= (uint8_t)bytes[i + 2];
        out += kAlphabet[v >> 18 & 63];
        out += kAlphabet[v >> 12 & 63];
        out += i + 1 < bytes.size() ? kAlphabet[v >> 6 & 63] : '=';
        out += i + 2 < bytes.size() ? kAlphabet[v & 63] : '=';
    }
    return out;
}

bool FromBase64(std::string_view text, std::string& out) {
    out.clear();
    uint32_t bits = 0;
    int count = 0, padding = 0;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') { ++padding; continue; }
        const char* at = c ? strchr(kAlphabet, c) : nullptr;
        if (!at || padding) return false;  // nothing but whitespace may follow the padding
        bits = bits << 6 | (uint32_t)(at - kAlphabet);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += (char)(bits >> count & 0xff);
        }
    }
    // What is left over must be the zero bits before the padding, and the padding must fill the group.
    return count < 6 && !(bits & ((1u << count) - 1)) && padding == (count == 2 ? 1 : count == 4 ? 2 : 0);
}

std::vector<std::string> Pem(std::string_view text, std::string_view label) {
    std::vector<std::string> out;
    const std::string begin = "-----BEGIN " + std::string(label) + "-----", end = "-----END " + std::string(label) + "-----";
    for (size_t at = 0; (at = text.find(begin, at)) != text.npos;) {
        size_t stop = text.find(end, at);
        if (stop == text.npos) break;
        std::string der;
        if (FromBase64(text.substr(at + begin.size(), stop - at - begin.size()), der) && !der.empty()) out.push_back(std::move(der));
        at = stop + end.size();
    }
    return out;
}

std::string ToPem(std::string_view der, std::string_view label) {
    std::string text = Base64(der), out = "-----BEGIN " + std::string(label) + "-----\n";
    for (size_t i = 0; i < text.size(); i += 64) out += text.substr(i, 64) + '\n';
    return out + "-----END " + std::string(label) + "-----\n";
}

}  // namespace web::der
