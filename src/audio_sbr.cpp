// Cordinal Spectral Band Replication (SBR) and Parametric Stereo (PS) decoder,
// from ISO/IEC 14496-3 Subpart 4 (SBR) and Subpart 8 (PS). Uses 64-point FFTs for
// both QMF filterbanks (dispatched to AVX2 when available) instead of direct matrix multiplies.
#include "audio_sbr.h"
#include "audio_sbr_tables.h"
#include "audio_huffman.h"
#include "audio_kernels.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace nativeaudio {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kMaxSbrElements = 4;
constexpr unsigned kQmfSlots = 32;
constexpr unsigned kHfGen = 8;
constexpr unsigned kHfAdj = 2;
constexpr unsigned kMaxEnv = 5;
constexpr unsigned kMaxNoise = 2;
constexpr unsigned kMaxPatches = 5;
constexpr unsigned kMaxBands = 49;

struct Complex {
    float r = 0, i = 0;
};

inline Complex Mul(Complex a, Complex b) { return {a.r * b.r - a.i * b.i, a.r * b.i + a.i * b.r}; }
inline Complex MulConj(Complex a, Complex b) { return {a.r * b.r + a.i * b.i, a.i * b.r - a.r * b.i}; }

struct SbrBooks {
    PrefixBook tEnv1_5{sbrdata::tEnv1_5};
    PrefixBook fEnv1_5{sbrdata::fEnv1_5};
    PrefixBook tEnvBal1_5{sbrdata::tEnvBal1_5};
    PrefixBook fEnvBal1_5{sbrdata::fEnvBal1_5};
    PrefixBook tEnv3_0{sbrdata::tEnv3_0};
    PrefixBook fEnv3_0{sbrdata::fEnv3_0};
    PrefixBook tEnvBal3_0{sbrdata::tEnvBal3_0};
    PrefixBook fEnvBal3_0{sbrdata::fEnvBal3_0};
    PrefixBook tNoise3_0{sbrdata::tNoise3_0};
    PrefixBook tNoiseBal3_0{sbrdata::tNoiseBal3_0};
    PrefixBook psIidDf1{sbrdata::psIidDf1};
    PrefixBook psIidDt1{sbrdata::psIidDt1};
    PrefixBook psIidDf0{sbrdata::psIidDf0};
    PrefixBook psIidDt0{sbrdata::psIidDt0};
    PrefixBook psIccDf{sbrdata::psIccDf};
    PrefixBook psIccDt{sbrdata::psIccDt};
    PrefixBook psIpdDf{sbrdata::psIpdDf};
    PrefixBook psIpdDt{sbrdata::psIpdDt};
    PrefixBook psOpdDf{sbrdata::psOpdDf};
    PrefixBook psOpdDt{sbrdata::psOpdDt};
};

const SbrBooks& Books() {
    static const auto* const books = std::make_unique<SbrBooks>().release();
    return *books;
}

unsigned CeilLog2(unsigned n) {
    unsigned k = 0;
    while ((1u << k) < n) ++k;
    return k;
}

// --- 64-point FFT + QMF 32-band analysis & 64-band synthesis ---

struct QmfTables {
    float cAna[320]{};
    float cSyn[640]{};
    unsigned rev64[64]{};
    Complex fftRoots[64]{};
    std::vector<Complex> fftQuad;
    Complex anaPre[64]{};
    Complex anaPost[32]{};
    Complex synPreEven[64]{};
    Complex synPreOdd[64]{};
    Complex synPostEven[64]{};
    Complex synPostOdd[64]{};
    bool avx = false;

    void Init() {
        avx = FastAudioKernels();
        float c[640]{};
        for (unsigned i = 0; i <= 320; ++i) c[i] = sbrdata::qmfHalf[i];
        for (unsigned i = 321; i < 640; ++i) {
            c[i] = (i == 384 || i == 512) ? -sbrdata::qmfHalf[640 - i] : sbrdata::qmfHalf[640 - i];
        }
        for (unsigned n = 0; n < 320; ++n) cAna[n] = c[2 * n];
        std::copy_n(c, 640, cSyn);

        auto phase = [](double x) -> Complex { return {(float)std::cos(x), (float)std::sin(x)}; };
        for (unsigned i = 0; i < 64; ++i) {
            unsigned r = 0, v = i;
            for (int b = 0; b < 6; ++b) { r = (r << 1) | (v & 1); v >>= 1; }
            rev64[i] = r;
        }
        for (unsigned half = 1; half < 64; half *= 2)
            for (unsigned j = 0; j < half; ++j)
                fftRoots[half + j] = phase(2.0 * kPi * (j * (64.0 / (half * 2))) / 64.0);
        if (avx) {
            fftQuad.clear();
            for (unsigned span = 8; span < 64; span *= 4)
                for (unsigned j = 0; j < span / 2; j += 4)
                    for (unsigned power : {2u, 1u, 3u})
                        for (unsigned k = 0; k < 4; ++k)
                            fftQuad.push_back(phase(2.0 * kPi * power * (j + k) / (span * 2)));
        }
        for (unsigned n = 0; n < 64; ++n) anaPre[n] = phase(kPi * n / 64.0);
        for (unsigned k = 0; k < 32; ++k) {
            Complex p = phase(-kPi * (k + 0.5) / 64.0);
            anaPost[k] = {2.0f * p.r, 2.0f * p.i};
        }
        for (unsigned k = 0; k < 64; ++k) {
            Complex p = phase(-255.0 * kPi * (k + 0.5) / 128.0);
            synPreEven[k] = {p.r * (1.0f / 64.0f), p.i * (1.0f / 64.0f)};
            synPreOdd[k] = Mul(synPreEven[k], phase(kPi * k / 64.0));
        }
        for (unsigned m = 0; m < 64; ++m) {
            synPostEven[m] = phase(kPi * m / 64.0);
            synPostOdd[m] = phase(kPi * (2.0 * m + 1.0) / 128.0);
        }
    }

    void Fft64(Complex* work) const {
        if (avx) {
            AudioFftAvx2(&work[0].r, &fftRoots[0].r, &fftQuad[0].r, 64, false);
            return;
        }
        for (unsigned span = 2; span <= 64; span *= 2) {
            unsigned half = span / 2;
            for (unsigned at = 0; at < 64; at += span) {
                Complex a0 = work[at], b0 = work[at + half];
                work[at] = {a0.r + b0.r, a0.i + b0.i};
                work[at + half] = {a0.r - b0.r, a0.i - b0.i};
                for (unsigned j = 1; j < half; ++j) {
                    Complex a = work[at + j], b = Mul(work[at + j + half], fftRoots[half + j]);
                    work[at + j] = {a.r + b.r, a.i + b.i};
                    work[at + j + half] = {a.r - b.r, a.i - b.i};
                }
            }
        }
    }

    void Analyze32(const float* in32, float* xRing, Complex* out32) const {
        memmove(xRing + 32, xRing, 288 * sizeof(float));
        for (unsigned i = 0; i < 32; ++i) xRing[31 - i] = in32[i];
        float u[64];
        if (avx) {
            AudioSbrQmfWindowAnaAvx2(xRing, cAna, u);
        } else {
            for (unsigned n = 0; n < 64; ++n)
                u[n] = xRing[n] * cAna[n] + xRing[64 + n] * cAna[64 + n] +
                       xRing[128 + n] * cAna[128 + n] + xRing[192 + n] * cAna[192 + n] +
                       xRing[256 + n] * cAna[256 + n];
        }
        Complex work[64];
        for (unsigned n = 0; n < 64; ++n)
            work[rev64[n]] = {u[n] * anaPre[n].r, u[n] * anaPre[n].i};
        Fft64(work);
        for (unsigned k = 0; k < 32; ++k) out32[k] = Mul(work[k], anaPost[k]);
    }

    void Synthesize64(const Complex* in64, float* vRing, float* out64) const {
        Complex even[64], odd[64];
        for (unsigned k = 0; k < 64; ++k) {
            unsigned r = rev64[k];
            even[r] = Mul(in64[k], synPreEven[k]);
            odd[r] = Mul(in64[k], synPreOdd[k]);
        }
        Fft64(even);
        Fft64(odd);
        memmove(vRing + 128, vRing, 1152 * sizeof(float));
        for (unsigned m = 0; m < 64; ++m) {
            vRing[2 * m] = even[m].r * synPostEven[m].r - even[m].i * synPostEven[m].i;
            vRing[2 * m + 1] = odd[m].r * synPostOdd[m].r - odd[m].i * synPostOdd[m].i;
        }
        if (avx) {
            AudioSbrQmfWindowSynAvx2(vRing, cSyn, out64);
        } else {
            for (unsigned n = 0; n < 64; ++n) {
                out64[n] = vRing[n] * cSyn[n] + vRing[192 + n] * cSyn[64 + n] +
                           vRing[256 + n] * cSyn[128 + n] + vRing[448 + n] * cSyn[192 + n] +
                           vRing[512 + n] * cSyn[256 + n] + vRing[704 + n] * cSyn[320 + n] +
                           vRing[768 + n] * cSyn[384 + n] + vRing[960 + n] * cSyn[448 + n] +
                           vRing[1024 + n] * cSyn[512 + n] + vRing[1216 + n] * cSyn[576 + n];
            }
        }
    }
};

// --- SBR Header, Frequency Tables & Channel State ---

struct SbrHeader {
    unsigned ampRes = 1;
    unsigned startFreq = 5, stopFreq = 0, xoverBand = 0;
    unsigned freqScale = 2, alterScale = 1, noiseBands = 2;
    unsigned limiterBands = 2, limiterGains = 2, interpolFreq = 1, smoothingMode = 1;
    bool operator==(const SbrHeader& o) const = default;
};

struct SbrGrid {
    unsigned frameClass = 0;
    unsigned L_E = 1, L_Q = 1;
    int t_E[kMaxEnv + 1]{0, 16};
    int t_Q[kMaxNoise + 1]{0, 16};
    uint8_t r[kMaxEnv]{1};
    int l_A = -1;
};

struct SbrChannel {
    float xRing[320]{};
    float vRing[1280]{};
    Complex xLow[40][32]{};
    Complex yPrev[6][64]{};  // slots 32..37 from previous frame when t_E[L_E] > 16

    SbrGrid grid{};
    uint8_t dfEnv[kMaxEnv]{};
    uint8_t dfNoise[kMaxNoise]{};
    uint8_t invfMode[5]{};
    uint8_t invfModePrev[5]{};
    float bwArray[5]{};

    int env[kMaxEnv][kMaxBands]{};
    int envPrev[kMaxBands]{};
    uint8_t rPrev = 1;
    unsigned ampResPrev = 1;
    bool hasPrevEnv = false;

