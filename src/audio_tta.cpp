// Cordinal True Audio (TTA1) decoder. TTA stores fixed-length, independent frames: an
// adaptive Rice residual for each channel, an 8-tap sign-sign LMS filter followed by a
// fixed first-order predictor, and a channel decorrelation pass. Every frame is checked
// against its trailing CRC-32 before any of its samples are returned.
#include "native_audio.h"
#include "audio_file.h"
#include "audio_kernels.h"
#include "audio_lsb.h"
#include "cord_codec.h"
#include "inflate.h"

#include <array>
#include <cmath>

namespace {
using namespace nativeaudio;

// Rice parameter adaptation thresholds. shift16[k] is the sum below which k falls and
// above which k+1 rises; the spec's tables saturate like the reference decoder's.
struct RiceTables {
    std::array<uint32_t, 41> one{}, sixteen{};
    constexpr RiceTables() {
        for (unsigned k = 0; k < 31; ++k) one[k] = 1u << k;
        for (unsigned k = 31; k < 40; ++k) one[k] = 0x80000000u;
        one[40] = 0xFFFFFFFFu;
        for (unsigned k = 0; k <= 36; ++k) sixteen[k] = one[k + 4];
    }
};
constexpr RiceTables riceTables;

inline int32_t WrapAdd(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
inline int32_t WrapSub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
inline int32_t WrapNeg(int32_t a) { return (int32_t)(0u - (uint32_t)a); }

// One channel's adaptive state. TTA resets all of it at every frame, so a seek can
// decode from any frame boundary without reading anything before it.
struct TtaChannel {
    int32_t qm[8]{}, dx[8]{}, dl[8]{}, error = 0, predictor = 0;
    uint32_t k0 = 10, k1 = 10, sum0 = 1u << 14, sum1 = 1u << 14;
};

class TtaDecoder final : public Decoder {
public:
    bool Open(const std::wstring& path) {
        if (!file_.Open(path)) return false;
        // Taggers may prepend an ID3v2 block; the TTA1 header and frame table follow it.
        uint64_t base = 0;
        if (const uint8_t* id3 = file_.View(0, 10); id3 && !memcmp(id3, "ID3", 3)) {
            if ((id3[6] | id3[7] | id3[8] | id3[9]) & 0x80) return false;
            base = 10 + ((uint64_t)id3[6] << 21) + ((uint64_t)id3[7] << 14) + ((uint64_t)id3[8] << 7) + id3[9] +
                   ((id3[5] & 0x10) ? 10 : 0);
        }
        const uint8_t* header = file_.View(base, 22);
        if (!header || memcmp(header, "TTA1", 4)) return false;
        const unsigned format = U16(header + 4);
        channels_ = U16(header + 6);
        bits_ = U16(header + 8);
        rate_ = U32(header + 10);
        dataLength_ = U32(header + 14);
        // Encrypted streams (format 2) need a password, which Cordinal does not take.
        if (format != 1 || !channels_ || channels_ > 8 || !rate_ || rate_ > 768000 ||
            (bits_ != 8 && bits_ != 16 && bits_ != 24) || !dataLength_) return false;
        if (Crc32(header, 18) != U32(header + 18)) return false;
        // TTA1's frame length is derived, not stored: 256/245 of a second, so a frame
        // covers a whole number of samples at every supported rate.
        frameSamples_ = (uint64_t)rate_ * 256 / 245;
        if (!frameSamples_) return false;
        frameCount_ = (dataLength_ + frameSamples_ - 1) / frameSamples_;
        if (!frameCount_ || frameCount_ > 1u << 24) return false;
        frames_ = dataLength_;
        const uint64_t tableBytes = frameCount_ * 4;
        if (base > file_.Size() || tableBytes > file_.Size() - base ||
            22 + tableBytes + 4 > file_.Size() - base) return false;
        const uint8_t* sizes = file_.View(base + 22, (size_t)tableBytes + 4);
        if (!sizes || Crc32(sizes, (size_t)tableBytes) != U32(sizes + tableBytes)) return false;
        // The seek table holds each frame's byte length; offsets are their running sum.
        // Trailing ID3v1/APEv2 tags are allowed after the last frame.
        uint64_t at = base + 22 + tableBytes + 4;
        framesIndex_.reserve((size_t)frameCount_);
        for (uint64_t i = 0; i < frameCount_; ++i) {
            const uint32_t bytes = U32(sizes + i * 4);
            if (bytes < 4 || at + bytes > file_.Size()) return false;
            const uint64_t samples = i + 1 == frameCount_ ? dataLength_ - (frameCount_ - 1) * frameSamples_ : frameSamples_;
            framesIndex_.push_back({at, bytes, (uint32_t)samples});
            at += bytes;
        }
        shift_ = bits_ == 16 ? 9 : bits_ == 24 ? 10 : 10;
        round_ = 1u << (shift_ - 1);
        predictionShift_ = bits_ == 8 ? 4 : 5;
        scale_ = 1.0f / (float)(1u << (bits_ - 1));
        return Seek(0);
    }

    bool LosslessFormat(PcmFormat& f) const override {
        if (channels_ > 2) return false;
        f.rate = rate_;
        f.channels = channels_;
        f.bits = bits_;
        f.frames = frames_;
        return true;
    }

    size_t Read(float* out, size_t count) override {
        if (failed_) return 0;
        size_t done = 0;
        while (done < count && position_ < frames_) {
            if (used_ == pcm_.size() / 2) {
                if (next_ == framesIndex_.size()) break;
                if (!DecodeFrame(next_++)) { pcm_.clear(); failed_ = true; break; }
                used_ = skip_;
                skip_ = 0;
            }
            const size_t take = (size_t)std::min<uint64_t>({count - done, pcm_.size() / 2 - used_, frames_ - position_});
            memcpy(out + done * 2, pcm_.data() + used_ * 2, take * 2 * sizeof(float));
            used_ += take;
            position_ += take;
            done += take;
        }
        return done;
    }

    bool Seek(double seconds) override {
        if (!std::isfinite(seconds)) return false;
        position_ = (uint64_t)std::min<double>((double)frames_, std::max(0.0, std::floor(seconds * rate_)));
        next_ = position_ == frames_ ? framesIndex_.size() : (size_t)(position_ / frameSamples_);
        skip_ = position_ == frames_ ? 0 : (size_t)(position_ % frameSamples_);
        pcm_.clear();
        used_ = 0;
        failed_ = false;
        return true;
    }
    bool FastSeek() const override { return true; }
    bool CanPreview() const override { return true; }
    double Duration() const override { return (double)frames_ / rate_; }
    unsigned Rate() const override { return rate_; }
    bool Failed() const override { return failed_; }

private:
    struct Frame { uint64_t offset; uint32_t bytes, samples; };

    // The adaptive filter: sign-sign weight update, then an 8-tap prediction from the
    // reconstructed history. The wrapping arithmetic is part of the format.
    __forceinline int32_t Filter(TtaChannel& c, int32_t in) {
        uint32_t* qm = (uint32_t*)c.qm;
        // The update direction is the coded residual's sign, which alternates with the
        // waveform, so branching on it mispredicts. Mask the addend instead.
        const uint32_t up = 0u - (uint32_t)(c.error > 0);
        const uint32_t down = 0u - (uint32_t)(c.error < 0);
        uint32_t sum = round_;
        if (fast_) sum = TtaFilterAvx2(c.qm, c.dx, c.dl, c.error, round_);
        else for (unsigned i = 0; i < 8; ++i) {
            qm[i] += ((uint32_t)c.dx[i] & up) - ((uint32_t)c.dx[i] & down);
            sum += (uint32_t)c.dl[i] * qm[i];
        }
        for (unsigned i = 0; i < 4; ++i) {
            c.dx[i] = c.dx[i + 1];
            c.dl[i] = c.dl[i + 1];
        }
        c.dx[4] = (c.dl[4] >> 30) | 1;
        c.dx[5] = ((c.dl[5] >> 30) | 2) & ~1;
        c.dx[6] = ((c.dl[6] >> 30) | 2) & ~1;
        c.dx[7] = ((c.dl[7] >> 30) | 4) & ~3;
        c.error = in;
        in = WrapAdd(in, (int32_t)sum >> shift_);
        c.dl[4] = WrapNeg(c.dl[5]);
        c.dl[5] = WrapNeg(c.dl[6]);
        c.dl[6] = WrapSub(in, c.dl[7]);
        c.dl[7] = in;
        c.dl[5] = WrapAdd(c.dl[5], c.dl[6]);
        c.dl[4] = WrapAdd(c.dl[4], c.dl[5]);
        return in;
    }

    // The Rice parameters follow the running sums and produce one signed residual before
    // the adaptive LMS filter runs. Splitting Rice decode from Filter lets each channel's
    // 8-tap LMS filter run chunked in YMM registers with zero store-forwarding stalls.
    __forceinline int32_t DecodeResidual(TtaChannel& c, LittleBits& bits, LittleWindow& w) {
        uint32_t unary;
        bool second;
        uint32_t k;
        uint32_t value;
        if (w.Safe()) {
            w.Refill();
            unary = w.Run();
            if (unary < 32) {
                w.Drop(unary + 1);
                second = unary != 0;
                if (second) --unary;
                k = second ? c.k1 : c.k0;
                value = unary;
                if (k) {
                    if (k > 31 || unary > (uint32_t)INT32_MAX >> k) { w.Commit(); bits.Fail(); return 0; }
                    w.Refill();
                    value = (unary << k) | w.Get(k);
                }
                goto adapt;
            }
        }
        w.Commit();
        unary = bits.Ones(1u << 20);
        second = unary != 0;
        if (second) --unary;
        k = second ? c.k1 : c.k0;
        if (k) {
            if (k > 31 || unary > (uint32_t)INT32_MAX >> k) { bits.Fail(); return 0; }
            value = (unary << k) | bits.Get(k);
        } else {
            value = unary;
        }
        w.Load();
    adapt:
        if (second) {
            c.sum1 += value - (c.sum1 >> 4);
            const bool lower = c.k1 > 0 && c.sum1 < riceTables.sixteen[c.k1];
            c.k1 += lower ? -1 : (c.sum1 > riceTables.sixteen[c.k1 + 1] ? 1 : 0);
            value += riceTables.one[c.k0];
        }
        c.sum0 += value - (c.sum0 >> 4);
        const bool lower = c.k0 > 0 && c.sum0 < riceTables.sixteen[c.k0];
        c.k0 += lower ? -1 : (c.sum0 > riceTables.sixteen[c.k0 + 1] ? 1 : 0);
        if (c.k0 > 31) c.k0 = 31;
        if (c.k1 > 31) c.k1 = 31;
        return 1 + ((value >> 1) ^ ((value & 1) - 1));
    }

    __forceinline int32_t FilterAndPredict(TtaChannel& c, int32_t residual) {
        int32_t res = Filter(c, residual);
        const uint64_t previous = (uint64_t)(int64_t)c.predictor;
        res += (int32_t)(((previous << predictionShift_) - previous) >> predictionShift_);
        c.predictor = res;
        return res;
    }

    __forceinline int32_t Sample(TtaChannel& c, LittleBits& bits, LittleWindow& w) {
        return FilterAndPredict(c, DecodeResidual(c, bits, w));
    }

    static void EmitMonoChunk(const int32_t* in, size_t n, float scale, float* out) {
        const __m128 vs = _mm_set1_ps(scale);
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            const __m128 f = _mm_mul_ps(_mm_cvtepi32_ps(_mm_loadu_si128((const __m128i*)(in + i))), vs);
            _mm_storeu_ps(out + i * 2, _mm_unpacklo_ps(f, f));
            _mm_storeu_ps(out + i * 2 + 4, _mm_unpackhi_ps(f, f));
        }
        for (; i < n; ++i) {
            const float v = (float)in[i] * scale;
            out[i * 2] = v;
            out[i * 2 + 1] = v;
        }
    }

    static void EmitStereoChunk(const int32_t* in0, const int32_t* in1, size_t n, float scale, float* out) {
        const __m128 vs = _mm_set1_ps(scale);
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            const __m128i f = _mm_loadu_si128((const __m128i*)(in0 + i));
            const __m128i r = _mm_loadu_si128((const __m128i*)(in1 + i));
            const __m128i halfF = _mm_srai_epi32(_mm_sub_epi32(f, _mm_srai_epi32(f, 31)), 1);
            const __m128i last = _mm_add_epi32(r, halfF);
            const __m128i first = _mm_sub_epi32(last, f);
            const __m128 pf = _mm_mul_ps(_mm_cvtepi32_ps(first), vs);
            const __m128 pl = _mm_mul_ps(_mm_cvtepi32_ps(last), vs);
            _mm_storeu_ps(out + i * 2, _mm_unpacklo_ps(pf, pl));
            _mm_storeu_ps(out + i * 2 + 4, _mm_unpackhi_ps(pf, pl));
        }
        for (; i < n; ++i) {
            int32_t first = in0[i];
            int32_t last = WrapAdd(in1[i], first / 2);
            first = WrapSub(last, first);
            out[i * 2] = (float)first * scale;
            out[i * 2 + 1] = (float)last * scale;
        }
    }

