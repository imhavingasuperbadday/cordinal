// Cordinal PCM/container decoder: RIFF/RIFX, RF64, Wave64, AIFF and AIFF-C.
// Formats: integer/IEEE PCM, G.711 A-law/mu-law, Microsoft and IMA WAVE ADPCM.
// Written from the Microsoft WAVE and Apple AIFF specifications; see docs/decoders.md.
#include "native_audio.h"
#include "audio_file.h"
#include <cmath>
#include <cfloat>
#include <climits>
#include <limits>

namespace {
using namespace nativeaudio;

// Format and channel count are constant across a block. Four 24-bit samples come from two
// overlapping 8-byte loads (bytes 0-7 and 6-13): each 64-bit lane holds two samples, at bit 0
// and bit 24. That reads two bytes past the group, so a group with no sample after it takes
// the scalar tail rather than load beyond the data.
template<unsigned Bits, bool Mono> void IntegerPcm(const uint8_t* p, float* out, size_t frames) {
    constexpr float scale = 1.0f / (uint64_t(1) << (Bits - 1));
    const size_t samples = frames * (Mono ? 1 : 2);
    // A 24-bit group reads into the sample after it, so it needs one more sample to follow.
    const size_t limit = Bits == 24 && samples ? samples - 1 : samples;
    size_t i = 0;
    for (; i + 4 <= limit; i += 4) {
        __m128i v;
        if constexpr (Bits == 16) {
            v = _mm_loadl_epi64((const __m128i*)(p + i * 2));
            v = _mm_unpacklo_epi16(v, _mm_srai_epi16(v, 15));
        } else if constexpr (Bits == 24) {
            const uint8_t* s = p + i * 3;
            __m128i pair = _mm_unpacklo_epi64(_mm_loadl_epi64((const __m128i*)s), _mm_loadl_epi64((const __m128i*)(s + 6)));
            __m128i even = _mm_shuffle_epi32(pair, 0x08), odd = _mm_shuffle_epi32(_mm_srli_epi64(pair, 24), 0x08);
            v = _mm_unpacklo_epi32(even, odd);
            v = _mm_srai_epi32(_mm_slli_epi32(v, 8), 8);
        } else v = _mm_loadu_si128((const __m128i*)(p + i * 4));
        const __m128 f = _mm_mul_ps(_mm_cvtepi32_ps(v), _mm_set1_ps(scale));
        if constexpr (Mono) {
            _mm_storeu_ps(out + i * 2, _mm_unpacklo_ps(f, f));
            _mm_storeu_ps(out + i * 2 + 4, _mm_unpackhi_ps(f, f));
        } else _mm_storeu_ps(out + i, f);
    }
    for (; i < samples; ++i) {
        const uint8_t* s = p + i * (Bits / 8);
        int32_t v;
        if constexpr (Bits == 16) v = (int16_t)U16(s);
        else if constexpr (Bits == 24) v = (int32_t)((s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16) << 8) >> 8;
        else v = (int32_t)U32(s);
        if constexpr (Mono) out[i * 2] = out[i * 2 + 1] = (float)v * scale;
        else out[i] = (float)v * scale;
    }
}

// Single-file PCM decoder covering the common RIFF/AIFF container layouts.
// Samples come straight from the mapped file except for ADPCM, which is expanded
// one block at a time into decoded_.
class PcmDecoder final : public Decoder {
public:
    bool Open(const std::wstring& path);
    size_t Read(float* out, size_t frames) override;
    bool Seek(double seconds) override;
    double Duration() const override { return (double)total_ / rate_; }
    unsigned Rate() const override { return rate_; }
    bool Failed() const override { return failed_; }
    // Exact reading (PcmExact). ExactFormat refuses samples that aren't integers.
    bool ExactFormat(PcmFormat& f);
    size_t ReadExact(int32_t* out, size_t frames, bool& damaged);
private:
    bool WaveFormat(const uint8_t* p, size_t n);
    bool Adpcm(uint64_t block);
    float Sample(const uint8_t* p) const;
    int32_t Raw(const uint8_t* p) const;
    File file_;
    uint64_t start_ = 0, bytes_ = 0, total_ = 0, pos_ = 0;
    unsigned rate_ = 0, channels_ = 0, bits_ = 0, align_ = 0, format_ = 0, blockFrames_ = 0;
    // The header's significant bits when narrower than the container (0 = all of it), the
    // WAVE speaker mask, and the padding bits ExactFormat found to be zero.
    unsigned valid_ = 0, shift_ = 0;
    uint32_t mask_ = 0;
    bool big_ = false, signed8_ = false, failed_ = false;
    std::vector<float> decoded_;
    int coeff_[7][2]{};
};

// Decode a WAVE fmt chunk. WAVE_FORMAT_EXTENSIBLE (0xFFFE) is unwrapped to its
// real format tag; the ADPCM tags also capture frames-per-block and the
// predictor coefficients from the extended block.
bool PcmDecoder::WaveFormat(const uint8_t* p, size_t n) {
    if (n < 16) return false;
    format_ = U16(p, big_);
    channels_ = U16(p + 2, big_);
    rate_ = U32(p + 4, big_);
    align_ = U16(p + 12, big_);
    bits_ = U16(p + 14, big_);
    if (format_ == 0xFFFE) {
        static const uint8_t suffix[] = {0,0,0x10,0,0x80,0,0,0xAA,0,0x38,0x9B,0x71};
        if (big_ || n < 40 || U16(p + 16) < 22 || U16(p + 18) > bits_ ||
            memcmp(p + 28, suffix, sizeof suffix) != 0) return false;
        format_ = U32(p + 24);
        valid_ = U16(p + 18);
        mask_ = U32(p + 20);
    }
    if (format_ == 0x11 || format_ == 2) {
        if (big_ || n < 20 || bits_ != 4 || channels_ > 2 || !channels_) return false;
        blockFrames_ = U16(p + 18);
        unsigned header = (format_ == 2 ? 7 : 4) * channels_;
        if (align_ < header) return false;
        unsigned maxFrames = (align_ - header) * 2 / channels_ + (format_ == 2 ? 2 : 1);
        if (!blockFrames_ || blockFrames_ > maxFrames) return false;
        if (format_ == 2) {
            if (blockFrames_ < 2 || n < 22 || U16(p + 20) != 7 || n < 50) return false;
            for (int i = 0; i < 7; ++i) {
                coeff_[i][0] = (int16_t)U16(p + 22 + i * 4);
                coeff_[i][1] = (int16_t)U16(p + 24 + i * 4);
            }
        }
    }
    return true;
}

// Walk the container chunks. AIFF's sample rate is an 80-bit IEEE extended
// float; RF64/BW64 get 64-bit sizes from a ds64 chunk; Wave64 uses 64-bit GUID
// chunks. The declared sample count (fact/ds64/COMM) caps the size-derived total.
bool PcmDecoder::Open(const std::wstring& path) {
    if (!file_.Open(path) || file_.Size() < 12) return false;
    file_.Sequential();
    const uint8_t* p = file_.View(0, 12);
    if (!p) return false;
    uint64_t limit = file_.Size(), declaredFrames = UINT64_MAX;
    bool formatFound = false, dataFound = false;
    if (Is(p, "FORM") && (Is(p + 8, "AIFF") || Is(p + 8, "AIFC"))) {
        big_ = signed8_ = true;
        bool compressed = Is(p + 8, "AIFC");
        limit = std::min<uint64_t>(limit, (uint64_t)U32(p + 4, true) + 8);
        for (uint64_t at = 12; at + 8 <= limit;) {
            p = file_.View(at, 8);
            if (!p) return false;
            uint64_t n = U32(p + 4, true), body = at + 8;
            if (n > limit - body) return false;
            bool comm = Is(p, "COMM"), data = Is(p, "SSND");
            if (comm && !formatFound) {
                if (n < (compressed ? 22u : 18u)) return false;
                p = file_.View(body, compressed ? 22 : 18);
                if (!p) return false;
                channels_ = U16(p, true);
                declaredFrames = U32(p + 2, true);
                bits_ = U16(p + 6, true);
                int exponent = U16(p + 8, true);
                uint64_t mantissa = U64(p + 10, true);
                double rate = std::ldexp((double)mantissa, exponent - 16383 - 63);
                if (exponent & 0x8000 || !std::isfinite(rate) || rate < 1 || rate > 1048575) return false;
                rate_ = (unsigned)std::llround(rate);
                format_ = 1;
                if (compressed) {
                    const uint8_t* code = p + 18;
                    if (Is(code, "sowt")) big_ = false;
                    else if (Is(code, "fl32") || Is(code, "FL32")) { format_ = 3; if (bits_ != 32) return false; }
                    else if (Is(code, "fl64") || Is(code, "FL64")) { format_ = 3; if (bits_ != 64) return false; }
                    else if (Is(code, "ulaw") || Is(code, "ULAW")) { format_ = 7; bits_ = 8; }
                    else if (Is(code, "alaw") || Is(code, "ALAW")) { format_ = 6; bits_ = 8; }
                    else if (!Is(code, "NONE") && !Is(code, "twos")) return false;
                }
                align_ = channels_ * ((bits_ + 7) / 8);
                // Non-byte AIFF samples are stored in the high bits of their container.
                if (format_ == 1) { valid_ = bits_; bits_ = ((bits_ + 7) / 8) * 8; }
                formatFound = true;
            } else if (data && !dataFound) {
                if (n < 8) return false;
                p = file_.View(body, 8);
                if (!p) return false;
                uint64_t offset = U32(p, true);
                if (offset > n - 8) return false;
                start_ = body + 8 + offset;
                bytes_ = n - 8 - offset;
                dataFound = true;
            }
            at = body + n + (n & 1);
        }
    } else {
        bool rf64 = Is(p, "RF64") || Is(p, "BW64"), w64 = Is(p, "riff");
        big_ = Is(p, "RIFX");
        if (!rf64 && !w64 && !big_ && !Is(p, "RIFF")) return false;
        uint64_t at = 12, largeData = 0;
        if (w64) {
            static const uint8_t riffTail[] = {0x2e,0x91,0xcf,0x11,0xa5,0xd6,0x28,0xdb,0x04,0xc1,0,0};
            static const uint8_t waveTail[] = {0xf3,0xac,0xd3,0x11,0x8c,0xd1,0,0xc0,0x4f,0x8e,0xdb,0x8a};
            p = file_.View(0, 40);
            if (!p || memcmp(p + 4, riffTail, 12) || !Is(p + 24, "wave") || memcmp(p + 28, waveTail, 12)) return false;
            limit = std::min(limit, U64(p + 16));
            at = 40;
        } else {
            if (!Is(p + 8, "WAVE")) return false;
            if (!rf64) limit = std::min<uint64_t>(limit, (uint64_t)U32(p + 4, big_) + 8);
        }
        unsigned header = w64 ? 24 : 8;
        for (; at <= limit && header <= limit - at;) {
            p = file_.View(at, header);
            if (!p) return false;
            bool fmt = Is(p, "fmt "), data = Is(p, "data"), ds64 = Is(p, "ds64"), fact = Is(p, "fact");
            uint64_t n = w64 ? U64(p + 16) : U32(p + 4, big_);
            if (w64) { if (n < 24) return false; n -= 24; }
            if (rf64 && data && n == UINT32_MAX) { if (!largeData) return false; n = largeData; }
            uint64_t body = at + header;
            if (n > limit - body) return false;
            if (ds64 && rf64) {
                if (n < 28) return false;
                p = file_.View(body, 28);
                if (!p) return false;
                uint64_t riffSize = U64(p), samples = U64(p + 16);
                if (riffSize > UINT64_MAX - 8 || riffSize + 8 > limit) return false;
                largeData = U64(p + 8);
                if (samples) declaredFrames = samples;
                limit = riffSize + 8;
            } else if (fmt && !formatFound) {
                p = file_.View(body, (size_t)std::min<uint64_t>(n, 64));
                if (!p || !WaveFormat(p, (size_t)std::min<uint64_t>(n, 64))) return false;
                formatFound = true;
            } else if (data && !dataFound) {
                start_ = body; bytes_ = n; dataFound = true;
            } else if (fact && n >= 4 && declaredFrames == UINT64_MAX) {
                p = file_.View(body, 4);
                if (!p) return false;
                uint32_t count = U32(p, big_);
                if (count) declaredFrames = count;
            }
            unsigned padding = w64 ? 8 : 2;
            at = body + n + (padding - n % padding) % padding;
        }
    }
    if (!formatFound || !dataFound || !rate_ || rate_ > 1048575 || !channels_ || channels_ > 8 || !align_) return false;
    if (format_ == 0x11 || format_ == 2) {
        unsigned header = channels_ * (format_ == 2 ? 7 : 4);
        total_ = bytes_ / align_ * blockFrames_;
        unsigned tail = (unsigned)(bytes_ % align_);
        if (tail >= header) total_ += std::min(blockFrames_, (tail - header) * 2 / channels_ + (format_ == 2 ? 2 : 1));
    } else {
        if ((format_ == 1 && bits_ != 8 && bits_ != 16 && bits_ != 24 && bits_ != 32) ||
            (format_ == 3 && bits_ != 32 && bits_ != 64) ||
            ((format_ == 6 || format_ == 7) && bits_ != 8) ||
            (format_ != 1 && format_ != 3 && format_ != 6 && format_ != 7) ||
            align_ != channels_ * (bits_ / 8)) return false;
        total_ = bytes_ / align_;
    }
    total_ = std::min(total_, declaredFrames);
    return true;
}

// One sample to float. IEEE doubles are clamped to FLT_MAX and NaN/Inf map to 0
// so the mixer never sees a non-finite value; A-law/mu-law expand to 16-bit range.
float PcmDecoder::Sample(const uint8_t* p) const {
    if (format_ == 3) {
        if (bits_ == 32) { uint32_t u = U32(p, big_); float f; memcpy(&f, &u, 4); return std::isfinite(f) ? f : 0; }
        uint64_t u = U64(p, big_); double d; memcpy(&d, &u, 8);
        return std::isfinite(d) ? (float)std::clamp(d, -(double)FLT_MAX, (double)FLT_MAX) : 0;
    }
    if (format_ == 6 || format_ == 7) {
        unsigned u = *p;
        int v;
        if (format_ == 7) {
            u = (~u) & 255;
            v = (((u & 15) << 3) + 132) << ((u >> 4) & 7);
            v = (u & 128) ? 132 - v : v - 132;
        } else {
            u ^= 0x55;
            unsigned e = (u >> 4) & 7;
            v = (u & 15) << 4;
            if (e == 0) v += 8;
            else { v += 264; if (e > 1) v <<= e - 1; }
            if (!(u & 128)) v = -v;
        }
        return v * (1.0f / 32768.0f);
    }
    static constexpr float scale[] = {1.0f / 128.0f, 1.0f / 32768.0f, 1.0f / 8388608.0f, 1.0f / 2147483648.0f};
    return (float)Raw(p) * scale[bits_ / 8 - 1];
}

// One integer sample as its container holds it, sign-extended; WAV's unsigned 8-bit samples
// are re-centred on zero.
int32_t PcmDecoder::Raw(const uint8_t* p) const {
    if (bits_ == 8) return signed8_ ? (int8_t)*p : (int)*p - 128;
    if (bits_ == 16) return (int16_t)U16(p, big_);
    if (bits_ == 32) return (int32_t)U32(p, big_);
    uint32_t u = big_ ? ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) :
                       (p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16);
    return (int32_t)(u << 8) >> 8;
}

