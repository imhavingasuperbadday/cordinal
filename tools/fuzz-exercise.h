// What one fuzz case does with a file: open it through the player's own decoder dispatch, read,
// seek and preview it, read it again through the exact readers that conversion uses, and read its
// tags, art, lyrics, chapters and loop points. Shared by the MSVC mutation fuzzer
// (tools/fuzz-parsers.cpp) and the libFuzzer targets (tools/fuzz-media.cpp), so both hold the
// parsers to the same contract. Sanitizers catch memory errors; Finding() records the rest: a
// broken Decoder contract, a non-finite sample or an exception that escapes a parser, any of which
// would end the player on the scan or playback path.
#pragma once
#include "native_audio.h"
#include "library.h"
#include "tags.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace fuzzcase {

// ---- Deterministic randomness ----
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t Next() {  // splitmix64
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    size_t Below(size_t n) { return n ? (size_t)(Next() % n) : 0; }
    bool Chance(unsigned percent) { return Below(100) < percent; }
};

inline uint64_t Hash(const void* data, size_t size) {  // FNV-1a
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; ++i) h = (h ^ ((const unsigned char*)data)[i]) * 0x100000001B3ull;
    return h;
}

// The first finding of the current case; empty while it passes.
inline std::string& Problem() {
    static std::string problem;
    return problem;
}
inline void Finding(const std::string& what) {
    if (Problem().empty()) Problem() = what;
}

// Frames one case may decode in all, so a damaged header claiming hours of audio stays quick.
constexpr size_t kFrameBudget = 1 << 18;

inline size_t BlockSize(Rng& r) {
    switch (r.Below(5)) {
    case 0: return 1 + r.Below(4);
    case 1: return 1 + r.Below(1024);
    case 2: return 4096;
    case 3: return 1 + r.Below(20000);
    default: return 1152 * (1 + r.Below(4));
    }
}

// Reads blocks of random size into buffers of exactly that size, so AddressSanitizer sees any write
// past the frames requested. Returns the frames read.
inline size_t ReadSome(Decoder& d, Rng& r, size_t budget, size_t& spent) {
    size_t total = 0;
    while (spent < budget) {
        const size_t want = BlockSize(r);
        std::vector<float> out(want * 2);
        const size_t got = d.Read(out.data(), want);
        if (got > want) {
            Finding("Read returned " + std::to_string(got) + " frames for a request of " + std::to_string(want));
            return total;
        }
        for (size_t i = 0; i < got * 2; ++i)
            if (!std::isfinite(out[i])) {
                Finding("Read returned a non-finite sample");
                return total;
            }
        if (!got) break;
        total += got;
        spent += got;
        if (r.Chance(3)) break;  // stop early now and then, as a seek or a skip does
    }
    return total;
}

inline void ExerciseDecoder(const std::wstring& path, Rng& r) {
    std::wstring error;
    auto d = OpenDecoder(path, 44100, error);  // the player's own dispatch by extension
    if (d) {
        const double duration = d->Duration();
        const unsigned rate = d->Rate();
        if (!rate) Finding("decoder opened with a rate of 0");
        if (!std::isfinite(duration) || duration < 0)
            Finding("decoder opened with a duration of " + std::to_string(duration));
        float none[2] = {};
        if (d->Read(none, 0)) Finding("a zero-frame Read returned frames");
        size_t spent = 0;
        ReadSome(*d, r, kFrameBudget / 2, spent);
        const double span = std::isfinite(duration) && duration > 0 ? std::min(duration, 1e7) : 10.0;
        for (int i = 0; i < 4 && Problem().empty(); ++i) {
            double to;
            switch (r.Below(5)) {
            case 0: to = 0; break;
            case 1: to = span; break;
            case 2: to = span * 1.5; break;
            default: to = span * (double)r.Below(10001) / 10000; break;
            }
            if (r.Chance(25) && d->CanPreview()) d->PreviewSeek(to);
            else d->Seek(to);
            ReadSome(*d, r, spent + kFrameBudget / 8, spent);
        }
        d->Failed();
        d->Problem();
    }
    // The exact readers that conversion uses.
    const std::wstring ext = ExtOf(path);
    std::unique_ptr<PcmReader> exact;
    if (ext == L"flac") exact = OpenFlacExact(path);
    else if (ext == L"wav" || ext == L"aif" || ext == L"aiff" || ext == L"aifc" || ext == L"au" || ext == L"caf")
        exact = OpenPcmExact(path);
    else if (ext == L"cord") exact = OpenCordExact(path);
    if (exact) {
        const unsigned channels = exact->Format().channels;
        if (!channels || channels > 64) {
            Finding("exact reader opened with " + std::to_string(channels) + " channels");
            return;
        }
        size_t spent = 0;
        while (spent < kFrameBudget) {
            const size_t want = BlockSize(r);
            std::vector<int32_t> out(want * channels);
            const size_t got = exact->Read(out.data(), want);
            if (got > want) {
                Finding("exact Read returned more frames than requested");
                return;
            }
            if (!got) break;
            spent += got;
        }
    }
}

// Every metadata reader the scanner and the now-playing view use. Returns the embedded picture's
// bytes, for a caller that decodes images too.
inline std::string ExerciseTags(const std::wstring& path) {
    TagData t;
    ReadTags(path, t);
    std::string art, text;
    ReadEmbeddedArt(path, art);
    ReadLyrics(path, text);
    std::vector<Chapter> chapters;
    ReadChapters(path, chapters);
    int64_t loopStart = 0, loopEnd = 0;
    ReadLoopPoints(path, loopStart, loopEnd);
    return art;
}

enum Mode { kDecoder = 1, kTags = 2, kBoth = 3 };

// Runs one file and returns the finding, empty when it passed. The randomness (block sizes, seek
// targets) comes from the bytes alone, so a saved case replays exactly.
inline std::string Run(const std::wstring& path, const void* bytes, size_t size, int mode) {
    Problem().clear();
    Rng r(Hash(bytes, size));
    try {
        if (mode & kTags) ExerciseTags(path);
        if (mode & kDecoder) ExerciseDecoder(path, r);
    } catch (const std::exception& e) {
        Finding(std::string("uncaught exception: ") + e.what());
    } catch (...) {
        Finding("uncaught exception of an unknown type");
    }
    return Problem();
}

}  // namespace fuzzcase
