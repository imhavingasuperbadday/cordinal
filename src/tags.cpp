// Native metadata reader (the implementation behind tags.h). Sniffs the container from its magic
// bytes and parses tags and technical info with bounded random-access reads rather than loading
// whole files, so multi-GB media stays cheap. Output text is always UTF-8.
#include "sort.h"
#include "tags.h"

#include "archive.h"
#include "cord.h"
#include "cue.h"
#include "library.h"
#include "game_music.h"
#include "native_audio.h"
#include "helpers.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string_view>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Byte helpers

using sv = std::string_view;
inline uint8_t U8(sv s, size_t i) { return i < s.size() ? (uint8_t)s[i] : 0; }
inline uint32_t BE16(sv s, size_t i) { return (uint32_t)U8(s, i) << 8 | U8(s, i + 1); }
inline uint32_t BE24(sv s, size_t i) { return BE16(s, i) << 8 | U8(s, i + 2); }
inline uint32_t BE32(sv s, size_t i) { return BE16(s, i) << 16 | BE16(s, i + 2); }
inline uint64_t BE64(sv s, size_t i) { return (uint64_t)BE32(s, i) << 32 | BE32(s, i + 4); }
inline uint32_t LE16(sv s, size_t i) { return U8(s, i) | (uint32_t)U8(s, i + 1) << 8; }
inline uint32_t LE32(sv s, size_t i) { return LE16(s, i) | LE16(s, i + 2) << 16; }
inline uint64_t LE64(sv s, size_t i) { return LE32(s, i) | (uint64_t)LE32(s, i + 4) << 32; }
inline uint32_t Syncsafe(sv s, size_t i) {
    return (U8(s, i) & 0x7F) << 21 | (U8(s, i + 1) & 0x7F) << 14 | (U8(s, i + 2) & 0x7F) << 7 | (U8(s, i + 3) & 0x7F);
}
inline sv Sub(sv s, size_t pos, size_t n = sv::npos) { return pos >= s.size() ? sv() : s.substr(pos, n); }

// Random-access reader over one file. Reads are bounds-checked against the real size and capped
// at 64 MB, so a corrupt length in a tag header can't become a huge allocation.
class File {
public:
    explicit File(const std::wstring& path) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        LARGE_INTEGER sz{};
        if (h_ != INVALID_HANDLE_VALUE && GetFileSizeEx(h_, &sz)) size_ = (uint64_t)sz.QuadPart;
        else if (h_ == INVALID_HANDLE_VALUE && (member_ = ReadArchiveMember(path))) size_ = member_->size();
    }
    ~File() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    bool Ok() const { return h_ != INVALID_HANDLE_VALUE || member_; }
    uint64_t Size() const { return size_; }
    std::string Read(uint64_t offset, size_t n) const {
        if (offset >= size_ || n > (64u << 20)) return {};
        n = (size_t)std::min<uint64_t>(n, size_ - offset);
        if (member_) return member_->substr((size_t)offset, n);
        std::string buf(n, '\0');
        OVERLAPPED ov{};
        ov.Offset = (DWORD)offset;
        ov.OffsetHigh = (DWORD)(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(h_, buf.data(), (DWORD)n, &got, &ov)) got = 0;
        buf.resize(got);
        return buf;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    std::shared_ptr<const std::string> member_;  // a song inside a ZIP (archive.h)
};

// ---------------------------------------------------------------------------
// Text

std::string Latin1ToUtf8(sv s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c < 0x80) out += (char)c;
        else { out += (char)(0xC0 | c >> 6); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::string Utf16ToUtf8(sv s, bool bigEndian) {
    std::wstring w;
    w.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        w += (wchar_t)(bigEndian ? (U8(s, i) << 8 | U8(s, i + 1)) : (U8(s, i) | U8(s, i + 1) << 8));
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), r.data(), n, nullptr, nullptr);
    return r;
}

std::string Trim(std::string s) {
    while (!s.empty() && (unsigned char)s.back() <= ' ') s.pop_back();
    size_t b = 0;
    while (b < s.size() && (unsigned char)s[b] <= ' ' && s[b] != 0) ++b;
    s.erase(0, b);
    return s;
}

std::string Upper(sv s) {
    std::string r(s);
    for (auto& c : r) c = (char)toupper((unsigned char)c);
    return r;
}

// The ID3v1 genre list (0-79, then Winamp's extensions), shared by the numeric genre byte of
// ID3v1, ID3v2 "(17)"-style text, MP4 'gnre' and numeric Vorbis comments.
const char* const kGenres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age",
    "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion",
    "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "Alternative Rock",
    "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy",
    "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro",
    "Musical", "Rock & Roll", "Hard Rock",
};

std::string GenreFromId(int id) { return id >= 0 && id < (int)std::size(kGenres) ? kGenres[id] : std::string(); }

std::string CleanGenre(std::string g) {
    // ID3 "(17)" or "(17)Rock" or "17"
    if (!g.empty() && g[0] == '(') {
        size_t close = g.find(')');
        if (close != std::string::npos) {
            std::string rest = g.substr(close + 1);
            return rest.empty() ? GenreFromId(atoi(g.c_str() + 1)) : rest;
        }
    }
    if (!g.empty() && std::all_of(g.begin(), g.end(), ::isdigit)) return GenreFromId(atoi(g.c_str()));
    return g;
}

