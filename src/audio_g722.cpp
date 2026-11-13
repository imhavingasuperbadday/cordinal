// Cordinal G.722 decoder (ITU-T G.722, 64 kbit/s mode), written from the recommendation's block
// descriptions. Each byte holds a 6-bit lower-band and a 2-bit higher-band ADPCM code. Each band
// has an adaptive quantizer scale and a pole-zero predictor; a 24-tap QMF then joins the two
// 8 kHz bands into 16 kHz audio. Raw .g722 files are 16 kHz mono; WAV (tag 0x028F) and AU
// (encoding 24) files give their own channel count. The 56 and 48 kbit/s modes, which drop low
// bits that a 64 kbit/s file keeps, are not needed for files.
#include "native_audio.h"
#include "audio_coded.h"

namespace {
using namespace nativeaudio;

const int kWl[8] = {-60, -30, 58, 172, 334, 538, 1198, 3042};
const int kRl42[16] = {0, 7, 6, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3, 2, 1, 0};
const int kIlb[32] = {2048, 2093, 2139, 2186, 2233, 2282, 2332, 2383, 2435, 2489, 2543, 2599, 2656, 2714, 2774, 2834,
                      2896, 2960, 3025, 3091, 3158, 3228, 3298, 3371, 3444, 3520, 3597, 3676, 3756, 3838, 3922, 4008};
const int kWh[3] = {0, -214, 798};
const int kRh2[4] = {2, 1, 2, 1};
const int kQm2[4] = {-7408, -1616, 7408, 1616};
const int kQm4[16] = {0, -20456, -12896, -8968, -6288, -4240, -2584, -1200, 20456, 12896, 8968, 6288, 4240, 2584, 1200, 0};
const int kQm6[64] = {-136, -136, -136, -136, -24808, -21904, -19008, -16704, -14984, -13512, -12280, -11192, -10232,
                      -9360, -8576, -7856, -7192, -6576, -6000, -5456, -4944, -4464, -4008, -3576, -3168, -2776, -2400,
                      -2032, -1688, -1360, -1040, -728, 24808, 21904, 19008, 16704, 14984, 13512, 12280, 11192, 10232,
                      9360, 8576, 7856, 7192, 6576, 6000, 5456, 4944, 4464, 4008, 3576, 3168, 2776, 2400, 2032, 1688,
                      1360, 1040, 728, 432, 136, -432, -136};
const int kQmf[12] = {3, -11, 12, 32, -210, 951, 3876, -805, 362, -156, 53, -11};

inline int Sat(int v) { return std::clamp(v, -32768, 32767); }

// One band's adaptive predictor: two poles (a) on the reconstructed signal and six zeros (b) on
// the quantized differences, updated by sign agreement (block 4 of the recommendation).
struct Band {
    int s = 0, sp = 0, sz = 0, nb = 0, det = 0;
    int r[3]{}, a[3]{}, ap[3]{}, p[3]{}, d[7]{}, b[7]{}, bp[7]{}, sg[7]{};

    void Update(int dq) {
        d[0] = dq;
        r[0] = Sat(s + dq);
        p[0] = Sat(sz + dq);
        // UPPOL2: the second pole.
        for (int i = 0; i < 3; ++i) sg[i] = p[i] >> 15;
        int wd1 = Sat(a[1] * 4);
        int wd2 = sg[0] == sg[1] ? -wd1 : wd1;
        if (wd2 > 32767) wd2 = 32767;
        int wd3 = (wd2 >> 7) + (sg[0] == sg[2] ? 128 : -128);
        wd3 += (a[2] * 32512) >> 15;
        ap[2] = std::clamp(wd3, -12288, 12288);
        // UPPOL1: the first pole, kept inside the stability triangle.
        sg[0] = p[0] >> 15;
        sg[1] = p[1] >> 15;
        wd1 = sg[0] == sg[1] ? 192 : -192;
        wd2 = (a[1] * 32640) >> 15;
        ap[1] = Sat(wd1 + wd2);
        wd3 = Sat(15360 - ap[2]);
        ap[1] = std::clamp(ap[1], -wd3, wd3);
        // UPZERO: the six zeros.
        wd1 = dq == 0 ? 0 : 128;
        sg[0] = dq >> 15;
        for (int i = 1; i < 7; ++i) {
            sg[i] = d[i] >> 15;
            wd2 = sg[i] == sg[0] ? wd1 : -wd1;
            wd3 = (b[i] * 32640) >> 15;
            bp[i] = Sat(wd2 + wd3);
        }
        // DELAYA
        for (int i = 6; i > 0; --i) { d[i] = d[i - 1]; b[i] = bp[i]; }
        for (int i = 2; i > 0; --i) { r[i] = r[i - 1]; p[i] = p[i - 1]; a[i] = ap[i]; }
        // FILTEP, FILTEZ, PREDIC: the next prediction.
        wd1 = (a[1] * Sat(r[1] + r[1])) >> 15;
        wd2 = (a[2] * Sat(r[2] + r[2])) >> 15;
        sp = Sat(wd1 + wd2);
        sz = 0;
        for (int i = 6; i > 0; --i) sz += (b[i] * Sat(d[i] + d[i])) >> 15;
        sz = Sat(sz);
        s = Sat(sp + sz);
    }
};

class G722Decoder final : public PacketDecoder {
public:
    bool Open(const std::wstring& path, const CodedStream* s) {
        if (!media_.file.Open(path)) return false;
        uint64_t start = 0, bytes = media_.file.Size(), frames = UINT64_MAX;
        unsigned rate = 16000;
        channels_ = 1;
        if (s) {
            start = s->start; bytes = s->bytes; channels_ = s->channels;
            // A code byte makes two output samples; the header may give either rate.
            rate = s->rate == 8000 ? 16000 : s->rate;
            if (s->frames != UINT64_MAX) frames = s->frames;
        }
        if (!channels_ || channels_ > 2 || !rate || rate > 1048575) return false;
        Reset();
        return FixedPackets(media_, start, bytes, 4096 * channels_, 4096 * 2, frames) && Ready(rate);
    }

private:
    struct Codec : State {
        Band band[2][2];  // [channel][low, high]
        int x[2][24]{};   // the QMF's history per channel
    };
    Codec c_;
    unsigned channels_ = 1;

