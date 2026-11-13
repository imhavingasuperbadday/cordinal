// The tag reader's MP4 and ASF (tags_read.h): the atoms of MP4, M4A and QuickTime files (ilst items, mdta
// keys and each track's codec and length) and the header objects of WMA and WMV.
#include "tags_read.h"

#include <vector>

namespace tagread {

// --- MP4 / M4A / MOV -------------------------------------------------------------------------------

namespace {

struct Mp4Reader {
    TagData& t;
    ArtSink& art;
    uint32_t movieScale = 0;
    uint64_t movieDuration = 0;
    bool sawAudio = false;
    std::vector<std::string> keys;  // mdta key names, indexed by an ilst item's numeric type

    // ISO mdta metadata names its fields in a 'keys' box and refers to them by index, as iPhone
    // and some QuickTime/ffmpeg files do instead of the ©nam/©ART atoms.
    void Keys(sv c) {
        if (c.size() < 8) return;
        uint32_t count = BE32(c, 4);
        size_t p = 8;
        for (uint32_t i = 0; i < count && p + 8 <= c.size(); ++i) {
            uint32_t len = BE32(c, p);
            if (len < 8 || p + len > c.size()) break;
            keys.emplace_back(Trim(std::string(c.substr(p + 8, len - 8))));
            p += len;
        }
    }

    void Walk(sv b, int depth) {
        if (depth > 64) return;
        size_t p = 0;
        while (p + 8 <= b.size()) {
            uint64_t size = BE32(b, p);
            size_t hl = 8;
            if (size == 1) { size = BE64(b, p + 8); hl = 16; }
            else if (size == 0) size = b.size() - p;
            if (size < hl || size > b.size() - p) break;
            sv type = b.substr(p + 4, 4);
            sv c = b.substr(p + hl, (size_t)size - hl);
            p += (size_t)size;
            if (type == "mvhd") {
                bool v1 = U8(c, 0) == 1;
                movieScale = BE32(c, v1 ? 20 : 12);
                movieDuration = v1 ? BE64(c, 24) : BE32(c, 16);
            } else if (type == "trak") {
                Track(c);
            } else if (type == "keys") {
                Keys(c);
            } else if (type == "udta" || type == "ilst") {
                Walk(c, depth + 1);
            } else if (type == "meta") {
                // ISO meta is a full box (4 bytes of version/flags); QuickTime's is not.
                Walk(BE32(c, 0) == 0 ? Sub(c, 4) : c, depth + 1);
            } else if (depth > 0 && b.size() >= 8) {
                Item(type, c);
            }
        }
    }

    void Track(sv trak) {
        sv hdlr, stsd;
        uint32_t scale = 0;
        uint64_t duration = 0;
        Find(trak, [&](sv type, sv c) {
            if (type == "hdlr") hdlr = c;
            else if (type == "stsd") stsd = c;
            else if (type == "mdhd") {
                bool v1 = U8(c, 0) == 1;
                scale = BE32(c, v1 ? 20 : 12);
                duration = v1 ? BE64(c, 24) : BE32(c, 16);
            }
        });
        sv handler = Sub(hdlr, 8, 4);
        sv entry = Sub(stsd, 8);
        sv format = Sub(entry, 4, 4);
        if (handler == "soun" && !sawAudio) {
            sawAudio = true;
            t.channels = BE16(entry, 24);
            t.sampleRate = BE32(entry, 32) >> 16;
            if (format == "mp4a") t.codec = t.video ? t.codec : Codec::AAC;
            else if (format == "alac") t.codec = t.video ? t.codec : Codec::ALAC;
            else if (format == "ac-3" || format == "ec-3") t.codec = t.video ? t.codec : Codec::AC3;
            else if (format == "Opus") t.codec = t.video ? t.codec : Codec::Opus;
            else if (format == "fLaC") t.codec = t.video ? t.codec : Codec::FLAC;
            if (scale && duration && t.duration <= 0) t.duration = (double)duration / scale;
        } else if (handler == "vide") {
            t.video = true;
            if (format == "avc1" || format == "avc3") t.codec = Codec::H264;
            else if (format == "hvc1" || format == "hev1") t.codec = Codec::HEVC;
            else if (format == "av01") t.codec = Codec::AV1;
            else if (format == "vp09") t.codec = Codec::VP9;
            else if (format == "mp4v") t.codec = Codec::MPEG4;
            else t.codec = Codec::Video;
        }
    }

    template <class F>
    static void Find(sv b, F&& f, int depth = 0) {
        if (depth > 64) return;
        size_t p = 0;
        while (p + 8 <= b.size()) {
            uint64_t size = BE32(b, p);
            size_t hl = 8;
            if (size == 1) { size = BE64(b, p + 8); hl = 16; }
            if (size < hl || size > b.size() - p) break;
            sv type = b.substr(p + 4, 4);
            sv c = b.substr(p + hl, (size_t)size - hl);
            f(type, c);
            if (type == "mdia" || type == "minf" || type == "stbl") Find(c, f, depth + 1);
            p += (size_t)size;
        }
    }

