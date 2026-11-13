// The tag reader's text and field handling (tags_read.h): Latin-1 and UTF-16 to UTF-8, the ID3v1 genre
// list, the generic KEY=value mapping that Vorbis comments, APE, MP4 freeform atoms, ASF and ID3 TXXX frames
// share, and the Vorbis comment block itself with the pictures, loops and chapters it may carry.
#include "tags_read.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tagread {

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

namespace {

// ApplyField with the key already upper-cased: ApplyComment needs it upper-cased too, and a Vorbis
// comment block can hold hundreds of keys.
void ApplyUpperField(TagData& t, const std::string& key, const std::string& value, bool append, std::string* lyrics) {
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
    else if (key == "MUSICBRAINZ_ALBUMID" || key == "MUSICBRAINZ_RELEASEID" || key == "MUSICBRAINZ ALBUM ID" ||
             key == "MUSICBRAINZ RELEASE ID") t.musicBrainzRelease = value;
    else if (key == "MUSICBRAINZ_RELEASEGROUPID" || key == "MUSICBRAINZ RELEASE GROUP ID")
        t.musicBrainzReleaseGroup = value;
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

}  // namespace

void ApplyField(TagData& t, sv key, const std::string& value, bool append, std::string* lyrics) {
    ApplyUpperField(t, Upper(key), value, append, lyrics);
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

// --- comments --------------------------------------------------------------------------------------

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
        ApplyUpperField(t, ukey, std::string(rawVal), true, art.lyrics);
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

}  // namespace tagread