void SetIfEmpty(std::string& field, const std::string& value) {
    if (field.empty() && !value.empty()) field = value;
}

void AppendValue(std::string& field, const std::string& value) {
    if (value.empty()) return;
    if (field.empty()) field = value;
    else if (field != value) field += "; " + value;
}

void ReadNumberPair(const std::string& value, uint32_t& number, uint32_t& total) {
    number = (uint32_t)atoi(value.c_str());
    size_t slash = value.find('/');
    if (slash != std::string::npos) total = (uint32_t)atoi(value.c_str() + slash + 1);
}

// Applies a generic KEY=value (Vorbis comment / APE / MP4 freeform / ASF / TXXX).
void ApplyField(TagData& t, sv rawKey, const std::string& value, bool append, std::string* lyrics = nullptr) {
    std::string key = Upper(rawKey);
    float v = (float)atof(value.c_str());
    static const struct { const char* key; std::string TagData::*field; } textFields[] = {
        {"TITLE", &TagData::title}, {"ARTIST", &TagData::artist}, {"AUTHOR", &TagData::artist},
        {"ALBUM", &TagData::album}, {"WM/ALBUMTITLE", &TagData::album},
        {"ALBUMARTIST", &TagData::albumArtist}, {"ALBUM ARTIST", &TagData::albumArtist}, {"WM/ALBUMARTIST", &TagData::albumArtist},
        {"GENRE", &TagData::genre}, {"WM/GENRE", &TagData::genre},
        {"COMPOSER", &TagData::composer}, {"WM/COMPOSER", &TagData::composer},
        {"COMMENT", &TagData::comment}, {"DESCRIPTION", &TagData::comment}, {"WM/COMMENTS", &TagData::comment},
        {"GROUPING", &TagData::grouping}, {"CONTENTGROUP", &TagData::grouping}, {"WM/CONTENTGROUPDESCRIPTION", &TagData::grouping},
        {"LABEL", &TagData::label}, {"PUBLISHER", &TagData::label}, {"ORGANIZATION", &TagData::label}, {"WM/PUBLISHER", &TagData::label},
    };
    for (const auto& field : textFields)
        if (key == field.key) {
            if (append) AppendValue(t.*field.field, value);
            else t.*field.field = value;
            return;
        }
    if (key == "DATE" || key == "YEAR" || key == "WM/YEAR") SetIfEmpty(t.date, value.substr(0, 10));
    else if (key == "TRACKNUMBER" || key == "TRACK" || key == "WM/TRACKNUMBER") {
        ReadNumberPair(value, t.track, t.trackTotal);
    }
    else if (key == "TRACKTOTAL" || key == "TOTALTRACKS") t.trackTotal = (uint32_t)atoi(value.c_str());
    else if (key == "DISCNUMBER" || key == "DISC" || key == "WM/PARTOFSET") {
        ReadNumberPair(value, t.disc, t.discTotal);
    }
    else if (key == "DISCTOTAL" || key == "TOTALDISCS") t.discTotal = (uint32_t)atoi(value.c_str());
    else if (key == "BPM" || key == "WM/BEATSPERMINUTE") t.bpm = (uint32_t)(atof(value.c_str()) + 0.5);
    else if (key == "COMPILATION" || key == "WM/ISCOMPILATION") {
        std::string vstr = Upper(value);
        t.compilation = (vstr == "1" || vstr == "TRUE" || vstr == "YES");
    }
    else if (key == "LYRICS" || key == "UNSYNCEDLYRICS" || key == "WM/LYRICS") { if (lyrics) *lyrics = value; }
    else if (key == "FMPS_RATING") t.rating = value;
    else if (key == "FMPS_PLAYCOUNT") t.playCount = value;
    else if (key == "REPLAYGAIN_TRACK_GAIN") t.rgTrackGain = v;
    else if (key == "REPLAYGAIN_TRACK_PEAK") t.rgTrackPeak = v;
    else if (key == "REPLAYGAIN_ALBUM_GAIN") t.rgAlbumGain = v;
    else if (key == "REPLAYGAIN_ALBUM_PEAK") t.rgAlbumPeak = v;
    // Opus R128 gains are Q7.8 dB relative to -23 LUFS; ReplayGain uses -18 LUFS. A file with
    // both keeps its REPLAYGAIN_* tags: they are the standard, R128 is the Opus-native fallback.
    else if (key == "R128_TRACK_GAIN") {
        t.r128TrackGain = atoi(value.c_str());
        if (std::isnan(t.rgTrackGain)) t.rgTrackGain = (float)t.r128TrackGain / 256.0f + 5.0f;
    } else if (key == "R128_ALBUM_GAIN") {
        t.r128AlbumGain = atoi(value.c_str());
        if (std::isnan(t.rgAlbumGain)) t.rgAlbumGain = (float)t.r128AlbumGain / 256.0f + 5.0f;
    }
}

std::string Base64Decode(sv in) {
    static int8_t table[256];
    static bool init = [] {
        memset(table, -1, sizeof(table));
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[(unsigned char)a[i]] = (int8_t)i;
        return true;
    }();
    (void)init;
    std::string out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : in) {
        if (table[c] < 0) continue;
        acc = acc << 6 | (uint32_t)table[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)(acc >> bits & 0xFF);
        }
    }
    return out;
}