    int noise[kMaxNoise][5]{};
    int noisePrev[5]{};
    bool hasPrevNoise = false;

    bool hasHarmonic = false;
    uint8_t addHarmonic[kMaxBands]{};
    uint8_t addHarmonicPrev[kMaxBands]{};

    float eOrig[kMaxEnv][kMaxBands]{};
    float qOrig[kMaxEnv][5]{};
    float gTempPrev[4][48]{};
    float qTempPrev[4][48]{};
    int prevLA = -1;
    unsigned prevLE = 1;
    unsigned indexNoise = 0;
    uint8_t indexSine = 0;
    bool resetDelay = true;

    void Reset() {
        std::memset(xRing, 0, sizeof(xRing));
        std::memset(vRing, 0, sizeof(vRing));
        std::memset(xLow, 0, sizeof(xLow));
        std::memset(yPrev, 0, sizeof(yPrev));
        grid = SbrGrid{};
        std::memset(invfMode, 0, sizeof(invfMode));
        std::memset(invfModePrev, 0, sizeof(invfModePrev));
        std::memset(bwArray, 0, sizeof(bwArray));
        hasPrevEnv = false;
        hasPrevNoise = false;
        hasHarmonic = false;
        std::memset(addHarmonic, 0, sizeof(addHarmonic));
        std::memset(addHarmonicPrev, 0, sizeof(addHarmonicPrev));
        std::memset(gTempPrev, 0, sizeof(gTempPrev));
        std::memset(qTempPrev, 0, sizeof(qTempPrev));
        prevLA = -1;
        prevLE = 1;
        indexNoise = 0;
        indexSine = 0;
        resetDelay = true;
    }
};

struct SbrElement {
    bool headerSeen = false;
    bool dataParsed = false;
    bool coupling = false;
    SbrHeader header{};
    SbrChannel ch[2]{};

    unsigned k0 = 0, k2 = 0, kx = 0, M = 0;
    unsigned kxPrev = 0, mPrev = 0;
    unsigned nMaster = 0, nHigh = 0, nLow = 0, nQ = 1, nL = 1;
    unsigned fMaster[kMaxBands + 1]{};
    unsigned fHigh[kMaxBands + 1]{};
    unsigned fLow[kMaxBands + 1]{};
    unsigned fNoise[6]{};
    unsigned fLim[kMaxBands + 10]{};
    unsigned numPatches = 0;
    unsigned patchSubbands[kMaxPatches]{};
    unsigned patchStart[kMaxPatches]{};

    bool BuildTables(unsigned sbrRate) {
        int row = 4;
        if (sbrRate <= 16000) row = 0;
        else if (sbrRate <= 22050) row = 1;
        else if (sbrRate <= 24000) row = 2;
        else if (sbrRate <= 32000) row = 3;
        else if (sbrRate <= 64000) row = 4;
        else row = 5;

        const double refStart = sbrRate < 32000 ? 3000.0 : (sbrRate < 64000 ? 4000.0 : 5000.0);
        const int startMin = (int)std::lround(refStart * 128.0 / sbrRate);
        const int stopMin = (int)std::lround(2.0 * refStart * 128.0 / sbrRate);
        int k0i = startMin + sbrdata::sbrOffset[row][header.startFreq];
        if (k0i < 1 || k0i > 64) return false;

        int k2i = 0;
        if (header.stopFreq == 14) k2i = std::min(64, 2 * k0i);
        else if (header.stopFreq == 15) k2i = std::min(64, 3 * k0i);
        else {
            int stopDk[13]{};
            for (int i = 0; i < 13; ++i) {
                int s0 = (int)std::lround(stopMin * std::pow(64.0 / stopMin, i / 13.0));
                int s1 = (int)std::lround(stopMin * std::pow(64.0 / stopMin, (i + 1) / 13.0));
                stopDk[i] = s1 - s0;
            }
            std::sort(stopDk, stopDk + 13);
            k2i = stopMin;
            for (unsigned i = 0; i < header.stopFreq && i < 13; ++i) k2i += stopDk[i];
        }
        if (k2i <= k0i || k2i > 64) return false;
        const int maxSpan = sbrRate <= 32000 ? 48 : (sbrRate == 44100 ? 35 : 32);
        if (k2i - k0i > maxSpan && sbrRate > 32000) {
            // Standard allows up to 48 total subbands; keep bounded at 48.
        }
        if (k2i - k0i > 48) return false;
        k0 = (unsigned)k0i;
        k2 = (unsigned)k2i;

        // Master frequency table (ISO/IEC 14496-3 4.6.18.3.2.1).
        if (header.freqScale == 0) {
            unsigned dk = header.alterScale ? 2u : 1u;
            unsigned numBands = 2 * ((k2 - k0 + (dk == 2 ? 2 : 0)) / (2 * dk));
            if (dk == 2 && numBands * dk > k2 - k0 + 2) numBands -= 2;
            if (!numBands || numBands > kMaxBands) return false;
            int k2Ach = (int)(k0 + numBands * dk);
            int k2Diff = (int)k2 - k2Ach;
            std::vector<int> vDk(numBands, (int)dk);
            if (k2Diff < 0) {
                int inc = -1;
                int i = 0;
                while (k2Diff && i < (int)numBands) {
                    vDk[i] += inc;
                    k2Diff -= inc;
                    ++i;
                }
            } else if (k2Diff > 0) {
                int inc = 1;
                int i = (int)numBands - 1;
                while (k2Diff && i >= 0) {
                    vDk[i] += inc;
                    k2Diff -= inc;
                    --i;
                }
            }
            nMaster = numBands;
            fMaster[0] = k0;
            for (unsigned i = 0; i < nMaster; ++i) fMaster[i + 1] = (unsigned)((int)fMaster[i] + vDk[i]);
        } else {
            static constexpr int kBandsPerOct[4] = {0, 12, 10, 8};
            int bands = kBandsPerOct[header.freqScale];
            double warp = header.alterScale ? 1.3 : 1.0;
            bool twoRegions = (double)k2 / (double)k0 > 2.2449;
            unsigned k1 = twoRegions ? std::min(k2, 2 * k0) : k2;
            int numBands0 = std::max(2, (int)(2 * std::lround(bands * std::log2((double)k1 / k0) / 2.0)));
            std::vector<int> vDk0((size_t)numBands0);
            for (int i = 0; i < numBands0; ++i) {
                int a = (int)std::lround(k0 * std::pow((double)k1 / k0, (double)i / numBands0));
                int b = (int)std::lround(k0 * std::pow((double)k1 / k0, (double)(i + 1) / numBands0));
                vDk0[i] = b - a;
            }
            if (vDk0[0] <= 0) return false;
            std::sort(vDk0.begin(), vDk0.end());
            std::vector<int> vDk1;
            if (twoRegions && k2 > k1) {
                int numBands1 = std::max(2, (int)(2 * std::lround(bands * std::log2((double)k2 / k1) / (2.0 * warp))));
                vDk1.resize((size_t)numBands1);
                for (int i = 0; i < numBands1; ++i) {
                    int a = (int)std::lround(k1 * std::pow((double)k2 / k1, (double)i / numBands1));
                    int b = (int)std::lround(k1 * std::pow((double)k2 / k1, (double)(i + 1) / numBands1));
                    vDk1[i] = b - a;
                }
                std::sort(vDk1.begin(), vDk1.end());
                if (vDk1[0] < vDk0.back()) {
                    int change = std::min(vDk0.back() - vDk1[0], (vDk1.back() - vDk1[0]) / 2);
                    vDk1[0] += change;
                    vDk1.back() -= change;
                    std::sort(vDk1.begin(), vDk1.end());
                }
                if (vDk1[0] <= 0) return false;
            }
            nMaster = (unsigned)(vDk0.size() + vDk1.size());
            if (!nMaster || nMaster > kMaxBands) return false;
            fMaster[0] = k0;
            unsigned at = 0;
            for (int d : vDk0) { fMaster[at + 1] = fMaster[at] + (unsigned)d; ++at; }
            for (int d : vDk1) { fMaster[at + 1] = fMaster[at] + (unsigned)d; ++at; }
        }

        if (header.xoverBand >= nMaster) return false;
        nHigh = nMaster - header.xoverBand;
        for (unsigned i = 0; i <= nHigh; ++i) fHigh[i] = fMaster[header.xoverBand + i];
        kx = fHigh[0];
        M = fHigh[nHigh] - kx;
        if (kx > 32 || kx + M > 64 || !M || M > 48) return false;

        // Low-resolution frequency table.
        nLow = nHigh / 2 + (nHigh & 1);
        fLow[0] = fHigh[0];
        for (unsigned i = 1; i <= nLow; ++i)
            fLow[i] = fHigh[(nHigh & 1) ? (2 * i - 1) : (2 * i)];

        // Noise floor frequency table.
        if (header.noiseBands == 0) nQ = 1;
        else nQ = std::clamp((unsigned)std::lround(header.noiseBands * std::log2((double)k2 / kx)), 1u, 5u);
        nQ = std::min(nQ, nLow);
        fNoise[0] = fLow[0];
        unsigned idx = 0;
        for (unsigned q = 1; q <= nQ; ++q) {
            idx += (nLow - idx) / (nQ + 1 - q);
            fNoise[q] = fLow[idx];
        }

        // Patch construction (ISO/IEC 14496-3 4.6.18.6.3).
        unsigned goalSb = std::min(64u, (unsigned)std::lround(2.048e6 / sbrRate));
        numPatches = 0;
        unsigned msb = k0;
        unsigned usb = kx;
        unsigned patchBorders[kMaxPatches + 1]{};
        patchBorders[0] = kx;
        while (usb < kx + M && numPatches < kMaxPatches) {
            unsigned k = nMaster;
            while (k > 0 && fMaster[k] > usb + msb - 1) --k;
            unsigned sb = fMaster[k];
            if (sb <= usb) return false;
            unsigned num = sb - usb;
            if (sb >= goalSb && msb > 1) {
                // Keep patches aligned to low-band origin when past goalSb.
            }
            unsigned odd = (num + k0) & 1;
            if (k0 < num + odd + 1) {
                num = k0 - 1;
                odd = 0;
            }
            patchStart[numPatches] = k0 - odd - num;
            if (patchStart[numPatches] < 1) {
                patchStart[numPatches] = 1;
                num = k0 - 1;
            }
            patchSubbands[numPatches] = num;
            usb += num;
            patchBorders[numPatches + 1] = usb;
            ++numPatches;
            if (usb < goalSb && sb < goalSb) msb = k0;
        }
        if (usb < kx + M) return false;

        // Limiter frequency table (ISO/IEC 14496-3 4.6.18.3.2.4).
        if (header.limiterBands == 0) {
            nL = 1;
            fLim[0] = kx;
            fLim[1] = kx + M;
        } else {
            static constexpr double kLimOct[4] = {0.0, 1.2, 2.0, 3.0};
            std::vector<unsigned> lim(fLow, fLow + nLow + 1);
            for (unsigned p = 1; p < numPatches; ++p) lim.push_back(patchBorders[p]);
            std::sort(lim.begin(), lim.end());
            lim.erase(std::unique(lim.begin(), lim.end()), lim.end());
            size_t k = 1;
            while (k < lim.size()) {
                double oct = std::log2((double)lim[k] / lim[k - 1]);
                if (oct < 0.49 / kLimOct[header.limiterBands]) {
                    bool curPatch = std::find(patchBorders, patchBorders + numPatches + 1, lim[k]) != patchBorders + numPatches + 1;
                    if (!curPatch) {
                        lim.erase(lim.begin() + (ptrdiff_t)k);
                        continue;
                    }
                    bool prevPatch = std::find(patchBorders, patchBorders + numPatches + 1, lim[k - 1]) != patchBorders + numPatches + 1;
                    if (k > 1 && !prevPatch) {
                        lim.erase(lim.begin() + (ptrdiff_t)(k - 1));
                        continue;
                    }
                }
                ++k;
            }
            nL = (unsigned)(lim.size() - 1);
            for (size_t i = 0; i < lim.size(); ++i) fLim[i] = lim[i];
        }
        return true;
    }
};