// Integer PCM only. A header that declares fewer significant bits than the container holds is
// believed only when every padding bit in the file is zero, as AIFF requires; otherwise the
// container's precision is reported, so no bit is ever dropped.
bool PcmDecoder::ExactFormat(PcmFormat& f) {
    if (format_ != 1) return false;
    if (valid_ && valid_ < bits_) {
        const int32_t padding = (int32_t)((1u << (bits_ - valid_)) - 1);
        bool clear = true;
        // Whole frames per view, so 3-byte samples never straddle two views.
        for (uint64_t at = 0; clear && at < total_ * align_;) {
            const size_t n = (size_t)std::min<uint64_t>(total_ * align_ - at, (1 << 20) / align_ * align_);
            const uint8_t* p = file_.View(start_ + at, n);
            if (!p) return false;
            for (size_t i = 0; clear && i < n; i += bits_ / 8) clear = !(Raw(p + i) & padding);
            at += n;
        }
        if (clear) shift_ = bits_ - valid_;
    }
    f.rate = rate_;
    f.channels = channels_;
    f.bits = bits_ - shift_;
    f.channelMask = mask_;
    f.frames = total_;
    return true;
}

size_t PcmDecoder::ReadExact(int32_t* out, size_t frames, bool& damaged) {
    frames = (size_t)std::min<uint64_t>(frames, total_ - pos_);
    const unsigned width = bits_ / 8;
    // ExactFormat found these bits clear; one set now means the file changed underneath.
    const int32_t padding = (int32_t)((1u << shift_) - 1);
    size_t done = 0;
    while (done < frames) {
        const size_t n = std::min<size_t>(frames - done, 16384);
        const uint8_t* p = file_.View(start_ + pos_ * align_, n * align_);
        if (!p) { damaged = true; break; }
        int32_t* o = out + done * channels_;
        for (size_t i = 0; i < n * channels_; ++i, p += width) {
            const int32_t v = Raw(p);
            if (v & padding) { damaged = true; return done; }
            o[i] = v >> shift_;
        }
        pos_ += n; done += n;
    }
    return done;
}

