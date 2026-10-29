// HLS audio playback: bounded playlist/segment requests, live reloads and native audio demuxing.
#include "radio_hls.h"
#include "radio_http.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <span>

namespace radio {
namespace {
std::string_view Trim(std::string_view s) {
    while (!s.empty() && (unsigned char)s.front() <= ' ') s.remove_prefix(1);
    while (!s.empty() && (unsigned char)s.back() <= ' ') s.remove_suffix(1);
    return s;
}
bool Number(std::string_view s, uint64_t& n) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), n);
    return !s.empty() && r.ec == std::errc() && r.ptr == s.data() + s.size();
}
std::map<std::string, std::string> Attributes(std::string_view s) {
    std::map<std::string, std::string> out;
    while (!s.empty()) {
        size_t eq = s.find('='); if (eq == s.npos) break;
        std::string key(Trim(s.substr(0, eq))); s.remove_prefix(eq + 1);
        size_t n;
        if (s.starts_with('"')) {
            s.remove_prefix(1); n = s.find('"'); if (n == s.npos) break;
            out[key] = s.substr(0, n); s.remove_prefix(n + 1);
            n = s.find(','); if (n == s.npos) break; s.remove_prefix(n + 1);
        } else {
            n = s.find(','); out[key] = Trim(s.substr(0, n));
            if (n == s.npos) break;
            s.remove_prefix(n + 1);
        }
    }
    return out;
}
bool Range(std::string_view value, HlsRange& range, bool& explicitOffset) {
    size_t at = value.find('@'); uint64_t size = 0, offset = range.offset;
    explicitOffset = at != value.npos;
    if (!Number(value.substr(0, at), size) || !size || size > (16u << 20) ||
        (explicitOffset && !Number(value.substr(at + 1), offset)) || offset > UINT64_MAX - size) return false;
    range = {offset, size}; return true;
}
using Bytes = std::span<const uint8_t>;
uint32_t Be(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
constexpr uint32_t Tag(const char* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
struct Box { uint32_t tag = 0; Bytes bytes; size_t start = 0; };
bool Next(Bytes all, size_t& at, Box& box) {
    if (at > all.size() || all.size() - at < 8) return false;
    uint64_t n = Be(all.data() + at); size_t head = 8, start = at;
    if (n == 1) {
        if (all.size() - at < 16) return false;
        n = uint64_t(Be(all.data() + at + 8)) << 32 | Be(all.data() + at + 12); head = 16;
    }
    if (!n) n = all.size() - at;
    if (n < head || n > all.size() - at) return false;
    at += (size_t)n; box = {Be(all.data() + start + 4), all.subspan(start + head, (size_t)n - head), start}; return true;
}
Bytes Child(Bytes all, uint32_t type) {
    size_t at = 0; Box b; while (Next(all, at, b)) if (b.tag == type) return b.bytes;
    return {};
}
Bytes Path(Bytes all, std::initializer_list<uint32_t> path) { for (uint32_t t : path) all = Child(all, t); return all; }
bool Descriptor(Bytes& list, unsigned& tag, Bytes& value) {
    if (list.empty()) return false;
    tag = list[0]; size_t n = 0;
    for (size_t i = 1; i <= 4 && i < list.size(); ++i) {
        n = n * 128 + (list[i] & 127);
        if (!(list[i] & 128)) {
            if (n > list.size() - i - 1) return false;
            value = list.subspan(i + 1, n); list = list.subspan(i + 1 + n); return true;
        }
    }
    return false;
}
Bytes AacConfig(Bytes esds) {
    if (esds.size() < 4) return {};
    Bytes list = esds.subspan(4), value; unsigned tag;
    if (!Descriptor(list, tag, value) || tag != 3 || value.size() < 3) return {};
    size_t skip = 3; unsigned flags = value[2];
    if (flags & 128) skip += 2;
    if (flags & 64) { if (skip >= value.size()) return {}; skip += 1 + value[skip]; }
    if (flags & 32) skip += 2;
    if (skip > value.size()) return {};
    list = value.subspan(skip);
    if (!Descriptor(list, tag, value) || tag != 4 || value.size() < 13) return {};
    list = value.subspan(13);
    while (Descriptor(list, tag, value)) if (tag == 5) return value;
    return {};
}
bool Fetch(const std::string& url, HlsRange range, const std::atomic<bool>& cancel,
           std::string& bytes, std::string* final = nullptr) {
    bytes.clear(); unsigned status = 0; bool bounded = true;
    std::string request;
    if (range.size) request = "Range: bytes=" + std::to_string(range.offset) + "-" +
                              std::to_string(range.offset + range.size - 1) + "\r\n";
    bool ok = HttpStream(url, cancel, [&](unsigned code, const Headers&) { status = code; return code == 200 || code == 206; },
        [&](std::string_view data) {
            if (data.size() > (16u << 20) - bytes.size()) { bounded = false; return false; }
            bytes.append(data); return !cancel;
        }, request, final);
    if (!ok || !bounded || cancel || bytes.empty()) return false;
    if (range.size) {
        if (status == 200) {
            if (range.offset > bytes.size() || range.size > bytes.size() - range.offset) return false;
            bytes = bytes.substr((size_t)range.offset, (size_t)range.size);
        } else if (bytes.size() != range.size) return false;
    }
    return true;
}
}  // namespace

std::string ResolveRadioUrl(std::string_view base, std::string_view relative) {
    relative = Trim(relative);
    std::string url;
    size_t scheme = base.find("://"); if (scheme == base.npos) return {};
    if (relative.starts_with("http://") || relative.starts_with("https://")) url = relative;
    else if (relative.starts_with("//")) url = std::string(base.substr(0, scheme + 1)) + std::string(relative);
    else {
        size_t end = base.find('/', scheme + 3);
        size_t query = base.find_first_of("?#", scheme + 3);
        if (end == base.npos || (query != base.npos && query < end)) end = query;
        std::string origin(base.substr(0, end));
        std::string path = end == base.npos ? "/" : std::string(base.substr(end));
        if (relative.starts_with('/')) url = origin + std::string(relative);
        else if (relative.starts_with('?')) url = origin + path.substr(0, path.find_first_of("?#")) + std::string(relative);
        else {
            path.resize(path.find_first_of("?#") == path.npos ? path.size() : path.find_first_of("?#"));
            size_t slash = path.rfind('/');
            url = origin + (slash == path.npos ? "/" : path.substr(0, slash + 1)) + std::string(relative);
        }
    }
    if (url.size() > 8192 || std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; })) return {};
    size_t start = url.find('/', url.find("://") + 3);
    if (start == url.npos) return url;
    size_t tail = url.find_first_of("?#", start);
    std::string suffix = tail == url.npos ? "" : url.substr(tail);
    std::string_view path(url.data() + start, (tail == url.npos ? url.size() : tail) - start);
    std::vector<std::string_view> parts;
    for (size_t i = 1; i <= path.size();) {
        size_t n = path.find('/', i); if (n == path.npos) n = path.size();
        auto part = path.substr(i, n - i);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (part != ".") parts.push_back(part);
        i = n + 1;
    }
    std::string out = url.substr(0, start);
    for (auto p : parts) { out += '/'; out += p; }
    if (parts.empty()) out += '/';
    return out + suffix;
}

