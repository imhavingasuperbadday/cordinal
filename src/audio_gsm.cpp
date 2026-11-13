// Cordinal GSM 06.10 decoder (ETSI GSM full-rate speech, 13 kbit/s), written from the
// recommendation's decoder description, whose 16-bit saturating arithmetic it follows exactly.
// Each 20 ms frame gives eight log-area ratios for the short-term filter and, for each of four
// subframes, a long-term lag and gain and 13 regular-pulse samples. Raw .gsm files hold 33-byte
// frames (a 0xD signature, then the fields high bits first) at 8 kHz mono; WAV files (tag 0x31,
// Microsoft's "WAV49") hold two frames in each 65-byte block, packed low bits first.
#include "native_audio.h"
#include "audio_coded.h"

namespace {
using namespace nativeaudio;

using word = int16_t;
inline word Add(int a, int b) { return (word)std::clamp(a + b, -32768, 32767); }
inline word Sub(int a, int b) { return (word)std::clamp(a - b, -32768, 32767); }
inline word MultR(int a, int b) { return (word)((a * b + 16384) >> 15); }
inline word Asr(int a, int n) {
    if (n >= 16) return (word)-(a < 0);
    if (n <= -16) return 0;
    return n < 0 ? (word)(a << -n) : (word)(a >> n);
}
inline word Asl(int a, int n) {
    if (n >= 16) return 0;
    if (n <= -16) return (word)-(a < 0);
    return n < 0 ? Asr(a, -n) : (word)(a << n);
}

const word kQlb[4] = {3277, 11469, 21299, 32767};  // long-term gains
const word kFac[8] = {18431, 20479, 22527, 24575, 26623, 28671, 30719, 32767};
// Per log-area ratio: the offset of its code, its B term and 1/A (decoding of the coded LARs).
const word kMic[8] = {-32, -32, -16, -16, -8, -8, -4, -4};
const word kB[8] = {0, 0, 2048, -2560, 94, -1792, -341, -1144};
const word kInvA[8] = {13107, 13107, 13107, 13107, 19223, 17476, 31454, 29708};
const unsigned kLarBits[8] = {6, 6, 5, 5, 4, 4, 3, 3};

struct Frame { word lar[8], nc[4], bc[4], mc[4], xmaxc[4], xmc[4][13]; };

// Reads the fields in their order: high bits first (.gsm) or low bits first (WAV49).
class FieldReader {
public:
    FieldReader(const uint8_t* p, bool lowFirst, unsigned skipBits) : p_(p), low_(lowFirst), bit_(skipBits) {}
    word Get(unsigned n) {
        unsigned v = 0;
        for (unsigned i = 0; i < n; ++i, ++bit_) {
            const unsigned b = low_ ? (p_[bit_ >> 3] >> (bit_ & 7)) & 1 : (p_[bit_ >> 3] >> (7 - (bit_ & 7))) & 1;
            v |= low_ ? b << i : b << (n - 1 - i);
        }
        return (word)v;
    }
    void Read(Frame& f) {
        for (int i = 0; i < 8; ++i) f.lar[i] = Get(kLarBits[i]);
        for (int j = 0; j < 4; ++j) {
            f.nc[j] = Get(7); f.bc[j] = Get(2); f.mc[j] = Get(2); f.xmaxc[j] = Get(6);
            for (int i = 0; i < 13; ++i) f.xmc[j][i] = Get(3);
        }
    }
private:
    const uint8_t* p_;
    bool low_;
    unsigned bit_;
};

struct Synthesis {
    word dp[160]{};  // the reconstructed residual: 120 samples of history, then the subframe
    word v[9]{};
    word larpp[2][8]{};
    word msr = 0, nrp = 40;
    int j = 0;

    // RPE decoding: the 13 pulses' scale, then their positions on the 40-sample grid.
    static void Rpe(word xmaxc, word mc, const word* xmc, word* erp) {
        word exp = 0, mant;
        if (xmaxc > 15) exp = (word)((xmaxc >> 3) - 1);
        mant = (word)(xmaxc - (exp << 3));
        if (mant == 0) { exp = -4; mant = 7; }
        else {
            while (mant <= 7) { mant = (word)(mant << 1 | 1); --exp; }
            mant -= 8;
        }
        const word t1 = kFac[mant], t2 = Sub(6, exp), t3 = Asl(1, Sub(t2, 1));
        std::fill_n(erp, 40, (word)0);
        for (int i = 0; i < 13; ++i) {
            const word t = MultR(t1, (word)(((xmc[i] << 1) - 7) << 12));
            erp[mc + 3 * i] = Asr(Add(t, t3), t2);
        }
    }

    void LongTerm(word nc, word bc, const word* erp) {
        const word nr = nc < 40 || nc > 120 ? nrp : nc;
        nrp = nr;
        const word brp = kQlb[bc];
        word* drp = dp + 120;
        for (int k = 0; k < 40; ++k) drp[k] = Add(erp[k], MultR(brp, drp[k - nr]));
        for (int k = 0; k < 120; ++k) dp[k] = dp[k + 40];
    }