// Expand one ADPCM block into decoded_. Both layouts carry per-channel
// predictors/deltas in the block header, then 4-bit nibbles; IMA interleaves
// nibble pairs per channel in turn, MS/ADPCM uses a fixed coefficient table.
// Malformed predictor/step indices fail the block.
bool PcmDecoder::Adpcm(uint64_t block) {
    uint64_t offset = block * align_;
    if (offset >= bytes_) return false;
    size_t n = (size_t)std::min<uint64_t>(align_, bytes_ - offset);
    unsigned header = channels_ * (format_ == 2 ? 7 : 4);
    if (n < header) return false;
    const uint8_t* p = file_.View(start_ + offset, n);
    if (!p) return false;
    size_t frames = std::min<size_t>(blockFrames_, (n - header) * 2 / channels_ + (format_ == 2 ? 2 : 1));
    decoded_.assign(frames * channels_, 0);
    int s1[2]{}, s2[2]{}, delta[2]{}, predictor[2]{};
    if (format_ == 2) {
        for (unsigned c = 0; c < channels_; ++c) {
            predictor[c] = p[c];
            if (predictor[c] >= 7) return false;
            delta[c] = U16(p + channels_ + c * 2);
            s1[c] = (int16_t)U16(p + channels_ * 3 + c * 2);
            s2[c] = (int16_t)U16(p + channels_ * 5 + c * 2);
            decoded_[c] = s2[c] * (1.0f / 32768);
            decoded_[channels_ + c] = s1[c] * (1.0f / 32768);
        }
        static const int adapt[16] = {230,230,230,230,307,409,512,614,768,614,512,409,307,230,230,230};
        for (size_t i = channels_ * 2; i < decoded_.size(); ++i) {
            unsigned c = (unsigned)(i % channels_), nibble = (unsigned)(i - channels_ * 2);
            unsigned q = (p[header + nibble / 2] >> ((nibble & 1) ? 0 : 4)) & 15;
            const int* co = coeff_[predictor[c]];
            int64_t v = ((int64_t)s1[c] * co[0] + (int64_t)s2[c] * co[1]) / 256 + (int64_t)(q < 8 ? (int)q : (int)q - 16) * delta[c];
            s2[c] = s1[c];
            s1[c] = (int)std::clamp<int64_t>(v, -32768, 32767);
            delta[c] = (int)std::clamp<int64_t>((int64_t)delta[c] * adapt[q] / 256, 16, INT_MAX);
            decoded_[i] = s1[c] * (1.0f / 32768);
        }
    } else {
        static const int steps[] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
        static const int changes[] = {-1,-1,-1,-1,2,4,6,8};
        for (unsigned c = 0; c < channels_; ++c) {
            s1[c] = (int16_t)U16(p + c * 4);
            delta[c] = p[c * 4 + 2];
            if (delta[c] > 88 || p[c * 4 + 3]) return false;
            decoded_[c] = s1[c] * (1.0f / 32768);
        }
        size_t written[2] = {1,1};
        for (size_t at = header; at < n;) {
            for (unsigned c = 0; c < channels_ && at < n; ++c) {
                size_t stop = std::min(at + 4, n);
                while (at < stop) {
                    unsigned packed = p[at++];
                    for (int half = 0; half < 2; ++half) {
                        unsigned q = (packed >> (half * 4)) & 15;
                        int step = steps[delta[c]], diff = step >> 3;
                        if (q & 1) diff += step >> 2;
                        if (q & 2) diff += step >> 1;
                        if (q & 4) diff += step;
                        s1[c] = std::clamp(s1[c] + ((q & 8) ? -diff : diff), -32768, 32767);
                        delta[c] = std::clamp(delta[c] + changes[q & 7], 0, 88);
                        if (written[c] < frames) decoded_[written[c] * channels_ + c] = s1[c] * (1.0f / 32768);
                        ++written[c];
                    }
                }
            }
        }
        if (channels_ == 2) decoded_.resize(std::min({frames, written[0], written[1]}) * channels_);
    }
    return true;
}