    // Lower band (blocks 5L, 6L, 2L, 3L): the 6-bit code makes the output, its top 4 bits drive
    // the adaptation.
    static int Low(Band& band, unsigned code) {
        const int rlow = std::clamp(band.s + ((band.det * kQm6[code]) >> 15), -16384, 16383);
        const unsigned top = code >> 2;
        const int dlow = (band.det * kQm4[top]) >> 15;
        band.nb = std::clamp(((band.nb * 127) >> 7) + kWl[kRl42[top]], 0, 18432);
        const int shift = 8 - (band.nb >> 11);
        const int scale = kIlb[(band.nb >> 6) & 31];
        band.det = (shift < 0 ? scale << -shift : scale >> shift) << 2;
        band.Update(dlow);
        return rlow;
    }
    // Higher band (blocks 2H, 3H): a 2-bit code.
    static int High(Band& band, unsigned code) {
        const int dhigh = (band.det * kQm2[code]) >> 15;
        const int rhigh = std::clamp(dhigh + band.s, -16384, 16383);
        band.nb = std::clamp(((band.nb * 127) >> 7) + kWh[kRh2[code]], 0, 22528);
        const int shift = 10 - (band.nb >> 11);
        const int scale = kIlb[(band.nb >> 6) & 31];
        band.det = (shift < 0 ? scale << -shift : scale >> shift) << 2;
        band.Update(dhigh);
        return rhigh;
    }
    bool Decode(const uint8_t* p, size_t bytes, std::vector<float>& out) override {
        const size_t codes = bytes / channels_;
        out.resize(codes * 4);
        for (size_t i = 0; i < codes; ++i) {
            float frame[2][2];
            for (unsigned ch = 0; ch < channels_; ++ch) {
                const unsigned code = p[i * channels_ + ch];
                const int rlow = Low(c_.band[ch][0], code & 63), rhigh = High(c_.band[ch][1], code >> 6);
                // Receive QMF: the two band sums enter the delay line and each produces one sample.
                int* x = c_.x[ch];
                memmove(x, x + 2, 22 * sizeof(int));
                x[22] = rlow + rhigh;
                x[23] = rlow - rhigh;
                int even = 0, odd = 0;
                for (int k = 0; k < 12; ++k) {
                    even += x[2 * k] * kQmf[k];
                    odd += x[2 * k + 1] * kQmf[11 - k];
                }
                frame[0][ch] = Sat(odd >> 11) * (1.0f / 32768);
                frame[1][ch] = Sat(even >> 11) * (1.0f / 32768);
            }
            for (int k = 0; k < 2; ++k) {
                float* o = out.data() + (i * 2 + k) * 2;
                o[0] = frame[k][0];
                o[1] = frame[k][channels_ - 1];
            }
        }
        return true;
    }
    void Reset() override {
        c_ = Codec{};
        for (auto& channel : c_.band) { channel[0].det = 32; channel[1].det = 8; }
    }
    size_t Preroll() const override { return SIZE_MAX; }
    std::shared_ptr<const State> Capture() const override { return std::make_shared<Codec>(c_); }
    bool Restore(const State& s) override { c_ = static_cast<const Codec&>(s); return true; }
};
}  // namespace

std::unique_ptr<Decoder> OpenG722Decoder(const std::wstring& path, const CodedStream* s) {
    auto d = std::make_unique<G722Decoder>();
    return d->Open(path, s) ? std::move(d) : nullptr;
}