    static void ToReflection(word* larp) {
        for (int i = 0; i < 8; ++i) {
            const bool negative = larp[i] < 0;
            const word t = negative ? (larp[i] == -32768 ? 32767 : (word)-larp[i]) : larp[i];
            const word r = t < 11059 ? (word)(t << 1) : t < 20070 ? (word)(t + 11059) : Add(t >> 2, 26112);
            larp[i] = negative ? (word)-r : r;
        }
    }

    void Filter(const word* rrp, int count, const word* wt, word* out) {
        for (int k = 0; k < count; ++k) {
            word sri = wt[k];
            for (int i = 7; i >= 0; --i) {
                const word r = rrp[i];
                const word t2 = r == -32768 && v[i] == -32768 ? 32767 : (word)((r * v[i] + 16384) >> 15);
                sri = Sub(sri, t2);
                const word t1 = r == -32768 && sri == -32768 ? 32767 : (word)((r * sri + 16384) >> 15);
                v[i + 1] = Add(v[i], t1);
            }
            out[k] = v[0] = sri;
        }
    }

    // One frame to 160 samples.
    void Decode(const Frame& f, word* s) {
        word wt[160], erp[40];
        for (int sub = 0; sub < 4; ++sub) {
            Rpe(f.xmaxc[sub], f.mc[sub], f.xmc[sub], erp);
            LongTerm(f.nc[sub], f.bc[sub], erp);
            std::copy_n(dp + 120, 40, wt + sub * 40);
        }
        // Short-term synthesis: the LARs interpolate from the last frame's over the first 40 samples.
        word* now = larpp[j];
        const word* before = larpp[j ^= 1];
        for (int i = 0; i < 8; ++i) {
            word t = (word)(Add(f.lar[i], kMic[i]) << 10);
            t = Sub(t, kB[i] * 2);
            t = MultR(kInvA[i], t);
            now[i] = Add(t, t);
        }
        word larp[8];
        for (int i = 0; i < 8; ++i) larp[i] = Add(Add(before[i] >> 2, now[i] >> 2), before[i] >> 1);
        ToReflection(larp);
        Filter(larp, 13, wt, s);
        for (int i = 0; i < 8; ++i) larp[i] = Add(before[i] >> 1, now[i] >> 1);
        ToReflection(larp);
        Filter(larp, 14, wt + 13, s + 13);
        for (int i = 0; i < 8; ++i) larp[i] = Add(Add(before[i] >> 2, now[i] >> 2), now[i] >> 1);
        ToReflection(larp);
        Filter(larp, 13, wt + 27, s + 27);
        std::copy_n(now, 8, larp);
        ToReflection(larp);
        Filter(larp, 120, wt + 40, s + 40);
        // De-emphasis, then upscaling with the low three bits cleared.
        for (int k = 0; k < 160; ++k) {
            msr = Add(s[k], MultR(msr, 28180));
            s[k] = (word)(Add(msr, msr) & 0xFFF8);
        }
    }
};

class GsmDecoder final : public PacketDecoder {
public:
    bool Open(const std::wstring& path, const CodedStream* s) {
        if (!media_.file.Open(path)) return false;
        uint64_t start = 0, bytes = media_.file.Size(), frames = UINT64_MAX;
        unsigned rate = 8000;
        if (s) {
            if (s->channels != 1 || s->align != 65 || (s->blockFrames && s->blockFrames != 320)) return false;
            wav49_ = true;
            start = s->start; bytes = s->bytes; frames = s->frames; rate = s->rate;
        } else {
            const uint8_t* p = media_.file.View(0, 1);
            if (!p || (*p >> 4) != 0xD) return false;
        }
        if (!rate || rate > 1048575) return false;
        const uint32_t unit = wav49_ ? 65 : 33, unitFrames = wav49_ ? 320 : 160;
        return FixedPackets(media_, start, bytes / unit * unit, unit * 32, unitFrames * 32, frames) && Ready(rate);
    }

private:
    struct Saved : State { Synthesis g; };
    Synthesis g_;
    bool wav49_ = false;
    bool Decode(const uint8_t* p, size_t bytes, std::vector<float>& out) override {
        const size_t unit = wav49_ ? 65 : 33, units = bytes / unit;
        out.resize(units * (wav49_ ? 320 : 160) * 2);
        float* o = out.data();
        word pcm[160];
        Frame f;
        for (size_t u = 0; u < units; ++u, p += unit) {
            for (int half = 0; half < (wav49_ ? 2 : 1); ++half) {
                if (wav49_) FieldReader(p, true, half * 260).Read(f);
                else {
                    if ((*p >> 4) != 0xD) return false;
                    FieldReader(p, false, 4).Read(f);
                }
                g_.Decode(f, pcm);
                for (int k = 0; k < 160; ++k, o += 2) o[0] = o[1] = pcm[k] * (1.0f / 32768);
            }
        }
        return true;
    }
    void Reset() override { g_ = Synthesis{}; }
    size_t Preroll() const override { return SIZE_MAX; }
    std::shared_ptr<const State> Capture() const override { auto s = std::make_shared<Saved>(); s->g = g_; return s; }
    bool Restore(const State& s) override { g_ = static_cast<const Saved&>(s).g; return true; }
};
}  // namespace

std::unique_ptr<Decoder> OpenGsmDecoder(const std::wstring& path, const CodedStream* s) {
    auto d = std::make_unique<GsmDecoder>();
    return d->Open(path, s) ? std::move(d) : nullptr;
}