// The SSE2 fast paths cover little-endian mono/stereo 16/24/32-bit and stereo float
// layouts; every other layout goes sample by sample through Mix. ADPCM reads only
// from the current decoded_ block, re-decoding when the cursor crosses one.
size_t PcmDecoder::Read(float* out, size_t frames) {
    if (failed_) return 0;
    frames = (size_t)std::min<uint64_t>(frames, total_ - pos_);
    size_t done = 0;
    while (done < frames) {
        size_t n = std::min<size_t>(frames - done, 16384);
        if (blockFrames_) {
            uint64_t block = pos_ / blockFrames_;
            size_t within = (size_t)(pos_ % blockFrames_);
            if ((decoded_.empty() || !within) && !Adpcm(block)) { failed_ = true; break; }
            if (within >= decoded_.size() / channels_) { failed_ = true; break; }
            n = std::min(n, decoded_.size() / channels_ - within);
            for (size_t i = 0; i < n; ++i) Mix(decoded_.data() + (within + i) * channels_, channels_, out + (done + i) * 2);
        } else {
            const uint8_t* p = file_.View(start_ + pos_ * align_, n * align_);
            if (!p) { failed_ = true; break; }
            float* dst = out + done * 2;
            if (!big_ && format_ == 1 && channels_ == 2 && bits_ == 16) Stereo16(p, dst, n * 2);
            else if (!big_ && format_ == 1 && channels_ == 2 && bits_ == 32) Stereo32(p, dst, n * 2);
            else if (!big_ && format_ == 1 && channels_ == 2 && bits_ == 24) IntegerPcm<24, false>(p, dst, n);
            else if (!big_ && format_ == 1 && channels_ == 1 && bits_ == 16) IntegerPcm<16, true>(p, dst, n);
            else if (!big_ && format_ == 1 && channels_ == 1 && bits_ == 24) IntegerPcm<24, true>(p, dst, n);
            else if (!big_ && format_ == 1 && channels_ == 1 && bits_ == 32) IntegerPcm<32, true>(p, dst, n);
            else if (!big_ && format_ == 3 && channels_ == 2 && bits_ == 32) StereoFloat32(p, dst, n * 2);
            else {
                unsigned width = bits_ / 8;
                for (size_t i = 0; i < n; ++i, p += align_) {
                    float samples[8]{};
                    for (unsigned c = 0; c < channels_; ++c) samples[c] = Sample(p + c * width);
                    Mix(samples, channels_, dst + i * 2);
                }
            }
        }
        pos_ += n; done += n;
    }
    return done;
}

