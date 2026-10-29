// cordinal's own audio format, version 1 (docs/cord-spec.md, frozen): the container and tag layout
// shared by the decoder (audio_cord.cpp), the encoder (cord_encode.cpp), the tag reader (tags.cpp)
// and the tag writer (tagwriter_cord.cpp). Major version 0 was the development bitstream; it is
// refused, so no development file passes for a frozen one. Change nothing here that the spec fixes.
//
//   "cord", u8 major, u8 minor, u16 reserved (0)
//   chunks to the end of the file: a 4-character id, a u64 payload size, the payload
//
// INFO comes first; META, PICT and PAD may follow in any order; INDX and then AUDI come after them.
// Readers skip chunks they don't know and writers keep them. All integers are little-endian.
// Fixed-size chunk headers let the encoder write in one pass and patch sizes at the end, and let a
// tag edit grow or shrink META without the header changing width.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

class PcmReader;

namespace cord {

constexpr uint8_t kMajor = 1, kMinor = 0;
constexpr size_t kHeaderBytes = 8, kChunkHeaderBytes = 12;

constexpr uint32_t FourCc(const char (&id)[5]) {
    return (uint32_t)(uint8_t)id[0] | (uint32_t)(uint8_t)id[1] << 8 | (uint32_t)(uint8_t)id[2] << 16 |
           (uint32_t)(uint8_t)id[3] << 24;
}
constexpr uint32_t kInfo = FourCc("INFO"), kMeta = FourCc("META"), kPicture = FourCc("PICT"), kPad = FourCc("PAD "),
                   kIndex = FourCc("INDX"), kAudio = FourCc("AUDI");
// The lossless codec (cord_codec.h), frozen with version 1. Its development predecessors are not
// read: CL01 was Rice-coded, and CL02 had no cascade.
constexpr uint32_t kLossless = FourCc("CL03");

inline uint32_t Le32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
inline uint64_t Le64(const uint8_t* p) { return Le32(p) | (uint64_t)Le32(p + 4) << 32; }
inline void PutLe(std::string& s, uint64_t v, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) s += (char)(v >> (8 * i));
}

// The 8-byte file header of a version this build reads.
inline bool IsHeader(const uint8_t* p) { return !memcmp(p, "cord", 4) && p[4] == kMajor && !p[6] && !p[7]; }
inline std::string Header() { return std::string("cord", 4) + (char)kMajor + (char)kMinor + std::string(2, '\0'); }
inline std::string ChunkHeader(uint32_t id, uint64_t size) {
    std::string s;
    PutLe(s, id, 4);
    PutLe(s, size, 8);
    return s;
}

// INFO: codec, rate, channels (u16), bits (u16), channel mask, frames (u64), block and frame
// lengths, flags (0), then the MD5 of the samples in FLAC's layout. Readers accept a longer INFO
// and ignore what follows, so a minor version can append fields.
struct Info {
    uint32_t codec = kLossless, rate = 0, channels = 0, bits = 0;
    uint32_t channelMask = 0;      // WAVE speaker bits; 0 = the default order for the channel count
    uint64_t frames = 0;
    uint32_t blockLength = 0;      // frames per block: the unit of seeking, whose state starts afresh
    uint32_t frameLength = 0;      // frames per frame: the unit of coding choices, dividing a block
    std::array<uint8_t, 16> md5{};
    uint64_t Blocks() const { return (frames + blockLength - 1) / blockLength; }
};
constexpr size_t kInfoBytes = 52;
// Limits the decoder relies on: they bound its block buffer at 2 MB.
constexpr uint32_t kMaxChannels = 8, kMaxFrameLength = 16384, kMaxBlockLength = 65536;

inline bool ParseInfo(const uint8_t* p, size_t n, Info& info) {
    if (n < kInfoBytes) return false;
    info.codec = Le32(p);
    info.rate = Le32(p + 4);
    info.channels = (uint32_t)(p[8] | p[9] << 8);
    info.bits = (uint32_t)(p[10] | p[11] << 8);
    info.channelMask = Le32(p + 12);
    info.frames = Le64(p + 16);
    info.blockLength = Le32(p + 24);
    info.frameLength = Le32(p + 28);
    memcpy(info.md5.data(), p + 36, 16);
    return info.codec == kLossless && info.rate && info.rate <= 1048575 && info.channels && info.channels <= kMaxChannels &&
           info.bits && info.bits <= 32 && info.frameLength && info.frameLength <= kMaxFrameLength &&
           info.blockLength <= kMaxBlockLength && info.blockLength >= info.frameLength &&
           info.blockLength % info.frameLength == 0 && Le32(p + 32) == 0;
}