// FLAC METADATA_BLOCK_PICTURE body -> image bytes. Returns the picture type, or -1.
int ParseFlacPicture(sv b, sv& image) {
    size_t p = 4;
    uint32_t mimeLen = BE32(b, p);
    p += 4 + mimeLen;
    uint32_t descLen = BE32(b, p);
    p += 4 + descLen + 16;
    uint32_t len = BE32(b, p);
    p += 4;
    if (p + len > b.size()) return -1;
    image = b.substr(p, len);
    return (int)BE32(b, 0);
}

// Loop points as the file states them; ReadLoopPoints interprets them.
struct LoopTags {
    std::string start, end, length;     // Vorbis comment values (first of each)
    bool wav = false;                   // the first loop of a WAV smpl chunk
    int64_t wavStart = 0, wavEnd = 0;   // frames; wavEnd exclusive
};

struct ArtSink {
    std::string* image = nullptr;  // null when not collecting art
    std::string* lyrics = nullptr;  // null when not collecting lyrics
    LoopTags* loops = nullptr;     // null when not collecting loop points
    std::map<int, Chapter>* chapters = nullptr;  // null when not collecting chapters; keyed by order
    int bestType = -1;
    // Picks the picture to keep: the front cover (type 3) always wins; otherwise the first seen.
    void Offer(int type, sv data) {
        if (!image || data.empty()) return;
        if (bestType == 3) return;  // already have the front cover
        if (bestType < 0 || type == 3) {
            image->assign(data);
            bestType = type < 0 ? 0 : type;
        }
    }
};

// ---------------------------------------------------------------------------
// Vorbis comments, ID3, APE

// One KEY=value comment: loop points, chapters and pictures it may carry, else an ordinary field.
// Vorbis comments and the free-form entries of cordinal's own format both go through here.
void ApplyComment(sv key, sv rawVal, TagData& t, ArtSink& art) {
    std::string ukey = Upper(key);
    if (art.loops) {
        std::string* slot = ukey == "LOOPSTART" ? &art.loops->start : ukey == "LOOPEND" ? &art.loops->end
                          : ukey == "LOOPLENGTH" ? &art.loops->length : nullptr;
        if (slot && slot->empty()) *slot = std::string(rawVal);
    }
    if (art.chapters && ukey.size() >= 10 && ukey.compare(0, 7, "CHAPTER") == 0 && isdigit((unsigned char)ukey[7]) &&
        isdigit((unsigned char)ukey[8]) && isdigit((unsigned char)ukey[9])) {
        int n = atoi(ukey.c_str() + 7);
        std::string value(rawVal);
        if (ukey.size() == 10) {
            int h = 0, m = 0;
            double s = 0;
            if (sscanf_s(value.c_str(), "%d:%d:%lf", &h, &m, &s) == 3) (*art.chapters)[n].start = h * 3600.0 + m * 60.0 + s;
            else (*art.chapters)[n].start = -1;
        } else if (ukey.compare(10, std::string::npos, "NAME") == 0) {
            (*art.chapters)[n].title = Trim(value);
        }
        return;
    }
    if (ukey == "METADATA_BLOCK_PICTURE") {
        if (art.image) {
            std::string pic = Base64Decode(rawVal);
            sv img;
            int type = ParseFlacPicture(pic, img);
            if (type >= 0) art.Offer(type, img);
        }
    } else if (ukey == "COVERART") {
        if (art.image) art.Offer(3, Base64Decode(rawVal));
    } else {
        ApplyField(t, key, std::string(rawVal), true, art.lyrics);
    }
}

void ParseVorbisComments(sv b, TagData& t, ArtSink& art) {
    size_t p = 4 + LE32(b, 0);
    uint32_t count = LE32(b, p);
    p += 4;
    for (uint32_t i = 0; i < count && p + 4 <= b.size(); ++i) {
        uint32_t len = LE32(b, p);
        p += 4;
        if (p + len > b.size()) break;
        sv c = b.substr(p, len);
        p += len;
        size_t eq = c.find('=');
        if (eq == sv::npos) continue;
        ApplyComment(c.substr(0, eq), c.substr(eq + 1), t, art);
    }
}

