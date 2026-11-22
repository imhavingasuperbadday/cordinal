// Rewriting a fragmented MP4 (moof/mdat runs) into one standard fast-start M4A, so a
// stream saved from the web becomes a normal library file that every player and every
// tag writer understands. The sample runs are read by audio_mp4.cpp's own parser; this
// module only rebuilds the container around the same compressed frames, byte for byte.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nativeaudio {

// One fragmented MP4 audio track as the parser read it: every sample's bytes in the
// source, plus the raw boxes a lossless rewrite has to carry over.
struct Mp4Fragments {
    std::vector<uint8_t> ftyp;         // the source brand box, copied verbatim (empty when absent)
    std::vector<uint8_t> sampleDesc;   // the stsd sample description (mp4a + esds etc.), verbatim
    uint32_t movieScale = 0, mediaScale = 0;  // the mvhd / mdhd timescales, kept as they are
    uint64_t mediaDuration = 0;        // every sample duration added, in media ticks
    uint64_t lead = 0;                 // the first fragment's decode time (tfdt), in media ticks
    // The track's edit list as the source states it: an optional empty edit (leading
    // silence, mediaTime < 0) and then the playable start. Times are media ticks and
    // durations movie ticks; `hasEdit` false means the track had no edit list at all.
    bool hasEdit = false;
    uint64_t emptyEditDuration = 0;
    int64_t editMediaTime = 0;
    uint64_t editDuration = 0;         // 0 when the source never stated a length
    struct Run { uint64_t offset; uint32_t bytes, frames; };
    std::vector<Run> runs;             // the compressed samples, in play order
    // Codec facts of the copied sample description, for verifying the rewrite.
    uint32_t codec = 0, channels = 0, rate = 0;
    std::vector<uint8_t> config;       // the codec's setup bytes (AudioSpecificConfig)
};

// True when the file holds a top-level moof box: an MP4 whose samples live in fragment
// runs rather than one sample table.
bool IsFragmentedMp4(const std::wstring& path);

// Parse one fragmented MP4's audio track. False, with `error` in words the save queue
// can show as they are, when the file is not fragmented, has no readable audio track,
// or describes a timeline the rewrite could not represent exactly.
bool ReadMp4Fragments(const std::wstring& path, Mp4Fragments& out, std::wstring& error);

// Rewrite the fragmented MP4 `from` into a standard fast-start M4A at `to`: the same
// compressed frames, a full sample table, no mvex. The result is read back and compared
// with the source before this returns; `to` is deleted when anything fails.
bool RemuxFragmentedMp4(const std::wstring& from, const std::wstring& to, std::wstring& error);

}  // namespace nativeaudio
