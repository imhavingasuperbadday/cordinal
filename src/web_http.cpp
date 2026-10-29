// Small HTTP parser, JSON escaping, media type negotiation and the shared web theme catalog.
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#include <cerrno>
#endif
#include "web_internal.h"
#include "library.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

namespace web {
std::string PhoneAddress(const Status& status) {
    for (const auto& address : status.addresses)
        if (address.size() > 7 && address[7] >= '0' && address[7] <= '9' &&
            address.find("http://127.") != 0) return address;
    for (const auto& address : status.addresses)
        if (address.find("localhost") == address.npos && address.find("127.0.0.1") == address.npos &&
            address.find("[::1]") == address.npos) return address;
    return status.addresses.empty() ? "" : status.addresses.front();
}
uint64_t ClockMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string Lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
std::string RandomHex(size_t size) {
    std::string bytes(size, '\0');
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, (PUCHAR)bytes.data(), (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
#else
    size_t at = 0;
    while (at < size) {
        ssize_t n = getrandom(bytes.data() + at, size - at, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return {};
        at += (size_t)n;
    }
#endif
    std::string out;
    for (unsigned char c : bytes) { out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15]; }
    return out;
}
std::string Json(std::string_view value) {
    std::string out = "\"";
    // Invalid tags must not make the whole response invalid UTF-8.
    for (unsigned char c : ToUtf8(FromUtf8(value))) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 32) { char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", c); out += escaped; }
        else out += (char)c;
    }
    return out + '"';
}
namespace {
bool Number(std::string_view text, uint64_t& value) {
    if (text.empty()) return false;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
int Hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
bool Decode(std::string_view text, std::string& out) {
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '%') {
            if (i + 2 >= text.size() || Hex(text[i + 1]) < 0 || Hex(text[i + 2]) < 0) return false;
            c = (unsigned char)(Hex(text[i + 1]) * 16 + Hex(text[i + 2])); i += 2;
        } else if (c == '+') c = ' ';
        if (c < 32 || c == 127) return false;
        out += (char)c;
    }
    return true;
}
}
bool ParseRange(std::string_view header, uint64_t size, ByteRange& range) {
    range = {0, size, false};
    if (header.empty()) return true;
    if (!size || !header.starts_with("bytes=") || header.find(',') != header.npos) return false;
    header.remove_prefix(6);
    size_t dash = header.find('-');
    if (dash == header.npos) return false;
    uint64_t a = 0, b = 0;
    if (!dash) {
        if (!Number(header.substr(1), b) || !b) return false;
        range.size = std::min(b, size); range.first = size - range.size;
    } else {
        if (!Number(header.substr(0, dash), a) || a >= size) return false;
        if (dash + 1 == header.size()) b = size - 1;
        else if (!Number(header.substr(dash + 1), b) || b < a) return false;
        range.first = a; range.size = std::min(b, size - 1) - a + 1;
    }
    range.partial = true;
    return true;
}
bool ParseRequest(std::string_view bytes, Request& r, size_t& required) {
    required = 0;
    size_t end = bytes.find("\r\n\r\n");
    if (end == bytes.npos) { required = bytes.size() + 1; return bytes.size() <= 16384; }
    if (end > 16384 || bytes.find('\0') != bytes.npos) return false;
    size_t line = bytes.find("\r\n"), a = bytes.find(' '), b = bytes.find(' ', a + 1);
    if (a == bytes.npos || b == bytes.npos || b >= line || a > 8) return false;
    auto version = bytes.substr(b + 1, line - b - 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") return false;
    r = {};
    r.method = bytes.substr(0, a); r.target = bytes.substr(a + 1, b - a - 1);
    if (r.target.empty() || r.target.front() != '/' || r.target.size() > 4096 ||
        r.target.find_first_of("\\#\r\n\t ") != r.target.npos) return false;
    size_t query = r.target.find('?');
    r.path = r.target.substr(0, query);
    if (r.path.find('%') != r.path.npos || r.path.find("..") != r.path.npos) return false;
    if (query != r.target.npos) {
        for (size_t at = query + 1; at < r.target.size();) {
            size_t next = r.target.find('&', at); if (next == r.target.npos) next = r.target.size();
            std::string_view part(r.target.data() + at, next - at);
            size_t eq = part.find('='); std::string key, value;
            if (!Decode(part.substr(0, eq), key) || !Decode(eq == part.npos ? "" : part.substr(eq + 1), value) ||
                key.empty() || !r.query.emplace(key, value).second) return false;
            at = next + 1;
        }
    }
    for (size_t at = line + 2; at < end;) {
        size_t next = bytes.find("\r\n", at), colon = bytes.find(':', at);
        if (colon == bytes.npos || colon >= next || bytes[at] == ' ' || bytes[at] == '\t') return false;
        std::string name = Lower(std::string(bytes.substr(at, colon - at)));
        for (unsigned char c : name) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        auto value = bytes.substr(colon + 1, next - colon - 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
        for (unsigned char c : value) if (c < 32 && c != '\t') return false;
        if (!r.headers.emplace(name, value).second) return false;
        at = next + 2;
    }
    if (!r.headers.count("host") || r.headers.count("transfer-encoding")) return false;
    uint64_t length = 0;
    if (r.headers.count("content-length") && !Number(r.headers["content-length"], length)) return false;
    if (length > 8192 || (r.method != "POST" && length)) return false;
    required = end + 4 + (size_t)length;
    if (bytes.size() >= required) r.body = bytes.substr(end + 4, (size_t)length);
    return bytes.size() <= required;
}
std::string Mime(const std::wstring& path, const std::string& codec) {
    auto ext = ExtOf(path);
    if (ext == L"mp3") return "audio/mpeg";
    if (ext == L"flac" || ext == L"fla") return "audio/flac";
    if (ext == L"wav" || ext == L"wave") return "audio/wav";
    if (ext == L"ogg" || ext == L"oga") return codec == "Opus" ? "audio/ogg; codecs=opus" : "audio/ogg; codecs=vorbis";
    if (ext == L"opus") return "audio/ogg; codecs=opus";
    if (ext == L"m4a" || ext == L"m4b" || ext == L"mp4")
        return codec == "ALAC" ? "audio/mp4; codecs=alac" : "audio/mp4; codecs=mp4a.40.2";
    if (ext == L"aac" || ext == L"adts") return "audio/aac";
    return {};
}
std::string Themes(ThemeMode current, bool systemDark) {
    static const char* roles[] = {"bg", "surface", "surface2", "border", "text", "sub", "faint", "accent",
        "accent2", "hover", "select", "selectText", "playing", "error", "amber", "ok"};
    std::string out = "{\"current\":" + std::to_string((int)current) + ",\"dark\":" +
        (systemDark ? "true" : "false") + ",\"themes\":[";
    for (int i = 0; i < (int)ThemeMode::Count; ++i) {
        const auto& t = ThemeInfo((ThemeMode)i);
        const auto& p = t.palette ? *t.palette : systemDark ? DarkPalette() : LightPalette();
        std::string name = ToUtf8(t.name); name.erase(std::remove(name.begin(), name.end(), '&'), name.end());
        if (!i) name = "System";
        if (i) out += ',';
        out += "{\"name\":" + Json(name) + ",\"font\":" + Json(t.font ? ToUtf8(t.font) : "") +
            ",\"mark\":" + Json(ToUtf8(t.playingMark)) + ",\"colors\":{";
        const Rgb values[] = {p.bg,p.surface,p.surface2,p.border,p.text,p.sub,p.faint,p.accent,p.accent2,p.hover,
            p.select,p.selectText,p.playing,p.error,p.amber,p.ok};
        for (size_t k = 0; k < std::size(roles); ++k) {
            char color[8]; snprintf(color, sizeof(color), "#%06x", values[k]);
            if (k) out += ',';
            out += Json(roles[k]) + ':' + Json(color);
        }
        out += "}}";
    }
    return out + "]}";
}
}  // namespace web
