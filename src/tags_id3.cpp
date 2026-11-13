// The tag reader's ID3 family (tags_read.h): ID3v2 (2.2 to 2.4, read whole or streamed), ID3v1 and APEv2
// tags, and the technical details of MPEG audio (from the first frame and a Xing, Info or VBRI header) and
// Musepack (SV7 and SV8 stream headers).
#include "tags_read.h"

#include <cstring>

namespace tagread {

static std::string Id3Text(int enc, sv s) {
    std::string r;
    switch (enc) {
        case 0: r = Latin1ToUtf8(s); break;
        case 1: {
            bool be = s.size() >= 2 && U8(s, 0) == 0xFE && U8(s, 1) == 0xFF;
            bool bom = s.size() >= 2 && ((U8(s, 0) == 0xFF && U8(s, 1) == 0xFE) || be);
            r = Utf16ToUtf8(bom ? s.substr(2) : s, be);
            break;
        }
        case 2: r = Utf16ToUtf8(s, true); break;
        default: r = std::string(s); break;
    }
    // ID3v2.4 multiple values are NUL separated.
    std::string out;
    size_t start = 0;
    for (size_t i = 0; i <= r.size(); ++i) {
        if (i == r.size() || r[i] == '\0') {
            AppendValue(out, Trim(r.substr(start, i - start)));
            start = i + 1;
        }
    }
    return out;
}

// Splits "terminated string, rest" according to the ID3 text encoding.
static sv Id3Terminated(int enc, sv s, sv& rest) {
    if (enc == 1 || enc == 2) {
        for (size_t i = 0; i + 1 < s.size(); i += 2)
            if (s[i] == 0 && s[i + 1] == 0) {
                rest = s.substr(i + 2);
                return s.substr(0, i);
            }
    } else {
        size_t z = s.find('\0');
        if (z != sv::npos) {
            rest = s.substr(z + 1);
            return s.substr(0, z);
        }
    }
    rest = sv();
    return s;
}

// One registry describes both the frame filter and its decoder, including ID3v2.2 aliases.
namespace {
enum class Id3Kind { Text, Genre, Date, Track, Disc, Bpm, Compilation, User, Comment, Lyrics, Picture, Chapter };
struct Id3Frame {
    const char* id;
    const char* legacy;
    Id3Kind kind;
    std::string TagData::*field = nullptr;
};
}  // namespace

static const Id3Frame* FindId3Frame(sv id) {
    static const Id3Frame frames[] = {
        {"TIT2", "TT2", Id3Kind::Text, &TagData::title}, {"TPE1", "TP1", Id3Kind::Text, &TagData::artist},
        {"TALB", "TAL", Id3Kind::Text, &TagData::album}, {"TPE2", "TP2", Id3Kind::Text, &TagData::albumArtist},
        {"TCOM", "TCM", Id3Kind::Text, &TagData::composer}, {"TIT1", "TT1", Id3Kind::Text, &TagData::grouping},
        {"TPUB", "TPB", Id3Kind::Text, &TagData::label}, {"TCON", "TCO", Id3Kind::Genre},
        {"TRCK", "TRK", Id3Kind::Track}, {"TPOS", "TPA", Id3Kind::Disc}, {"TBPM", "TBP", Id3Kind::Bpm},
        {"TCMP", "TCP", Id3Kind::Compilation}, {"TDRC", "TDRC", Id3Kind::Date}, {"TYER", "TYE", Id3Kind::Date},
        {"TXXX", "TXX", Id3Kind::User}, {"COMM", "COM", Id3Kind::Comment}, {"USLT", "ULT", Id3Kind::Lyrics},
        {"APIC", "PIC", Id3Kind::Picture}, {"CHAP", "CHAP", Id3Kind::Chapter},
    };
    for (const auto& frame : frames)
        if (id == frame.id || id == frame.legacy) return &frame;
    return nullptr;
}

static bool WantsId3Frame(sv id, const ArtSink& art) {
    const Id3Frame* frame = FindId3Frame(id);
    if (!frame) return false;
    switch (frame->kind) {
        case Id3Kind::Picture: return art.WantsImage();
        case Id3Kind::Lyrics: return art.lyrics != nullptr;
        case Id3Kind::Chapter: return art.chapters != nullptr;
        default: return true;
    }
}

static void ParseId3Frame(int version, sv id, uint16_t fflags, sv f, TagData& t, ArtSink& art) {
    const Id3Frame* frame = FindId3Frame(id);
    if (!frame) return;
    if (version == 4 && (fflags & 0x0001)) f = Sub(f, 4);   // data length indicator
    if (fflags & (version == 4 ? 0x000C : 0x00C0)) return;  // compressed/encrypted
    if (f.empty()) return;
    if (frame->kind == Id3Kind::Chapter) {
        // Element ID, start and end (ms), start and end byte offsets, then embedded frames (TIT2).
        if (!art.chapters) return;
        sv rest;
        Id3Terminated(0, f, rest);
        if (rest.size() < 16) return;
        Chapter c;
        c.start = BE32(rest, 0) / 1000.0;
        sv frames = rest.substr(16);
        for (size_t p = 0; p + 10 <= frames.size();) {
            sv sid = frames.substr(p, 4);
            size_t len = version == 4 ? Syncsafe(frames, p + 4) : BE32(frames, p + 4);
            p += 10;
            if (len == 0 || p + len > frames.size()) break;
            if (sid == "TIT2" && len > 1) c.title = Id3Text(U8(frames, p), frames.substr(p + 1, len - 1));
            p += len;
        }
        (*art.chapters)[(int)art.chapters->size()] = c;
        return;
    }
    int enc = U8(f, 0);
    auto text = [&] { return Id3Text(enc, f.substr(1)); };
    switch (frame->kind) {
        case Id3Kind::Text: t.*frame->field = text(); break;
        case Id3Kind::Genre: t.genre = CleanGenre(text()); break;
        case Id3Kind::Date: SetIfEmpty(t.date, text().substr(0, 10)); break;
        case Id3Kind::Track: ReadNumberPair(text(), t.track, t.trackTotal); break;
        case Id3Kind::Disc: ReadNumberPair(text(), t.disc, t.discTotal); break;
        case Id3Kind::Bpm: t.bpm = (uint32_t)(atof(text().c_str()) + 0.5); break;
        case Id3Kind::Compilation: t.compilation = (text() == "1"); break;
        case Id3Kind::User: {
            sv rest;
            sv desc = Id3Terminated(enc, f.substr(1), rest);
            ApplyField(t, Id3Text(enc, desc), Id3Text(enc, rest), false, art.lyrics);
            break;
        }
        case Id3Kind::Comment:
        case Id3Kind::Lyrics: {
            // Encoding (1), language (3), description, then the comment/lyrics. Ignore named descriptions.
            sv value;
            sv desc = Id3Terminated(enc, Sub(f, 4), value);
            std::string* target = frame->kind == Id3Kind::Comment ? &t.comment : art.lyrics;
            if (Id3Text(enc, desc).empty() && target) *target = Id3Text(enc, value);
            break;
        }
        case Id3Kind::Picture: {
            if (!art.image) break;
            sv rest = f.substr(1);
            if (id == "APIC") {
                size_t z = rest.find('\0');
                if (z == sv::npos) return;
                rest = rest.substr(z + 1);
            } else {
                rest = Sub(rest, 3);  // image format
            }
            int type = U8(rest, 0);
            sv data;
            Id3Terminated(enc, Sub(rest, 1), data);
            art.Offer(type, data);
            break;
        }
        default: break;  // chapters were handled before the text encoding byte
    }
}

void ParseId3v2(sv tag, TagData& t, ArtSink& art) {
    if (tag.size() < 10 || tag.substr(0, 3) != "ID3") return;
    int version = U8(tag, 3);
    uint8_t flags = U8(tag, 5);
    size_t size = Syncsafe(tag, 6);
    sv b = Sub(tag, 10, size);
    std::string clean;
    if ((flags & 0x80) && version < 4) {  // unsynchronisation
        clean.reserve(b.size());
        for (size_t i = 0; i < b.size(); ++i) {
            clean += b[i];
            if ((uint8_t)b[i] == 0xFF && i + 1 < b.size() && b[i + 1] == 0) ++i;
        }
        b = clean;
    }
    size_t p = 0;
    if (flags & 0x40) p += version == 4 ? Syncsafe(b, 0) : BE32(b, 0) + 4;  // extended header
    const size_t idLen = version == 2 ? 3 : 4, headLen = version == 2 ? 6 : 10;
    while (p + headLen <= b.size()) {
        sv id = b.substr(p, idLen);
        if (id[0] == 0) break;
        size_t len = version == 2 ? BE24(b, p + 3) : version == 4 ? Syncsafe(b, p + 4) : BE32(b, p + 4);
        uint16_t fflags = version == 2 ? 0 : (uint16_t)BE16(b, p + 8);
        p += headLen;
        if (len == 0 || p + len > b.size()) break;
        sv f = b.substr(p, len);
        p += len;
        if (WantsId3Frame(id, art)) ParseId3Frame(version, id, fflags, f, t, art);
    }
}

// A tag of up to 64 KiB is read in one go and parsed in memory. A larger one is streamed frame by frame
// straight from the file instead, reading only the frames wanted: an APIC frame can make an ID3v2 tag
// tens of megabytes. Streaming costs two reads a frame, so a scan of ordinary tags (a few kilobytes of
// text and padding) made some forty reads a file where it now makes one.
void ParseId3v2File(const File& file, uint64_t tagPos, TagData& t, ArtSink& art) {
    constexpr size_t kWhole = 64 * 1024;
    std::string h = file.Read(tagPos, 10);
    if (h.size() < 10 || h.compare(0, 3, "ID3") != 0) return;
    int version = U8(h, 3);
    uint8_t flags = U8(h, 5);
    size_t size = Syncsafe(h, 6);
    if (size <= kWhole || ((flags & 0x80) && version < 4)) {  // unsynchronised tags need the whole tag too
        ParseId3v2(file.Read(tagPos, std::min<size_t>(10 + size, 64u << 20)), t, art);
        return;
    }
    uint64_t p = tagPos + 10;
    uint64_t end = std::min<uint64_t>(file.Size(), p + size);
    if (flags & 0x40) {
        std::string ext = file.Read(p, 4);
        if (ext.size() < 4) return;
        p += version == 4 ? Syncsafe(ext, 0) : BE32(ext, 0) + 4;
    }
    const size_t idLen = version == 2 ? 3 : 4, headLen = version == 2 ? 6 : 10;
    while (p + headLen <= end) {
        std::string fh = file.Read(p, headLen);
        if (fh.size() < headLen || fh[0] == 0) break;  // stop at padding without reading it
        sv id = sv(fh).substr(0, idLen);
        size_t len = version == 2 ? BE24(fh, 3) : version == 4 ? Syncsafe(fh, 4) : BE32(fh, 4);
        uint16_t fflags = version == 2 ? 0 : (uint16_t)BE16(fh, 8);
        p += headLen;
        if (len == 0 || len > end - p || len > (64u << 20)) break;
        if (WantsId3Frame(id, art)) {
            std::string body = file.Read(p, len);
            ParseId3Frame(version, id, fflags, body, t, art);
        }
        p += len;
    }
}

static void ParseApe(const File& f, uint64_t end, TagData& t, ArtSink& art) {
    if (end < 32) return;
    std::string foot = f.Read(end - 32, 32);
    if (foot.size() < 32 || foot.compare(0, 8, "APETAGEX") != 0) return;
    uint32_t size = LE32(foot, 12), count = LE32(foot, 16);
    if (size < 32 || size > end || size > (32u << 20)) return;
    std::string items = f.Read(end - size, size - 32);
    sv b = items;
    size_t p = 0;
    for (uint32_t i = 0; i < count && p + 8 < b.size(); ++i) {
        uint32_t len = LE32(b, p), flags = LE32(b, p + 4);
        size_t k = p + 8, z = b.find('\0', k);
        if (z == sv::npos || z + 1 + len > b.size()) break;
        sv key = b.substr(k, z - k), value = b.substr(z + 1, len);
        p = z + 1 + len;
        if ((flags >> 1 & 3) == 1) {  // binary
            if (std::string upper = Upper(key); art.WantsImage() && upper.starts_with("COVER ART")) {
                size_t nz = value.find('\0');
                if (nz != sv::npos) art.Offer(upper == "COVER ART (FRONT)" ? 3 : 0, value.substr(nz + 1));
            }
            continue;
        }
        ApplyField(t, key, Trim(std::string(value)), false, art.lyrics);
    }
}

static void ParseId3v1(sv b, TagData& t) {
    if (b.size() < 128 || b.substr(0, 3) != "TAG") return;
    auto field = [&](size_t off, size_t n) { return Trim(Latin1ToUtf8(Sub(b, off, n).substr(0, Sub(b, off, n).find('\0')))); };
    SetIfEmpty(t.title, field(3, 30));
    SetIfEmpty(t.artist, field(33, 30));
    SetIfEmpty(t.album, field(63, 30));
    SetIfEmpty(t.date, field(93, 4));
    if (!t.track && U8(b, 125) == 0 && U8(b, 126)) t.track = U8(b, 126);
    if (t.genre.empty()) t.genre = GenreFromId(U8(b, 127));
}

// --- MPEG audio and Musepack ------------------------------------------------------------------------

static bool ScanMpegHead(sv b, bool complete, uint64_t start, uint64_t audioEnd, TagData& t);

// The first MPEG frame after the tags. Most files have it within 64 KiB; a rip with an undersized
// second ID3 tag can put it about 300 KB in, so the wider window is read only when the first misses.
static bool ParseMpegAudio(const File& f, uint64_t start, uint64_t audioEnd, TagData& t) {
    constexpr size_t kNear = 64 * 1024, kFar = 2u << 20;
    std::string head = f.Read(start, kNear);
    if (ScanMpegHead(head, head.size() < kNear, start, audioEnd, t)) return true;
    if (head.size() < kNear) return false;
    head = f.Read(start, kFar);
    return ScanMpegHead(head, head.size() < kFar, start, audioEnd, t);
}

static bool ScanMpegHead(sv b, bool complete, uint64_t start, uint64_t audioEnd, TagData& t) {
    static const int kBitrates[2][3][15] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};
    static const int kRates[3] = {44100, 48000, 32000};
    for (size_t i = 0; i + 4 <= b.size(); ++i) {
        if (U8(b, i) != 0xFF || (U8(b, i + 1) & 0xE0) != 0xE0) continue;
        int ver = U8(b, i + 1) >> 3 & 3, layer = U8(b, i + 1) >> 1 & 3;
        int bri = U8(b, i + 2) >> 4, sri = U8(b, i + 2) >> 2 & 3;
        if (ver == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) continue;
        int lsf = ver == 3 ? 0 : 1, li = 3 - layer;  // li: 0 = Layer I
        int rate = kRates[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
        int kbps = kBitrates[lsf][li][bri];
        bool mono = (U8(b, i + 3) >> 6) == 3;
        int spf = li == 0 ? 384 : (li == 2 && lsf) ? 576 : 1152;
        // A sync pattern inside tag data can look like a header, so the next frame must sync too,
        // unless the window ends before it (then the next, wider window decides).
        size_t frameLen = li == 0 ? (12 * kbps * 1000 / rate + (U8(b, i + 2) >> 1 & 1)) * 4
                                  : (lsf && li == 2 ? 72 : 144) * kbps * 1000 / rate + (U8(b, i + 2) >> 1 & 1);
        if (i + frameLen + 4 <= b.size()) {
            if (U8(b, i + frameLen) != 0xFF || (U8(b, i + frameLen + 1) & 0xE0) != 0xE0) continue;
        } else if (!complete) {
            continue;
        }
        t.codec = li == 2 ? Codec::MP3 : Codec::MP2;
        t.sampleRate = (uint32_t)rate;
        t.channels = mono ? 1 : 2;
        uint64_t audioBytes = audioEnd > start + i ? audioEnd - (start + i) : 0;
        // Xing/Info or VBRI frame count.
        size_t side = ver == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        size_t x = i + 4 + side;
        uint32_t frames = 0;
        if (Sub(b, x, 4) == "Xing" || Sub(b, x, 4) == "Info") {
            if (BE32(b, x + 4) & 1) frames = BE32(b, x + 8);
        } else if (Sub(b, i + 36, 4) == "VBRI") {
            frames = BE32(b, i + 36 + 14);
        }
        if (frames) {
            t.duration = (double)frames * spf / rate;
            t.bitrate = t.duration > 0 ? Kbps(audioBytes, t.duration) : (uint32_t)kbps;
        } else {
            t.bitrate = (uint32_t)kbps;
            t.duration = (double)audioBytes * 8 / (kbps * 1000.0);
        }
        return true;
    }
    return false;
}

// Musepack: the stream header's rate, channels and length, and the APEv2 tag at the end (with an
// ID3v1 tag after it, as some taggers leave).
bool ReadMusepack(const File& f, TagData& t, ArtSink& art) {
    static constexpr uint32_t rates[4] = {44100, 48000, 37800, 32000};
    t.codec = Codec::Musepack;
    uint64_t samples = 0;
    std::string h = f.Read(0, 32);
    if (h.compare(0, 3, "MP+") == 0 && h.size() >= 24) {
        uint32_t frames = LE32(h, 4), flags = LE32(h, 8), gapless = LE32(h, 20), last = (gapless >> 20) & 2047;
        t.sampleRate = rates[(flags >> 16) & 3];
        t.channels = 2;
        samples = frames ? (gapless >> 31) && last ? (uint64_t)(frames - 1) * 1152 + last : (uint64_t)frames * 1152 : 0;
    } else {
        // SV8: the first packets, for the stream header (SH): sizes and counts are 7-bit varints.
        for (uint64_t at = 4; at < f.Size() && at < 65536 && !t.sampleRate;) {
            std::string p = f.Read(at, 64);
            if (p.size() < 3) break;
            uint64_t length = 0;
            size_t i = 2;
            for (; i < p.size() && i < 11; ++i) {
                length = (length << 7) | (U8(p, i) & 127);
                if (!(U8(p, i) & 128)) { ++i; break; }
            }
            if (length < i) break;
            if (p.compare(0, 2, "SH") == 0) {
                size_t j = i + 5;  // past the CRC and the stream version
                auto number = [&] {
                    uint64_t v = 0;
                    for (int n = 0; n < 9 && j < p.size(); ++n) {
                        uint8_t byte = U8(p, j++);
                        v = (v << 7) | (byte & 127);
                        if (!(byte & 128)) break;
                    }
                    return v;
                };
                samples = number();
                uint64_t silence = number();
                samples = samples > silence ? samples - silence : 0;
                t.sampleRate = rates[(U8(p, j) >> 5) & 3];
                t.channels = ((U8(p, j + 1) >> 4) & 15) + 1;
            }
            at += length;
        }
    }
    if (!t.sampleRate) return false;
    t.duration = (double)samples / t.sampleRate;
    uint64_t end = f.Size();
    std::string tail = f.Read(end >= 128 ? end - 128 : 0, 128);
    if (tail.size() == 128 && tail.compare(0, 3, "TAG") == 0) end -= 128;
    ParseApe(f, end, t, art);
    if (t.duration > 0) t.bitrate = Kbps(f.Size(), t.duration);
    return true;
}

bool ReadMpeg(const File& f, TagData& t, ArtSink& art) {
    uint64_t start = 0, end = f.Size();
    std::string h = f.Read(0, 10);
    bool hasId3 = h.size() == 10 && h.compare(0, 3, "ID3") == 0;
    if (hasId3) start = 10 + Syncsafe(h, 6) + ((U8(h, 5) & 0x10) ? 10 : 0);
    // APEv2 first, then ID3v2 on top: ID3v2 is what most taggers (and cordinal) write, so it
    // wins where both have a field. ID3v1 only fills gaps.
    std::string tail = f.Read(end >= 128 ? end - 128 : 0, 128);
    if (tail.size() == 128 && tail.compare(0, 3, "TAG") == 0) {
        TagData v1;
        ParseId3v1(tail, v1);
        ParseApe(f, end - 128, t, art);
        if (hasId3) ParseId3v2File(f, 0, t, art);
        SetIfEmpty(t.title, v1.title);
        SetIfEmpty(t.artist, v1.artist);
        SetIfEmpty(t.album, v1.album);
        SetIfEmpty(t.date, v1.date);
        SetIfEmpty(t.genre, v1.genre);
        if (!t.track) t.track = v1.track;
        end -= 128;
    } else {
        ParseApe(f, end, t, art);
        if (hasId3) ParseId3v2File(f, 0, t, art);
    }
    return ParseMpegAudio(f, start, end, t);
}

}  // namespace tagread
