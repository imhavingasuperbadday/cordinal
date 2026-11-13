// The tag reader's chunked formats (tags_read.h): RIFF WAV, RF64 and AVI, AIFF and AIFF-C, Sun AU, Apple CAF,
// DSD's DSF and DSDIFF, and Matroska/WebM's EBML header elements.
#include "tags_read.h"

#include <cmath>
#include <cstring>

namespace tagread {

// --- RIFF WAV / AVI, AIFF, AU, CAF -----------------------------------------------------------------

static double Extended80(sv b, size_t i) {
    int exp = (int)(BE16(b, i) & 0x7FFF) - 16383 - 63;
    uint64_t mant = BE64(b, i + 2);
    return std::ldexp((double)mant, exp);
}

bool ReadRiff(const File& f, TagData& t, ArtSink& art) {
    std::string h = f.Read(0, 12);
    bool avi = Sub(h, 8, 4) == "AVI ";  // a file of 4 to 7 bytes has no form type
    uint64_t pos = 12, size = f.Size();
    uint32_t byteRate = 0, factFrames = 0;
    uint64_t dataBytes = 0, rf64Data = 0;
    while (pos + 8 <= size) {
        std::string ch = f.Read(pos, 12);
        if (ch.size() < 8) break;
        uint64_t len = LE32(ch, 4);
        sv id = sv(ch).substr(0, 4);
        if (id == "ds64" && len >= 28) {
            rf64Data = LE64(f.Read(pos + 8, 28), 8);
        } else if (id == "fmt ") {
            std::string fmt = f.Read(pos + 8, 40);
            uint32_t tag = LE16(fmt, 0);
            t.channels = LE16(fmt, 2);
            t.sampleRate = LE32(fmt, 4);
            byteRate = LE32(fmt, 8);
            if (tag == 0xFFFE) tag = Sub(fmt, 24, 4) == sv("\x3a\xc1\xfa\x38", 4) ? 0x10001 : LE16(fmt, 24);  // DFPWM's GUID
            t.codec = tag == 0x55 ? Codec::MP3 : tag == 0x31 ? Codec::GSM : tag == 0x45 ? Codec::G726 : tag == 0x028F ? Codec::G722
                    : tag == 0x10001 ? Codec::DFPWM : Codec::PCM;
        } else if (id == "fact" && len >= 4) {
            factFrames = LE32(f.Read(pos + 8, 4), 0);
        } else if (id == "data") {
            if (len == UINT32_MAX && rf64Data) len = rf64Data;
            dataBytes = len;
        } else if (id == "smpl" && art.loops && len >= 36) {
            // 36-byte header (the loop count is at 28), then 24 bytes per loop: its end is inclusive.
            std::string s = f.Read(pos + 8, 60);
            if (LE32(s, 28) > 0 && s.size() >= 60) {
                art.loops->wav = true;
                art.loops->wavStart = LE32(s, 44);
                art.loops->wavEnd = (int64_t)LE32(s, 48) + 1;
            }
        } else if (id == "LIST" && len < (16u << 20)) {
            std::string list = f.Read(pos + 8, (size_t)len);
            sv l = list;
            if (l.substr(0, 4) == "INFO") {
                for (size_t q = 4; q + 8 <= l.size();) {
                    sv key = l.substr(q, 4);
                    uint32_t n = LE32(l, q + 4);
                    std::string v = Trim(std::string(Sub(l, q + 8, n)).c_str());
                    if (key == "INAM") t.title = v;
                    else if (key == "IART") t.artist = v;
                    else if (key == "IPRD") t.album = v;
                    else if (key == "ICRD") t.date = v.substr(0, 10);
                    else if (key == "IGNR") t.genre = v;
                    else if (key == "ITRK" || key == "IPRT") t.track = (uint32_t)atoi(v.c_str());
                    q += 8 + n + (n & 1);
                }
            } else if (l.substr(0, 4) == "hdrl" && avi) {
                size_t a = list.find("avih");
                if (a != std::string::npos) {
                    uint32_t usPerFrame = LE32(l, a + 8), frames = LE32(l, a + 8 + 16);
                    t.duration = (double)frames * usPerFrame / 1e6;
                }
                t.video = true;
                t.codec = Codec::Video;
            }
        } else if ((id == "id3 " || id == "ID3 ") && len < (64u << 20)) {
            ParseId3v2(f.Read(pos + 8, (size_t)len), t, art);
        }
        pos += 8 + len + (len & 1);
    }
    if (!avi && byteRate) {
        t.duration = (double)dataBytes / byteRate;
        t.bitrate = byteRate * 8 / 1000;
    }
    // A coded stream's fact chunk counts its samples, where a byte rate may be only nominal (DFPWM's).
    if (!avi && factFrames && t.sampleRate && t.codec != Codec::PCM && t.codec != Codec::MP3) {
        t.duration = (double)factFrames / t.sampleRate;
        t.bitrate = Kbps(dataBytes, t.duration);
    }
    return avi || t.sampleRate != 0;
}

bool ReadAiff(const File& f, TagData& t, ArtSink& art) {
    uint64_t pos = 12, size = f.Size();
    t.codec = Codec::PCM;
    while (pos + 8 <= size) {
        std::string ch = f.Read(pos, 8);
        if (ch.size() < 8) break;
        uint64_t len = BE32(ch, 4);
        sv id = sv(ch).substr(0, 4);
        if (id == "COMM") {
            std::string c = f.Read(pos + 8, 22);
            t.channels = BE16(c, 0);
            uint64_t frames = BE32(c, 2), bits = BE16(c, 6);
            // AIFF-C counts IMA4 in packets of 64 frames, 34 bytes a channel.
            if (len >= 22 && f.Read(8, 4) == "AIFC" && Sub(c, 18, 4) == "ima4") { frames *= 64; bits = 34 * 8 / 64; }
            t.sampleRate = (uint32_t)Extended80(c, 8);
            if (t.sampleRate) t.duration = (double)frames / t.sampleRate;
            t.bitrate = (uint32_t)(t.sampleRate * t.channels * bits / 1000);
        } else if ((id == "ID3 " || id == "id3 ") && len < (64u << 20)) {
            ParseId3v2(f.Read(pos + 8, (size_t)len), t, art);
        }
        pos += 8 + len + (len & 1);
    }
    return t.sampleRate != 0;
}

// Sun/NeXT AU (".snd", or "dns." little-endian): a 24-byte header, then an annotation that is
// often a short text, up to the data offset.
bool ReadAu(const File& f, TagData& t) {
    std::string h = f.Read(0, 24);
    if (h.size() < 24) return false;
    const bool be = h.substr(0, 4) == ".snd";
    auto u32 = [&](size_t i) { return be ? BE32(h, i) : LE32(h, i); };
    const uint32_t offset = u32(4), size = u32(8), encoding = u32(12);
    // Bits per sample: PCM, G.711, G.721 (23), G.722 (24, one byte for every two samples), G.723 (25, 26).
    static const uint8_t bits[] = {0, 8, 8, 16, 24, 32, 32, 64};
    const unsigned width = encoding >= 1 && encoding <= 7 ? bits[encoding] : encoding == 27 ? 8 : encoding == 23 ? 4
                         : encoding == 24 ? 4 : encoding == 25 ? 3 : encoding == 26 ? 5 : 0;
    t.codec = encoding == 24 ? Codec::G722 : encoding == 23 || encoding == 25 || encoding == 26 ? Codec::G726 : Codec::PCM;
    t.sampleRate = u32(16);
    t.channels = u32(20);
    if (!t.sampleRate || !t.channels || t.channels > 8 || !width || offset < 24 || offset > f.Size()) return false;
    if (encoding == 24 && t.sampleRate == 8000) t.sampleRate = 16000;  // G.722 headers may give the band rate
    const uint64_t bytes = size == UINT32_MAX ? f.Size() - offset : std::min<uint64_t>(size, f.Size() - offset);
    t.duration = (double)(bytes * 8 / (t.channels * width)) / t.sampleRate;
    t.bitrate = t.sampleRate * t.channels * width / 1000;
    if (offset > 24 && offset <= 4096) {
        std::string note = f.Read(24, offset - 24);
        note = note.substr(0, note.find('\0'));
        bool text = !note.empty();
        for (char c : note) text = text && ((unsigned char)c >= 32 || c == '\n' || c == '\t');
        if (text) t.comment = Trim(Latin1ToUtf8(note));
    }
    return true;
}

// Apple CAF: 'desc' for the format, 'pakt' for the exact length of a packet codec, 'data' for
// the size of a constant-size one, and 'info' for UTF-8 key/value text.
bool ReadCaf(const File& f, TagData& t) {
    uint64_t pos = 8, size = f.Size(), dataBytes = 0, packetBytes = 0, packetFrames = 0, packets = 0, valid = 0;
    int64_t trimmed = -1;
    bool desc = false;
    while (pos + 12 <= size) {
        std::string ch = f.Read(pos, 12);
        if (ch.size() < 12) break;
        uint64_t len = BE64(ch, 4);
        const sv id = sv(ch).substr(0, 4);
        if (id == "data" && len == UINT64_MAX) len = size - pos - 12;
        if (len > size - pos - 12) break;
        if (id == "desc" && len >= 32) {
            std::string d = f.Read(pos + 12, 32);
            const uint64_t bits = BE64(d, 0);
            double rate;
            memcpy(&rate, &bits, 8);
            if (!(rate >= 1 && rate <= 1048575)) return false;
            t.sampleRate = (uint32_t)(rate + 0.5);
            const sv format = Sub(d, 8, 4);
            t.codec = format == "alac" ? Codec::ALAC : format == "lpcm" || format == "ulaw" || format == "alaw" || format == "ima4" ? Codec::PCM : Codec::Unknown;
            packetBytes = BE32(d, 16);
            packetFrames = BE32(d, 20);
            t.channels = BE32(d, 24);
            desc = true;
        } else if (id == "pakt" && len >= 24) {
            std::string d = f.Read(pos + 12, 24);
            packets = BE64(d, 0);
            valid = BE64(d, 8);
            trimmed = (int64_t)BE32(d, 16) + BE32(d, 20);
        } else if (id == "data" && len >= 4) {
            dataBytes = len - 4;
        } else if (id == "info" && len >= 4 && len < (1u << 20)) {
            std::string d = f.Read(pos + 12, (size_t)len);
            size_t at = 4;
            auto next = [&] {
                const size_t end = d.find('\0', at);
                std::string s = end == std::string::npos ? std::string() : d.substr(at, end - at);
                at = end == std::string::npos ? d.size() : end + 1;
                return s;
            };
            for (uint32_t i = 0, n = BE32(d, 0); i < n && at < d.size(); ++i) {
                std::string key = next(), value = Trim(next());
                for (char& c : key) c = (char)std::tolower((unsigned char)c);
                if (key == "title") t.title = value;
                else if (key == "artist") t.artist = value;
                else if (key == "album") t.album = value;
                else if (key == "genre") t.genre = value;
                else if (key == "year" || (key == "recorded date" && t.date.empty())) t.date = value;
                else if (key == "composer") t.composer = value;
                else if (key == "comments") t.comment = value;
                else if (key == "track number") {
                    t.track = (uint32_t)std::strtoul(value.c_str(), nullptr, 10);
                    if (const size_t slash = value.find('/'); slash != std::string::npos)
                        t.trackTotal = (uint32_t)std::strtoul(value.c_str() + slash + 1, nullptr, 10);
                }
            }
        }
        pos += 12 + len;
    }
    if (!desc || !t.sampleRate || !t.channels) return false;
    uint64_t frames = 0;
    if (valid) frames = valid;
    else if (packets && packetFrames) frames = packets * packetFrames - std::min<uint64_t>(packets * packetFrames, trimmed < 0 ? 0 : (uint64_t)trimmed);
    else if (packetBytes && packetFrames) frames = dataBytes / packetBytes * packetFrames;
    t.duration = (double)frames / t.sampleRate;
    if (t.duration > 0) t.bitrate = Kbps(dataBytes, t.duration);
    return true;
}

// --- DSD -------------------------------------------------------------------------------------------
// DSF (Sony) and DSDIFF (Philips). The sample rate shown is the DSD bit rate per channel.

bool ReadDsf(const File& f, TagData& t, ArtSink& art) {
    std::string h = f.Read(0, 28 + 52);
    if (h.size() < 80 || h.substr(28, 4) != "fmt ") return false;
    t.codec = Codec::DSD;
    t.channels = LE32(h, 28 + 24);
    t.sampleRate = LE32(h, 28 + 28);
    const uint64_t samples = LE64(h, 28 + 36);
    if (!t.sampleRate || !t.channels) return false;
    t.duration = (double)samples / t.sampleRate;
    t.bitrate = t.sampleRate / 1000 * t.channels;
    if (const uint64_t meta = LE64(h, 20)) ParseId3v2File(f, meta, t, art);
    return true;
}

bool ReadDff(const File& f, TagData& t, ArtSink& art) {
    t.codec = Codec::DSD;
    uint64_t pos = 16, size = f.Size(), bytes = 0;
    while (pos + 12 <= size) {
        std::string ch = f.Read(pos, 12);
        if (ch.size() < 12) break;
        const uint64_t len = BE64(ch, 4);
        sv id = sv(ch).substr(0, 4);
        if (id == "PROP") {  // the properties form: its chunks follow its "SND " type
            pos += 16;
            continue;
        }
        if (id == "FS  ") t.sampleRate = BE32(f.Read(pos + 12, 4), 0);
        else if (id == "CHNL") t.channels = BE16(f.Read(pos + 12, 2), 0);
        else if (id == "DSD ") bytes = len;
        else if ((id == "ID3 " || id == "id3 ") && len < (64u << 20)) ParseId3v2(f.Read(pos + 12, (size_t)len), t, art);
        if (len > size) break;
        pos += 12 + len + (len & 1);
    }
    if (!t.sampleRate || !t.channels) return false;
    t.duration = (double)bytes * 8 / t.channels / t.sampleRate;
    t.bitrate = t.sampleRate / 1000 * t.channels;
    return true;
}

// --- Matroska / WebM -------------------------------------------------------------------------------

namespace {

struct Ebml {
    sv b;
    size_t p = 0;
    static int Width(uint8_t first) {
        for (int w = 1; w <= 8; ++w)
            if (first & (0x80 >> (w - 1))) return w;
        return 0;
    }
    bool Id(uint32_t& id) {
        int w = Width(U8(b, p));
        if (!w || w > 4 || p + w > b.size()) return false;
        id = 0;
        for (int i = 0; i < w; ++i) id = id << 8 | U8(b, p + i);
        p += w;
        return true;
    }
    bool Size(uint64_t& size) {
        int w = Width(U8(b, p));
        if (!w || p + w > b.size()) return false;
        size = U8(b, p) & (0xFF >> w);
        bool unknown = size == (uint64_t)(0xFF >> w);
        for (int i = 1; i < w; ++i) {
            size = size << 8 | U8(b, p + i);
            if (U8(b, p + i) != 0xFF) unknown = false;
        }
        p += w;
        if (unknown) size = ~0ull;
        return true;
    }
};

}  // namespace

static uint64_t EbmlUInt(sv v) {
    uint64_t r = 0;
    for (unsigned char c : v) r = r << 8 | c;
    return r;
}

static double EbmlFloat(sv v) {
    if (v.size() == 4) {
        uint32_t u = (uint32_t)EbmlUInt(v);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    if (v.size() == 8) {
        uint64_t u = EbmlUInt(v);
        double d;
        memcpy(&d, &u, 8);
        return d;
    }
    return 0;
}

static void WalkMatroska(sv b, TagData& t, uint64_t& scale, double& duration, int depth) {
    if (depth > 64) return;
    Ebml e{b};
    int trackType = 0;
    std::string codecId;
    while (e.p < b.size()) {
        uint32_t id;
        uint64_t size;
        if (!e.Id(id) || !e.Size(size)) break;
        size_t len = (size_t)std::min<uint64_t>(size, b.size() - e.p);
        sv v = b.substr(e.p, len);
        switch (id) {
            case 0x18538067:  // Segment
            case 0x1549A966:  // Info
            case 0x1654AE6B:  // Tracks
            case 0xE0:        // Video
            case 0xE1:        // Audio
                WalkMatroska(v, t, scale, duration, depth + 1);
                break;
            case 0xAE: {  // TrackEntry
                WalkMatroska(v, t, scale, duration, depth + 1);
                break;
            }
            case 0x2AD7B1: scale = EbmlUInt(v); break;
            case 0x4489: duration = EbmlFloat(v); break;
            case 0x7BA9: if (t.title.empty()) t.title = std::string(v); break;
            case 0x83: trackType = (int)EbmlUInt(v); break;
            case 0x86: codecId = std::string(v); break;
            case 0xB5: if (!t.sampleRate) t.sampleRate = (uint32_t)EbmlFloat(v); break;
            case 0x9F: if (!t.channels) t.channels = (uint32_t)EbmlUInt(v); break;
            case 0x1F43B675: return;  // Cluster: metadata is over
        }
        if (size == ~0ull) break;
        e.p += len;
    }
    if (trackType == 1 || codecId.rfind("V_", 0) == 0) {
        t.video = true;
        if (codecId == "V_MPEG4/ISO/AVC") t.codec = Codec::H264;
        else if (codecId == "V_MPEGH/ISO/HEVC") t.codec = Codec::HEVC;
        else if (codecId == "V_VP9") t.codec = Codec::VP9;
        else if (codecId == "V_VP8") t.codec = Codec::VP8;
        else if (codecId == "V_AV1") t.codec = Codec::AV1;
        else if (t.codec == Codec::Unknown) t.codec = Codec::Video;
    } else if ((trackType == 2 || codecId.rfind("A_", 0) == 0) && !t.video && t.codec == Codec::Unknown) {
        if (codecId == "A_OPUS") t.codec = Codec::Opus;
        else if (codecId == "A_VORBIS") t.codec = Codec::Vorbis;
        else if (codecId == "A_FLAC") t.codec = Codec::FLAC;
        else if (codecId.rfind("A_AAC", 0) == 0) t.codec = Codec::AAC;
        else if (codecId.rfind("A_AC3", 0) == 0) t.codec = Codec::AC3;
        else if (codecId == "A_MPEG/L3") t.codec = Codec::MP3;
    }
}

bool ReadMatroska(const File& f, TagData& t) {
    std::string head = f.Read(0, 1 << 20);
    uint64_t scale = 1000000;
    double duration = 0;
    Ebml e{head};
    uint32_t id;
    uint64_t size;
    if (!e.Id(id) || id != 0x1A45DFA3 || !e.Size(size) ||
        size > head.size() || e.p > head.size() - (size_t)size) return false;
    e.p += (size_t)size;
    WalkMatroska(sv(head).substr(e.p), t, scale, duration, 0);
    t.duration = duration * scale / 1e9;
    if (t.duration > 0) t.bitrate = Kbps(f.Size(), t.duration);
    return true;
}

}  // namespace tagread