// --- Parametric Stereo (ISO/IEC 14496-3 Subpart 8) ---

struct PsTables {
    Complex f20_0[8][13]{};
    Complex f34_0[12][13]{};
    Complex f34_1[8][13]{};
    Complex f34_2[4][13]{};
    float ha[46][8][4]{};
    float hb[46][8][4]{};
    Complex qFract[2][50][3]{};
    Complex phiFract[2][50]{};
    float pdReSmooth[512]{};
    float pdImSmooth[512]{};

    void Init() {
        auto makeProto = [](Complex (*out)[13], const float* proto, int bands) {
            for (int q = 0; q < bands; ++q) {
                for (int n = 0; n < 7; ++n) {
                    double th = 2.0 * kPi * (q + 0.5) * (n - 6) / bands;
                    out[q][n] = {(float)(proto[n] * std::cos(th)), (float)(proto[n] * -std::sin(th))};
                }
                for (int n = 7; n < 13; ++n)
                    out[q][n] = {out[q][12 - n].r, -out[q][12 - n].i};
            }
        };
        makeProto(f20_0, sbrdata::g0Q8, 8);
        makeProto(f34_0, sbrdata::g0Q12, 12);
        makeProto(f34_1, sbrdata::g1Q8, 8);
        makeProto(f34_2, sbrdata::g2Q4, 4);

        static constexpr float ipdSin[8] = {0, 0.70710678f, 1, 0.70710678f, 0, -0.70710678f, -1, -0.70710678f};
        static constexpr float ipdCos[8] = {1, 0.70710678f, 0, -0.70710678f, -1, -0.70710678f, 0, 0.70710678f};
        for (int p0 = 0; p0 < 8; ++p0)
            for (int p1 = 0; p1 < 8; ++p1)
                for (int p2 = 0; p2 < 8; ++p2) {
                    float re = 0.25f * ipdCos[p0] + 0.5f * ipdCos[p1] + ipdCos[p2];
                    float im = 0.25f * ipdSin[p0] + 0.5f * ipdSin[p1] + ipdSin[p2];
                    float inv = 1.0f / std::hypot(re, im);
                    pdReSmooth[p0 * 64 + p1 * 8 + p2] = re * inv;
                    pdImSmooth[p0 * 64 + p1 * 8 + p2] = im * inv;
                }

        for (int iid = 0; iid < 46; ++iid) {
            float c = sbrdata::iidDequant[iid];
            float c1 = 1.41421356f / std::sqrt(1.0f + c * c);
            float c2 = c * c1;
            for (int icc = 0; icc < 8; ++icc) {
                const double iccVal = std::clamp((double)sbrdata::iccDequant[icc], -1.0, 1.0);
                // atan2(sqrt(1 - x^2), x) matches acos(x) across [-1, 1] without requiring libm acosf.
                float acosIcc = (float)std::atan2(std::sqrt(std::max(0.0, 1.0 - iccVal * iccVal)), iccVal);
                float alpha = 0.5f * acosIcc;
                float beta = alpha * (c1 - c2) * 0.70710678f;
                ha[iid][icc][0] = c2 * std::cos(beta + alpha);
                ha[iid][icc][1] = c1 * std::cos(beta - alpha);
                ha[iid][icc][2] = c2 * std::sin(beta + alpha);
                ha[iid][icc][3] = c1 * std::sin(beta - alpha);

                float rho = std::max(sbrdata::iccDequant[icc], 0.05f);
                float a = 0.5f * std::atan2(2.0f * c * rho, c * c - 1.0f);
                if (a < 0) a += (float)(kPi * 0.5);
                float mu = c + 1.0f / c;
                mu = std::sqrt(std::max(0.0f, 1.0f + (4.0f * rho * rho - 4.0f) / (mu * mu)));
                float g = std::atan(std::sqrt(std::max(0.0f, (1.0f - mu) / (1.0f + mu))));
                float ac = std::cos(a), as = std::sin(a), gc = std::cos(g), gs = std::sin(g);
                hb[iid][icc][0] = 1.41421356f * ac * gc;
                hb[iid][icc][1] = 1.41421356f * as * gc;
                hb[iid][icc][2] = -1.41421356f * as * gs;
                hb[iid][icc][3] = 1.41421356f * ac * gs;
            }
        }

        static constexpr float kFracLinks[3] = {0.43f, 0.75f, 0.347f};
        constexpr float kFracGain = 0.39f;
        for (int k = 0; k < 30; ++k) {
            double fc = k < 10 ? sbrdata::fCenter20[k] * 0.125 : k - 6.5;
            for (int m = 0; m < 3; ++m) {
                double th = -kPi * kFracLinks[m] * fc;
                qFract[0][k][m] = {(float)std::cos(th), (float)std::sin(th)};
            }
            double th = -kPi * kFracGain * fc;
            phiFract[0][k] = {(float)std::cos(th), (float)std::sin(th)};
        }
        for (int k = 0; k < 50; ++k) {
            double fc = k < 32 ? sbrdata::fCenter34[k] / 24.0 : k - 26.5;
            for (int m = 0; m < 3; ++m) {
                double th = -kPi * kFracLinks[m] * fc;
                qFract[1][k][m] = {(float)std::cos(th), (float)std::sin(th)};
            }
            double th = -kPi * kFracGain * fc;
            phiFract[1][k] = {(float)std::cos(th), (float)std::sin(th)};
        }
    }
};

struct PsState {
    bool headerSeen = false;
    bool use34 = false;
    bool enableIid = false;
    bool enableIcc = false;
    bool enableExt = false;
    unsigned iidMode = 0;
    unsigned iidQuant = 0;
    unsigned iccMode = 0;
    bool enableIpdOpd = false;
    bool frameClass = false;
    unsigned numEnv = 0, numEnvPrev = 0;
    int border[6]{0, 32};
    int iidPar[5][34]{}, iidPrev[34]{};
    int iccPar[5][34]{}, iccPrev[34]{};
    int ipdPar[5][17]{}, ipdPrev[17]{}, ipdHist[2][17]{};
    int opdPar[5][17]{}, opdPrev[17]{}, opdHist[2][17]{};

    Complex buf[64][13]{};       // hybrid analysis delay line (13 taps, center at 6)
    Complex delay[91][14]{};      // decorrelator delay line
    Complex apDelay[50][3][5]{};  // 3-link allpass delay
    float peakDecayNrg[34]{};
    float powerSmooth[34]{};
    float transSmooth[34]{};
    float h11Prev[34]{}, h12Prev[34]{}, h21Prev[34]{}, h22Prev[34]{};
    bool hInitialized = false;

    void Reset() {
        std::memset(buf, 0, sizeof(buf));
        std::memset(delay, 0, sizeof(delay));
        std::memset(apDelay, 0, sizeof(apDelay));
        std::memset(peakDecayNrg, 0, sizeof(peakDecayNrg));
        std::memset(powerSmooth, 0, sizeof(powerSmooth));
        std::memset(transSmooth, 0, sizeof(transSmooth));
        std::memset(iidPrev, 0, sizeof(iidPrev));
        std::memset(iccPrev, 0, sizeof(iccPrev));
        std::memset(ipdPrev, 0, sizeof(ipdPrev));
        std::memset(opdPrev, 0, sizeof(opdPrev));
        std::memset(ipdHist, 0, sizeof(ipdHist));
        std::memset(opdHist, 0, sizeof(opdHist));
        headerSeen = false;
        enableIid = false;
        enableIcc = false;
        enableExt = false;
        iidMode = 0;
        iidQuant = 0;
        iccMode = 0;
        hInitialized = false;
    }

