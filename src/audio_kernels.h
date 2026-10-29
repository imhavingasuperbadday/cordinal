// Declarations for the audio SIMD kernels. The SSE2 baseline implementations are
// always available; the AVX2/FMA versions live in the AVX2 translation unit and
// are only called after FastAudioKernels() returns true.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

namespace nativeaudio {
// NaN/infinity tests by exponent bits. MSVC's std::isfinite(float) is an out-of-line CRT
// call, which cost several percent of a decoder that checks every output sample.
inline bool Finite(float v) { uint32_t b; memcpy(&b,&v,4); return (b&0x7F800000u)!=0x7F800000u; }
inline bool AllFinite(const float* v,size_t n) {
    const __m128i exponent=_mm_set1_epi32(0x7F800000);
    __m128i bad=_mm_setzero_si128();
    size_t i=0;
    for (;i+4<=n;i+=4) bad=_mm_or_si128(bad,_mm_cmpeq_epi32(_mm_and_si128(_mm_loadu_si128((const __m128i*)(v+i)),exponent),exponent));
    bool ok=!_mm_movemask_epi8(bad);
    for (;i<n;++i) ok&=Finite(v[i]);
    return ok;
}
// x64's SSE2 path is always available. AVX2/FMA is entered only after checking
// CPU support and Windows' preservation of the YMM register state.
bool FastAudioKernels();
// FastAudioKernels() and carry-less multiplication (PCLMULQDQ), for AudioCrc16FoldAvx2.
bool FastCrcKernel();
// CRC-16 (polynomial 0x8005, MSB first) of p's whole 16-byte blocks, n >= 64: folds them into
// 16 bytes whose CRC, continued over the rest of p, is the CRC of all n bytes. Returns the
// bytes folded.
size_t AudioCrc16FoldAvx2(const uint8_t* p, size_t n, uint8_t* folded);
// FFT roots are packed by stage: complex root (half+j) serves butterfly j. quad holds the
// radix-4 passes' twiddles in pass order: t^2, t, t^3 for each four butterflies (Mdct::Init).
// grouped: the first two stages are already done (AudioMdctFoldAvx2 with grouped).
void AudioFftAvx2(float* work, const float* roots, const float* quad, unsigned size, bool grouped=false);
// FFT of 15*m points (m a power of two, at least 4): work holds 15 rows of m values, each
// bit-reversed in place, plus a sixteenth row of room; roots serve the m-point rows and
// twiddles[row*m+k] = W^(row*k). Writes the natural-order transform to out.
void AudioFft15Avx2(float* work, float* out, const float* roots, const float* twiddles, unsigned m, bool grouped=false);
// MDCT fold: `work[reverse[j]] = Mul({in[2j],-in[m-1-2j]},pre[j])`, each input first
// multiplied by its curve value unless curve is null (a flat floor). Processes four
// coefficients per iteration; work is the FFT's input order. grouped (q a multiple of 16, a
// power of two or 15 rows) also performs the first two stages of the FFT or of each row.
void AudioMdctFoldAvx2(float* work,const float* in,const float* curve,const float* pre,const unsigned* reverse,
    unsigned q,unsigned m,bool grouped=false);
// AAC long-block window/overlap for a 2048-point transform. transformed holds the
// 1024 DCT-IV coefficients; left/right are the current/next 1024-tap windows.
void AudioAacLongWindowAvx2(const float* transformed,const float* left,const float* right,
    const float* overlap,float* out,float* overlapOut);
// MDCT post-rotation: `out[2j]=Re(work[j]*post[j])`, `out[m-1-2j]=Im(...)`.
void AudioMdctPostAvx2(const float* work,const float* post,float* out,unsigned q,unsigned m);
// MPEG history has two identical 1024-float halves to make wrapped reads contiguous.
void AudioMpegSynthesisAvx2(const float* in,float* history,unsigned head,const float* factors,float* out);
void AudioMpegSynthesis18Avx2(const float* in,float* history,unsigned& head,const float* factors,float* out);
// Compact the interpolated weights for every reachable rational phase in place.
// denominator <= 256 ensures that no destination overwrites an unread source row.
void AudioResamplePrepareAvx2(float* table,unsigned denominator);
void AudioResampleAvx2(const float* history,const float* table,size_t frames,unsigned step,unsigned remainder,
    unsigned denominator,size_t& index,uint64_t& numerator,float* output);
// One WavPack decorrelation pass with a positive term (1-8, 17 or 18) over a stereo block of
// interleaved values, both channels at once; memory is each channel's stored history and
// weight the two adaptive weights, updated in place. Same integer results as the scalar pass.
void WavPackPredictAvx2(int32_t* values,size_t frames,int term,int* weight,int step,const int32_t (*memory)[8]);
}
