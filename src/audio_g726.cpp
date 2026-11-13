// Cordinal G.726 decoder (ITU-T G.726 ADPCM at 16, 24, 32 and 40 kbit/s; the 32 kbit/s rate is the
// former G.721 and the 24 and 40 kbit/s rates the former G.723), written from the recommendation's
// functional blocks. Each 2- to 5-bit code is an adaptively scaled difference from a prediction
// by two poles and six zeros; the quantizer adapts fast or slow, steered by tone and transition
// detection. Numbers move between blocks in the recommendation's own floating formats, so the
// output is the recommendation's to the bit. 8 kHz mono, in WAV (tag 0x45, codes packed from the
// high bits of each byte) or AU (encodings 23, 25 and 26, packed from the low bits).
#include "native_audio.h"
#include "audio_coded.h"
#include <cstdlib>

namespace {
using namespace nativeaudio;

// Per code size: log of the quantized difference (DQLN), scale factor adaptation (W) and speed
// control (F), indexed by the code.
struct Tables { const int* dqln; const int* w; const int* f; };
const int kDqln2[4] = {116, 365, 365, 116}, kW2[4] = {-704, 14048, 14048, -704}, kF2[4] = {0, 0xE00, 0xE00, 0};
const int kDqln3[8] = {-2048, 135, 273, 373, 373, 273, 135, -2048}, kW3[8] = {-128, 960, 4384, 18624, 18624, 4384, 960, -128},
          kF3[8] = {0, 0x200, 0x400, 0xE00, 0xE00, 0x400, 0x200, 0};
const int kDqln4[16] = {-2048, 4, 135, 213, 273, 323, 373, 425, 425, 373, 323, 273, 213, 135, 4, -2048},
          kW4[16] = {-384, 576, 1312, 2048, 3584, 6336, 11360, 35904, 35904, 11360, 6336, 3584, 2048, 1312, 576, -384},
          kF4[16] = {0, 0, 0, 0x200, 0x200, 0x200, 0x600, 0xE00, 0xE00, 0x600, 0x200, 0x200, 0x200, 0, 0, 0};
const int kDqln5[32] = {-2048, -66, 28, 104, 169, 224, 274, 318, 358, 395, 429, 459, 488, 514, 539, 566,
                        566, 539, 514, 488, 459, 429, 395, 358, 318, 274, 224, 169, 104, 28, -66, -2048},
          kW5[32] = {448, 448, 768, 1248, 1280, 1312, 1856, 3200, 4512, 5728, 7008, 8960, 11456, 14080, 16928, 22272,
                     22272, 16928, 14080, 11456, 8960, 7008, 5728, 4512, 3200, 1856, 1312, 1280, 1248, 768, 448, 448},
          kF5[32] = {0, 0, 0, 0, 0, 0x200, 0x200, 0x200, 0x200, 0x200, 0x400, 0x600, 0x800, 0xA00, 0xC00, 0xC00,
                     0xC00, 0xC00, 0xA00, 0x800, 0x600, 0x400, 0x200, 0x200, 0x200, 0x200, 0x200, 0, 0, 0, 0, 0};
const Tables kTables[4] = {{kDqln2, kW2, kF2}, {kDqln3, kW3, kF3}, {kDqln4, kW4, kF4}, {kDqln5, kW5, kF5}};

// The position of the highest set bit plus one: the exponent of the recommendation's floats.
inline int Exponent(int v) {
    int e = 0;
    while (e < 15 && v >= (1 << e)) ++e;
    return e;
}

// FMULT: a 16-bit predictor coefficient times a signal held as sign, 4-bit exponent and 6-bit
// mantissa, in the recommendation's floating arithmetic.
inline int Multiply(int an, int srn) {
    const int magnitude = an > 0 ? an : (-an) & 0x1FFF;
    const int exponent = Exponent(magnitude) - 6;
    const int mantissa = magnitude == 0 ? 32 : exponent >= 0 ? magnitude >> exponent : magnitude << -exponent;
    const int wexp = exponent + ((srn >> 6) & 15) - 13;
    const int wmant = (mantissa * (srn & 63) + 0x30) >> 4;
    const int product = wexp >= 0 ? (wmant << wexp) & 0x7FFF : wmant >> -wexp;
    return (an ^ srn) < 0 ? -product : product;
}

// A value as the predictor's float: exponent in bits 6-9, mantissa in bits 0-5, sign as -0x400.
inline int Float(int v) {
    if (v == 0) return 0x20;
    const int magnitude = v < 0 ? -v : v, exponent = Exponent(magnitude);
    const int f = (exponent << 6) + ((magnitude << 6) >> exponent);
    return v < 0 ? f - 0x400 : f;
}

struct G726State {
    int yl = 34816, yu = 544, dms = 0, dml = 0, ap = 0, td = 0;
    int a[2]{}, b[6]{}, pk[2]{}, dq[6] = {32, 32, 32, 32, 32, 32}, sr[2] = {32, 32};