    bool Parse(Bits& b, size_t& bitsLeft) {
        size_t startLeft = b.Left();
        auto checkUsed = [&]() -> bool {
            size_t used = startLeft - b.Left();
            if (used > bitsLeft || !b.Good()) return false;
            bitsLeft -= used;
            startLeft = b.Left();
            return true;
        };

        const SbrBooks& books = Books();
        bool enableHeader = b.Get(1) != 0;
        static constexpr int kParBands[6] = {10, 20, 34, 10, 20, 34};
        if (enableHeader) {
            headerSeen = true;
            enableIid = b.Get(1) != 0;
            if (enableIid) {
                iidMode = (unsigned)b.Get(3);
                if (iidMode > 5) return false;
                iidQuant = iidMode >= 3 ? 1u : 0u;
            }
            enableIcc = b.Get(1) != 0;
            if (enableIcc) {
                iccMode = (unsigned)b.Get(3);
                if (iccMode > 5) return false;
            }
            use34 = (enableIid && (iidMode == 2 || iidMode == 5)) ||
                    (enableIcc && (iccMode == 2 || iccMode == 5));
            enableExt = b.Get(1) != 0;
        }
        if (!headerSeen) return false;
        frameClass = b.Get(1) != 0;
        unsigned numEnvIdx = (unsigned)b.Get(2);
        static constexpr unsigned kNumEnvTab[2][4] = {{0, 1, 2, 4}, {1, 2, 3, 4}};
        numEnv = kNumEnvTab[frameClass][numEnvIdx];
        if (frameClass) {
            for (unsigned e = 1; e <= numEnv; ++e) border[e] = (int)b.Get(5) + 1;
            border[0] = 0;
            if (border[numEnv] > 32) return false;
        } else {
            for (unsigned e = 0; e <= numEnv; ++e)
                border[e] = numEnv ? (int)(e * 32 / numEnv) : 0;
        }
        if (!checkUsed()) return false;

        int nrIid = kParBands[iidMode];
        int nrIcc = kParBands[iccMode];
        int nrIpd = nrIid == 34 ? 17 : (nrIid == 20 ? 11 : 5);
        int iidSteps = iidQuant ? 15 : 7;

        auto readDelta = [&](const PrefixBook& book, int offset, int dt, int* dst, const int* prev, int count, int stride, int lo, int hi, int mod = 0) {
            for (int i = 0; i < count; ++i) {
                int delta = book.Read(b) - offset;
                int base = dt ? prev[i * stride] : (i ? dst[i - 1] : 0);
                int v = base + delta;
                if (mod) v = ((v % mod) + mod) % mod;
                else v = std::clamp(v, lo, hi);
                dst[i] = v;
            }
        };

        if (enableIid) {
            const PrefixBook& df = iidSteps == 15 ? books.psIidDf1 : books.psIidDf0;
            const PrefixBook& dt = iidSteps == 15 ? books.psIidDt1 : books.psIidDt0;
            int off = iidSteps == 15 ? 30 : 14;
            for (unsigned e = 0; e < numEnv; ++e) {
                int isDt = (int)b.Get(1);
                const int* prev = e ? iidPar[e - 1] : iidPrev;
                readDelta(isDt ? dt : df, off, isDt, iidPar[e], prev, nrIid, 1, -iidSteps, iidSteps);
            }
            if (numEnv) std::copy_n(iidPar[numEnv - 1], nrIid, iidPrev);
        } else if (numEnv) {
            for (unsigned e = 0; e < numEnv; ++e) std::copy_n(iidPrev, 34, iidPar[e]);
        }

        if (enableIcc) {
            for (unsigned e = 0; e < numEnv; ++e) {
                int isDt = (int)b.Get(1);
                const int* prev = e ? iccPar[e - 1] : iccPrev;
                readDelta(isDt ? books.psIccDt : books.psIccDf, 7, isDt, iccPar[e], prev, nrIcc, 1, 0, 7);
            }
            if (numEnv) std::copy_n(iccPar[numEnv - 1], nrIcc, iccPrev);
        } else if (numEnv) {
            for (unsigned e = 0; e < numEnv; ++e) std::copy_n(iccPrev, 34, iccPar[e]);
        }

        enableIpdOpd = false;
        if (enableExt) {
            unsigned cnt = (unsigned)b.Get(4);
            if (cnt == 15) cnt += (unsigned)b.Get(8);
            size_t extBits = cnt * 8;
            if (!checkUsed()) return false;
            if (extBits > bitsLeft) return false;
            size_t extLeft = extBits;
            size_t s0 = b.Left();
            while (extLeft >= 2) {
                unsigned id = (unsigned)b.Get(2);
                extLeft -= 2;
                if (id == 0) {
                    enableIpdOpd = b.Get(1) != 0;
                    if (enableIpdOpd) {
                        for (unsigned e = 0; e < numEnv; ++e) {
                            int dtIpd = (int)b.Get(1);
                            readDelta(dtIpd ? books.psIpdDt : books.psIpdDf, 0, dtIpd, ipdPar[e], e ? ipdPar[e - 1] : ipdPrev, nrIpd, 1, 0, 7, 8);
                            int dtOpd = (int)b.Get(1);
                            readDelta(dtOpd ? books.psOpdDt : books.psOpdDf, 0, dtOpd, opdPar[e], e ? opdPar[e - 1] : opdPrev, nrIpd, 1, 0, 7, 8);
                        }
                        if (numEnv) {
                            std::copy_n(ipdPar[numEnv - 1], nrIpd, ipdPrev);
                            std::copy_n(opdPar[numEnv - 1], nrIpd, opdPrev);
                        }
                    }
                    b.Skip(1);
                } else break;
                size_t used = s0 - b.Left();
                if (used > extBits) return false;
                extLeft = extBits - used;
            }
            size_t used = s0 - b.Left();
            if (used < extBits) b.Skip(extBits - used);
            bitsLeft -= extBits;
            startLeft = b.Left();
        }
        return checkUsed();
    }

    void Apply(const PsTables& tbl, Complex (*lQmf)[64], Complex (*rQmf)[64]) {
        if (!headerSeen) {
            for (unsigned t = 0; t < kQmfSlots; ++t)
                for (unsigned k = 0; k < 64; ++k) rQmf[t][k] = lQmf[t][k];
            return;
        }
        const int nrBands = use34 ? 34 : 20;
        const int nrAllpass = use34 ? 50 : 30;
        const int nrHybrid = use34 ? 32 : 20;
        const int nrQmf = use34 ? 91 : 71;
        const int8_t* mapBand = use34 ? sbrdata::kToI34 : sbrdata::kToI20;

        // Hybrid filterbank on QMF bands 0..2, 6-slot delay on QMF bands 3..63.
        Complex s[32][91]{};
        for (int t = 0; t < 32; ++t) {
            for (int k = 0; k < 64; ++k) {
                memmove(buf[k], buf[k] + 1, 12 * sizeof(Complex));
                buf[k][12] = lQmf[t][k];
            }
            if (!use34) {
                Complex tmp[8]{};
                for (int q = 0; q < 8; ++q) {
                    Complex acc{};
                    for (int n = 0; n < 13; ++n) {
                        Complex p = Mul(buf[0][n], tbl.f20_0[q][n]);
                        acc.r += p.r; acc.i += p.i;
                    }
                    tmp[q] = acc;
                }
                s[t][0] = tmp[6];
                s[t][1] = tmp[7];
                s[t][2] = tmp[0];
                s[t][3] = tmp[1];
                s[t][4] = {tmp[2].r + tmp[5].r, tmp[2].i + tmp[5].i};
                s[t][5] = {tmp[3].r + tmp[4].r, tmp[3].i + tmp[4].i};
                for (int k = 1; k <= 2; ++k) {
                    Complex a = buf[k][6];
                    Complex b{0.25f * (buf[k][0].r + buf[k][12].r) - 0.5f * buf[k][6].r,
                              0.25f * (buf[k][0].i + buf[k][12].i) - 0.5f * buf[k][6].i};
                    s[t][4 + k * 2] = {0.5f * (a.r + b.r), 0.5f * (a.i + b.i)};
                    s[t][5 + k * 2] = {0.5f * (a.r - b.r), 0.5f * (a.i - b.i)};
                }
                for (int k = 3; k < 64; ++k) s[t][7 + k] = buf[k][6];
            } else {
                for (int q = 0; q < 12; ++q) {
                    Complex acc{};
                    for (int n = 0; n < 13; ++n) {
                        Complex p = Mul(buf[0][n], tbl.f34_0[q][n]);
                        acc.r += p.r; acc.i += p.i;
                    }
                    s[t][q] = acc;
                }
                for (int q = 0; q < 8; ++q) {
                    Complex acc{};
                    for (int n = 0; n < 13; ++n) {
                        Complex p = Mul(buf[1][n], tbl.f34_1[q][n]);
                        acc.r += p.r; acc.i += p.i;
                    }
                    s[t][12 + q] = acc;
                }
                for (int q = 0; q < 4; ++q) {
                    Complex acc{};
                    for (int n = 0; n < 13; ++n) {
                        Complex p = Mul(buf[2][n], tbl.f34_2[q][n]);
                        acc.r += p.r; acc.i += p.i;
                    }
                    s[t][20 + q] = acc;
                }
                for (int k = 3; k < 64; ++k) s[t][21 + k] = buf[k][6];
            }
        }

        // Decorrelation + transient damping.
        Complex d[32][91]{};
        static constexpr float kApG[3] = {0.651439057f, 0.564718122f, 0.489541659f};
        static constexpr int kApDelay[3] = {3, 4, 5};
        for (int t = 0; t < 32; ++t) {
            float power[34]{};
            for (int k = 0; k < nrQmf; ++k) {
                int b = mapBand[k];
                power[b] += s[t][k].r * s[t][k].r + s[t][k].i * s[t][k].i;
            }
            float transRatio[34]{};
            for (int b = 0; b < nrBands; ++b) {
                peakDecayNrg[b] = std::max(power[b], peakDecayNrg[b] * 0.765928338f);
                powerSmooth[b] += 0.25f * (power[b] - powerSmooth[b]);
                transSmooth[b] += 0.25f * (peakDecayNrg[b] - transSmooth[b]);
                transRatio[b] = (transSmooth[b] > 1e-12f && 1.5f * powerSmooth[b] < transSmooth[b])
                                    ? std::min(1.0f, 1.5f * powerSmooth[b] / transSmooth[b])
                                    : 1.0f;
            }
            for (int k = 0; k < nrAllpass; ++k) {
                Complex xIn = s[t][k];
                Complex tmp = Mul(delay[k][1], tbl.phiFract[use34 ? 1 : 0][k]);
                memmove(delay[k] + 1, delay[k], 13 * sizeof(Complex));
                delay[k][0] = xIn;
                Complex y = tmp;
                for (int m = 0; m < 3; ++m) {
                    int len = kApDelay[m];
                    Complex dIn = apDelay[k][m][len - 1];
                    memmove(apDelay[k][m] + 1, apDelay[k][m], (size_t)(len - 1) * sizeof(Complex));
                    Complex frac = Mul(dIn, tbl.qFract[use34 ? 1 : 0][k][m]);
                    Complex vOut{frac.r - kApG[m] * y.r, frac.i - kApG[m] * y.i};
                    apDelay[k][m][0] = {y.r + kApG[m] * vOut.r, y.i + kApG[m] * vOut.i};
                    y = vOut;
                }
                float g = transRatio[mapBand[k]];
                d[t][k] = {y.r * g, y.i * g};
            }
            for (int k = nrAllpass; k < nrQmf; ++k) {
                int dl = k < nrHybrid ? 1 : 14;
                Complex y = delay[k][dl - 1];
                memmove(delay[k] + 1, delay[k], 13 * sizeof(Complex));
                delay[k][0] = s[t][k];
                float g = transRatio[mapBand[k]];
                d[t][k] = {y.r * g, y.i * g};
            }
        }

        // Stereo mixing per envelope.
        Complex left[32][91]{}, right[32][91]{};
        const unsigned envCount = std::max(1u, numEnv);
        for (unsigned e = 0; e < envCount; ++e) {
            int t0 = std::clamp(border[e], 0, 32);
            int t1 = std::clamp(border[e + 1], t0, 32);
            if (t1 <= t0) continue;
            float invLen = 1.0f / (float)(t1 - t0);
            static constexpr int kParBands[6] = {10, 20, 34, 10, 20, 34};
            static constexpr uint8_t kMap20To34[34] = {
                0,1,2,3,4,5,6,7,8,9, 10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17, 18,18,18,18, 19,19,19,19
            };
            auto expandBands = [&](const int* src, int nr, int* dst) {
                int tmp20[20]{};
                const int* p20 = src;
                if (nr == 10) {
                    for (int i = 0; i < 10; ++i) tmp20[2 * i] = tmp20[2 * i + 1] = src[i];
                    p20 = tmp20;
                }
                if (use34 && nr != 34) {
                    for (int i = 0; i < 34; ++i) dst[i] = p20[kMap20To34[i]];
                } else {
                    std::copy_n(p20, nrBands, dst);
                }
            };
            int iidMapped[34]{}, iccMapped[34]{};
            expandBands(iidPar[e], kParBands[iidMode], iidMapped);
            expandBands(iccPar[e], kParBands[iccMode], iccMapped);
            float h11[34]{}, h12[34]{}, h21[34]{}, h22[34]{};
            for (int b = 0; b < nrBands; ++b) {
                int iidIdx = iidQuant ? (15 + std::clamp(iidMapped[b] + 15, 0, 30))
                                      : std::clamp(iidMapped[b] + 7, 0, 14);
                int iccIdx = std::clamp(iccMapped[b], 0, 7);
                const float* h = (iccMode < 3) ? tbl.ha[iidIdx][iccIdx] : tbl.hb[iidIdx][iccIdx];
                h11[b] = h[0]; h12[b] = h[1]; h21[b] = h[2]; h22[b] = h[3];
            }
            if (!hInitialized) {
                std::copy_n(h11, nrBands, h11Prev);
                std::copy_n(h12, nrBands, h12Prev);
                std::copy_n(h21, nrBands, h21Prev);
                std::copy_n(h22, nrBands, h22Prev);
                hInitialized = true;
            }
            float s11[34], s12[34], s21[34], s22[34];
            for (int b = 0; b < nrBands; ++b) {
                s11[b] = (h11[b] - h11Prev[b]) * invLen;
                s12[b] = (h12[b] - h12Prev[b]) * invLen;
                s21[b] = (h21[b] - h21Prev[b]) * invLen;
                s22[b] = (h22[b] - h22Prev[b]) * invLen;
            }
            for (int t = t0; t < t1; ++t) {
                float step = (float)(t - t0 + 1);
                for (int k = 0; k < nrQmf; ++k) {
                    int b = mapBand[k];
                    float g11 = h11Prev[b] + s11[b] * step;
                    float g12 = h12Prev[b] + s12[b] * step;
                    float g21 = h21Prev[b] + s21[b] * step;
                    float g22 = h22Prev[b] + s22[b] * step;
                    Complex sk = s[t][k], dk = d[t][k];
                    left[t][k]  = {g11 * sk.r + g21 * dk.r, g11 * sk.i + g21 * dk.i};
                    right[t][k] = {g12 * sk.r + g22 * dk.r, g12 * sk.i + g22 * dk.i};
                }
            }
            std::copy_n(h11, nrBands, h11Prev);
            std::copy_n(h12, nrBands, h12Prev);
            std::copy_n(h21, nrBands, h21Prev);
            std::copy_n(h22, nrBands, h22Prev);
        }

        // Hybrid synthesis back into 64 QMF subbands.
        for (int t = 0; t < 32; ++t) {
            if (!use34) {
                lQmf[t][0] = {};
                rQmf[t][0] = {};
                for (int q = 0; q < 6; ++q) {
                    lQmf[t][0].r += left[t][q].r;  lQmf[t][0].i += left[t][q].i;
                    rQmf[t][0].r += right[t][q].r; rQmf[t][0].i += right[t][q].i;
                }
                lQmf[t][1] = {left[t][6].r + left[t][7].r, left[t][6].i + left[t][7].i};
                rQmf[t][1] = {right[t][6].r + right[t][7].r, right[t][6].i + right[t][7].i};
                lQmf[t][2] = {left[t][8].r + left[t][9].r, left[t][8].i + left[t][9].i};
                rQmf[t][2] = {right[t][8].r + right[t][9].r, right[t][8].i + right[t][9].i};
                for (int k = 3; k < 64; ++k) {
                    lQmf[t][k] = left[t][7 + k];
                    rQmf[t][k] = right[t][7 + k];
                }
            } else {
                lQmf[t][0] = {}; rQmf[t][0] = {};
                for (int q = 0; q < 12; ++q) {
                    lQmf[t][0].r += left[t][q].r;  lQmf[t][0].i += left[t][q].i;
                    rQmf[t][0].r += right[t][q].r; rQmf[t][0].i += right[t][q].i;
                }
                lQmf[t][1] = {}; rQmf[t][1] = {};
                for (int q = 0; q < 8; ++q) {
                    lQmf[t][1].r += left[t][12 + q].r;  lQmf[t][1].i += left[t][12 + q].i;
                    rQmf[t][1].r += right[t][12 + q].r; rQmf[t][1].i += right[t][12 + q].i;
                }
                lQmf[t][2] = {}; rQmf[t][2] = {};
                for (int q = 0; q < 4; ++q) {
                    lQmf[t][2].r += left[t][20 + q].r;  lQmf[t][2].i += left[t][20 + q].i;
                    rQmf[t][2].r += right[t][20 + q].r; rQmf[t][2].i += right[t][20 + q].i;
                }
                for (int k = 3; k < 64; ++k) {
                    lQmf[t][k] = left[t][21 + k];
                    rQmf[t][k] = right[t][21 + k];
                }
            }
        }
    }
};