    bool DecodeFrame(size_t index) {
        const Frame& frame = framesIndex_[index];
        const uint8_t* bytes = file_.View(frame.offset, frame.bytes);
        if (!bytes) return false;
        // The trailing CRC covers the compressed frame body; cord's CRC-32 is the same polynomial
        // taken eight bytes a step.
        if (cord::Crc32(bytes, frame.bytes - 4) != U32(bytes + frame.bytes - 4)) return false;
        LittleBits bits({bytes, (size_t)(frame.bytes - 4)});
        std::array<TtaChannel, 8> state{};
        pcm_.resize((size_t)frame.samples * 2);
        const unsigned channels = channels_;
        LittleWindow window(bits);
        constexpr uint32_t kChunk = 2048;
        if (channels == 1) {
            TtaChannel& c = state[0];
            int32_t buf0[kChunk];
            for (uint32_t base = 0; base < frame.samples; base += kChunk) {
                const uint32_t n = std::min(kChunk, frame.samples - base);
                for (uint32_t i = 0; i < n; ++i) buf0[i] = DecodeResidual(c, bits, window);
                if (!bits.Good()) { window.Commit(); return false; }
                if (fast_) TtaFilterBlockAvx2(c.qm, c.dx, c.dl, c.error, c.predictor, buf0, n, round_, shift_, predictionShift_);
                else for (uint32_t i = 0; i < n; ++i) buf0[i] = FilterAndPredict(c, buf0[i]);
                EmitMonoChunk(buf0, n, scale_, pcm_.data() + size_t(base) * 2);
            }
        } else if (channels == 2) {
            TtaChannel& left = state[0];
            TtaChannel& right = state[1];
            int32_t buf0[kChunk], buf1[kChunk];
            for (uint32_t base = 0; base < frame.samples; base += kChunk) {
                const uint32_t n = std::min(kChunk, frame.samples - base);
                for (uint32_t i = 0; i < n; ++i) {
                    buf0[i] = DecodeResidual(left, bits, window);
                    buf1[i] = DecodeResidual(right, bits, window);
                }
                if (!bits.Good()) { window.Commit(); return false; }
                if (fast_) {
                    TtaFilterBlockAvx2(left.qm, left.dx, left.dl, left.error, left.predictor, buf0, n, round_, shift_, predictionShift_);
                    TtaFilterBlockAvx2(right.qm, right.dx, right.dl, right.error, right.predictor, buf1, n, round_, shift_, predictionShift_);
                } else {
                    for (uint32_t i = 0; i < n; ++i) buf0[i] = FilterAndPredict(left, buf0[i]);
                    for (uint32_t i = 0; i < n; ++i) buf1[i] = FilterAndPredict(right, buf1[i]);
                }
                EmitStereoChunk(buf0, buf1, n, scale_, pcm_.data() + size_t(base) * 2);
            }
        } else {
            int32_t values[8];
            for (uint32_t i = 0; i < frame.samples; ++i) {
                for (unsigned c = 0; c < channels; ++c) values[c] = Sample(state[c], bits, window);
                // The last channel carries the running sum; walk backwards turning each
                // pair into the difference and its reconstruction.
                values[channels - 1] = WrapAdd(values[channels - 1], values[channels - 2] / 2);
                for (unsigned c = channels - 1; c > 0; --c) values[c - 1] = WrapSub(values[c], values[c - 1]);
                float mixed[8];
                for (unsigned c = 0; c < channels; ++c) mixed[c] = (float)values[c] * scale_;
                Mix(mixed, channels, pcm_.data() + i * 2);
            }
        }
        window.Commit();
        return bits.Good();
    }

    File file_;
    std::vector<Frame> framesIndex_;
    std::vector<float> pcm_;
    uint64_t frames_ = 0, frameCount_ = 0, dataLength_ = 0, frameSamples_ = 0, position_ = 0;
    size_t next_ = 0, used_ = 0, skip_ = 0;
    unsigned channels_ = 0, bits_ = 0, rate_ = 0, shift_ = 0, predictionShift_ = 0;
    uint32_t round_ = 0;
    float scale_ = 0;
    bool failed_ = false;
    const bool fast_ = FastAudioKernels();
};

}  // namespace

std::unique_ptr<Decoder> OpenTtaDecoder(const std::wstring& path) {
    auto decoder = std::make_unique<TtaDecoder>();
    if (!decoder->Open(path)) return nullptr;
    return decoder;
}