    // One code to one 14-bit sample, updating every adaptive part.
    int Decode(unsigned code, unsigned bits) {
        const Tables& t = kTables[bits - 2];
        // Prediction: six zeros, then two poles (FMULT, ACCUM). The sums wrap at 16 bits, as the
        // recommendation's do; an overloaded signal depends on it.
        int sezi = 0;
        for (int i = 0; i < 6; ++i) sezi += Multiply(b[i] >> 2, dq[i]);
        sezi = (int16_t)sezi;
        const int sez = sezi >> 1;
        const int se = (int16_t)(sezi + Multiply(a[1] >> 2, sr[1]) + Multiply(a[0] >> 2, sr[0])) >> 1;
        // Step size: a mix of the fast and slow scale factors (MIX), all fast while ap is high.
        int y = yu;
        if (ap < 256) {
            y = yl >> 6;
            const int dif = yu - y, al = ap >> 2;
            if (dif > 0) y += (dif * al) >> 6;
            else if (dif < 0) y += (dif * al + 0x3F) >> 6;
        }
        // Inverse quantization (RECONST, ADDA, ANTILOG): sign and magnitude, sign as -0x8000.
        const bool negative = code & (1u << (bits - 1));
        const int dql = t.dqln[code] + (y >> 2);
        int dqv;
        if (dql < 0) dqv = negative ? -0x8000 : 0;
        else {
            const int dex = (dql >> 7) & 15, dqt = 128 + (dql & 127);
            dqv = (dqt << 7) >> (14 - dex);
            if (negative) dqv -= 0x8000;
        }
        // ADDB and ADDC, wrapping at 16 bits.
        const int difference = dqv < 0 ? -(dqv & 0x7FFF) : dqv;
        const int srv = (int16_t)(se + difference);
        const int dqsez = (int16_t)(difference + sez);
        Update(bits, y, t.w[code], t.f[code], dqv, srv, dqsez);
        return srv;
    }