bool ParseHls(std::string_view text, const std::string& base, HlsPlaylist& out, std::wstring& problem) {
    problem.clear(); HlsPlaylist list;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    if (text.size() > (1u << 20) || !text.starts_with("#EXTM3U")) { problem = L"The station returned an invalid HLS playlist."; return false; }
    HlsSegment next; uint64_t sequence = 0; std::string lastRangeUrl; bool ranged = false, explicitOffset = false;
    uint64_t nextOffset = 0; std::map<std::string, std::string> variant;
    struct Choice { std::string url, group; uint64_t rank; }; std::vector<Choice> choices;
    struct Audio { std::string url, group; bool preferred; }; std::vector<Audio> audio;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at); if (end == text.npos) end = text.size();
        auto line = Trim(text.substr(at, end - at)); at = end + 1;
        if (line.size() > 8192) return false;
        if (line.starts_with("#EXT-X-TARGETDURATION:")) {
            uint64_t n; if (!Number(line.substr(22), n) || !n || n > 86400) return false; list.target = (unsigned)n;
        } else if (line.starts_with("#EXT-X-MEDIA-SEQUENCE:")) {
            if (!Number(line.substr(22), sequence)) return false;
        } else if (line.starts_with("#EXT-X-STREAM-INF:")) variant = Attributes(line.substr(18));
        else if (line.starts_with("#EXT-X-MEDIA:")) {
            auto a = Attributes(line.substr(13));
            if (a["TYPE"] == "AUDIO" && !a["URI"].empty())
                audio.push_back({ResolveRadioUrl(base, a["URI"]), a["GROUP-ID"], a["DEFAULT"] == "YES"});
        } else if (line.starts_with("#EXT-X-MAP:")) {
            auto a = Attributes(line.substr(11)); next.init = ResolveRadioUrl(base, a["URI"]); next.initRange = {};
            next.initKey = next.key; next.initIv = next.iv;
            if (!next.initKey.empty() && !next.explicitIv) { problem = L"The encrypted HLS initialization has no IV."; return false; }
            bool offset = false;
            if (next.init.empty() || (!a["BYTERANGE"].empty() && !Range(a["BYTERANGE"], next.initRange, offset))) return false;
        } else if (line.starts_with("#EXT-X-KEY:")) {
            auto a = Attributes(line.substr(11)); next.key.clear(); next.explicitIv = false; next.iv = {};
            if (a["METHOD"] == "NONE") continue;
            if (a["METHOD"] != "AES-128" || (!a["KEYFORMAT"].empty() && a["KEYFORMAT"] != "identity")) {
                problem = L"This station uses an HLS encryption format that cordinal cannot decode."; return false;
            }
            next.key = ResolveRadioUrl(base, a["URI"]); if (next.key.empty()) return false;
            if (!a["IV"].empty()) {
                std::string_view iv = a["IV"];
                if (!iv.starts_with("0x") || iv.size() > 34 || iv.size() < 3) return false;
                iv.remove_prefix(2); size_t digit = 32 - iv.size();
                for (char c : iv) {
                    unsigned n = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
                    if (n == 16) return false;
                    next.iv[digit / 2] |= (uint8_t)(n << (digit % 2 ? 0 : 4)); ++digit;
                }
                next.explicitIv = true;
            }
        } else if (line.starts_with("#EXT-X-BYTERANGE:")) {
            next.range.offset = nextOffset;
            if (!Range(line.substr(17), next.range, explicitOffset)) return false;
            ranged = true;
        } else if (line == "#EXT-X-DISCONTINUITY") next.discontinuity = true;
        else if (line == "#EXT-X-GAP") next.gap = true;
        else if (line == "#EXT-X-ENDLIST") list.ended = true;
        else if (!line.empty() && line.front() != '#') {
            std::string url = ResolveRadioUrl(base, line); if (url.empty()) return false;
            if (!variant.empty()) {
                uint64_t bandwidth = 0; Number(variant["BANDWIDTH"], bandwidth);
                bandwidth = std::min<uint64_t>(bandwidth, 1000000000);
                bool video = !variant["RESOLUTION"].empty() || variant["CODECS"].find("avc") != std::string::npos || variant["CODECS"].find("hvc") != std::string::npos;
                uint64_t rank = video ? 10000000 : 0;
                if (variant["CODECS"].find("mp4a.40.5") != std::string::npos || variant["CODECS"].find("mp4a.40.29") != std::string::npos)
                    rank += 20000000;
                rank += bandwidth > 512000 ? 1000000 + bandwidth : 512000 - bandwidth;
                choices.push_back({url, variant["AUDIO"], rank}); variant.clear();
            } else {
                if (sequence == UINT64_MAX || list.segments.size() >= 10000) return false;
                if (ranged && !explicitOffset && lastRangeUrl != url) return false;
                next.url = url; next.sequence = sequence++;
                if (ranged) { nextOffset = next.range.offset + next.range.size; lastRangeUrl = url; }
                list.segments.push_back(next); next.range = {}; next.discontinuity = next.gap = false; ranged = false;
            }
        }
    }
    if (!choices.empty()) {
        auto choice = std::min_element(choices.begin(), choices.end(), [](const auto& a, const auto& b) { return a.rank < b.rank; });
        list.variant = choice->url;
        for (const auto& a : audio) if (a.group == choice->group && !a.url.empty()) {
            list.variant = a.url; if (a.preferred) break;
        }
    } else if (!audio.empty()) list.variant = audio.front().url;
    if (list.variant.empty() && list.segments.empty()) { problem = L"The HLS playlist has no audio segments."; return false; }
    out = std::move(list); return true;
}

