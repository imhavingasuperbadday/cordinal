// Decoder composition and format dispatch behind decoder.h. A path's extension selects a native
// codec or exact reader (see native_audio.h); OpenRange and OpenLooped wrap the result to limit or
// loop its timeline. Sub-track and game-music paths are unwrapped here so callers can pass a library
// path straight through. The Resampler declared in decoder.h is implemented in audio_resample.cpp.
#include "decoder.h"

#include "game_music.h"
#include "library.h"
#include "native_audio.h"
#include "helpers.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>


namespace {

// Frames [first, last) of an inner decoder. Seeks go through the inner decoder at the middle of
// the target frame, so decoders that truncate seconds to a frame land exactly on it.
class RangeDecoder final : public Decoder {
public:
    RangeDecoder(std::unique_ptr<Decoder> inner, uint64_t first, uint64_t last, bool bounded)
        : inner_(std::move(inner)), first_(first), last_(last), bounded_(bounded), rate_(inner_->Rate()) {}

    size_t Read(float* out, size_t frames) override {
        if (bounded_) {
            if (pos_ >= last_) return 0;
            frames = (size_t)std::min<uint64_t>(frames, last_ - pos_);
        }
        size_t got = 0;
        while (got < frames) {  // a short read from the inner decoder isn't the end of the range
            size_t n = inner_->Read(out + got * 2, frames - got);
            if (!n) break;
            got += n;
        }
        pos_ += got;
        return got;
    }
    bool Seek(double seconds) override {
        if (!std::isfinite(seconds) || seconds >= (double)(INT64_MAX - first_) / rate_) return false;
        uint64_t frame = first_ + (uint64_t)std::llround(std::max(0.0, seconds) * rate_);
        if (!inner_->Seek((frame + 0.5) / rate_)) return false;
        pos_ = frame;
        return true;
    }
    double Duration() const override {
        if (bounded_) return (double)(last_ - first_) / rate_;
        double d = inner_->Duration();  // to the end of the file
        return d > 0 ? std::max(0.0, d - (double)first_ / rate_) : 0;
    }
    unsigned Rate() const override { return rate_; }
    void Abort() override { inner_->Abort(); }
    bool Failed() const override { return inner_->Failed(); }
    std::wstring Problem() const override { return inner_->Problem(); }

private:
    std::unique_ptr<Decoder> inner_;
    uint64_t first_, last_;
    bool bounded_;
    unsigned rate_;
    uint64_t pos_ = 0;  // absolute frame of the next sample
};

// Intro, loop, and the loop again while fading out (see OpenLooped). Frames: `pos_` is the position
// on the looped timeline, `file_` the inner decoder's position.
class LoopDecoder final : public Decoder {
public:
    LoopDecoder(std::unique_ptr<Decoder> inner, uint64_t loopStart, uint64_t loopEnd, uint64_t fadeFrom, uint64_t total)
        : inner_(std::move(inner)), loopStart_(loopStart), loopEnd_(loopEnd), fadeFrom_(fadeFrom), total_(total),
          rate_(inner_->Rate()) {}