// Seeking only moves the frame cursor; an ADPCM seek additionally re-decodes the
// containing block into decoded_ so the cursor is valid immediately.
bool PcmDecoder::Seek(double seconds) {
    if (!std::isfinite(seconds)) return false;
    pos_ = seconds <= 0 ? 0 : seconds >= Duration() ? total_ : (uint64_t)(seconds * rate_);
    decoded_.clear();
    failed_ = blockFrames_ && pos_ < total_ && pos_ % blockFrames_ && !Adpcm(pos_ / blockFrames_);
    return !failed_;
}
} // namespace

std::unique_ptr<Decoder> OpenPcmDecoder(const std::wstring& path) {
    auto d = std::make_unique<PcmDecoder>();
    return d->Open(path) ? std::move(d) : nullptr;
}

namespace {
// Exact reading through the same container parsing. PCM has no checksum: a file shorter than its
// header claims does not open, and PcmReader checks the length read.
class PcmExact final : public PcmReader {
public:
    bool Open(const std::wstring& path) { return decoder_.Open(path) && decoder_.ExactFormat(format_); }
private:
    size_t Next(int32_t* out, size_t frames) override { return decoder_.ReadExact(out, frames, damaged_); }
    PcmDecoder decoder_;
};
} // namespace

std::unique_ptr<PcmReader> OpenPcmExact(const std::wstring& path) {
    auto r = std::make_unique<PcmExact>();
    return r->Open(path) ? std::move(r) : nullptr;
}
