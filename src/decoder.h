// Audio decoders. Each produces interleaved stereo float at its own native rate; the engine
// resamples the joined stream so track boundaries stay seamless. PcmReader instead reads a lossless
// file's exact integer samples, for conversion.
//
// Cordinal's native, in-process codecs: FLAC, WAV/AIFF, MPEG Layers I/II/III
// (including LAME gapless) and Ogg Vorbis; additional codecs are declared in native_audio.h.
#pragma once
#include "md5.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Decoder {
public:
    virtual ~Decoder() = default;
    // Reads up to `frames` frames; 0 means the end or a failure (see Failed()). `out` must hold
    // frames*2 floats (interleaved stereo). A zero-size request changes no state.
    virtual size_t Read(float* out, size_t frames) = 0;
    // Positions the stream at `seconds` from the start of this decoder's own timeline.
    virtual bool Seek(double seconds) = 0;
    virtual double Duration() const = 0;
    virtual unsigned Rate() const = 0;
    // Optional cancellation for decoders that perform blocking work.
    virtual void Abort() {}
    // Sticky when decoding or reading fails, including an end before the declared sample count.
    // A short read may return the good samples before the failure; subsequent reads return 0.
    // A successful Seek clears it, so intact parts of a damaged file can still be played.
    virtual bool Failed() const { return false; }
    // Why it failed, when it can say more than that the audio is damaged: a helper program that
    // stopped (helpers.h) says what it printed. Empty otherwise.
    virtual std::wstring Problem() const { return {}; }
};

// A sub-track path ("file.flac|3") opens the real file; OpenRange limits its extent.
// The rate parameter is retained for callers; owned decoders use their native rate.
std::unique_ptr<Decoder> OpenDecoder(const std::wstring& path, unsigned outputRate, std::wstring& error);
// A hybrid WavPack file plays its lossy part only: true when a .wvc correction file lies beside it,
// so the player can say that what it plays is not the lossless stream (docs/decoders.md).
bool IgnoresWavPackCorrection(const std::wstring& path);

// Exact integer PCM, for lossless conversion and its verification (docs/cord-format.md): the
// stream's own channels and bit depth, with no downmix and no float conversion.
struct PcmFormat {
    unsigned rate = 0, channels = 0;
    unsigned bits = 0;              // significant bits per sample, 1..32
    uint32_t channelMask = 0;       // WAVE speaker bits; 0 = the format's own channel order
    uint64_t frames = 0;            // the stream's length
    std::array<uint8_t, 16> md5{};  // the MD5 the source stores for these samples (FLAC); zeros = none
};

class PcmReader {
public:
    virtual ~PcmReader() = default;
    // Reads up to `frames` frames into out[frames * channels], interleaved and right-justified:
    // 16-bit audio reads as -32768..32767. 0 means the end or damage, and Damaged() tells which.
    // Every sample is hashed on the way; at the end the stream must have its declared length and,
    // when the source stores an MD5, that MD5, so a damaged file never reads as a complete one.
    size_t Read(int32_t* out, size_t frames);
    const PcmFormat& Format() const { return format_; }
    bool Damaged() const { return damaged_; }
    // The MD5 of every sample read, in FLAC's layout, once Read has reached the end undamaged.
    // Zeros until then.
    const std::array<uint8_t, 16>& Digest() const { return digest_; }

protected:
    // The format's own reader: fills `out` as Read does and returns 0 at its end. Data that fails
    // a check sets damaged_, and none of it is returned.
    virtual size_t Next(int32_t* out, size_t frames) = 0;
    PcmFormat format_;
    bool damaged_ = false;

private:
    Md5 md5_;
    std::array<uint8_t, 16> digest_{};
    uint64_t read_ = 0;
    bool ended_ = false;
};

inline size_t PcmReader::Read(int32_t* out, size_t frames) {
    if (!frames || ended_ || damaged_) return 0;
    if (const size_t n = Next(out, frames)) {
        md5_.AddPcm(out, n * format_.channels, format_.bits);
        read_ += n;
        return n;
    }
    ended_ = true;
    if (damaged_ || read_ != format_.frames) {
        damaged_ = true;
        return 0;
    }
    digest_ = md5_.Finish();
    if (format_.md5 != std::array<uint8_t, 16>{} && digest_ != format_.md5) {
        damaged_ = true;
        digest_ = {};
    }
    return 0;
}

// Opens a file for exact reading: FLAC, integer PCM in WAV, RF64, Wave64 and AIFF, and cordinal's
// own format. Float, ADPCM, G.711 and lossy streams are refused. On failure `error` is set to a
// user-readable line.
std::unique_ptr<PcmReader> OpenPcmReader(const std::wstring& path, std::wstring& error);

// Plays only [start, end) seconds of `inner` (end 0: to the end of the file). Start and end are
// rounded to the nearest frame at the decoder's rate, and Read stops exactly at the end frame.
// Seek and Duration are relative to start, so the result behaves like a track of length end-start.
// Returns null if the range can't be set up (the decoder can't seek there).
std::unique_ptr<Decoder> OpenRange(std::unique_ptr<Decoder> inner, double start, double end);

// Game-music loops, in frames at the decoder's rate: the intro [0, loopStart), then the loop
// [loopStart, loopEnd) `count` times in total, then the loop again while fading out over
// fadeSeconds, after which the decoder ends. count 0 loops without end and never fades: the
// decoder reports the track's own length, for a section armed to loop (app_region.cpp). Loops are
// sample-exact: each pass seeks back to loopStart. Seek and Duration are on this timeline. Null if
// the arguments make no sense.
std::unique_ptr<Decoder> OpenLooped(std::unique_ptr<Decoder> inner, int64_t loopStart, int64_t loopEnd, int count,
                                    double fadeSeconds);

// Polyphase windowed-sinc resampler for interleaved stereo float (Kaiser window, 32 taps,
// 256 phases with linear interpolation). Its state carries across Push calls, so a stream
// joined from several tracks is resampled without seams.
class Resampler {
public:
    void Init(unsigned inRate, unsigned outRate);
    void Reset();  // after a seek
    bool Passthrough() const { return in_ == out_; }
    unsigned InRate() const { return inRate_; }
    // Consumes all input; appends output frames to `out`.
    void Push(const float* in, size_t frames, std::vector<float>& out);
    // Replaces output, retaining its initialized storage between decode blocks.
    void Process(const float* in,size_t frames,std::vector<float>& out);
    void Flush(std::vector<float>& out);  // end of stream: emit the tail
    // Output frames still owed for input already pushed (for marking track boundaries).
    double PendingOutput() const;

private:
    void Convert(const float* in,size_t frames,std::vector<float>& out,bool append);
    static constexpr int kTaps = 32, kPhases = 256;
    unsigned inRate_ = 0;
    unsigned in_ = 0, out_ = 0;  // reduced by their GCD
    // Exact position in hist_: idx_ + num_ / out_ input frames. Integer stepping makes the
    // output independent of how the input is chunked.
    size_t idx_ = 0;
    uint64_t num_ = 0;
    std::unique_ptr<float[]> table_;
    std::vector<float> hist_;  // stereo history + pending input
    bool fast_=false;
};
