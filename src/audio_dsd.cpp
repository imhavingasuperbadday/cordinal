// DSD: DSF (Sony) and uncompressed DSDIFF (Philips .dff) 1-bit streams, turned into PCM by one
// linear-phase low-pass FIR evaluated a byte at a time. The decimation keeps the output near
// 88.2 kHz (DSD64 / 32, DSD128 / 64, ...), and the filter, 32 taps per decimation step, passes
// the audio band flat and removes DSD's rising noise before the output's Nyquist frequency. The
// filter is centred on each output sample and has no state, so seeking is exact. DST-compressed
// DSDIFF is not read.
#include "audio_file.h"
#include "native_audio.h"

#include <cmath>
#include <vector>

namespace {
using namespace nativeaudio;

constexpr uint8_t kSilence = 0x69;  // the idle pattern: equal ones and zeros
constexpr size_t kChunk = 1024;     // output frames per filter pass
constexpr double kPi = 3.14159265358979323846;

uint8_t Reverse(uint8_t b) {
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

// Modified Bessel function of the first kind, order 0, for the Kaiser window.
double BesselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 32; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

class DsdDecoder final : public Decoder {
public:
    bool Open(const std::wstring& path) {
        if (!file_.Open(path) || file_.Size() < 16) return false;
        file_.Sequential();
        const uint8_t* h = file_.View(0, 16);
        if (!h) return false;
        if (Is(h, "DSD ")) return OpenDsf();
        if (Is(h, "FRM8")) return OpenDff();
        return false;
    }

    size_t Read(float* out, size_t frames) override {
        frames = (size_t)std::min<uint64_t>(frames, frames_ - pos_);
        size_t done = 0;
        while (done < frames) {
            const size_t n = std::min(kChunk, frames - done);
            Filter(out + done * 2, n);
            done += n;
            pos_ += n;
        }
        return done;
    }

    bool Seek(double seconds) override {
        if (!std::isfinite(seconds)) return false;
        pos_ = std::min<uint64_t>(frames_, (uint64_t)std::llround(std::max(0.0, seconds) * rate_));
        return true;
    }
    double Duration() const override { return (double)frames_ / rate_; }
    unsigned Rate() const override { return rate_; }

private:
    bool OpenDsf() {
        const uint8_t* h = file_.View(0, 92);
        // The fmt chunk: version, format (0: raw DSD), channel type and count, rate, bit order,
        // samples per channel, block size per channel.
        if (!h || U64(h + 4) != 28 || !Is(h + 28, "fmt ") || U32(h + 28 + 16) != 0) return false;
        const unsigned type = U32(h + 28 + 20);
        channels_ = U32(h + 28 + 24);
        dsdRate_ = U32(h + 28 + 28);
        const unsigned bits = U32(h + 28 + 32);
        const uint64_t samples = U64(h + 28 + 36);
        block_ = U32(h + 28 + 44);
        const uint64_t data = 28 + U64(h + 28 + 4);
        const uint8_t* d = file_.View(data, 12);
        if (!d || !Is(d, "data") || (bits != 1 && bits != 8) || !block_ || block_ > (1u << 20)) return false;
        lsbFirst_ = bits == 1;
        data_ = data + 12;
        bytes_ = (samples + 7) / 8;
        const uint64_t blocks = (bytes_ + block_ - 1) / block_;
        if (blocks > (file_.Size() - data_) / block_ / std::max(channels_, 1u)) return false;
        // Channel types 1..7: mono, stereo, 3 (L R C), quad, 4 (L R C LFE), 5 (L R C Ls Rs), 5.1.
        static const char* const kLayouts[] = {"", "M", "LR", "LRC", "LRlr", "LRCF", "LRClr", "LRCFlr"};
        if (type < 1 || type > 7 || std::strlen(kLayouts[type]) != channels_) return false;
        return Setup(kLayouts[type]);
    }

    bool OpenDff() {
        const uint8_t* h = file_.View(0, 16);
        if (!h || !Is(h + 12, "DSD ")) return false;
        std::string layout;
        bool compressed = false;
        for (uint64_t pos = 16; pos + 12 <= file_.Size();) {
            const uint8_t* c = file_.View(pos, 12);
            if (!c) return false;
            const uint64_t len = U64(c + 4, true);
            if (Is(c, "PROP")) {  // its sub-chunks follow its "SND " form type
                pos += 16;
                continue;
            }
            const uint8_t* body = file_.View(pos + 12, (size_t)std::min<uint64_t>(len, 4096));
            if (!body && len) return false;
            if (Is(c, "FS  ") && len >= 4) dsdRate_ = U32(body, true);
            else if (Is(c, "CHNL") && len >= 2) {
                channels_ = U16(body, true);
                if (len < 2 + 4ull * channels_) return false;
                for (unsigned i = 0; i < channels_; ++i) {
                    const uint8_t* id = body + 2 + i * 4;
                    layout += Is(id, "SLFT") || Is(id, "MLFT") ? 'L' : Is(id, "SRGT") || Is(id, "MRGT") ? 'R'
                            : Is(id, "C   ") ? 'C' : Is(id, "LFE ") ? 'F' : Is(id, "LS  ") ? 'l' : Is(id, "RS  ") ? 'r' : '?';
                }
            } else if (Is(c, "CMPR") && len >= 4) compressed = !Is(body, "DSD ");
            else if (Is(c, "DSD ")) {
                data_ = pos + 12;
                bytes_ = channels_ ? std::min(len, file_.Size() - data_) / channels_ : 0;
                break;
            } else if (Is(c, "DST ")) return false;
            if (len > file_.Size()) return false;
            pos += 12 + len + (len & 1);
        }
        if (compressed || !data_ || !channels_) return false;
        if (channels_ == 1) layout = "M";
        return Setup(layout);
    }

    // Mixing weights from the layout, the decimation and the filter tables.
    bool Setup(const std::string& layout) {
        if (dsdRate_ < 1000000 || dsdRate_ > 50000000 || !channels_ || channels_ > 8 || layout.size() != channels_) return false;
        decimation_ = 8;
        while (dsdRate_ / (2 * decimation_) >= 88200) decimation_ *= 2;
        rate_ = dsdRate_ / decimation_;
        step_ = decimation_ / 8;
        frames_ = bytes_ / step_;
        // Centre and surrounds fold into the front pair at -3 dB, as the PCM decoders do; LFE is left out.
        const bool surround = layout.find_first_of("Clr") != std::string::npos;
        const float c = 0.7071f, norm = surround ? 1.0f / (1.0f + 2 * c) : 1.0f;
        for (char ch : layout) {
            float l = 0, r = 0;
            switch (ch) {
                case 'M': l = r = 1; break;
                case 'L': l = 1; break;
                case 'R': r = 1; break;
                case 'C': l = r = c; break;
                case 'l': l = c; break;
                case 'r': r = c; break;
                default: break;
            }
            weights_.push_back(l * norm);
            weights_.push_back(r * norm);
        }
        // Kaiser-windowed sinc, cut off at 34% of the output rate (30 kHz for 88.2 kHz).
        taps_ = 4 * decimation_;  // bytes: 32 bits per decimation step
        const size_t bits = taps_ * 8;
        const double fc = 0.34 * rate_ / dsdRate_, beta = 9, centre = (bits - 1) / 2.0;
        std::vector<double> h(bits);
        double sum = 0;
        for (size_t i = 0; i < bits; ++i) {
            const double x = i - centre, w = (double)i / (bits - 1) * 2 - 1;
            h[i] = (x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x)) * BesselI0(beta * std::sqrt(1 - w * w));
            sum += h[i];
        }
        table_.assign(taps_ * 256, 0.0f);
        for (size_t t = 0; t < taps_; ++t)
            for (int b = 0; b < 256; ++b) {
                double v = 0;
                for (int k = 0; k < 8; ++k) v += h[t * 8 + k] * ((b >> (7 - k) & 1) ? 1.0 : -1.0);
                table_[t * 256 + b] = (float)(v / sum);
            }
        return frames_ > 0;
    }

    // Channel c's bytes [first, first + n), MSB first, with silence outside the stream.
    void Gather(unsigned c, int64_t first, size_t n, uint8_t* out) {
        size_t i = 0;
        for (; i < n && first + (int64_t)i < 0; ++i) out[i] = kSilence;
        while (i < n) {
            const uint64_t at = (uint64_t)(first + (int64_t)i);
            if (at >= bytes_) {
                std::fill(out + i, out + n, kSilence);
                return;
            }
            if (block_) {  // DSF: blocks of one channel at a time
                const uint64_t block = at / block_;
                const size_t within = (size_t)(at % block_);
                const size_t run = (size_t)std::min<uint64_t>({n - i, block_ - within, bytes_ - at});
                const uint8_t* p = file_.View(data_ + (block * channels_ + c) * block_ + within, run);
                if (p) std::memcpy(out + i, p, run);
                else std::fill(out + i, out + i + run, kSilence);
                if (lsbFirst_)
                    for (size_t k = 0; k < run; ++k) out[i + k] = Reverse(out[i + k]);
                i += run;
            } else {  // DSDIFF: one byte of each channel in turn
                const size_t run = (size_t)std::min<uint64_t>(n - i, bytes_ - at);
                const uint8_t* p = file_.View(data_ + at * channels_, run * channels_);
                for (size_t k = 0; k < run; ++k) out[i + k] = p ? p[k * channels_ + c] : kSilence;
                i += run;
            }
        }
    }

    void Filter(float* out, size_t n) {
        std::fill(out, out + n * 2, 0.0f);
        const size_t span = (n - 1) * step_ + taps_;
        bytes1_.resize(span);
        const int64_t first = (int64_t)(pos_ * step_) - (int64_t)(taps_ / 2) + (int64_t)step_ / 2;
        for (unsigned c = 0; c < channels_; ++c) {
            const float wl = weights_[c * 2], wr = weights_[c * 2 + 1];
            if (wl == 0 && wr == 0) continue;
            Gather(c, first, span, bytes1_.data());
            for (size_t k = 0; k < n; ++k) {
                const uint8_t* b = bytes1_.data() + k * step_;
                float v = 0;
                for (size_t t = 0; t < taps_; ++t) v += table_[t * 256 + b[t]];
                out[k * 2] += v * wl;
                out[k * 2 + 1] += v * wr;
            }
        }
    }

    File file_;
    unsigned channels_ = 0, dsdRate_ = 0, rate_ = 0, decimation_ = 0, block_ = 0;
    size_t step_ = 0, taps_ = 0;
    bool lsbFirst_ = false;
    uint64_t data_ = 0, bytes_ = 0;  // bytes_: per channel
    uint64_t frames_ = 0, pos_ = 0;
    std::vector<float> weights_, table_;
    std::vector<uint8_t> bytes1_;
};

}  // namespace

std::unique_ptr<Decoder> OpenDsdDecoder(const std::wstring& path) {
    auto d = std::make_unique<DsdDecoder>();
    if (!d->Open(path)) return nullptr;
    return d;
}