std::string Id3Text(int enc, sv s) {
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
sv Id3Terminated(int enc, sv s, sv& rest) {
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
enum class Id3Kind { Text, Genre, Date, Track, Disc, Bpm, Compilation, User, Comment, Lyrics, Picture, Chapter };
struct Id3Frame {
    const char* id;
    const char* legacy;
    Id3Kind kind;
    std::string TagData::*field = nullptr;
};
const Id3Frame* FindId3Frame(sv id) {
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

bool WantsId3Frame(sv id, const ArtSink& art) {
    const Id3Frame* frame = FindId3Frame(id);
    if (!frame) return false;
    switch (frame->kind) {
        case Id3Kind::Picture: return art.image != nullptr && art.bestType != 3;
        case Id3Kind::Lyrics: return art.lyrics != nullptr;
        case Id3Kind::Chapter: return art.chapters != nullptr;
        default: return true;
    }
}

void ParseId3Frame(int version, sv id, uint16_t fflags, sv f, TagData& t, ArtSink& art) {
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

// Streams the tag frame by frame straight from the file rather than loading it whole: an APIC
// frame can make an ID3v2 tag tens of megabytes.
void ParseId3v2File(const File& file, uint64_t tagPos, TagData& t, ArtSink& art) {
    std::string h = file.Read(tagPos, 10);
    if (h.size() < 10 || h.compare(0, 3, "ID3") != 0) return;
    int version = U8(h, 3);
    uint8_t flags = U8(h, 5);
    size_t size = Syncsafe(h, 6);
    if ((flags & 0x80) && version < 4) {
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

void ParseApe(const File& f, uint64_t end, TagData& t, ArtSink& art) {
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
            if (Upper(key).rfind("COVER ART", 0) == 0) {
                size_t nz = value.find('\0');
                if (nz != sv::npos) art.Offer(Upper(key) == "COVER ART (FRONT)" ? 3 : 0, value.substr(nz + 1));
            }
            continue;
        }
        ApplyField(t, key, Trim(std::string(value)), false, art.lyrics);
    }
}

void ParseId3v1(sv b, TagData& t) {
    if (b.size() < 128 || b.substr(0, 3) != "TAG") return;
    auto field = [&](size_t off, size_t n) { return Trim(Latin1ToUtf8(Sub(b, off, n).substr(0, Sub(b, off, n).find('\0')))); };
    SetIfEmpty(t.title, field(3, 30));
    SetIfEmpty(t.artist, field(33, 30));
    SetIfEmpty(t.album, field(63, 30));
    SetIfEmpty(t.date, field(93, 4));
    if (!t.track && U8(b, 125) == 0 && U8(b, 126)) t.track = U8(b, 126);
    if (t.genre.empty()) t.genre = GenreFromId(U8(b, 127));
}

// ---------------------------------------------------------------------------
// MP3

bool ParseMpegAudio(const File& f, uint64_t start, uint64_t audioEnd, TagData& t) {
    std::string head = f.Read(start, 64 * 1024);
    sv b = head;
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
            t.bitrate = t.duration > 0 ? (uint32_t)(audioBytes * 8 / t.duration / 1000 + 0.5) : (uint32_t)kbps;
        } else {
            t.bitrate = (uint32_t)kbps;
            t.duration = (double)audioBytes * 8 / (kbps * 1000.0);
        }
        return true;
    }
    return false;
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

// ---------------------------------------------------------------------------
// FLAC

bool ReadFlac(const File& f, uint64_t pos, TagData& t, ArtSink& art) {
    t.codec = Codec::FLAC;
    for (int guard = 0; guard < 256; ++guard) {
        std::string h = f.Read(pos, 4);
        if (h.size() < 4) break;
        bool last = U8(h, 0) & 0x80;
        int type = U8(h, 0) & 0x7F;
        uint32_t len = BE24(h, 1);
        if (type == 0 || type == 4 || (type == 6 && art.image && art.bestType != 3)) {
            std::string body = f.Read(pos + 4, len);
            if (type == 0 && body.size() >= 18) {
                t.sampleRate = BE24(body, 10) >> 4;
                t.channels = ((U8(body, 12) >> 1) & 7) + 1;
                uint64_t total = (uint64_t)(U8(body, 13) & 0x0F) << 32 | BE32(body, 14);
                if (t.sampleRate) t.duration = (double)total / t.sampleRate;
                if (body.size() >= 34) t.audioHash = BE64(body, 18);  // STREAMINFO's MD5 starts at byte 18
            } else if (type == 4) {
                ParseVorbisComments(body, t, art);
            } else if (type == 6) {
                sv img;
                int ptype = ParseFlacPicture(body, img);
                if (ptype >= 0) art.Offer(ptype, img);
            }
        }
        pos += 4 + (uint64_t)len;
        if (last) break;
    }
    if (t.duration > 0) t.bitrate = (uint32_t)(f.Size() * 8 / t.duration / 1000 + 0.5);
    return t.sampleRate != 0;
}

// ---------------------------------------------------------------------------
// cordinal's own format (cord.h): INFO, then the typed and free-form entries of META and the PICT
// pictures, all before INDX.

void ParseCordMeta(sv meta, TagData& t, ArtSink& art) {
    size_t at = 0;
    cord::Entry e;
    while (cord::NextEntry(meta, at, e)) {
        const std::string text(e.value);
        const uint32_t number = e.value.size() == 4 ? LE32(e.value, 0) : 0;
        float gain = NAN;
        if (e.value.size() == 4) memcpy(&gain, e.value.data(), 4);
        if (!std::isfinite(gain)) gain = NAN;
        switch (e.id) {
        case cord::kFreeForm: ApplyComment(e.key, e.value, t, art); break;
        case cord::kTitle: AppendValue(t.title, text); break;
        case cord::kArtist: AppendValue(t.artist, text); break;
        case cord::kAlbum: AppendValue(t.album, text); break;
        case cord::kAlbumArtist: AppendValue(t.albumArtist, text); break;
        case cord::kGenre: AppendValue(t.genre, text); break;
        case cord::kDate: SetIfEmpty(t.date, text.substr(0, 10)); break;
        case cord::kComposer: AppendValue(t.composer, text); break;
        case cord::kComment: AppendValue(t.comment, text); break;
        case cord::kGrouping: AppendValue(t.grouping, text); break;
        case cord::kLabel: AppendValue(t.label, text); break;
        case cord::kLyrics: if (art.lyrics) *art.lyrics = text; break;
        case cord::kTrack: t.track = number; break;
        case cord::kTrackTotal: t.trackTotal = number; break;
        case cord::kDisc: t.disc = number; break;
        case cord::kDiscTotal: t.discTotal = number; break;
        case cord::kBpm: t.bpm = number; break;
        case cord::kCompilation: t.compilation = !e.value.empty() && e.value[0]; break;
        case cord::kTrackGain: t.rgTrackGain = gain; break;
        case cord::kTrackPeak: t.rgTrackPeak = gain; break;
        case cord::kAlbumGain: t.rgAlbumGain = gain; break;
        case cord::kAlbumPeak: t.rgAlbumPeak = gain; break;
        default: break;  // a field a later version added
        }
    }
}

bool ReadCord(const File& f, TagData& t, ArtSink& art) {
    t.codec = Codec::Cord;
    bool info = false, meta = false;
    for (uint64_t at = cord::kHeaderBytes; at + cord::kChunkHeaderBytes <= f.Size();) {
        std::string h = f.Read(at, cord::kChunkHeaderBytes);
        const uint32_t id = LE32(h, 0);
        const uint64_t bytes = LE64(h, 4), body = at + cord::kChunkHeaderBytes;
        if (h.size() < cord::kChunkHeaderBytes || bytes > f.Size() - body || id == cord::kIndex) break;
        if (!info) {
            cord::Info i;
            std::string b = f.Read(body, (size_t)std::min<uint64_t>(bytes, 256));
            if (id != cord::kInfo || !cord::ParseInfo((const uint8_t*)b.data(), b.size(), i)) return false;
            info = true;
            t.sampleRate = i.rate;
            t.channels = i.channels;
            t.duration = (double)i.frames / i.rate;
            t.audioHash = BE64(sv((const char*)i.md5.data(), 16), 0);  // as FLAC's: the MD5's first 8 bytes
        } else if (id == cord::kMeta && !meta) {
            meta = true;
            ParseCordMeta(f.Read(body, (size_t)bytes), t, art);
        } else if (id == cord::kPicture && art.image && art.bestType != 3) {
            std::string b = f.Read(body, (size_t)bytes);
            uint8_t type = 0;
            sv image;
            if (cord::ParsePicture(b, type, image)) art.Offer(type, image);
        }
        at = body + bytes;
    }
    if (t.duration > 0) t.bitrate = (uint32_t)(f.Size() * 8 / t.duration / 1000 + 0.5);
    return info;
}

// ---------------------------------------------------------------------------
// Ogg

bool ReadOgg(const File& f, TagData& t, ArtSink& art) {
    // Collect the first two packets.
    std::string packets[2];
    int index = 0;
    uint64_t pos = 0;
    for (int page = 0; page < 256 && index < 2; ++page) {
        std::string h = f.Read(pos, 27);
        if (h.size() < 27 || h.compare(0, 4, "OggS") != 0) break;
        int segs = U8(h, 26);
        std::string table = f.Read(pos + 27, (size_t)segs);
        size_t bodyLen = 0;
        for (unsigned char s : table) bodyLen += s;
        std::string body = f.Read(pos + 27 + segs, bodyLen);
        size_t off = 0;
        for (int s = 0; s < segs && index < 2; ++s) {
            uint8_t len = U8(table, (size_t)s);
            packets[index].append(Sub(body, off, len));
            off += len;
            if (len < 255) ++index;
        }
        pos += 27 + (uint64_t)segs + bodyLen;
    }
    sv id = packets[0], comments = packets[1];
    uint32_t granuleRate = 0;
    uint64_t preSkip = 0;
    if (id.substr(0, 7) == "\x01vorbis") {
        t.codec = Codec::Vorbis;
        t.channels = U8(id, 11);
        t.sampleRate = granuleRate = LE32(id, 12);
        if (comments.substr(0, 7) == "\x03vorbis") ParseVorbisComments(comments.substr(7), t, art);
    } else if (id.substr(0, 8) == "OpusHead") {
        t.codec = Codec::Opus;
        t.channels = U8(id, 9);
        preSkip = LE16(id, 10);
        t.sampleRate = LE32(id, 12) ? LE32(id, 12) : 48000;
        granuleRate = 48000;
        if (comments.substr(0, 8) == "OpusTags") ParseVorbisComments(comments.substr(8), t, art);
    } else if (id.substr(0, 5) == "\x7F" "FLAC") {
        t.codec = Codec::FLAC;
        // Packet: 0x7F "FLAC", version (2), header count (2), "fLaC", the STREAMINFO block header
        // (4 bytes at 13), then STREAMINFO itself at 17.
        sv info = Sub(id, 17);
        t.sampleRate = granuleRate = BE24(info, 10) >> 4;
        t.channels = ((U8(info, 12) >> 1) & 7) + 1;
        if (info.size() >= 34) t.audioHash = BE64(info, 18);
        if (!comments.empty()) ParseVorbisComments(Sub(comments, 4), t, art);
    } else if (id.substr(0, 7) == "\x80theora") {
        t.codec = Codec::Theora;
        t.video = true;
    } else {
        return false;
    }
    // Duration from the last page's granule position.
    uint64_t size = f.Size();
    std::string tail = f.Read(size > 65536 ? size - 65536 : 0, 65536);
    size_t last = tail.rfind("OggS");
    if (last != std::string::npos && granuleRate) {
        uint64_t granule = LE64(tail, last + 6);
        if (granule != ~0ull && granule > preSkip) t.duration = (double)(granule - preSkip) / granuleRate;
    }
    if (t.duration > 0) t.bitrate = (uint32_t)(size * 8 / t.duration / 1000 + 0.5);
    return true;
}

// ---------------------------------------------------------------------------
// MP4 / M4A / MOV

struct Mp4Reader {
    TagData& t;
    ArtSink& art;
    uint32_t movieScale = 0;
    uint64_t movieDuration = 0;
    bool sawAudio = false;

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
        std::string text(data);
        if (type == "\xA9nam") t.title = text;
        else if (type == "\xA9" "ART") t.artist = text;
        else if (type == "\xA9" "alb") t.album = text;
        else if (type == "aART") t.albumArtist = text;
        else if (type == "\xA9" "day") t.date = text.substr(0, 10);
        else if (type == "\xA9gen") t.genre = text;
        else if (type == "\xA9" "wrt") t.composer = text;
        else if (type == "\xA9" "cmt") t.comment = text;
        else if (type == "\xA9" "grp") t.grouping = text;
        else if (type == "\xA9" "lyr") { if (art.lyrics) *art.lyrics = text; }
        else if (type == "gnre" && data.size() >= 2) t.genre = GenreFromId((int)BE16(data, 0) - 1);
        else if (type == "trkn" && data.size() >= 4) {
            t.track = BE16(data, 2);
            if (data.size() >= 6) t.trackTotal = BE16(data, 4);
        }
        else if (type == "disk" && data.size() >= 6) {
            t.disc = BE16(data, 2);
            t.discTotal = BE16(data, 4);
        }
        else if (type == "tmpo" && data.size() >= 2) t.bpm = BE16(data, 0);
        else if (type == "cpil" && data.size() >= 1) t.compilation = (U8(data, 0) != 0);
        else if (type == "covr") art.Offer(3, data);
        else if (type == "----" && !name.empty()) ApplyField(t, name, Trim(text), false, art.lyrics);
        (void)dataType;
    }
};

bool ReadMp4(const File& f, TagData& t, ArtSink& art) {
    uint64_t pos = 0, size = f.Size();
    bool any = false;
    while (pos + 8 <= size) {
        std::string h = f.Read(pos, 16);
        uint64_t box = BE32(h, 0);
        size_t hl = 8;
        if (box == 1) { box = BE64(h, 8); hl = 16; }
        else if (box == 0) box = size - pos;
        if (box < hl || box > size - pos) break;
        if (h.compare(4, 4, "moov") == 0 && box < (128u << 20)) {
            std::string moov = f.Read(pos + hl, (size_t)(box - hl));
            Mp4Reader r{t, art};
            r.Walk(moov, 0);
            if (r.movieScale && r.movieDuration) t.duration = (double)r.movieDuration / r.movieScale;
            any = true;
        } else if (h.compare(4, 4, "ftyp") == 0) {
            any = true;
        }
        pos += box;
    }
    if (t.duration > 0) t.bitrate = (uint32_t)(size * 8 / t.duration / 1000 + 0.5);
    if (t.codec == Codec::Unknown && t.video) t.codec = Codec::Video;
    return any;
}

// ---------------------------------------------------------------------------
// ASF (WMA/WMV)

bool GuidIs(sv b, size_t at, const char* g) { return Sub(b, at, 16) == sv(g, 16); }
const char kAsfHeader[] = "\x30\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C";
const char kAsfFileProps[] = "\xA1\xDC\xAB\x8C\x47\xA9\xCF\x11\x8E\xE4\x00\xC0\x0C\x20\x53\x65";
const char kAsfStreamProps[] = "\x91\x07\xDC\xB7\xB7\xA9\xCF\x11\x8E\xE6\x00\xC0\x0C\x20\x53\x65";
const char kAsfContent[] = "\x33\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C";
const char kAsfExtContent[] = "\x40\xA4\xD0\xD2\x07\xE3\xD2\x11\x97\xF0\x00\xA0\xC9\x5E\xA8\x50";
const char kAsfAudio[] = "\x40\x9E\x69\xF8\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B";
const char kAsfVideo[] = "\xC0\xEF\x19\xBC\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B";

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
                if (Upper(name) == "WM/PICTURE" && type == 1 && art.image) {
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
    if (t.duration > 0 && !t.bitrate) t.bitrate = (uint32_t)(f.Size() * 8 / t.duration / 1000 + 0.5);
    return true;
}

// ---------------------------------------------------------------------------
// RIFF WAV / AVI, AIFF

double Extended80(sv b, size_t i) {
    int exp = (int)(BE16(b, i) & 0x7FFF) - 16383 - 63;
    uint64_t mant = BE64(b, i + 2);
    return std::ldexp((double)mant, exp);
}

bool ReadRiff(const File& f, TagData& t, ArtSink& art) {
    std::string h = f.Read(0, 12);
    bool avi = h.compare(8, 4, "AVI ") == 0;
    uint64_t pos = 12, size = f.Size();
    uint32_t byteRate = 0;
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
            if (tag == 0xFFFE) tag = LE16(fmt, 24);
            t.codec = tag == 0x55 ? Codec::MP3 : (tag == 1 || tag == 3) ? Codec::PCM : Codec::PCM;
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
            std::string c = f.Read(pos + 8, 18);
            t.channels = BE16(c, 0);
            uint32_t frames = BE32(c, 2), bits = BE16(c, 6);
            t.sampleRate = (uint32_t)Extended80(c, 8);
            if (t.sampleRate) t.duration = (double)frames / t.sampleRate;
            t.bitrate = t.sampleRate * t.channels * bits / 1000;
        } else if ((id == "ID3 " || id == "id3 ") && len < (64u << 20)) {
            ParseId3v2(f.Read(pos + 8, (size_t)len), t, art);
        }
        pos += 8 + len + (len & 1);
    }
    return t.sampleRate != 0;
}