void HlsAudio::Discontinuity() { pmt_ = audioPid_ = continuity_ = -1; pat_.clear(); pmtBytes_.clear(); pesHeader_.clear(); pesLeft_ = 0; }
bool HlsAudio::Transport(std::string_view bytes, std::string& out) {
    auto section = [&](std::string& pending, const uint8_t* p, size_t n, bool start, auto parse) {
        auto consume = [&] {
            while (pending.size() >= 3) {
                const auto* s = (const uint8_t*)pending.data();
                if (s[0] == 255) { pending.clear(); break; }
                size_t len = 3 + ((s[1] & 15) << 8 | s[2]);
                if (len > 4096) { pending.clear(); break; }
                if (pending.size() < len) break;
                parse(s, len); pending.erase(0, len);
            }
        };
        if (start) {
            if (!n || size_t(p[0]) + 1 > n) { pending.clear(); return; }
            size_t skip = p[0] + 1;
            if (!pending.empty()) { pending.append((const char*)p + 1, skip - 1); consume(); }
            pending.clear(); p += skip; n -= skip;
        }
        pending.append((const char*)p, n); consume();
    };
    for (size_t at = 0; at < bytes.size(); at += 188) {
        if (bytes.size() - at < 188) return false;
        const auto* p = (const uint8_t*)bytes.data() + at;
        if (p[0] != 0x47 || (p[1] & 128) || (p[3] & 192)) return false;
        int pid = (p[1] & 31) << 8 | p[2], cc = p[3] & 15; bool start = p[1] & 64;
        if (!(p[3] & 16)) continue;
        size_t offset = 4;
        if (p[3] & 32) {
            if (pid == audioPid_ && p[4] && (p[5] & 128)) continuity_ = -1;
            offset += 1 + p[4];
        }
        if (offset > 188) return false;
        size_t n = 188 - offset; p += offset;
        if (pid == 0) section(pat_, p, n, start, [&](const uint8_t* s, size_t len) {
            if (s[0] || len < 12) return;
            for (size_t i = 8; i + 4 <= len - 4; i += 4) if (s[i] || s[i + 1]) { pmt_ = (s[i + 2] & 31) << 8 | s[i + 3]; break; }
        });
        else if (pid == pmt_) section(pmtBytes_, p, n, start, [&](const uint8_t* s, size_t len) {
            if (s[0] != 2 || len < 16) return;
            size_t i = 12 + ((s[10] & 15) << 8 | s[11]);
            for (; i + 5 <= len - 4;) {
                int streamPid = (s[i + 1] & 31) << 8 | s[i + 2];
                if (s[i] == 15 || s[i] == 3 || s[i] == 4) { audioPid_ = streamPid; break; }
                i += 5 + ((s[i + 3] & 15) << 8 | s[i + 4]);
            }
        });
        else if (pid == audioPid_) {
            if (cc == continuity_) continue;
            if (continuity_ >= 0 && cc != (continuity_ + 1) % 16 && !start) return false;
            continuity_ = cc;
            if (start) { pesHeader_.clear(); pesLeft_ = 0; }
            if (start || !pesHeader_.empty()) {
                pesHeader_.append((const char*)p, n);
                const auto* h = (const uint8_t*)pesHeader_.data();
                if (pesHeader_.size() < 9) continue;
                if (h[0] || h[1] || h[2] != 1 || h[3] < 0xc0 || h[3] > 0xdf) return false;
                size_t head = 9 + h[8], length = (h[4] << 8) | h[5];
                if (pesHeader_.size() < head) continue;
                if (length && length < head - 6) return false;
                pesLeft_ = length ? length - (head - 6) : SIZE_MAX;
                size_t take = std::min(pesLeft_, pesHeader_.size() - head);
                out.append(pesHeader_.data() + head, take); pesLeft_ -= take; pesHeader_.clear();
            } else {
                size_t take = std::min(pesLeft_, n); out.append((const char*)p, take); pesLeft_ -= take;
            }
        }
    }
    return audioPid_ >= 0;
}