// --- SBR Bitstream Parsing Helpers ---

bool ReadGrid(Bits& b, SbrChannel& c) {
    SbrGrid& g = c.grid;
    g.frameClass = (unsigned)b.Get(2);
    switch (g.frameClass) {
    case 0: {  // FIXFIX
        unsigned n = 1u << b.Get(2);
        if (n > 4) return false;
        g.L_E = n;
        uint8_t r0 = (uint8_t)b.Get(1);
        for (unsigned i = 0; i < g.L_E; ++i) g.r[i] = r0;
        int step = 16 / (int)g.L_E;
        for (unsigned i = 0; i <= g.L_E; ++i) g.t_E[i] = (int)i * step;
        g.l_A = -1;
        break;
    }
    case 1: {  // FIXVAR
        int varBor = (int)b.Get(2);
        unsigned rel = (unsigned)b.Get(2);
        g.L_E = rel + 1;
        if (g.L_E > 4) return false;
        int relVal[4]{};
        for (unsigned i = 0; i < rel; ++i) relVal[i] = 2 * ((int)b.Get(2) + 1);
        unsigned ptr = (unsigned)b.Get(CeilLog2(g.L_E + 1));
        for (unsigned i = 0; i < g.L_E; ++i) g.r[g.L_E - 1 - i] = (uint8_t)b.Get(1);
        g.t_E[0] = 0;
        g.t_E[g.L_E] = 16 + varBor;
        for (int i = (int)rel - 1; i >= 0; --i) {
            g.t_E[i + 1] = g.t_E[i + 2] - relVal[rel - 1 - i];
            if (g.t_E[i + 1] < g.t_E[0] || g.t_E[i + 1] > 16) return false;
        }
        g.l_A = ptr == 0 ? -1 : (ptr == 1 ? (int)g.L_E - 1 : (int)g.L_E - (int)ptr);
        break;
    }
    case 2: {  // VARFIX
        int varBor = (int)b.Get(2);
        unsigned rel = (unsigned)b.Get(2);
        g.L_E = rel + 1;
        if (g.L_E > 4) return false;
        int relVal[4]{};
        for (unsigned i = 0; i < rel; ++i) relVal[i] = 2 * ((int)b.Get(2) + 1);
        unsigned ptr = (unsigned)b.Get(CeilLog2(g.L_E + 1));
        for (unsigned i = 0; i < g.L_E; ++i) g.r[i] = (uint8_t)b.Get(1);
        g.t_E[0] = varBor;
        g.t_E[g.L_E] = 16;
        for (unsigned i = 0; i < rel; ++i) {
            g.t_E[i + 1] = g.t_E[i] + relVal[i];
            if (g.t_E[i + 1] > 16) return false;
        }
        g.l_A = ptr == 0 ? -1 : (ptr == 1 ? 0 : (int)ptr - 1);
        break;
    }
    case 3: {  // VARVAR
        int varLeft = (int)b.Get(2);
        int varRight = (int)b.Get(2);
        unsigned relL = (unsigned)b.Get(2);
        unsigned relR = (unsigned)b.Get(2);
        g.L_E = relL + relR + 1;
        if (g.L_E > kMaxEnv) return false;
        int valL[4]{}, valR[4]{};
        for (unsigned i = 0; i < relL; ++i) valL[i] = 2 * ((int)b.Get(2) + 1);
        for (unsigned i = 0; i < relR; ++i) valR[i] = 2 * ((int)b.Get(2) + 1);
        unsigned ptr = (unsigned)b.Get(CeilLog2(g.L_E + 1));
        for (unsigned i = 0; i < g.L_E; ++i) g.r[i] = (uint8_t)b.Get(1);
        g.t_E[0] = varLeft;
        g.t_E[g.L_E] = 16 + varRight;
        for (unsigned i = 0; i < relL; ++i) g.t_E[i + 1] = g.t_E[i] + valL[i];
        for (unsigned i = 0; i < relR; ++i) g.t_E[g.L_E - 1 - i] = g.t_E[g.L_E - i] - valR[i];
        for (unsigned i = 0; i < g.L_E; ++i)
            if (g.t_E[i] >= g.t_E[i + 1]) return false;
        g.l_A = ptr == 0 ? -1 : (ptr == 1 ? (int)g.L_E - 1 : (int)g.L_E - (int)ptr);
        break;
    }
    }
    g.L_Q = g.L_E > 1 ? 2 : 1;
    g.t_Q[0] = g.t_E[0];
    g.t_Q[g.L_Q] = g.t_E[g.L_E];
    if (g.L_Q == 2) {
        int mid = 1;
        if (g.frameClass == 0) mid = (int)g.L_E / 2;
        else {
            while ((unsigned)mid + 1 < g.L_E && g.t_E[mid] < 8) ++mid;
        }
        g.t_Q[1] = g.t_E[mid];
    }
    return b.Good();
}