// ---------------------------------------------------------------------------
// DSD: DSF (Sony) and DSDIFF (Philips). The sample rate shown is the DSD bit rate per channel.

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

// ---------------------------------------------------------------------------
// Matroska / WebM

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

uint64_t EbmlUInt(sv v) {
    uint64_t r = 0;
    for (unsigned char c : v) r = r << 8 | c;
    return r;
}

double EbmlFloat(sv v) {
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

void WalkMatroska(sv b, TagData& t, uint64_t& scale, double& duration, int depth) {
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
    if (t.duration > 0) t.bitrate = (uint32_t)(f.Size() * 8 / t.duration / 1000 + 0.5);
    return true;
}

// Dispatches on the file's first bytes. Anything unrecognised falls through to the MPEG parser,
// which scans for a frame sync, so a bare MP3 with no tag still reads.
bool ReadAny(const std::wstring& path, TagData& t, ArtSink& art) {
    File f(path);
    if (!f.Ok()) return false;
    std::string magic = f.Read(0, 12);
    sv m = magic;
    bool ok = false;
    if (m.substr(0, 4) == "fLaC") ok = ReadFlac(f, 4, t, art);
    else if (m.size() >= cord::kHeaderBytes && cord::IsHeader((const uint8_t*)m.data())) ok = ReadCord(f, t, art);
    else if (m.substr(0, 4) == "OggS") ok = ReadOgg(f, t, art);
    else if (m.substr(4, 4) == "ftyp" || m.substr(4, 4) == "moov" || m.substr(4, 4) == "mdat" || m.substr(4, 4) == "wide" || m.substr(4, 4) == "free")
        ok = ReadMp4(f, t, art);
    else if (m.substr(0, 4) == "RIFF" || m.substr(0, 4) == "RF64" || m.substr(0, 4) == "BW64") ok = ReadRiff(f, t, art);
    else if (m.substr(0, 4) == "FORM") ok = ReadAiff(f, t, art);
    else if (m.substr(0, 4) == "DSD ") ok = ReadDsf(f, t, art);
    else if (m.substr(0, 4) == "FRM8") ok = ReadDff(f, t, art);
    else if (GuidIs(m, 0, kAsfHeader) || (magic.size() >= 4 && GuidIs(f.Read(0, 16), 0, kAsfHeader))) ok = ReadAsf(f, t, art);
    else if (m.substr(0, 4) == "\x1A\x45\xDF\xA3") ok = ReadMatroska(f, t);
    else if (m.size() >= 10 && m.substr(0, 3) == "ID3" &&
             f.Read(10 + Syncsafe(m, 6) + ((U8(m, 5) & 0x10) ? 10 : 0), 4) == "fLaC") {
        // FLAC with a (non-standard) leading ID3 tag
        uint64_t skip = 10 + Syncsafe(m, 6) + ((U8(m, 5) & 0x10) ? 10 : 0);
        ParseId3v2File(f, 0, t, art);
        ok = ReadFlac(f, skip + 4, t, art);
    } else ok = ReadMpeg(f, t, art);
    return ok;
}

// A CUE track: the file's technical info, with the sheet's song tags on top. ReplayGain of the
// whole file doesn't apply to one song, so only its album gain is kept.
bool ReadSubtrackTags(const std::wstring& path, TagData& t) {
    CueSubtrack sub;
    if (!LookupSubtrack(path, sub)) return false;
    ArtSink none;
    TagData file;
    bool ok = ReadAny(sub.audioPath, file, none);
    const CueSheet& sheet = *sub.sheet;
    const CueTrack& track = sub.Track();
    t = file;
    t.audioHash = 0;  // the MD5 is of the whole file
    t.rgTrackGain = t.rgTrackPeak = NAN;
    if (!track.title.empty()) t.title = track.title;
    else t.title = track.number < 10 ? "Track 0" + std::to_string(track.number) : "Track " + std::to_string(track.number);
    if (!track.performer.empty()) t.artist = track.performer;
    else if (!sheet.performer.empty()) t.artist = sheet.performer;
    if (!sheet.performer.empty()) t.albumArtist = sheet.performer;
    if (!sheet.title.empty()) t.album = sheet.title;
    if (!sheet.date.empty()) t.date = sheet.date;
    if (!sheet.genre.empty()) t.genre = sheet.genre;
    if (!sheet.comment.empty()) t.comment = sheet.comment;
    if (!track.songwriter.empty()) t.composer = track.songwriter;
    t.track = (uint32_t)track.number;
    t.trackTotal = (uint32_t)sheet.TrackCount();
    if (sheet.discNumber > 0) t.disc = (uint32_t)sheet.discNumber;
    if (sheet.totalDiscs > 0) t.discTotal = (uint32_t)sheet.totalDiscs;
    // The last track runs to the end of the file.
    t.duration = track.end > 0 ? track.end - track.start : std::max(0.0, file.duration - track.start);
    return ok;
}

}  // namespace

