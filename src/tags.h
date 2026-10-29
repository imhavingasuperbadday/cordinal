// Native metadata reader: tags, technical info, ReplayGain and embedded art.
// Formats: MP3/MP2 (ID3v1/v2, APEv2, Xing/VBRI), FLAC, Ogg Vorbis/Opus/FLAC, MP4/M4A/MOV,
// WMA/WMV (ASF), WAV, AIFF, Matroska/WebM, AVI, and cordinal's own format (cord.h).
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

enum class Codec : uint8_t {
    Unknown, MP3, MP2, FLAC, AAC, ALAC, PCM, WMA, Vorbis, Opus, AC3,
    H264, HEVC, VP8, VP9, AV1, MPEG4, MPEG2, WMV, Theora, Video,
    SPC, NSF, GBS, VGM,  // game music (game_music.h); append only, values are stored in the library
    Cord,                // cordinal's own format (cord.h)
    DSD, SID, Module, MIDI,
};
constexpr Codec kLastCodec = Codec::MIDI;  // what a stored codec may be
const wchar_t* CodecName(Codec c);

struct TagData {
    std::string title, artist, album, albumArtist, genre, date;  // UTF-8
    std::string composer, comment, grouping, label;  // UTF-8 (label = publisher / record label)
    uint32_t track = 0, trackTotal = 0, disc = 0, discTotal = 0, bpm = 0;
    double duration = 0;
    uint32_t sampleRate = 0, channels = 0, bitrate = 0;  // bitrate in kbps
    Codec codec = Codec::Unknown;
    bool video = false, compilation = false;
    float rgTrackGain = NAN, rgTrackPeak = NAN, rgAlbumGain = NAN, rgAlbumPeak = NAN;
    // Raw Opus R128 tags (Q7.8 dB relative to -23 LUFS), kept for the tag-undo journal.
    // INT32_MIN = the tag is absent.
    int32_t r128TrackGain = INT32_MIN, r128AlbumGain = INT32_MIN;
    // FMPS_RATING and FMPS_PLAYCOUNT as written (TagEdit), kept for the tag-undo journal. Empty = absent.
    std::string rating, playCount;
    // FLAC: the first 8 bytes of the MD5 of the decoded audio, big endian. Identical audio has the
    // same value whatever the tags or name; 0 = unknown (or the encoder left the MD5 zero).
    uint64_t audioHash = 0;
};

// Fills `out` from scratch (unknown codec, empty strings, NAN ReplayGain) — never merges with its
// previous value, and a failed read still leaves a valid default TagData.
bool ReadTags(const std::wstring& path, TagData& out);
// Front cover (or first picture) as encoded image bytes (JPEG/PNG/...).
bool ReadEmbeddedArt(const std::wstring& path, std::string& image);
// Unsynchronized lyrics (ID3 USLT, Vorbis/APE LYRICS or UNSYNCEDLYRICS, MP4 ©lyr, ASF WM/Lyrics). Empty if none.
bool ReadLyrics(const std::wstring& path, std::string& lyrics);
// Chapter marks: ID3 CHAP frames (MP3) or CHAPTERnnn / CHAPTERnnnNAME comments (FLAC, Ogg, Opus),
// as yt-dlp and mkvmerge write them. Sorted by start; empty if the file has none.
struct Chapter {
    double start = 0;   // seconds
    std::string title;  // UTF-8, may be empty
};
bool ReadChapters(const std::wstring& path, std::vector<Chapter>& chapters);
// The current chapter label and the next chapter boundary on the track's timeline.
std::wstring ChapterTextAt(const std::vector<Chapter>& chapters, double position);
double ChapterStartAfter(const std::vector<Chapter>& chapters, double position);
// Game-music loop points: LOOPSTART with LOOPEND or LOOPLENGTH (Vorbis, Opus and FLAC comments), or
// the first loop of a WAV smpl chunk. Frames at the file's native rate; loopEnd is exclusive.
// False if the file has none, or they're invalid (negative, or end not after start).
bool ReadLoopPoints(const std::wstring& path, int64_t& loopStart, int64_t& loopEnd);