bool HlsAudio::Init(std::string_view bytes) {
    Bytes all((const uint8_t*)bytes.data(), bytes.size()), moov = Child(all, Tag("moov"));
    size_t at = 0; Box track;
    while (Next(moov, at, track)) {
        if (track.tag != Tag("trak")) continue;
        Bytes mdia = Child(track.bytes, Tag("mdia")), handler = Child(mdia, Tag("hdlr"));
        Bytes tkhd = Child(track.bytes, Tag("tkhd"));
        if (handler.size() < 12 || Be(handler.data() + 8) != Tag("soun") || tkhd.size() < 16) continue;
        size_t idAt = tkhd[0] ? 20 : 12; if (tkhd.size() < idAt + 4) return false;
        track_ = Be(tkhd.data() + idAt);
        Bytes stsd = Path(mdia, {Tag("minf"), Tag("stbl"), Tag("stsd")});
        if (stsd.size() < 8) return false;
        size_t pos = 8; Box entry;
        if (!Next(stsd, pos, entry) || entry.tag != Tag("mp4a") || entry.bytes.size() < 28) return false;
        Bytes config = AacConfig(Child(entry.bytes.subspan(28), Tag("esds")));
        if (config.size() < 2 || config[0] >> 3 != 2) return false;
        unsigned index = (config[0] & 7) << 1 | config[1] >> 7, layout = (config[1] >> 3) & 15;
        if (index >= 13 || !layout || layout > 7) return false;
        config_ = index << 4 | layout; sampleSize_ = 0;
        Bytes mvex = Child(moov, Tag("mvex")); size_t i = 0; Box trex;
        while (Next(mvex, i, trex)) if (trex.tag == Tag("trex") && trex.bytes.size() >= 24 && Be(trex.bytes.data() + 4) == track_)
            sampleSize_ = Be(trex.bytes.data() + 16);
        return true;
    }
    return false;
}
bool HlsAudio::Fragment(std::string_view bytes, std::string& out) {
    Bytes all((const uint8_t*)bytes.data(), bytes.size()); size_t at = 0; Box moof;
    while (Next(all, at, moof)) {
        if (moof.tag != Tag("moof")) continue;
        size_t after = at; Box mdat;
        while (Next(all, after, mdat) && mdat.tag != Tag("mdat")) {}
        if (mdat.tag != Tag("mdat")) return false;
        size_t dataStart = mdat.bytes.data() - all.data(), dataEnd = dataStart + mdat.bytes.size();
        size_t t = 0; Box traf;
        while (Next(moof.bytes, t, traf)) {
            if (traf.tag != Tag("traf")) continue;
            Bytes tfhd = Child(traf.bytes, Tag("tfhd"));
            if (tfhd.size() < 8 || Be(tfhd.data() + 4) != track_) continue;
            unsigned flags = Be(tfhd.data()) & 0xffffff; size_t pos = 8;
            uint64_t base = moof.start; uint32_t size = sampleSize_;
            if (flags & 1) { if (tfhd.size() - pos < 8) return false; base = uint64_t(Be(tfhd.data() + pos)) << 32 | Be(tfhd.data() + pos + 4); pos += 8; }
            if (flags & 2) pos += 4;
            if (flags & 8) pos += 4;
            if (flags & 16) { if (pos > tfhd.size() || tfhd.size() - pos < 4) return false; size = Be(tfhd.data() + pos); pos += 4; }
            if (flags & 32) pos += 4;
            if (pos > tfhd.size()) return false;
            size_t runAt = 0, cursor = dataStart; Box run;
            while (Next(traf.bytes, runAt, run)) {
                if (run.tag != Tag("trun") || run.bytes.size() < 8) continue;
                const auto* p = run.bytes.data(); unsigned f = Be(p) & 0xffffff; uint32_t count = Be(p + 4); pos = 8;
                if (count > 100000) return false;
                if (f & 1) {
                    if (run.bytes.size() - pos < 4) return false;
                    int64_t offset = (int32_t)Be(p + pos); pos += 4;
                    if (base > (uint64_t)INT64_MAX || offset < -(int64_t)base || base + offset > all.size()) return false;
                    cursor = (size_t)(base + offset);
                }
                if (f & 4) pos += 4;
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t n = size;
                    if (f & 256) pos += 4;
                    if (f & 512) { if (pos > run.bytes.size() || run.bytes.size() - pos < 4) return false; n = Be(p + pos); pos += 4; }
                    if (f & 1024) pos += 4;
                    if (f & 2048) pos += 4;
                    if (pos > run.bytes.size() || !n || n > 8184 || cursor < dataStart || cursor > dataEnd || n > dataEnd - cursor) return false;
                    unsigned index = config_ >> 4, layout = config_ & 15, len = n + 7;
                    const uint8_t header[] = {255, 241, (uint8_t)(64 | index << 2 | layout >> 2),
                        (uint8_t)((layout & 3) << 6 | len >> 11), (uint8_t)(len >> 3), (uint8_t)((len & 7) << 5 | 31), 252};
                    out.append((const char*)header, 7); out.append(bytes.substr(cursor, n)); cursor += n;
                }
            }
        }
    }
    return !out.empty();
}
bool HlsAudio::Segment(std::string_view bytes, std::string& out) {
    out.clear();
    if (bytes.size() > (16u << 20)) return false;
    if (!bytes.empty() && (uint8_t)bytes[0] == 0x47) return Transport(bytes, out);
    if (track_) return Fragment(bytes, out);
    while (bytes.starts_with("ID3")) {
        if (bytes.size() < 10) return false;
        const auto* p = (const uint8_t*)bytes.data();
        if ((p[6] | p[7] | p[8] | p[9]) & 128) return false;
        size_t n = 10 + (size_t(p[6]) << 21 | size_t(p[7]) << 14 | size_t(p[8]) << 7 | p[9]) + ((p[5] & 16) ? 10 : 0);
        if (n > bytes.size()) return false;
        bytes.remove_prefix(n);
    }
    if (bytes.size() < 4 || (uint8_t)bytes[0] != 255 || ((uint8_t)bytes[1] & 224) != 224) return false;
    out = bytes; return true;
}

