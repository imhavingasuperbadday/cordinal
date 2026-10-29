// Inverse MDCT via a DCT-IV and a quarter-size complex FFT. Tables are derived
// from the transform equations at setup, shared by every channel in a stream.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "audio_kernels.h"

namespace nativeaudio {
// Shared per-stream inverse MDCT. One instance serves one block size; tables are
// derived in Init and reused for every channel and packet.
class Mdct {
    struct Complex { float r, i; };
    static Complex Mul(Complex a, Complex b) { return {a.r*b.r-a.i*b.i, a.r*b.i+a.i*b.r}; }
public:
    // size is the full transformed block length; n_/4 selects the FFT size, a power
    // of two or 15 times one (Opus's 240-1920 sample blocks).
    void Init(unsigned size);
    // in: interleaved spectral input, curve: floor applied during load, out: n/2
    // real DCT-IV coefficients (Run reorders them into the time block).
    void Dct(const float* in, const float* curve, float* out);
    // Full IMDCT: Dct plus the reordering into n time-domain samples.
    void Run(const float* in, const float* curve, float* out);
    // Unity floor: the curve multiplication is skipped entirely. Codecs whose
    // spectral floor is flat (AAC, AC3, WMA, Opus) use these.
    void Dct(const float* in, float* out);
    void Run(const float* in, float* out);
private:
    // Shared body of the two Dct overloads. Unity is a compile-time choice so the
    // load loop never tests for a represented floor.
    template<bool Unity> void DctCore(const float* in,const float* curve,float* out);
    // q = 15*rowSize_: rowSize_-point FFTs along 15 rows, then the 15-point DFT down
    // each column. Returns the natural-order result in mixed_.
    void Fft15();
    unsigned n_ = 0, rowSize_ = 0;
    bool fast_ = false,radixTwo_=true,rows15_=false;
    std::vector<Complex> pre_, post_, roots_, work_, mixed_, twiddles_, quad_;
    std::vector<unsigned> reverse_;
    std::vector<float> dct_;
};
} // namespace nativeaudio
