#include "platform/x11_wire.h"

#include <cstdlib>
#include <cstring>

namespace lx::x11 {
namespace {

struct In {
    std::string_view d;
    size_t p = 0;
    bool ok = true;
    bool Need(size_t n) {
        if (!ok || p > d.size() || d.size() - p < n) return ok = false;
        return true;
    }
    uint8_t U8() { return Need(1) ? (uint8_t)d[p++] : 0; }
    uint16_t U16() {
        if (!Need(2)) return 0;
        uint16_t v = (uint16_t)((uint8_t)d[p] | (uint8_t)d[p + 1] << 8);
        p += 2;
        return v;
    }
    uint32_t U32() {
        if (!Need(4)) return 0;
        uint32_t v = (uint32_t)(uint8_t)d[p] | (uint32_t)(uint8_t)d[p + 1] << 8 | (uint32_t)(uint8_t)d[p + 2] << 16 |
                     (uint32_t)(uint8_t)d[p + 3] << 24;
        p += 4;
        return v;
    }
    std::string_view Bytes(size_t n) {
        if (!Need(n)) return {};
        std::string_view v = d.substr(p, n);
        p += n;
        return v;
    }
    void Skip(size_t n) {
        if (Need(n)) p += n;
    }
};

size_t Pad4(size_t n) { return (n + 3) & ~(size_t)3; }

}  // namespace

std::optional<Setup> ParseSetup(std::string_view bytes) {
    In in{bytes};
    Setup s;
    uint8_t status = in.U8();
    uint8_t reasonLen = in.U8();
    in.Skip(4);  // protocol version
    uint16_t extra = in.U16();
    if (!in.ok || bytes.size() < 8 + (size_t)extra * 4) return std::nullopt;
    if (status == 0) {
        s.failure = std::string(in.Bytes(reasonLen));
        if (!in.ok) return std::nullopt;
        return s;
    }
    if (status != 1) {  // 2: authentication needed: the reason is in the extra data
        size_t n = (size_t)extra * 4;
        std::string_view why = in.Bytes(n);
        s.failure = std::string(why.substr(0, why.find('\0')));
        if (s.failure.empty()) s.failure = "the X server wants authentication";
        return in.ok ? std::optional<Setup>(s) : std::nullopt;
    }
    in = In{bytes.substr(0, 8 + (size_t)extra * 4), 8};
    in.Skip(4);  // release
    s.idBase = in.U32();
    s.idMask = in.U32();
    in.Skip(4);  // motion buffer
    uint16_t vendorLen = in.U16();
    s.maxRequest = in.U16();
    uint8_t nScreens = in.U8(), nFormats = in.U8();
    s.imageByteOrder = in.U8();
    in.Skip(3);  // bitmap order, scanline unit and pad
    s.minKeycode = in.U8();
    s.maxKeycode = in.U8();
    in.Skip(4);
    in.Skip(Pad4(vendorLen));
    for (int i = 0; i < nFormats && in.ok; ++i) {
        uint8_t depth = in.U8(), bpp = in.U8();
        in.Skip(6);
        s.formats.emplace_back(depth, bpp);
    }
    for (int i = 0; i < nScreens && in.ok; ++i) {
        Screen sc;
        sc.root = in.U32();
        sc.colormap = in.U32();
        sc.white = in.U32();
        sc.black = in.U32();
        in.Skip(4);  // current input masks
        sc.width = in.U16();
        sc.height = in.U16();
        in.Skip(8);  // millimetres, installed maps
        sc.rootVisual = in.U32();
        in.Skip(2);  // backing stores, save unders
        sc.rootDepth = in.U8();
        uint8_t nDepths = in.U8();
        for (int d = 0; d < nDepths && in.ok; ++d) {
            uint8_t depth = in.U8();
            in.Skip(1);
            uint16_t nVisuals = in.U16();
            in.Skip(4);
            for (int v = 0; v < nVisuals && in.ok; ++v) {
                Visual vis;
                vis.id = in.U32();
                vis.cls = in.U8();
                in.Skip(3);
                vis.red = in.U32();
                vis.green = in.U32();
                vis.blue = in.U32();
                in.Skip(4);
                sc.visuals.emplace_back(depth, vis);
            }
        }
        s.screens.push_back(std::move(sc));
    }
    if (!in.ok || s.screens.empty() || s.minKeycode < 8 || s.maxKeycode < s.minKeycode) return std::nullopt;
    return s;
}

size_t NextUnit(std::string_view b) {
    if (b.size() < 32) return 0;
    const uint8_t type = (uint8_t)b[0];
    if (type == 1 || (type & 0x7f) == 35) {  // a reply, or a generic event: 32 + 4 * length
        uint32_t len = (uint32_t)(uint8_t)b[4] | (uint32_t)(uint8_t)b[5] << 8 | (uint32_t)(uint8_t)b[6] << 16 |
                       (uint32_t)(uint8_t)b[7] << 24;
        if (len > (kMaxReply - 32) / 4) return SIZE_MAX;
        size_t total = 32 + (size_t)len * 4;
        return b.size() >= total ? total : 0;
    }
    return 32;
}

std::optional<std::string> FindCookie(std::string_view file, std::string_view host, std::string_view number) {
    In in{file};
    std::optional<std::string> wild;
    while (in.ok && in.p < file.size()) {
        auto be16 = [&] {
            uint8_t hi = in.U8(), lo = in.U8();
            return (uint16_t)(hi << 8 | lo);
        };
        uint16_t family = be16();
        std::string_view addr = in.Bytes(be16());
        std::string_view num = in.Bytes(be16());
        std::string_view name = in.Bytes(be16());
        std::string_view data = in.Bytes(be16());
        if (!in.ok) break;
        if (name != "MIT-MAGIC-COOKIE-1" || data.size() != 16) continue;
        if (!num.empty() && num != number) continue;
        if (family == 0xFFFF) {  // FamilyWild
            if (!wild) wild = std::string(data);
        } else if (family == 256 && addr == host) {  // FamilyLocal, this host
            return std::string(data);
        }
    }
    return wild;
}

std::optional<DisplayName> ParseDisplay(std::string_view name) {
    DisplayName d;
    if (name.empty()) return std::nullopt;
    if (name[0] == '/') {  // a socket path, as XQuartz and some containers set: "/path/X0" or ":0" inside
        size_t colon = name.rfind(':');
        std::string_view path = colon != std::string_view::npos && colon > 0 ? name.substr(0, colon) : name;
        d.socket = std::string(path);
        size_t x = path.rfind('X');
        if (x != std::string_view::npos) d.number = std::string(path.substr(x + 1));
        return d;
    }
    size_t colon = name.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    std::string_view host = name.substr(0, colon), rest = name.substr(colon + 1);
    if (!host.empty() && host != "unix") return std::nullopt;  // TCP: not spoken here
    size_t dot = rest.find('.');
    std::string_view num = rest.substr(0, dot);
    if (num.empty() || num.size() > 6 || num.find_first_not_of("0123456789") != std::string_view::npos) return std::nullopt;
    d.number = std::string(num);
    if (dot != std::string_view::npos) {
        std::string_view sc = rest.substr(dot + 1);
        if (sc.empty() || sc.size() > 4 || sc.find_first_not_of("0123456789") != std::string_view::npos) return std::nullopt;
        d.screen = std::stoi(std::string(sc));
    }
    d.socket = "/tmp/.X11-unix/X" + d.number;
    return d;
}

std::vector<std::pair<std::string, int32_t>> ParseXSettings(std::string_view data) {
    std::vector<std::pair<std::string, int32_t>> out;
    In in{data};
    uint8_t order = in.U8();
    if (!in.ok || order != 0) return out;  // LSBFirst only (what every manager on x86 writes)
    in.Skip(3);
    in.Skip(4);  // serial
    uint32_t n = in.U32();
    for (uint32_t i = 0; i < n && in.ok && i < 4096; ++i) {
        uint8_t type = in.U8();
        in.Skip(1);
        uint16_t nameLen = in.U16();
        std::string_view name = in.Bytes(nameLen);
        in.Skip(Pad4(nameLen) - nameLen);
        in.Skip(4);  // last change serial
        if (type == 0) {  // integer
            int32_t v = (int32_t)in.U32();
            if (in.ok) out.emplace_back(std::string(name), v);
        } else if (type == 1) {  // string
            uint32_t len = in.U32();
            in.Skip(Pad4(len));
        } else if (type == 2) {  // colour
            in.Skip(8);
        } else {
            break;
        }
    }
    return out;
}

double ResourceDpi(std::string_view res) {
    size_t at = 0;
    while (at < res.size()) {
        size_t nl = res.find('\n', at);
        std::string_view line = res.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? res.size() : nl + 1;
        if (line.rfind("Xft.dpi:", 0) != 0) continue;
        line.remove_prefix(8);
        while (!line.empty() && (line[0] == ' ' || line[0] == '\t')) line.remove_prefix(1);
        double v = 0;
        bool digits = false;
        double frac = 0;
        for (char c : line) {
            if (c >= '0' && c <= '9') {
                digits = true;
                if (frac) v += (c - '0') * frac, frac /= 10;
                else v = v * 10 + (c - '0');
                if (v > 10000) return 0;
            } else if (c == '.' && !frac) {
                frac = 0.1;
            } else {
                break;
            }
        }
        return digits && v >= 24 ? v : 0;
    }
    return 0;
}

std::string ResourceString(std::string_view res, std::string_view name) {
    size_t at = 0;
    while (at < res.size()) {
        size_t nl = res.find('\n', at);
        std::string_view line = res.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? res.size() : nl + 1;
        if (line.size() <= name.size() || line.substr(0, name.size()) != name || line[name.size()] != ':') continue;
        line.remove_prefix(name.size() + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
        return std::string(line);
    }
    return {};
}

std::optional<CursorImage> ParseXcursor(std::string_view f, int size) {
    auto u32 = [&](size_t at, uint32_t& v) {
        if (at > f.size() || f.size() - at < 4) return false;
        v = (uint32_t)(uint8_t)f[at] | (uint32_t)(uint8_t)f[at + 1] << 8 | (uint32_t)(uint8_t)f[at + 2] << 16 |
            (uint32_t)(uint8_t)f[at + 3] << 24;
        return true;
    };
    uint32_t magic, header, version, ntoc;
    if (!u32(0, magic) || magic != 0x72756358 /* "Xcur" */ || !u32(4, header) || !u32(8, version) || !u32(12, ntoc)) return std::nullopt;
    if (header < 16 || ntoc > 0x10000) return std::nullopt;
    constexpr uint32_t kImage = 0xfffd0002;
    uint32_t best = 0, bestPos = 0;
    bool found = false;
    for (uint32_t i = 0; i < ntoc; ++i) {
        uint32_t type, subtype, pos;
        const size_t at = (size_t)header + (size_t)i * 12;
        if (!u32(at, type) || !u32(at + 4, subtype) || !u32(at + 8, pos)) return std::nullopt;
        if (type != kImage) continue;
        const uint32_t d = (uint32_t)std::abs((int64_t)subtype - size);
        if (!found || d < (uint32_t)std::abs((int64_t)best - size)) best = subtype, bestPos = pos, found = true;
    }
    if (!found) return std::nullopt;
    uint32_t chunk, type, subtype, cversion;
    CursorImage img;
    if (!u32(bestPos, chunk) || !u32(bestPos + 4, type) || !u32(bestPos + 8, subtype) || !u32(bestPos + 12, cversion) ||
        !u32(bestPos + 16, img.width) || !u32(bestPos + 20, img.height) || !u32(bestPos + 24, img.xhot) || !u32(bestPos + 28, img.yhot))
        return std::nullopt;
    if (type != kImage || chunk < 36 || !img.width || !img.height || img.width > 1024 || img.height > 1024 || img.xhot > img.width ||
        img.yhot > img.height)
        return std::nullopt;
    const size_t pixels = (size_t)img.width * img.height, start = (size_t)bestPos + chunk;
    if (start > f.size() || (f.size() - start) / 4 < pixels) return std::nullopt;
    img.argb.resize(pixels);
    for (size_t i = 0; i < pixels; ++i) u32(start + 4 * i, img.argb[i]);
    return img;
}

std::vector<std::string> ParseUriList(std::string_view data, bool localOnly) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < data.size() && out.size() < 65536) {
        size_t nl = data.find('\n', at);
        std::string_view line = data.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at);
        at = nl == std::string_view::npos ? data.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("file://", 0) != 0) {
            if (!localOnly) out.emplace_back(line);
            continue;
        }
        std::string_view rest = line.substr(7);
        size_t slash = rest.find('/');
        if (slash == std::string_view::npos) continue;
        std::string_view host = rest.substr(0, slash);
        if (!host.empty() && host != "localhost") continue;  // another machine's file
        rest = rest.substr(slash);
        std::string path;
        bool bad = false;
        for (size_t i = 0; i < rest.size(); ++i) {
            if (rest[i] != '%') {
                path += rest[i];
                continue;
            }
            auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
            if (i + 2 >= rest.size() || hex(rest[i + 1]) < 0 || hex(rest[i + 2]) < 0) {
                bad = true;
                break;
            }
            char c = (char)(hex(rest[i + 1]) * 16 + hex(rest[i + 2]));
            if (c == 0) {
                bad = true;
                break;
            }
            path += c;
            i += 2;
        }
        if (!bad) out.push_back(std::move(path));
    }
    return out;
}

}  // namespace lx::x11