bool PlayHls(std::string url, std::string text, const std::atomic<bool>& cancel,
             const std::function<bool(std::string_view)>& push,
             const std::function<bool(unsigned)>& wait, std::wstring& problem) {
    HlsAudio demux; std::string init, initKey, keyUrl, key; HlsRange initRange;
    std::array<uint8_t, 16> initIv{};
    uint64_t next = 0; bool started = false; unsigned nesting = 0, unchanged = 0;
    while (!cancel) {
        HlsPlaylist list;
        if (!ParseHls(text, url, list, problem)) {
            if (problem.empty()) problem = L"The station returned a malformed HLS playlist.";
            return false;
        }
        if (!list.variant.empty()) {
            if (++nesting > 6) { problem = L"The station's HLS playlists form a loop."; return false; }
            url = list.variant;
            if (!Fetch(url, {}, cancel, text, &url)) { problem = L"Couldn't load the station's HLS playlist."; return false; }
            continue;
        }
        if (!started && !list.segments.empty()) {
            size_t first = list.ended || list.segments.size() <= 3 ? 0 : list.segments.size() - 3;
            next = list.segments[first].sequence; started = true;
        }
        bool advanced = false;
        for (const auto& s : list.segments) {
            if (s.sequence < next) continue;
            if (s.gap) { next = s.sequence + 1; demux.Discontinuity(); continue; }
            if (s.discontinuity) demux.Discontinuity();
            if (!s.init.empty() && (s.init != init || s.initKey != initKey || s.initIv != initIv ||
                                   s.initRange.offset != initRange.offset || s.initRange.size != initRange.size)) {
                std::string bytes;
                bool initialized = Fetch(s.init, s.initRange, cancel, bytes);
                if (initialized && !s.initKey.empty()) {
                    if (s.initKey != keyUrl) { initialized = Fetch(s.initKey, {}, cancel, key); keyUrl = s.initKey; }
                    if (initialized) initialized = DecryptRadioAes(bytes, key, s.initIv);
                }
                if (!initialized || !demux.Init(bytes)) {
                    problem = L"Couldn't read the HLS audio initialization data."; return false;
                }
                init = s.init; initRange = s.initRange; initKey = s.initKey; initIv = s.initIv;
            }
            std::string bytes; bool fetched = false;
            for (unsigned attempt = 0; attempt < 3 && !cancel; ++attempt) {
                if (Fetch(s.url, s.range, cancel, bytes)) { fetched = true; break; }
                if (!wait(250 * (attempt + 1))) return false;
            }
            if (!fetched) { problem = L"Couldn't load the next HLS audio segment. Select the station to retry."; return false; }
            if (!s.key.empty()) {
                if (s.key != keyUrl) {
                    if (!Fetch(s.key, {}, cancel, key) || key.size() != 16) { problem = L"Couldn't load the HLS audio key."; return false; }
                    keyUrl = s.key;
                }
                auto iv = s.iv;
                if (!s.explicitIv) for (unsigned i = 0; i < 8; ++i) iv[15 - i] = (uint8_t)(s.sequence >> (i * 8));
                if (!DecryptRadioAes(bytes, key, iv)) { problem = L"Couldn't decrypt the HLS audio segment."; return false; }
            }
            std::string audio;
            if (!demux.Segment(bytes, audio)) { problem = L"This HLS station's audio format is unsupported or damaged."; return false; }
            if (!audio.empty() && !push(audio)) return false;
            next = s.sequence + 1; advanced = true;
        }
        if (list.ended) return true;
        unchanged = advanced ? 0 : unchanged + 1;
        if (unchanged > 12) { problem = L"The station stopped updating its HLS playlist."; return false; }
        unsigned delay = std::clamp(list.target * (advanced ? 1000u : 500u), 500u, 30000u);
        if (!wait(delay)) return false;
        if (!Fetch(url, {}, cancel, text, &url)) { problem = L"Couldn't refresh the station's HLS playlist."; return false; }
    }
    return false;
}
}  // namespace radio