const wchar_t* CodecName(Codec c) {
    static const wchar_t* const names[] = {
        L"", L"MP3", L"MP2", L"FLAC", L"AAC", L"ALAC", L"PCM", L"WMA", L"Vorbis", L"Opus", L"AC3",
        L"H.264", L"HEVC", L"VP8", L"VP9", L"AV1", L"MPEG-4", L"MPEG-2", L"WMV", L"Theora", L"Video",
        L"SPC", L"NSF", L"GBS", L"VGM", L"Cord", L"DSD", L"SID", L"Module", L"MIDI",
    };
    size_t i = (size_t)c;
    return i < std::size(names) ? names[i] : L"";
}

bool ReadTags(const std::wstring& path, TagData& out) {
    out = TagData{};
    if (IsGameMusicFile(path)) {  // "file.nsf|3" or a plain file: the emulator reads the song's tags
        std::wstring file;
        int subsong = 1;
        SplitGameMusicPath(path, file, subsong);
        return ReadGameMusicTags(file, subsong, out);
    }
    const std::wstring ext = ExtOf(path);
    if (ext == L"mod" || ext == L"s3m" || ext == L"xm" || ext == L"it") return ReadModuleTags(path, out);
    if (ext == L"mid" || ext == L"midi" || ext == L"kar" || ext == L"rmi") return ReadMidiTags(path, out);
    if (HelperForExtension(ext)) return ReadHelperTags(path, out);  // a file type a helper program plays
    ArtSink none;
    bool ok = IsSubtrack(path) ? ReadSubtrackTags(path, out) : ReadAny(path, out, none);
    if (out.title.empty() && out.artist.empty()) out.title.clear();
    return ok;
}