bool ReadEnvelope(Bits& b, const SbrElement& el, SbrChannel& c, bool coupledCh1) {
    const SbrBooks& books = Books();
    unsigned ampRes = el.header.ampRes;
    // ISO/IEC 14496-3 4.6.18.3.3: single-envelope FIXFIX frames always use 1.5 dB resolution.
    if (c.grid.frameClass == 0 && c.grid.L_E == 1) ampRes = 1;
    const PrefixBook& tBook = coupledCh1 ? (ampRes ? books.tEnvBal1_5 : books.tEnvBal3_0)
                                         : (ampRes ? books.tEnv1_5 : books.tEnv3_0);
    const PrefixBook& fBook = coupledCh1 ? (ampRes ? books.fEnvBal1_5 : books.fEnvBal3_0)
                                         : (ampRes ? books.fEnv1_5 : books.fEnv3_0);
    int off = coupledCh1 ? (ampRes ? 24 : 12) : (ampRes ? 60 : 31);
    unsigned bits0 = coupledCh1 ? (ampRes ? 6u : 5u) : (ampRes ? 7u : 6u);

    for (unsigned l = 0; l < c.grid.L_E; ++l) {
        unsigned bands = c.grid.r[l] ? el.nHigh : el.nLow;
        if (c.dfEnv[l] == 0) {
            c.env[l][0] = (int)b.Get(bits0);
            for (unsigned m = 1; m < bands; ++m)
                c.env[l][m] = c.env[l][m - 1] + (fBook.Read(b) - off);
        } else {
            const int* prev = l ? c.env[l - 1] : c.envPrev;
            unsigned prevR = l ? c.grid.r[l - 1] : c.rPrev;
            bool havePrev = l > 0 || c.hasPrevEnv;
            for (unsigned m = 0; m < bands; ++m) {
                int delta = tBook.Read(b) - off;
                int base = 0;
                if (havePrev) {
                    if (prevR == c.grid.r[l]) base = prev[m];
                    else if (prevR == 0 && c.grid.r[l] == 1) {
                        unsigned fk = el.fHigh[m];
                        unsigned j = 0;
                        while (j + 1 < el.nLow && el.fLow[j + 1] <= fk) ++j;
                        base = prev[j];
                    } else {
                        unsigned fk = el.fLow[m];
                        unsigned j = 0;
                        while (j + 1 < el.nHigh && el.fHigh[j] < fk) ++j;
                        base = prev[j];
                    }
                    if (l == 0 && c.ampResPrev != ampRes)
                        base = ampRes ? (base << 1) : (base >> 1);
                }
                c.env[l][m] = base + delta;
            }
        }
        if (!b.Good()) return false;
    }
    if (c.grid.L_E) {
        unsigned lastBands = c.grid.r[c.grid.L_E - 1] ? el.nHigh : el.nLow;
        std::copy_n(c.env[c.grid.L_E - 1], lastBands, c.envPrev);
        c.rPrev = c.grid.r[c.grid.L_E - 1];
        c.ampResPrev = ampRes;
        c.hasPrevEnv = true;
    }
    return true;
}

bool ReadNoise(Bits& b, const SbrElement& el, SbrChannel& c, bool coupledCh1) {
    const SbrBooks& books = Books();
    const PrefixBook& tBook = coupledCh1 ? books.tNoiseBal3_0 : books.tNoise3_0;
    const PrefixBook& fBook = coupledCh1 ? books.fEnvBal3_0 : books.fEnv3_0;
    int off = coupledCh1 ? 12 : 31;
    for (unsigned q = 0; q < c.grid.L_Q; ++q) {
        if (c.dfNoise[q] == 0) {
            c.noise[q][0] = (int)b.Get(5);
            for (unsigned m = 1; m < el.nQ; ++m)
                c.noise[q][m] = c.noise[q][m - 1] + (fBook.Read(b) - off);
        } else {
            const int* prev = q ? c.noise[q - 1] : c.noisePrev;
            for (unsigned m = 0; m < el.nQ; ++m) {
                int delta = tBook.Read(b) - off;
                c.noise[q][m] = (q > 0 || c.hasPrevNoise ? prev[m] : 0) + delta;
            }
        }
        if (!b.Good()) return false;
    }
    if (c.grid.L_Q) {
        std::copy_n(c.noise[c.grid.L_Q - 1], el.nQ, c.noisePrev);
        c.hasPrevNoise = true;
    }
    return true;
}

}  // namespace

struct SbrDecoder::Impl {
    unsigned coreRate = 0;
    unsigned coreChannels = 0;
    QmfTables qmf;
    PsTables psTables;
    PsState ps;
    SbrElement elem[kMaxSbrElements];
    Complex lQmf[kQmfSlots][64]{};
    Complex rQmf[kQmfSlots][64]{};