    size_t Read(float* out, size_t frames) override {
        if (failed_) return 0;
        size_t done = 0;
        while (done < frames && pos_ < total_) {
            if (file_ >= loopEnd_) {  // the loop's end: back to its start, exactly
                if (!inner_->Seek((loopStart_ + 0.5) / rate_)) { failed_ = true; break; }
                file_ = loopStart_;
            }
            const size_t want = (size_t)std::min<uint64_t>({frames - done, loopEnd_ - file_, total_ - pos_});
            const size_t n = inner_->Read(out + done * 2, want);
            if (!n) { failed_ = true; break; }  // the file ends before the loop does
            for (size_t i = 0; i < n; ++i) {
                const uint64_t v = pos_ + i;
                if (v < fadeFrom_) continue;
                const float g = 1.0f - (float)(v - fadeFrom_ + 1) / (float)(total_ - fadeFrom_);
                out[(done + i) * 2] *= g;
                out[(done + i) * 2 + 1] *= g;
            }
            done += n;
            pos_ += n;
            file_ += n;
        }
        return done;
    }
    bool Seek(double seconds) override {
        if (!std::isfinite(seconds) || seconds >= (double)INT64_MAX / rate_) return false;
        const uint64_t v = (uint64_t)std::llround(std::max(0.0, seconds) * rate_);
        if (v >= total_) {
            pos_ = total_;
            failed_ = false;
            return true;
        }
        // Before the loop, the file is where the timeline is; after it, the loop repeats.
        const uint64_t f = v < loopStart_ ? v : loopStart_ + (v - loopStart_) % (loopEnd_ - loopStart_);
        if (!inner_->Seek((f + 0.5) / rate_)) return false;
        failed_ = false;
        pos_ = v;
        file_ = f;
        return true;
    }
    double Duration() const override { return total_ == UINT64_MAX ? inner_->Duration() : (double)total_ / rate_; }
    unsigned Rate() const override { return rate_; }
    void Abort() override { inner_->Abort(); }
    bool Failed() const override { return failed_ || (pos_ < total_ && inner_->Failed()); }
    std::wstring Problem() const override { return inner_->Problem(); }

private:
    std::unique_ptr<Decoder> inner_;
    uint64_t loopStart_, loopEnd_, fadeFrom_, total_;
    unsigned rate_;
    uint64_t pos_ = 0, file_ = 0;
    bool failed_ = false;
};



}  // namespace

std::unique_ptr<Decoder> OpenRange(std::unique_ptr<Decoder> inner, double start, double end) {
    if (!inner || inner->Rate() == 0 || !std::isfinite(start) || !std::isfinite(end)) return nullptr;
    const double rate = inner->Rate();
    if (start >= (double)INT64_MAX / rate || end >= (double)INT64_MAX / rate) return nullptr;
    const uint64_t first = (uint64_t)std::llround(std::max(0.0, start) * rate);
    const bool bounded = end > 0;
    const uint64_t last = bounded ? std::max(first, (uint64_t)std::llround(end * rate)) : 0;
    auto range = std::make_unique<RangeDecoder>(std::move(inner), first, last, bounded);
    // Decoders open at frame 0; only seek when the range starts later.
    if (first > 0 && !range->Seek(0)) return nullptr;
    return range;
}

// Game-music loop points: the intro, `count` passes through the loop in all, then the loop once
// more while fading out over `fadeSeconds`. count 0 loops without end, for a section armed to loop
// on the waveform: the loop never finishes, so the fade never starts and the track's own length is
// what the decoder reports.
std::unique_ptr<Decoder> OpenLooped(std::unique_ptr<Decoder> inner, int64_t loopStart, int64_t loopEnd, int count,
                                    double fadeSeconds) {
    if (!inner || inner->Rate() == 0 || loopStart < 0 || loopEnd <= loopStart || count < 0 || count > 1000 ||
        !std::isfinite(fadeSeconds)) return nullptr;
    const double rate = inner->Rate();
    const uint64_t ls = (uint64_t)loopStart, le = (uint64_t)loopEnd;
    if (count && (uint64_t)count > ((uint64_t)INT64_MAX - ls) / (le - ls)) return nullptr;
    const uint64_t fadeFrom = count > 0 ? ls + (uint64_t)count * (le - ls) : UINT64_MAX;
    if (count && std::max(0.0, fadeSeconds) * rate >= (double)((uint64_t)INT64_MAX - fadeFrom)) return nullptr;
    const uint64_t total = count > 0 ? fadeFrom + (uint64_t)std::llround(std::max(0.0, fadeSeconds) * rate) : UINT64_MAX;
    return std::make_unique<LoopDecoder>(std::move(inner), ls, le, fadeFrom, total);
}