bool ReadEmbeddedArt(const std::wstring& path, std::string& image) {
    TagData t;
    ArtSink art;
    art.image = &image;
    image.clear();
    ReadAny(AudioFileOf(path), t, art);  // a CUE track's cover is its file's
    return !image.empty();
}

bool ReadLyrics(const std::wstring& path, std::string& lyrics) {
    TagData t;
    ArtSink art;
    art.lyrics = &lyrics;
    lyrics.clear();
    ReadAny(AudioFileOf(path), t, art);
    return !lyrics.empty();
}

bool ReadChapters(const std::wstring& path, std::vector<Chapter>& chapters) {
    chapters.clear();
    TagData t;
    ArtSink art;
    std::map<int, Chapter> found;
    art.chapters = &found;
    ReadAny(AudioFileOf(path), t, art);
    for (auto& [n, c] : found)
        if (c.start >= 0) chapters.push_back(std::move(c));
    SortObjects(chapters.begin(), chapters.end(), [](const Chapter& a, const Chapter& b) { return a.start < b.start; });
    return !chapters.empty();
}

std::wstring ChapterTextAt(const std::vector<Chapter>& chapters, double position) {
    if (chapters.empty()) return {};
    size_t index = 0;
    while (index + 1 < chapters.size() && chapters[index + 1].start <= position) ++index;
    std::wstring title = FromUtf8(chapters[index].title);
    return L"Chapter " + std::to_wstring(index + 1) + L"/" + std::to_wstring(chapters.size()) +
           (title.empty() ? L"" : L": " + title);
}