    void Item(sv type, sv c) {
        // An ilst item holds one or more "data" atoms (and "mean"/"name" for freeform).
        std::string name;
        sv data;
        uint32_t dataType = 0;
        size_t p = 0;
        while (p + 8 <= c.size()) {
            uint32_t len = BE32(c, p);
            if (len < 8 || p + len > c.size()) break;
            sv atom = c.substr(p + 4, 4);
            if (atom == "name" && len > 12) name.assign(c.substr(p + 12, len - 12));
            else if (atom == "data" && len >= 16 && data.empty()) {
                dataType = BE32(c, p + 8) & 0xFFFFFF;
                data = c.substr(p + 16, len - 16);
            }
            p += len;
        }
        if (data.empty() && type != "----") return;
        // A data atom declares its text encoding: 2 is UTF-16BE (common in iPhone/QuickTime
        // files), 1 is UTF-8, and 0 leaves it to the atom. Reading UTF-16 as raw bytes made every
        // letter a separate NUL-separated byte, which the library showed as a spaced-out name.
        std::string text = dataType == 2 ? Utf16ToUtf8(data, true) : std::string(data);
        if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
        text = Trim(text);
        std::string field(type);
        if (type.size() == 4 && type[0] == 0 && type[1] == 0 && type[2] == 0) {
            uint32_t index = ((uint32_t)U8(type, 0) << 24) | ((uint32_t)U8(type, 1) << 16) |
                             ((uint32_t)U8(type, 2) << 8) | U8(type, 3);
            if (index >= 1 && index <= keys.size()) field = keys[index - 1];
        }
        if (field == "\xA9nam" || field == "com.apple.quicktime.title") t.title = text;
        else if (field == "\xA9" "ART" || field == "com.apple.quicktime.artist") t.artist = text;
        else if (field == "\xA9" "alb" || field == "com.apple.quicktime.album") t.album = text;
        else if (field == "aART" || field == "com.apple.quicktime.albumartist") t.albumArtist = text;
        else if (field == "\xA9" "day" || field == "com.apple.quicktime.creationdate") t.date = text.substr(0, 10);
        else if (field == "\xA9gen" || field == "com.apple.quicktime.genre") t.genre = text;
        else if (field == "\xA9" "wrt" || field == "com.apple.quicktime.composer") t.composer = text;
        else if (field == "\xA9" "cmt" || field == "com.apple.quicktime.comment" ||
                 field == "com.apple.quicktime.description") t.comment = text;
        else if (field == "\xA9" "grp") t.grouping = text;
        else if (field == "\xA9" "lyr") { if (art.lyrics) *art.lyrics = text; }
        else if (field == "gnre" && data.size() >= 2) t.genre = GenreFromId((int)BE16(data, 0) - 1);
        else if (field == "trkn" && data.size() >= 4) {
            t.track = BE16(data, 2);
            if (data.size() >= 6) t.trackTotal = BE16(data, 4);
        }
        else if (field == "disk" && data.size() >= 6) {
            t.disc = BE16(data, 2);
            t.discTotal = BE16(data, 4);
        }
        else if (field == "tmpo" && data.size() >= 2) t.bpm = BE16(data, 0);
        else if (field == "cpil" && data.size() >= 1) t.compilation = (U8(data, 0) != 0);
        else if (field == "covr") art.Offer(3, data);
        else if (field == "----" && !name.empty()) ApplyField(t, name, text, false, art.lyrics);
    }
};

}  // namespace

bool ReadMp4(const File& f, TagData& t, ArtSink& art) {
    uint64_t pos = 0, size = f.Size();
    bool sawMoov = false;
    while (pos + 8 <= size) {
        std::string h = f.Read(pos, 16);
        uint64_t box = BE32(h, 0);
        size_t hl = 8;
        if (box == 1) { box = BE64(h, 8); hl = 16; }
        else if (box == 0) box = size - pos;
        if (box < hl || box > size - pos) break;
        if (Sub(h, 4, 4) == "moov" && box < (128u << 20)) {  // h is short if the read failed
            std::string moov = f.Read(pos + hl, (size_t)(box - hl));
            Mp4Reader r{t, art};
            r.Walk(moov, 0);
            if (r.movieScale && r.movieDuration) t.duration = (double)r.movieDuration / r.movieScale;
            sawMoov = true;
        }
        pos += box;
    }
    // A file cut off after its ftyp box used to count as a successful read with no tags, which
    // then replaced good tags and was never retried. Only a moov (where the metadata lives)
    // proves the file could be read.
    if (!sawMoov) return false;
    if (t.duration > 0) t.bitrate = Kbps(size, t.duration);
    if (t.codec == Codec::Unknown && t.video) t.codec = Codec::Video;
    return true;
}

// --- ASF (WMA/WMV) ---------------------------------------------------------------------------------

static bool GuidIs(sv b, size_t at, const char* g) { return Sub(b, at, 16) == sv(g, 16); }
const char kAsfHeader[] = "\x30\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C";
const char kAsfFileProps[] = "\xA1\xDC\xAB\x8C\x47\xA9\xCF\x11\x8E\xE4\x00\xC0\x0C\x20\x53\x65";
const char kAsfStreamProps[] = "\x91\x07\xDC\xB7\xB7\xA9\xCF\x11\x8E\xE6\x00\xC0\x0C\x20\x53\x65";
const char kAsfContent[] = "\x33\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C";
const char kAsfExtContent[] = "\x40\xA4\xD0\xD2\x07\xE3\xD2\x11\x97\xF0\x00\xA0\xC9\x5E\xA8\x50";
const char kAsfAudio[] = "\x40\x9E\x69\xF8\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B";
const char kAsfVideo[] = "\xC0\xEF\x19\xBC\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B";

bool IsAsf(sv head) { return GuidIs(head, 0, kAsfHeader); }

bool ReadAsf(const File& f, TagData& t, ArtSink& art) {
    std::string head = f.Read(0, 30);
    if (!GuidIs(head, 0, kAsfHeader)) return false;
    uint64_t hsize = LE64(head, 16);
    std::string hdr = f.Read(0, (size_t)std::min<uint64_t>(hsize, 64u << 20));
    sv b = hdr;
    t.codec = Codec::WMA;
    size_t p = 30;
    while (p + 24 <= b.size()) {
        uint64_t size = LE64(b, p + 16);
        if (size < 24 || size > b.size() - p) break;
        sv o = b.substr(p + 24, (size_t)size - 24);
        if (GuidIs(b, p, kAsfFileProps)) {
            double dur = LE64(o, 40) / 1e7 - LE64(o, 56) / 1000.0;
            if (dur > 0) t.duration = dur;
        } else if (GuidIs(b, p, kAsfStreamProps)) {
            if (GuidIs(o, 0, kAsfAudio) && !t.sampleRate) {
                t.channels = LE16(o, 54 + 2);
                t.sampleRate = LE32(o, 54 + 4);
                t.bitrate = LE32(o, 54 + 8) * 8 / 1000;
            } else if (GuidIs(o, 0, kAsfVideo)) {
                t.video = true;
                t.codec = Codec::WMV;
            }
        } else if (GuidIs(b, p, kAsfContent)) {
            uint32_t lens[5];
            for (int i = 0; i < 5; ++i) lens[i] = LE16(o, (size_t)i * 2);
            size_t q = 10;
            std::string title = Utf16ToUtf8(Sub(o, q, lens[0]), false);
            q += lens[0];
            std::string author = Utf16ToUtf8(Sub(o, q, lens[1]), false);
            t.title = Trim(title.substr(0, title.find('\0')));
            t.artist = Trim(author.substr(0, author.find('\0')));
        } else if (GuidIs(b, p, kAsfExtContent)) {
            uint32_t count = LE16(o, 0);
            size_t q = 2;
            for (uint32_t i = 0; i < count && q + 6 <= o.size(); ++i) {
                uint32_t nameLen = LE16(o, q);
                std::string name = Utf16ToUtf8(Sub(o, q + 2, nameLen), false);
                name = name.substr(0, name.find('\0'));
                q += 2 + nameLen;
                uint32_t type = LE16(o, q), len = LE16(o, q + 2);
                sv value = Sub(o, q + 4, len);
                q += 4 + len;
                std::string text;
                if (type == 0) text = Utf16ToUtf8(value, false), text = Trim(text.substr(0, text.find('\0')));
                else if (type == 3 || type == 2) text = std::to_string(LE32(value, 0));
                else if (type == 5) text = std::to_string(LE16(value, 0));
                else if (type == 4) text = std::to_string(LE64(value, 0));
                if (type == 1 && art.WantsImage() && Upper(name) == "WM/PICTURE") {
                    int ptype = U8(value, 0);
                    uint32_t dlen = LE32(value, 1);
                    size_t r = 5;
                    for (int s = 0; s < 2; ++s) {  // mime, description (UTF-16, NUL terminated)
                        while (r + 1 < value.size() && (value[r] || value[r + 1])) r += 2;
                        r += 2;
                    }
                    art.Offer(ptype, Sub(value, r, dlen));
                } else if (!text.empty()) {
                    ApplyField(t, name, text, false, art.lyrics);
                }
            }
        }
        p += (size_t)size;
    }
    if (t.duration > 0 && !t.bitrate) t.bitrate = Kbps(f.Size(), t.duration);
    return true;
}

}  // namespace tagread