    void Update(unsigned bits, int y, int wi, int fi, int dqv, int srv, int dqsez) {
        const int pk0 = dqsez < 0 ? 1 : 0;
        const int mag = dqv & 0x7FFF;
        // TRANS: a transition from a partial band tone resets the predictor.
        const int ylint = yl >> 15, ylfrac = (yl >> 10) & 31;
        const int thr1 = (32 + ylfrac) << ylint, thr2 = ylint > 9 ? 31 << 10 : thr1;
        const int dqthr = (thr2 + (thr2 >> 1)) >> 1;
        const bool tr = td && mag > dqthr;
        // Scale factor adaptation (FUNCTW, FILTD, LIMB, FILTE).
        yu = std::clamp(y + ((wi - y) >> 5), 544, 5120);
        yl += yu + ((-yl) >> 6);
        int a2p = 0;
        if (tr) {
            a[0] = a[1] = 0;
            for (int& v : b) v = 0;
        } else {
            const int pks1 = pk0 ^ pk[0];
            // UPA2: the second pole.
            a2p = a[1] - (a[1] >> 7);
            if (dqsez != 0) {
                const int fa1 = pks1 ? a[0] : -a[0];
                if (fa1 < -8191) a2p -= 0x100;
                else if (fa1 > 8191) a2p += 0xFF;
                else a2p += fa1 >> 5;
                if (pk0 ^ pk[1]) {
                    if (a2p <= -12160) a2p = -12288;
                    else if (a2p >= 12416) a2p = 12288;
                    else a2p -= 0x80;
                } else if (a2p <= -12416) a2p = -12288;
                else if (a2p >= 12160) a2p = 12288;
                else a2p += 0x80;
            }
            a[1] = a2p;
            // UPA1 and LIMD: the first pole, inside the stability triangle.
            a[0] -= a[0] >> 8;
            if (dqsez != 0) a[0] += pks1 ? -192 : 192;
            const int a1ul = 15360 - a2p;
            a[0] = std::clamp(a[0], -a1ul, a1ul);
            // UPB: the zeros, leaking faster at 40 kbit/s.
            for (int i = 0; i < 6; ++i) {
                b[i] -= b[i] >> (bits == 5 ? 9 : 8);
                if (mag) b[i] += (dqv ^ dq[i]) >= 0 ? 128 : -128;
            }
        }
        // FLOATA, FLOATB and the delays.
        for (int i = 5; i > 0; --i) dq[i] = dq[i - 1];
        dq[0] = mag == 0 ? (dqv >= 0 ? 0x20 : 0x20 - 0x400) : Float(dqv >= 0 ? mag : -mag);
        sr[1] = sr[0];
        sr[0] = srv <= -32768 ? 0x20 - 0x400 : Float(srv);  // the 16-bit 0xFC20
        pk[1] = pk[0];
        pk[0] = pk0;
        // TONE: a partial band tone holds the second pole near its limit.
        td = !tr && a2p < -11776;
        // Adaptation speed control (FILTA, FILTB, SUBTC, FILTC).
        dms += (fi - dms) >> 5;
        dml += ((fi << 2) - dml) >> 7;
        if (tr) ap = 256;
        else if (y < 1536 || td || std::abs((dms << 2) - dml) >= (dml >> 3)) ap += (0x200 - ap) >> 4;
        else ap += (-ap) >> 4;
    }
};

class G726Decoder final : public PacketDecoder {
public:
    bool Open(const std::wstring& path, const CodedStream& s) {
        if (s.channels != 1 || s.bits < 2 || s.bits > 5 || !s.rate || s.rate > 1048575 || !media_.file.Open(path)) return false;
        bits_ = s.bits;
        low_ = s.lowBitsFirst;
        // A whole number of bytes and codes per packet: bits_ * 512 bytes hold 4096 codes.
        return FixedPackets(media_, s.start, s.bytes / bits_ * bits_, bits_ * 512, 4096, s.frames) && Ready(s.rate);
    }

private:
    G726State g_;
    unsigned bits_ = 4;
    bool low_ = false;
    bool Decode(const uint8_t* p, size_t bytes, std::vector<float>& out) override {
        const size_t codes = bytes * 8 / bits_;
        out.resize(codes * 2);
        uint32_t window = 0;
        unsigned held = 0;
        const unsigned mask = (1u << bits_) - 1;
        size_t at = 0;
        for (size_t i = 0; i < codes; ++i) {
            while (held < bits_) {
                window = low_ ? window | (uint32_t)p[at++] << held : window << 8 | p[at++];
                held += 8;
            }
            unsigned code;
            if (low_) { code = window & mask; window >>= bits_; }
            else code = (window >> (held - bits_)) & mask;
            held -= bits_;
            const int v = std::clamp(g_.Decode(code, bits_) * 4, -32768, 32767);
            out[i * 2] = out[i * 2 + 1] = v * (1.0f / 32768);
        }
        return true;
    }
    void Reset() override { g_ = G726State{}; }
    size_t Preroll() const override { return SIZE_MAX; }
    struct Saved : State { G726State g; };
    std::shared_ptr<const State> Capture() const override { auto s = std::make_shared<Saved>(); s->g = g_; return s; }
    bool Restore(const State& s) override { g_ = static_cast<const Saved&>(s).g; return true; }
};
}  // namespace

std::unique_ptr<Decoder> OpenG726Decoder(const std::wstring& path, const CodedStream& s) {
    auto d = std::make_unique<G726Decoder>();
    return d->Open(path, s) ? std::move(d) : nullptr;
}