    void ProcessChannel(SbrElement& el, unsigned chIdx, const float* in1024, Complex (*outQmf)[64]) {
        SbrChannel& c = el.ch[chIdx];
        // 1. Shift 8-slot lookback history in xLow and analyze 32 core slots into xLow[8..39].
        for (unsigned t = 0; t < kHfGen; ++t) std::copy_n(c.xLow[kQmfSlots + t], 32, c.xLow[t]);
        float scaled[32];
        for (unsigned slot = 0; slot < kQmfSlots; ++slot) {
            for (unsigned i = 0; i < 32; ++i) scaled[i] = in1024[slot * 32 + i] * 32768.0f;
            qmf.Analyze32(scaled, c.xRing, c.xLow[kHfGen + slot]);
        }

        // If no valid SBR header/data yet, upsample core QMF cleanly with zeroed highband.
        if (!el.headerSeen || !el.dataParsed || !el.M) {
            for (unsigned t = 0; t < kQmfSlots; ++t) {
                std::copy_n(c.xLow[kHfAdj + t], 32, outQmf[t]);
                std::fill_n(outQmf[t] + 32, 32, Complex{});
            }
            return;
        }

        // 2. Chirp smoothing & 2nd-order LPC HF generation (ISO/IEC 14496-3 4.6.18.6).
        static constexpr float kInvfBw[4] = {0.0f, 0.6f, 0.75f, 0.9f};
        float bwNew[5]{};
        for (unsigned g = 0; g < el.nQ; ++g) {
            float target = kInvfBw[std::min<unsigned>(c.invfMode[g], 3)];
            float prev = c.bwArray[g];
            float bw = target < prev ? 0.75f * target + 0.25f * prev : 0.90625f * target + 0.09375f * prev;
            bwNew[g] = bw < 0.015625f ? 0.0f : std::min(bw, 0.99609375f);
            c.bwArray[g] = bwNew[g];
        }

        Complex alpha0[32]{}, alpha1[32]{};
        for (unsigned k = 1; k < el.k0; ++k) {
            double r01 = 0, i01 = 0, r02 = 0, i02 = 0, r11 = 0, r12 = 0, i12 = 0, r22 = 0;
            for (unsigned n = kHfAdj; n < 38 + kHfAdj; ++n) {
                Complex x0 = c.xLow[n][k], x1 = c.xLow[n - 1][k], x2 = c.xLow[n - 2][k];
                Complex p01 = MulConj(x0, x1), p02 = MulConj(x0, x2), p12 = MulConj(x1, x2);
                r01 += p01.r; i01 += p01.i;
                r02 += p02.r; i02 += p02.i;
                r12 += p12.r; i12 += p12.i;
                r11 += (double)x1.r * x1.r + (double)x1.i * x1.i;
                r22 += (double)x2.r * x2.r + (double)x2.i * x2.i;
            }
            double d = r11 * r22 - (r12 * r12 + i12 * i12) * 0.999999;
            if (d > 1e-12) {
                double inv = 1.0 / d;
                double a1r = ((r01 * r12 - i01 * i12) - r02 * r11) * inv;
                double a1i = ((r01 * i12 + i01 * r12) - i02 * r11) * inv;
                double a0r = 0, a0i = 0;
                if (r11 > 1e-12) {
                    a0r = -(r01 + (a1r * r12 + a1i * i12)) / r11;
                    a0i = -(i01 + (a1i * r12 - a1r * i12)) / r11;
                }
                if (a0r * a0r + a0i * a0i < 16.0 && a1r * a1r + a1i * a1i < 16.0) {
                    alpha0[k] = {(float)a0r, (float)a0i};
                    alpha1[k] = {(float)a1r, (float)a1i};
                }
            }
        }

        Complex xHigh[38][48]{};  // subband kx + m -> [0 .. M-1], slots t = 0..37 (xLow[2..39])
        unsigned kOut = 0;
        for (unsigned p = 0; p < el.numPatches; ++p) {
            for (unsigned x = 0; x < el.patchSubbands[p] && kOut < el.M; ++x, ++kOut) {
                unsigned kSrc = el.patchStart[p] + x;
                unsigned kDst = el.kx + kOut;
                unsigned g = 0;
                while (g + 1 < el.nQ && kDst >= el.fNoise[g + 1]) ++g;
                float bw = bwNew[g], bw2 = bw * bw;
                Complex a0 = {alpha0[kSrc].r * bw, alpha0[kSrc].i * bw};
                Complex a1 = {alpha1[kSrc].r * bw2, alpha1[kSrc].i * bw2};
                for (unsigned t = 0; t < 38; ++t) {
                    Complex cur = c.xLow[kHfAdj + t][kSrc];
                    Complex p1 = Mul(a0, c.xLow[kHfAdj + t - 1][kSrc]);
                    Complex p2 = Mul(a1, c.xLow[kHfAdj + t - 2][kSrc]);
                    xHigh[t][kOut] = {cur.r + p1.r + p2.r, cur.i + p1.i + p2.i};
                }
            }
        }

        // 3. Envelope, noise, and sinusoid adjustment (ISO/IEC 14496-3 4.6.18.7).
        if (el.kx != el.kxPrev || el.M != el.mPrev) c.resetDelay = true;
        Complex yOut[38][64]{};
        const int t0First = std::clamp(2 * c.grid.t_E[0], 0, 6);
        for (int t = 0; t < t0First; ++t) std::copy_n(c.yPrev[t] + el.kx, el.M, yOut[t] + el.kx);

        static constexpr float kLimGains[4] = {0.5f, 1.0f, 2.0f, 1e10f};
        static constexpr float kHSmooth[5] = {0.33333333333333f, 0.30150283239582f, 0.21816949906249f,
                                              0.11516383427084f, 0.03183050093751f};
        const float limGain = kLimGains[el.header.limiterGains];
        const unsigned hSL = el.header.smoothingMode ? 0u : 4u;

        for (unsigned l = 0; l < c.grid.L_E; ++l) {
            int ts = std::clamp(2 * c.grid.t_E[l], 0, 38);
            int te = std::clamp(2 * c.grid.t_E[l + 1], ts, 38);
            if (te <= ts) continue;
            unsigned slots = (unsigned)(te - ts);

            // Map E_orig and Q_orig to subbands 0..M-1.
            float eOrigSub[48]{}, qOrigSub[48]{}, eCurrSub[48]{};
            uint8_t sMapped[48]{}, bandHasSin[48]{};
            const unsigned* fTab = c.grid.r[l] ? el.fHigh : el.fLow;
            const unsigned nBands = c.grid.r[l] ? el.nHigh : el.nLow;
            unsigned qIdx = 0;
            while (qIdx + 1 < c.grid.L_Q && c.grid.t_E[l] >= c.grid.t_Q[qIdx + 1]) ++qIdx;

            for (unsigned b = 0; b < nBands; ++b) {
                unsigned sLo = fTab[b] - el.kx, sHi = fTab[b + 1] - el.kx;
                for (unsigned m = sLo; m < sHi && m < el.M; ++m) eOrigSub[m] = c.eOrig[l][b];
            }
            for (unsigned g = 0; g < el.nQ; ++g) {
                unsigned sLo = el.fNoise[g] - el.kx, sHi = el.fNoise[g + 1] - el.kx;
                for (unsigned m = sLo; m < sHi && m < el.M; ++m) qOrigSub[m] = c.qOrig[qIdx][g];
            }

            // Sinusoid presence mapping on high-res grid.
            const bool delta = ((int)l == c.grid.l_A) || (l == 0 && c.prevLA == (int)c.prevLE - 1);
            for (unsigned b = 0; b < el.nHigh; ++b) {
                bool activeSin = c.addHarmonic[b] && (c.addHarmonicPrev[b] || delta);
                if (activeSin) {
                    unsigned mid = (el.fHigh[b] + el.fHigh[b + 1]) / 2;
                    if (mid >= el.kx && mid - el.kx < el.M) sMapped[mid - el.kx] = 1;
                }
            }
            for (unsigned b = 0; b < nBands; ++b) {
                unsigned sLo = fTab[b] - el.kx, sHi = fTab[b + 1] - el.kx;
                uint8_t any = 0;
                for (unsigned m = sLo; m < sHi && m < el.M; ++m) any |= sMapped[m];
                for (unsigned m = sLo; m < sHi && m < el.M; ++m) bandHasSin[m] = any;
            }

            if (el.header.interpolFreq) {
                for (unsigned m = 0; m < el.M; ++m) {
                    double eSum = 0;
                    for (int t = ts; t < te; ++t) {
                        Complex v = xHigh[t][m];
                        eSum += (double)v.r * v.r + (double)v.i * v.i;
                    }
                    eCurrSub[m] = (float)(eSum / slots);
                }
            } else {
                for (unsigned b = 0; b < nBands; ++b) {
                    unsigned sLo = fTab[b] - el.kx, sHi = fTab[b + 1] - el.kx;
                    double eSum = 0;
                    for (unsigned m = sLo; m < sHi && m < el.M; ++m)
                        for (int t = ts; t < te; ++t) {
                            Complex v = xHigh[t][m];
                            eSum += (double)v.r * v.r + (double)v.i * v.i;
                        }
                    float avg = (sHi > sLo) ? (float)(eSum / (slots * (sHi - sLo))) : 0.0f;
                    for (unsigned m = sLo; m < sHi && m < el.M; ++m) eCurrSub[m] = avg;
                }
            }

            float gTemp[48]{}, qTemp[48]{}, sTemp[48]{};
            for (unsigned m = 0; m < el.M; ++m) {
                float eO = eOrigSub[m], qO = qOrigSub[m], den = 1.0f + qO;
                qTemp[m] = std::sqrt(eO * qO / den);
                if (sMapped[m]) sTemp[m] = std::sqrt(eO / den);
                if (bandHasSin[m]) gTemp[m] = std::sqrt(eO * qO / ((1.0f + eCurrSub[m]) * den));
                else gTemp[m] = std::sqrt(eO / ((1.0f + eCurrSub[m]) * den));
            }

            // Limiter across fLim bands.
            for (unsigned lb = 0; lb < el.nL; ++lb) {
                unsigned sLo = el.fLim[lb] - el.kx, sHi = el.fLim[lb + 1] - el.kx;
                double eOrigSum = 1e-12, eCurrSum = 1e-12;
                for (unsigned m = sLo; m < sHi && m < el.M; ++m) {
                    eOrigSum += eOrigSub[m];
                    eCurrSum += eCurrSub[m];
                }
                float gMax = std::min((float)(std::sqrt(eOrigSum / eCurrSum) * limGain), 1e5f);
                double gBoostDen = 1e-12;
                for (unsigned m = sLo; m < sHi && m < el.M; ++m) {
                    if (gTemp[m] > gMax) {
                        qTemp[m] *= gMax / std::max(gTemp[m], 1e-12f);
                        gTemp[m] = gMax;
                    }
                    gBoostDen += (double)eCurrSub[m] * gTemp[m] * gTemp[m] +
                                 (sMapped[m] ? (double)sTemp[m] * sTemp[m] : (double)qTemp[m] * qTemp[m]);
                }
                float gBoost = std::min((float)std::sqrt(eOrigSum / gBoostDen), 1.584893192f);
                for (unsigned m = sLo; m < sHi && m < el.M; ++m) {
                    gTemp[m] *= gBoost;
                    qTemp[m] *= gBoost;
                    sTemp[m] *= gBoost;
                }
            }

            const bool noSmooth = delta || c.resetDelay || hSL == 0;
            if (c.resetDelay) {
                for (unsigned k = 0; k < 4; ++k) {
                    std::copy_n(gTemp, el.M, c.gTempPrev[k]);
                    std::copy_n(qTemp, el.M, c.qTempPrev[k]);
                }
                c.resetDelay = false;
            }
            for (int t = ts; t < te; ++t) {
                float gFilt[48]{}, qFilt[48]{};
                if (noSmooth) {
                    std::copy_n(gTemp, el.M, gFilt);
                    std::copy_n(qTemp, el.M, qFilt);
                } else {
                    for (unsigned m = 0; m < el.M; ++m) {
                        gFilt[m] = kHSmooth[0] * gTemp[m] + kHSmooth[1] * c.gTempPrev[0][m] +
                                   kHSmooth[2] * c.gTempPrev[1][m] + kHSmooth[3] * c.gTempPrev[2][m] +
                                   kHSmooth[4] * c.gTempPrev[3][m];
                        qFilt[m] = kHSmooth[0] * qTemp[m] + kHSmooth[1] * c.qTempPrev[0][m] +
                                   kHSmooth[2] * c.qTempPrev[1][m] + kHSmooth[3] * c.qTempPrev[2][m] +
                                   kHSmooth[4] * c.qTempPrev[3][m];
                    }
                }
                for (int k = 3; k > 0; --k) {
                    std::copy_n(c.gTempPrev[k - 1], el.M, c.gTempPrev[k]);
                    std::copy_n(c.qTempPrev[k - 1], el.M, c.qTempPrev[k]);
                }
                std::copy_n(gTemp, el.M, c.gTempPrev[0]);
                std::copy_n(qTemp, el.M, c.qTempPrev[0]);

                c.indexSine = (c.indexSine + 1) & 3;
                static constexpr float kSinRe[4] = {1.0f, 0.0f, -1.0f, 0.0f};
                static constexpr float kSinIm[4] = {0.0f, 1.0f, 0.0f, -1.0f};
                float sSign = (c.indexSine & 1) ? 0.0f : 1.0f;
                (void)sSign;

                for (unsigned m = 0; m < el.M; ++m) {
                    c.indexNoise = (c.indexNoise + 1) & 511;
                    float re = xHigh[t][m].r * gFilt[m];
                    float im = xHigh[t][m].i * gFilt[m];
                    if (sMapped[m]) {
                        float pSign = ((el.kx + m) & 1) ? -1.0f : 1.0f;
                        re += sTemp[m] * kSinRe[c.indexSine];
                        im += sTemp[m] * kSinIm[c.indexSine] * pSign;
                    } else {
                        re += qFilt[m] * sbrdata::sbrNoise[c.indexNoise][0];
                        im += qFilt[m] * sbrdata::sbrNoise[c.indexNoise][1];
                    }
                    yOut[t][el.kx + m] = {re, im};
                }
            }
        }

        // Save slots 32..37 for the next frame and assemble 32 output QMF slots.
        for (int t = 32; t < 38; ++t) std::copy_n(yOut[t] + el.kx, el.M, c.yPrev[t - 32] + el.kx);
        for (unsigned t = 0; t < kQmfSlots; ++t) {
            std::copy_n(c.xLow[kHfAdj + t], el.kx, outQmf[t]);
            std::copy_n(yOut[t] + el.kx, el.M, outQmf[t] + el.kx);
            if (el.kx + el.M < 64) std::fill_n(outQmf[t] + el.kx + el.M, 64 - (el.kx + el.M), Complex{});
        }

        c.prevLA = c.grid.l_A;
        c.prevLE = c.grid.L_E;
        std::copy_n(c.addHarmonic, el.nHigh, c.addHarmonicPrev);
    }

    void Synthesize2048(SbrChannel& c, const Complex (*qmfSlots)[64], float* out2048) const {
        for (unsigned slot = 0; slot < kQmfSlots; ++slot) {
            float blk[64];
            qmf.Synthesize64(qmfSlots[slot], c.vRing, blk);
            for (unsigned i = 0; i < 64; ++i) out2048[slot * 64 + i] = blk[i] * (1.0f / 32768.0f);
        }
    }
};

SbrDecoder::SbrDecoder() = default;
SbrDecoder::~SbrDecoder() = default;
SbrDecoder::SbrDecoder(SbrDecoder&&) noexcept = default;
SbrDecoder& SbrDecoder::operator=(SbrDecoder&&) noexcept = default;

void SbrDecoder::Init(unsigned coreRate, unsigned coreChannels, bool psHint) {
    if (!d_) d_ = std::make_unique<Impl>();
    d_->coreRate = coreRate;
    d_->coreChannels = coreChannels;
    d_->qmf.Init();
    d_->psTables.Init();
    coreRate_ = coreRate;
    active_ = true;
    psActive_ = psHint && coreChannels == 1;
    Reset();
}

void SbrDecoder::Reset() {
    if (!d_) return;
    for (auto& el : d_->elem) {
        el.dataParsed = false;
        el.ch[0].Reset();
        el.ch[1].Reset();
    }
    d_->ps.Reset();
}