// Opens the decoder for a library path. On failure `error` is set to a user-readable line and null
// is returned. `fallbackRate` is a hint only; a decoder that knows its own rate ignores it.
std::unique_ptr<Decoder> OpenDecoder(const std::wstring& path, unsigned fallbackRate, std::wstring& error) {
    error.clear();
    // Chiptune rips play through Cordinal emulators; "file.nsf|3" selects song 3.
    if (IsGameMusicFile(path)) {
        std::wstring file;
        int subsong = 1;
        if (!SplitGameMusicPath(path, file, subsong)) {
            error = L"Couldn't play " + FileNameOf(path) + L": bad song number.";
            return nullptr;
        }
        return OpenGameMusicDecoder(file, subsong, error);
    }
    // Any other "file.flac|3" (a CUE track): the real file; the caller plays its range.
    std::wstring file;
    int index = 0;
    if (SplitSubtrack(path, file, index)) return OpenDecoder(file, fallbackRate, error);
    const std::wstring ext = ExtOf(path);
    std::unique_ptr<Decoder> d;
    if (ext == L"flac" || ext == L"fla") d = OpenFlacDecoder(path);
    else if (ext == L"wav" || ext == L"wave" || ext == L"aif" || ext == L"aiff" || ext == L"aifc") d = OpenPcmDecoder(path);
    else if (ext == L"mp3" || ext == L"mp2" || ext == L"mp1") d = OpenMpegDecoder(path);
    else if (ext == L"wv") d = OpenWavPackDecoder(path);
    else if (ext == L"ac3") d = OpenAc3Decoder(path);
    else if (ext == L"mka") d = OpenMatroskaDecoder(path);
    else if (ext == L"ape") d = OpenApeDecoder(path);
    else if (ext == L"ogg" || ext == L"oga") {
        d = OpenVorbisDecoder(path);
        if (!d) d = OpenOpusDecoder(path);
    } else if (ext == L"opus") d = OpenOpusDecoder(path);
    else if (ext == L"m4a" || ext == L"m4b" || ext == L"mp4" || ext == L"alac") {
        d = OpenAlacDecoder(path);
        if (!d) d = OpenAacDecoder(path);
    } else if (ext == L"aac" || ext == L"adts") d = OpenAacDecoder(path);
    else if (ext == L"wma" || ext == L"asf") d = OpenWmaDecoder(path);
    else if (ext == L"cord") d = OpenCordDecoder(path);
    else if (ext == L"dsf" || ext == L"dff") d = OpenDsdDecoder(path);
    else if (ext == L"mod" || ext == L"s3m" || ext == L"xm" || ext == L"it") d = OpenModuleDecoder(path);
    else if (ext == L"mid" || ext == L"midi" || ext == L"kar" || ext == L"rmi") {
        d = OpenMidiDecoder(path);
        if (!d) {
            error = L"Couldn't play " + FileNameOf(path) + L": it isn't a MIDI file, or there's no SoundFont to play it with.";
            return nullptr;
        }
    }
    // A file type cordinal doesn't play itself may belong to a helper program (helpers.h).
    else if (HelperForExtension(ext)) return OpenHelperDecoder(path, error);
    if (!d) error = L"Couldn't play " + FileNameOf(path) + L": unsupported or damaged audio stream.";
    return d;
}

bool IgnoresWavPackCorrection(const std::wstring& path) {
    std::wstring file = path;
    int index = 0;
    SplitSubtrack(path, file, index);
    return ExtOf(file) == L"wv" && GetFileAttributesW((file + L"c").c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::unique_ptr<PcmReader> OpenPcmReader(const std::wstring& path, std::wstring& error) {
    error.clear();
    const std::wstring ext = ExtOf(path);
    std::unique_ptr<PcmReader> r;
    if (ext == L"flac" || ext == L"fla") r = OpenFlacExact(path);
    else if (ext == L"wav" || ext == L"wave" || ext == L"aif" || ext == L"aiff" || ext == L"aifc") r = OpenPcmExact(path);
    else if (ext == L"cord") r = OpenCordExact(path);
    if (!r) error = L"Couldn't read the exact samples of " + FileNameOf(path) + L": only FLAC, integer WAV or AIFF and "
                    L"cordinal's own format can be read exactly, and the file must not be damaged.";
    return r;
}