double ChapterStartAfter(const std::vector<Chapter>& chapters, double position) {
    for (const Chapter& chapter : chapters)
        if (chapter.start > position + 0.25) return chapter.start;
    return 0;
}

bool ReadLoopPoints(const std::wstring& path, int64_t& loopStart, int64_t& loopEnd) {
    File f(path);
    if (!f.Ok()) return false;
    std::string magic = f.Read(0, 4);
    LoopTags tags;
    ArtSink art;
    art.loops = &tags;
    TagData t;
    if (magic == "fLaC") ReadFlac(f, 4, t, art);
    else if (magic == "cord") ReadCord(f, t, art);
    else if (magic == "OggS") ReadOgg(f, t, art);
    else if (magic == "RIFF" || magic == "RF64" || magic == "BW64") ReadRiff(f, t, art);
    else return false;
    int64_t start = 0, end = 0;
    if (tags.wav) {
        start = tags.wavStart;
        end = tags.wavEnd;
    } else {
        // Decimal digits only: a negative or garbled value is no loop point at all.
        auto parse = [](const std::string& text, int64_t& value) {
            std::string s = Trim(text);
            if (s.empty() || s.size() > 18) return false;
            int64_t v = 0;
            for (char c : s) {
                if (c < '0' || c > '9') return false;
                v = v * 10 + (c - '0');
            }
            value = v;
            return true;
        };
        int64_t length = 0;
        if (!parse(tags.start, start)) return false;
        if (!parse(tags.end, end)) {
            if (!parse(tags.length, length)) return false;
            end = start + length;
        }
    }
    if (start < 0 || end <= start) return false;
    loopStart = start;
    loopEnd = end;
    return true;
}