bool SbrDecoder::Parse(Bits& b, unsigned elemIdx, unsigned elemChannels, bool crc) {
    if (!d_ || elemIdx >= kMaxSbrElements || (elemChannels != 1 && elemChannels != 2)) return false;
    SbrElement& el = d_->elem[elemIdx];
    if (crc) b.Skip(10);
    bool headerFlag = b.Get(1) != 0;
    if (headerFlag) {
        SbrHeader h;
        // ISO/IEC 14496-3 Table 4.56: bs_amp_res == 0 signals 1.5 dB (fine), 1 signals 3.0 dB (coarse).
        h.ampRes = 1u - (unsigned)b.Get(1);
        h.startFreq = (unsigned)b.Get(4);
        h.stopFreq = (unsigned)b.Get(4);
        h.xoverBand = (unsigned)b.Get(3);
        b.Skip(2);  // bs_reserved
        const bool extra1 = b.Get(1) != 0;
        const bool extra2 = b.Get(1) != 0;
        if (extra1) {
            h.freqScale = (unsigned)b.Get(2);
            h.alterScale = (unsigned)b.Get(1);
            h.noiseBands = (unsigned)b.Get(2);
        }
        if (extra2) {
            h.limiterBands = (unsigned)b.Get(2);
            h.limiterGains = (unsigned)b.Get(2);
            h.interpolFreq = (unsigned)b.Get(1);
            h.smoothingMode = (unsigned)b.Get(1);
        }
        if (!b.Good()) return false;
        bool changed = !el.headerSeen || !(h == el.header);
        el.header = h;
        el.headerSeen = true;
        if (changed) {
            el.kxPrev = el.kx;
            el.mPrev = el.M;
            if (!el.BuildTables(coreRate_ * 2)) return false;
            if (el.kxPrev == 0) {
                el.kxPrev = el.kx;
                el.mPrev = el.M;
            }
            el.ch[0].hasPrevEnv = false;
            el.ch[1].hasPrevEnv = false;
            el.ch[0].hasPrevNoise = false;
            el.ch[1].hasPrevNoise = false;
        }
    }
    if (!el.headerSeen) {
        el.dataParsed = false;
        return b.Good();
    }

    if (elemChannels == 1) {
        if (b.Get(1)) b.Skip(4);
        el.coupling = false;
        if (!ReadGrid(b, el.ch[0])) return false;
        for (unsigned l = 0; l < el.ch[0].grid.L_E; ++l) el.ch[0].dfEnv[l] = (uint8_t)b.Get(1);
        for (unsigned q = 0; q < el.ch[0].grid.L_Q; ++q) el.ch[0].dfNoise[q] = (uint8_t)b.Get(1);
        for (unsigned g = 0; g < el.nQ; ++g) el.ch[0].invfMode[g] = (uint8_t)b.Get(2);
        if (!ReadEnvelope(b, el, el.ch[0], false) || !ReadNoise(b, el, el.ch[0], false)) return false;
        el.ch[0].hasHarmonic = b.Get(1) != 0;
        std::fill_n(el.ch[0].addHarmonic, el.nHigh, (uint8_t)0);
        if (el.ch[0].hasHarmonic)
            for (unsigned i = 0; i < el.nHigh; ++i) el.ch[0].addHarmonic[i] = (uint8_t)b.Get(1);
    } else {
        if (b.Get(1)) b.Skip(8);
        el.coupling = b.Get(1) != 0;
        if (el.coupling) {
            if (!ReadGrid(b, el.ch[0])) return false;
            el.ch[1].grid = el.ch[0].grid;
            for (unsigned l = 0; l < el.ch[0].grid.L_E; ++l) el.ch[0].dfEnv[l] = (uint8_t)b.Get(1);
            for (unsigned q = 0; q < el.ch[0].grid.L_Q; ++q) el.ch[0].dfNoise[q] = (uint8_t)b.Get(1);
            for (unsigned l = 0; l < el.ch[1].grid.L_E; ++l) el.ch[1].dfEnv[l] = (uint8_t)b.Get(1);
            for (unsigned q = 0; q < el.ch[1].grid.L_Q; ++q) el.ch[1].dfNoise[q] = (uint8_t)b.Get(1);
            for (unsigned g = 0; g < el.nQ; ++g) el.ch[0].invfMode[g] = el.ch[1].invfMode[g] = (uint8_t)b.Get(2);
            if (!ReadEnvelope(b, el, el.ch[0], false) || !ReadEnvelope(b, el, el.ch[1], true)) return false;
            if (!ReadNoise(b, el, el.ch[0], false) || !ReadNoise(b, el, el.ch[1], true)) return false;
        } else {
            if (!ReadGrid(b, el.ch[0]) || !ReadGrid(b, el.ch[1])) return false;
            for (unsigned l = 0; l < el.ch[0].grid.L_E; ++l) el.ch[0].dfEnv[l] = (uint8_t)b.Get(1);
            for (unsigned q = 0; q < el.ch[0].grid.L_Q; ++q) el.ch[0].dfNoise[q] = (uint8_t)b.Get(1);
            for (unsigned l = 0; l < el.ch[1].grid.L_E; ++l) el.ch[1].dfEnv[l] = (uint8_t)b.Get(1);
            for (unsigned q = 0; q < el.ch[1].grid.L_Q; ++q) el.ch[1].dfNoise[q] = (uint8_t)b.Get(1);
            for (unsigned g = 0; g < el.nQ; ++g) el.ch[0].invfMode[g] = (uint8_t)b.Get(2);
            for (unsigned g = 0; g < el.nQ; ++g) el.ch[1].invfMode[g] = (uint8_t)b.Get(2);
            if (!ReadEnvelope(b, el, el.ch[0], false) || !ReadEnvelope(b, el, el.ch[1], false)) return false;
            if (!ReadNoise(b, el, el.ch[0], false) || !ReadNoise(b, el, el.ch[1], false)) return false;
        }
        for (unsigned c = 0; c < 2; ++c) {
            el.ch[c].hasHarmonic = b.Get(1) != 0;
            std::fill_n(el.ch[c].addHarmonic, el.nHigh, (uint8_t)0);
            if (el.ch[c].hasHarmonic)
                for (unsigned i = 0; i < el.nHigh; ++i) el.ch[c].addHarmonic[i] = (uint8_t)b.Get(1);
        }
    }
    if (!b.Good()) return false;

    // Dequantize E_orig and Q_orig.
    if (el.coupling) {
        unsigned ampRes = (el.ch[0].grid.frameClass == 0 && el.ch[0].grid.L_E == 1) ? 1u : el.header.ampRes;
        double a = ampRes ? 2.0 : 1.0;
        double panOff = ampRes ? 24.0 : 12.0;
        for (unsigned l = 0; l < el.ch[0].grid.L_E; ++l) {
            unsigned bands = el.ch[0].grid.r[l] ? el.nHigh : el.nLow;
            for (unsigned m = 0; m < bands; ++m) {
                double e0 = std::clamp(el.ch[0].env[l][m], 0, 127);
                double e1 = std::clamp(el.ch[1].env[l][m], 0, 48);
                double sumE = std::exp2(e0 / a + 7.0);
                el.ch[0].eOrig[l][m] = (float)(sumE / (1.0 + std::exp2((panOff - e1) / a)));
                el.ch[1].eOrig[l][m] = (float)(sumE / (1.0 + std::exp2((e1 - panOff) / a)));
            }
        }
        for (unsigned q = 0; q < el.ch[0].grid.L_Q; ++q)
            for (unsigned m = 0; m < el.nQ; ++m) {
                double q0 = std::clamp(el.ch[0].noise[q][m], 0, 30);
                double q1 = std::clamp(el.ch[1].noise[q][m], 0, 24);
                double sumQ = std::exp2(7.0 - q0);
                el.ch[0].qOrig[q][m] = (float)(sumQ / (1.0 + std::exp2(12.0 - q1)));
                el.ch[1].qOrig[q][m] = (float)(sumQ / (1.0 + std::exp2(q1 - 12.0)));
            }
    } else {
        for (unsigned c = 0; c < elemChannels; ++c) {
            unsigned ampRes = (el.ch[c].grid.frameClass == 0 && el.ch[c].grid.L_E == 1) ? 1u : el.header.ampRes;
            double a = ampRes ? 2.0 : 1.0;
            for (unsigned l = 0; l < el.ch[c].grid.L_E; ++l) {
                unsigned bands = el.ch[c].grid.r[l] ? el.nHigh : el.nLow;
                for (unsigned m = 0; m < bands; ++m) {
                    double e = std::clamp(el.ch[c].env[l][m], 0, 127);
                    el.ch[c].eOrig[l][m] = (float)std::exp2(e / a + 6.0);
                }
            }
            for (unsigned q = 0; q < el.ch[c].grid.L_Q; ++q)
                for (unsigned m = 0; m < el.nQ; ++m) {
                    double qv = std::clamp(el.ch[c].noise[q][m], 0, 30);
                    el.ch[c].qOrig[q][m] = (float)std::exp2(6.0 - qv);
                }
        }
    }

    // Extended data (Parametric Stereo).
    if (b.Get(1)) {
        size_t cnt = (size_t)b.Get(4);
        if (cnt == 15) cnt += (size_t)b.Get(8);
        size_t bitsLeft = cnt * 8;
        if (bitsLeft > b.Left()) return false;
        while (bitsLeft >= 2) {
            unsigned extId = (unsigned)b.Get(2);
            bitsLeft -= 2;
            if (extId == 2 && elemChannels == 1 && d_->coreChannels == 1) {
                if (!d_->ps.Parse(b, bitsLeft)) return false;
                psActive_ = true;
            } else {
                break;
            }
        }
        if (bitsLeft) b.Skip(bitsLeft);
    }
    el.dataParsed = b.Good();
    return el.dataParsed;
}

void SbrDecoder::ApplyElement(unsigned elemIdx, unsigned elemChannels, const float* const* corePcm,
                              float* out0, float* out1, unsigned& outChannels) {
    if (!d_ || elemIdx >= kMaxSbrElements) { outChannels = 0; return; }
    SbrElement& el = d_->elem[elemIdx];
    d_->ProcessChannel(el, 0, corePcm[0], d_->lQmf);
    if (elemChannels == 1 && psActive_ && elemIdx == 0) {
        d_->ps.Apply(d_->psTables, d_->lQmf, d_->rQmf);
        d_->Synthesize2048(el.ch[0], d_->lQmf, out0);
        d_->Synthesize2048(el.ch[1], d_->rQmf, out1);
        outChannels = 2;
    } else if (elemChannels == 2) {
        d_->ProcessChannel(el, 1, corePcm[1], d_->rQmf);
        d_->Synthesize2048(el.ch[0], d_->lQmf, out0);
        d_->Synthesize2048(el.ch[1], d_->rQmf, out1);
        outChannels = 2;
    } else {
        d_->Synthesize2048(el.ch[0], d_->lQmf, out0);
        outChannels = 1;
    }
    el.dataParsed = false;
}

}  // namespace nativeaudio