inline std::string InfoBytes(const Info& info) {
    std::string s;
    PutLe(s, info.codec, 4);
    PutLe(s, info.rate, 4);
    PutLe(s, info.channels, 2);
    PutLe(s, info.bits, 2);
    PutLe(s, info.channelMask, 4);
    PutLe(s, info.frames, 8);
    PutLe(s, info.blockLength, 4);
    PutLe(s, info.frameLength, 4);
    PutLe(s, 0, 4);
    s.append((const char*)info.md5.data(), 16);
    return s;
}

// META: cordinal's own tags, a list of entries. Each is a u8 field id, then (free-form entries
// only) a u8 key length and the key, then a u32 value length and the value. The fields cordinal
// shows are typed; anything else a source carried is a free-form entry under its Vorbis comment
// key (MUSICBRAINZ_TRACKID, CHAPTER001, LOOPSTART...), so conversions lose nothing. Text is UTF-8
// and may repeat for several values. Readers skip ids they don't know; writers keep them.
enum Field : uint8_t {
    kFreeForm = 0,
    // UTF-8 text.
    kTitle = 1, kArtist, kAlbum, kAlbumArtist, kGenre, kDate, kComposer, kComment, kGrouping, kLabel, kLyrics,
    // u32 numbers, never 0.
    kTrack = 16, kTrackTotal, kDisc, kDiscTotal, kBpm,
    // u8 1: part of a compilation.
    kCompilation = 21,
    // f32: ReplayGain in dB, and peaks as linear amplitude.
    kTrackGain = 32, kTrackPeak, kAlbumGain, kAlbumPeak,
};

struct Entry {
    uint8_t id = 0;
    std::string_view key, value;
};

// The entry at `at`, which moves past it. False at the end, or when an entry runs past the payload.
inline bool NextEntry(std::string_view meta, size_t& at, Entry& e) {
    if (at >= meta.size()) return false;
    e.id = (uint8_t)meta[at++];
    e.key = {};
    if (e.id == kFreeForm) {
        if (at >= meta.size() || (uint8_t)meta[at] > meta.size() - at - 1) return false;
        e.key = meta.substr(at + 1, (uint8_t)meta[at]);
        at += 1 + e.key.size();
    }
    if (meta.size() - at < 4) return false;
    const uint32_t n = Le32((const uint8_t*)meta.data() + at);
    at += 4;
    if (n > meta.size() - at) return false;
    e.value = meta.substr(at, n);
    at += n;
    return true;
}

inline void PutEntry(std::string& meta, uint8_t id, std::string_view key, std::string_view value) {
    meta += (char)id;
    if (id == kFreeForm) {
        meta += (char)key.size();
        meta += key;
    }
    PutLe(meta, value.size(), 4);
    meta += value;
}

// PICT: one picture. A u8 picture type (3 is the front cover, as in ID3 and FLAC), a u8 MIME type
// length and the MIME type, a u32 description length and the UTF-8 description, then the image
// bytes to the end of the chunk.
inline bool ParsePicture(std::string_view b, uint8_t& type, std::string_view& image) {
    if (b.size() < 2 || (uint8_t)b[1] > b.size() - 2) return false;
    size_t at = 2 + (uint8_t)b[1];
    if (b.size() - at < 4) return false;
    const uint32_t described = Le32((const uint8_t*)b.data() + at);
    at += 4;
    if (described > b.size() - at) return false;
    type = (uint8_t)b[0];
    image = b.substr(at + described);
    return true;
}

inline std::string PictureBytes(std::string_view image, uint8_t type = 3, std::string_view mime = {},
                                std::string_view description = {}) {
    if (mime.empty() || mime.size() > 255)
        mime = image.size() >= 4 && image.substr(0, 4) == "\x89PNG" ? "image/png" : "image/jpeg";
    std::string s(1, (char)type);
    s += (char)mime.size();
    s += mime;
    PutLe(s, description.size(), 4);
    s += description;
    s += image;
    return s;
}

struct CascadeSpec;
// What a new file carries besides its audio, and encoder choices that never change what it decodes
// to. Up to `threads` blocks are encoded at once, and the file is the same for any count. `cascade`
// replaces the encoder's own cascades for every block; the self-test uses it to reach the
// decoder's general path.
struct EncodeOptions {
    unsigned threads = 1;
    bool background = false;  // the encoding threads run in Windows' background mode
    const CascadeSpec* cascade = nullptr;
    // Tags written into the new file: META's entries (PutEntry) and PICT payloads (PictureBytes).
    std::string meta;
    std::vector<std::string> pictures;
};

// Encodes `source` into a new .cord file at `path`. The file is built next to its destination and
// moved into place only when complete, so a failed or cancelled encode leaves nothing behind and
// an existing file is never replaced. The source must read to its end undamaged.
bool Encode(PcmReader& source, const std::wstring& path, std::wstring& error, const std::atomic<bool>* cancel = nullptr,
            const EncodeOptions& options = {});

}  // namespace cord
