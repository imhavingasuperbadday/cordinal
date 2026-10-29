// This translation unit alone is built for AVX2. The dispatcher is SSE2 code, so
// decoding stays available on machines without AVX2; these kernels mirror the
// baseline arithmetic and are only entered after FastAudioKernels() succeeds.
#include "audio_kernels.h"
#include "audio_mpeg_tables.h"
#include "cord_codec.h"
#include <algorithm>
#include <cmath>
#include <immintrin.h>

namespace nativeaudio {
// The channels sit in 32-bit lanes 0 and 2, so pmuldq forms each 64-bit weight*source. Only
// the low 32 bits of ((product+512)>>10) are kept, where a logical shift equals the
// arithmetic one. sign(sign(step,source),residual) is +step for matching signs, -step for
// opposite ones and 0 when either is zero: the scalar weight adaptation.
void WavPackPredictAvx2(int32_t* values,size_t frames,int term,int* weight,int step,const int32_t (*memory)[8]) {
    __m128i w=_mm_setr_epi32(weight[0],0,weight[1],0);
    const __m128i steps=_mm_set1_epi32(step),round=_mm_set1_epi64x(512);
    auto pair=[](const int32_t* p) { __m128i v=_mm_loadl_epi64((const __m128i*)p);return _mm_unpacklo_epi32(v,v); };
    auto apply=[&](size_t i,__m128i source) {
        __m128i residual=pair(values+i*2);
        __m128i value=_mm_add_epi32(residual,_mm_srli_epi64(_mm_add_epi64(_mm_mul_epi32(w,source),round),10));
        _mm_storel_epi64((__m128i*)(values+i*2),_mm_shuffle_epi32(value,0x08));
        w=_mm_add_epi32(w,_mm_sign_epi32(_mm_sign_epi32(steps,source),residual));
        return value;
    };
    if (term<=8) {
        const size_t head=std::min((size_t)term,frames);
        __m128i last=_mm_setzero_si128();
        for (size_t i=0;i<head;++i) last=apply(i,_mm_setr_epi32(memory[0][i],0,memory[1][i],0));
        // A one-sample term feeds each value straight back instead of through memory.
        if (term==1) for (size_t i=1;i<frames;++i) last=apply(i,last);
        else for (size_t i=term;i<frames;++i) apply(i,pair(values+(i-term)*2));
    } else {
        __m128i last=_mm_setr_epi32(memory[0][0],0,memory[1][0],0),before=_mm_setr_epi32(memory[0][1],0,memory[1][1],0);
        for (size_t i=0;i<frames;++i) {
            __m128i source=term==17?_mm_sub_epi32(_mm_add_epi32(last,last),before):
                _mm_add_epi32(last,_mm_srai_epi32(_mm_sub_epi32(last,before),1));
            before=last;last=apply(i,source);
        }
    }
    weight[0]=_mm_cvtsi128_si32(w);weight[1]=_mm_extract_epi32(w,2);
}
// CRC folding by carry-less multiplication. Read big-endian, a block is a polynomial with the
// message's first bit highest. For an accumulator A of 128 bits, A*x^d + B is congruent modulo
// P = x^16+x^15+x^2+1 to hi(A)*(x^(d+64) mod P) + lo(A)*(x^d mod P) + B, which again fits in
// 128 bits. Four accumulators fold 64 bytes per step (d = 512); they and any remaining whole
// blocks are then folded at d = 128.
size_t AudioCrc16FoldAvx2(const uint8_t* p,size_t n,uint8_t* folded) {
    const __m128i swap=_mm_setr_epi8(15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0);
    auto load=[&](size_t at) { return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(p+at)),swap); };
    // x^576, x^512, x^192 and x^128 modulo P: each pair multiplies the high and low halves.
    const __m128i far=_mm_set_epi64x(0x1446,0x8107),near=_mm_set_epi64x(0x1666,0x106);
    auto fold=[](__m128i a,__m128i k) { return _mm_xor_si128(_mm_clmulepi64_si128(a,k,0x11),_mm_clmulepi64_si128(a,k,0x00)); };
    __m128i a0=load(0),a1=load(16),a2=load(32),a3=load(48);
    size_t at=64;
    for (;at+64<=n;at+=64) {
        a0=_mm_xor_si128(fold(a0,far),load(at));a1=_mm_xor_si128(fold(a1,far),load(at+16));
        a2=_mm_xor_si128(fold(a2,far),load(at+32));a3=_mm_xor_si128(fold(a3,far),load(at+48));
    }
    __m128i a=_mm_xor_si128(fold(a0,near),a1);
    a=_mm_xor_si128(fold(a,near),a2);a=_mm_xor_si128(fold(a,near),a3);
    for (;at+16<=n;at+=16) a=_mm_xor_si128(fold(a,near),load(at));
    _mm_storeu_si128((__m128i*)folded,_mm_shuffle_epi8(a,swap));
    return at;
}
void AudioResamplePrepareAvx2(float* table,unsigned denominator) {
    const double phaseScale=256.0/denominator;
    for (unsigned num=0;num<denominator;++num) {
        double phase=num*phaseScale;unsigned p=(unsigned)phase;
        __m256 blend=_mm256_set1_ps((float)(phase-p));
        for (unsigned k=0;k<32;k+=8) {
            __m256 first=_mm256_loadu_ps(table+p*32+k),second=_mm256_loadu_ps(table+(p+1)*32+k);
            __m256 row=_mm256_fmadd_ps(_mm256_sub_ps(second,first),blend,first);
            // Put each half's four taps together so the hot loop duplicates weights
            // with lane-local unpacks instead of cross-lane variable permutations.
            _mm256_storeu_ps(table+num*32+k,_mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(row),0xD8)));
        }
    }
    _mm256_zeroupper();
}
// Common rate pairs visit at most 256 rational phases. Their weights are already
// interpolated, with the same FMA rounding, so convolution only loads one row.
template<bool Prepared> static void ResampleAvx2(const float* history,const float* table,size_t frames,unsigned step,unsigned remainder,
    unsigned denominator,size_t& index,uint64_t& numerator,float* output) {
    size_t at=index;uint64_t num=numerator;const double phaseScale=256.0/denominator;
    const __m256i low=_mm256_setr_epi32(0,0,1,1,2,2,3,3),high=_mm256_setr_epi32(4,4,5,5,6,6,7,7);
    for (size_t frame=0;frame<frames;++frame) {
        unsigned p=(unsigned)num;__m256 blend;
        if constexpr (!Prepared) {
            double phase=(unsigned)num*phaseScale;p=(unsigned)phase;blend=_mm256_set1_ps((float)(phase-p));
        }
        const float* first=table+p*32;const float* samples=history+(at-15)*2;
        __m256 a=_mm256_setzero_ps(),b=a;
        for (unsigned k=0;k<32;k+=8) {
            __m256 row=_mm256_loadu_ps(first+k);
            __m256 w=row;
            if constexpr (!Prepared) w=_mm256_fmadd_ps(_mm256_sub_ps(_mm256_loadu_ps(first+32+k),row),blend,row);
            __m256 lo,hi;
            if constexpr (Prepared) { lo=_mm256_unpacklo_ps(w,w);hi=_mm256_unpackhi_ps(w,w); }
            else { lo=_mm256_permutevar8x32_ps(w,low);hi=_mm256_permutevar8x32_ps(w,high); }
            a=_mm256_fmadd_ps(_mm256_loadu_ps(samples+k*2),lo,a);
            b=_mm256_fmadd_ps(_mm256_loadu_ps(samples+k*2+8),hi,b);
        }
        __m256 sum=_mm256_add_ps(a,b);
        __m128 halves=_mm_add_ps(_mm256_castps256_ps128(sum),_mm256_extractf128_ps(sum,1));
        __m128 stereo=_mm_add_ps(halves,_mm_movehl_ps(halves,halves));
        _mm_storel_epi64((__m128i*)(output+frame*2),_mm_castps_si128(stereo));
        num+=remainder;at+=step;if (num>=denominator) { num-=denominator;++at; }
    }
    index=at;numerator=num;_mm256_zeroupper();
}
void AudioResampleAvx2(const float* history,const float* table,size_t frames,unsigned step,unsigned remainder,
    unsigned denominator,size_t& index,uint64_t& numerator,float* output) {
    if (denominator<=256) ResampleAvx2<true>(history,table,frames,step,remainder,denominator,index,numerator,output);
    else ResampleAvx2<false>(history,table,frames,step,remainder,denominator,index,numerator,output);
}
// Four pre-rotated fold values z_j..z_j+3 as real and imaginary parts: z_j is
// (in[2j], -in[m-1-2j]) times pre[j]. A curve (Vorbis's floor) multiplies each input first.
template<bool Curve> static __forceinline void Fold4(const float* in,const float* curve,const float* pre,unsigned j,
    unsigned m,__m128& re,__m128& im) {
    const __m128 sign=_mm_castsi128_ps(_mm_set1_epi32((int)0x80000000u));
    // in[2j],in[2j+2],... and the mirrored in[m-1-2j],in[m-3-2j],...
    __m128 x=_mm_shuffle_ps(_mm_loadu_ps(in+j*2),_mm_loadu_ps(in+j*2+4),0x88);
    unsigned base=m-8-j*2;
    __m128 y=_mm_shuffle_ps(_mm_loadu_ps(in+base+4),_mm_loadu_ps(in+base),0x77);
    if constexpr (Curve) {
        x=_mm_mul_ps(x,_mm_shuffle_ps(_mm_loadu_ps(curve+j*2),_mm_loadu_ps(curve+j*2+4),0x88));
        y=_mm_mul_ps(y,_mm_shuffle_ps(_mm_loadu_ps(curve+base+4),_mm_loadu_ps(curve+base),0x77));
    }
    y=_mm_xor_ps(y,sign);
    // Deinterleave the four (re,im) rotation pairs.
    __m128 plo=_mm_loadu_ps(pre+j*2),phi=_mm_loadu_ps(pre+j*2+4);
    __m128 pr=_mm_shuffle_ps(plo,phi,0x88),pi=_mm_shuffle_ps(plo,phi,0xDD);
    re=_mm_sub_ps(_mm_mul_ps(x,pr),_mm_mul_ps(y,pi));
    im=_mm_add_ps(_mm_mul_ps(x,pi),_mm_mul_ps(y,pr));
}
// MDCT fold: each iteration writes four values to their bit-reversed destinations. m must be
// 2q and q a multiple of four.
template<bool Curve> static void FoldAvx2(float* work,const float* in,const float* curve,const float* pre,
    const unsigned* reverse,unsigned q,unsigned m) {
    for (unsigned j=0;j<q;j+=4) {
        __m128 re,im;Fold4<Curve>(in,curve,pre,j,m,re,im);
        __m128 a=_mm_unpacklo_ps(re,im),b=_mm_unpackhi_ps(re,im);
        _mm_storel_epi64((__m128i*)(work+reverse[j]*2),_mm_castps_si128(a));
        _mm_storel_epi64((__m128i*)(work+reverse[j+1]*2),_mm_srli_si128(_mm_castps_si128(a),8));
        _mm_storel_epi64((__m128i*)(work+reverse[j+2]*2),_mm_castps_si128(b));
        _mm_storel_epi64((__m128i*)(work+reverse[j+3]*2),_mm_srli_si128(_mm_castps_si128(b),8));
    }
    _mm256_zeroupper();
}
// The fold fused with a power-of-two FFT's first two stages (q a multiple of 16). FFT input
// positions 4g..4g+3 hold z at j, j+q/2, j+q/4 and j+3q/4, where 4g = reverse[j] and j < q/4,
// so four consecutive j fill four whole groups. Their 4-point DFTs run in lanes with
// Fft4Groups' arithmetic, and each group's four results are stored together.
template<bool Curve> static void FoldGroupsAvx2(float* work,const float* in,const float* curve,const float* pre,
    const unsigned* reverse,unsigned q,unsigned m) {
    const unsigned quarter=q/4;
    for (unsigned j=0;j<quarter;j+=4) {
        __m128 ar,ai,br,bi,cr,ci,dr,di;
        Fold4<Curve>(in,curve,pre,j,m,ar,ai);Fold4<Curve>(in,curve,pre,j+quarter*2,m,br,bi);
        Fold4<Curve>(in,curve,pre,j+quarter,m,cr,ci);Fold4<Curve>(in,curve,pre,j+quarter*3,m,dr,di);
        __m128 ur=_mm_add_ps(ar,br),ui=_mm_add_ps(ai,bi),vr=_mm_sub_ps(ar,br),vi=_mm_sub_ps(ai,bi);
        __m128 sr=_mm_add_ps(cr,dr),si=_mm_add_ps(ci,di),tr=_mm_sub_ps(cr,dr),ti=_mm_sub_ps(ci,di);
        // x1 = v + i(c-d) and x3 = v - i(c-d); subtracting equals adding the negation exactly.
        __m128 x[4][2]={{_mm_add_ps(ur,sr),_mm_add_ps(ui,si)},{_mm_sub_ps(vr,ti),_mm_add_ps(vi,tr)},
                        {_mm_sub_ps(ur,sr),_mm_sub_ps(ui,si)},{_mm_add_ps(vr,ti),_mm_sub_ps(vi,tr)}};
        __m128 lo[4],hi[4];
        for (unsigned r=0;r<4;++r) { lo[r]=_mm_unpacklo_ps(x[r][0],x[r][1]);hi[r]=_mm_unpackhi_ps(x[r][0],x[r][1]); }
        float* g=work+reverse[j]*2;
        _mm_storeu_ps(g,_mm_movelh_ps(lo[0],lo[1]));_mm_storeu_ps(g+4,_mm_movelh_ps(lo[2],lo[3]));
        g=work+reverse[j+1]*2;
        _mm_storeu_ps(g,_mm_movehl_ps(lo[1],lo[0]));_mm_storeu_ps(g+4,_mm_movehl_ps(lo[3],lo[2]));
        g=work+reverse[j+2]*2;
        _mm_storeu_ps(g,_mm_movelh_ps(hi[0],hi[1]));_mm_storeu_ps(g+4,_mm_movelh_ps(hi[2],hi[3]));
        g=work+reverse[j+3]*2;
        _mm_storeu_ps(g,_mm_movehl_ps(hi[1],hi[0]));_mm_storeu_ps(g+4,_mm_movehl_ps(hi[3],hi[2]));
    }
    _mm256_zeroupper();
}
void AudioMdctFoldAvx2(float* work,const float* in,const float* curve,const float* pre,const unsigned* reverse,
    unsigned q,unsigned m,bool grouped) {
    if (grouped) {
        if (curve) FoldGroupsAvx2<true>(work,in,curve,pre,reverse,q,m);
        else FoldGroupsAvx2<false>(work,in,nullptr,pre,reverse,q,m);
    } else if (curve) FoldAvx2<true>(work,in,curve,pre,reverse,q,m);
    else FoldAvx2<false>(work,in,nullptr,pre,reverse,q,m);
}
// AAC long-block windowing. Each scalar quarter is a mirrored read of the DCT-IV
// output; eight samples at a time are loaded, reversed, weighted and overlap-added.
void AudioAacLongWindowAvx2(const float* transformed,const float* left,const float* right,
    const float* overlap,float* out,float* overlapOut) {
    const __m256 scale=_mm256_set1_ps(1.0f/1024),negative=_mm256_set1_ps(-1.0f/1024);
    const __m256i flip=_mm256_setr_epi32(7,6,5,4,3,2,1,0);
    for (unsigned m=0;m<512;m+=8) {
        __m256 a=_mm256_loadu_ps(transformed+512+m);
        _mm256_storeu_ps(out+m,_mm256_add_ps(_mm256_loadu_ps(overlap+m),
            _mm256_mul_ps(a,_mm256_mul_ps(_mm256_loadu_ps(left+m),scale))));
        __m256 t=_mm256_permutevar8x32_ps(_mm256_loadu_ps(transformed+504-m),flip);
        __m256 r=_mm256_permutevar8x32_ps(_mm256_loadu_ps(right+1016-m),flip);
        _mm256_storeu_ps(overlapOut+m,_mm256_mul_ps(t,_mm256_mul_ps(r,negative)));
    }
    for (unsigned j=0;j<512;j+=8) {
        __m256 t=_mm256_permutevar8x32_ps(_mm256_loadu_ps(transformed+1016-j),flip);
        _mm256_storeu_ps(out+512+j,_mm256_add_ps(_mm256_loadu_ps(overlap+512+j),
            _mm256_mul_ps(t,_mm256_mul_ps(_mm256_loadu_ps(left+512+j),negative))));
        __m256 s=_mm256_loadu_ps(transformed+j);
        __m256 r=_mm256_permutevar8x32_ps(_mm256_loadu_ps(right+504-j),flip);
        _mm256_storeu_ps(overlapOut+512+j,_mm256_mul_ps(s,_mm256_mul_ps(r,negative)));
    }
    _mm256_zeroupper();
}
// MDCT post-rotation: out[2j] = Re(work[j]*post[j]) and out[m-1-2j] = Im(...), four products at
// a time with the scalar arithmetic. When q is a multiple of eight, a front block's real parts
// interleave with the matching back block's reversed imaginary parts (and the other way round),
// so the output is written with whole vectors.
void AudioMdctPostAvx2(const float* work,const float* post,float* out,unsigned q,unsigned m) {
    auto rotate=[&](unsigned j,__m128& re,__m128& im) {
        __m128 wlo=_mm_loadu_ps(work+j*2),whi=_mm_loadu_ps(work+j*2+4);
        __m128 wr=_mm_shuffle_ps(wlo,whi,0x88),wi=_mm_shuffle_ps(wlo,whi,0xDD);
        __m128 plo=_mm_loadu_ps(post+j*2),phi=_mm_loadu_ps(post+j*2+4);
        __m128 pr=_mm_shuffle_ps(plo,phi,0x88),pi=_mm_shuffle_ps(plo,phi,0xDD);
        re=_mm_sub_ps(_mm_mul_ps(wr,pr),_mm_mul_ps(wi,pi));
        im=_mm_add_ps(_mm_mul_ps(wr,pi),_mm_mul_ps(wi,pr));
    };
    if (q%8 || m!=q*2) {
        for (unsigned j=0;j<q;j+=4) {
            __m128 re,im;rotate(j,re,im);
            _mm_store_ss(out+j*2,re);
            _mm_store_ss(out+j*2+2,_mm_shuffle_ps(re,re,0x55));
            _mm_store_ss(out+j*2+4,_mm_shuffle_ps(re,re,0xAA));
            _mm_store_ss(out+j*2+6,_mm_shuffle_ps(re,re,0xFF));
            unsigned base=m-1-j*2;
            _mm_store_ss(out+base,im);
            _mm_store_ss(out+base-2,_mm_shuffle_ps(im,im,0x55));
            _mm_store_ss(out+base-4,_mm_shuffle_ps(im,im,0xAA));
            _mm_store_ss(out+base-6,_mm_shuffle_ps(im,im,0xFF));
        }
    } else for (unsigned j=0;j<q/2;j+=4) {
        unsigned back=q-4-j;
        __m128 frontRe,frontIm,backRe,backIm;rotate(j,frontRe,frontIm);rotate(back,backRe,backIm);
        frontIm=_mm_shuffle_ps(frontIm,frontIm,0x1B);backIm=_mm_shuffle_ps(backIm,backIm,0x1B);
        _mm_storeu_ps(out+j*2,_mm_unpacklo_ps(frontRe,backIm));_mm_storeu_ps(out+j*2+4,_mm_unpackhi_ps(frontRe,backIm));
        _mm_storeu_ps(out+back*2,_mm_unpacklo_ps(backRe,frontIm));_mm_storeu_ps(out+back*2+4,_mm_unpackhi_ps(backRe,frontIm));
    }
    _mm256_zeroupper();
}
// The first two radix-2 stages fused: a 4-point DFT of each group of four complex
// values (bit-reversed in, natural out), two groups per iteration. count is a
// multiple of eight. Their twiddles are only 1 and i, so no gathers or general
// complex multiplies are needed, and both blocks fit in YMMs.
// One pair of groups (four complex values each) through those two stages, in registers.
static __forceinline void Fft4Pair(__m256& lo,__m256& hi) {
    const __m256 sign = _mm256_castsi256_ps(_mm256_setr_epi32((int)0x80000000u,0,(int)0x80000000u,0,
                                                             (int)0x80000000u,0,(int)0x80000000u,0));
    __m256 ab = _mm256_permute2f128_ps(lo,hi,0x20), cd = _mm256_permute2f128_ps(lo,hi,0x31);
    __m256 a = _mm256_permute_ps(ab,0x44), b = _mm256_permute_ps(ab,0xEE);
    __m256 c = _mm256_permute_ps(cd,0x44), d = _mm256_permute_ps(cd,0xEE);
    __m256 u = _mm256_add_ps(a,b), v = _mm256_sub_ps(a,b);
    __m256 sum = _mm256_add_ps(c,d), difference = _mm256_sub_ps(c,d);
    __m256 turn = _mm256_xor_ps(_mm256_permute_ps(difference,0xB1),sign);
    __m256 x0 = _mm256_add_ps(u,sum), x1 = _mm256_add_ps(v,turn);
    __m256 x2 = _mm256_sub_ps(u,sum), x3 = _mm256_sub_ps(v,turn);
    __m256 first = _mm256_shuffle_ps(x0,x1,0x44), second = _mm256_shuffle_ps(x2,x3,0x44);
    lo = _mm256_permute2f128_ps(first,second,0x20);
    hi = _mm256_permute2f128_ps(first,second,0x31);
}
static __forceinline void Fft4Groups(float* work,unsigned count) {
    for (unsigned at = 0; at < count; at += 8) {
        __m256 lo = _mm256_loadu_ps(work+at*2), hi = _mm256_loadu_ps(work+at*2+8);
        Fft4Pair(lo,hi);
        _mm256_storeu_ps(work+at*2,lo);
        _mm256_storeu_ps(work+at*2+8,hi);
    }
}
// Complex products of four interleaved values: x*w.
static __forceinline __m256 ComplexMul(__m256 x,__m256 w) {
    return _mm256_fmaddsub_ps(_mm256_moveldup_ps(w),x,_mm256_mul_ps(_mm256_movehdup_ps(w),_mm256_permute_ps(x,0xB1)));
}
// Radix-2 stages with spans from firstSpan to lastSpan.
static __forceinline void FftSpans(float* work,const float* roots,unsigned size,unsigned firstSpan,unsigned lastSpan) {
    for (unsigned span = firstSpan; span <= lastSpan; span *= 2) {
        unsigned half = span/2;
        for (unsigned at = 0; at < size; at += span) {
            unsigned j = 0;
            for (; j+4 <= half; j += 4) {
                __m256 a = _mm256_loadu_ps(work+(at+j)*2);
                __m256 product = ComplexMul(_mm256_loadu_ps(work+(at+j+half)*2),_mm256_loadu_ps(roots+(half+j)*2));
                _mm256_storeu_ps(work+(at+j)*2,_mm256_add_ps(a,product));
                _mm256_storeu_ps(work+(at+j+half)*2,_mm256_sub_ps(a,product));
            }
            for (; j < half; ++j) {
                float* a = work+(at+j)*2;
                float* b = work+(at+j+half)*2;
                const float* w = roots+(half+j)*2;
                float br = b[0]*w[0]-b[1]*w[1], bi = b[0]*w[1]+b[1]*w[0], ar = a[0], ai = a[1];
                a[0] = ar+br; a[1] = ai+bi; b[0] = ar-br; b[1] = ai-bi;
            }
        }
    }
}
// Two radix-2 stages (spans S and 2S) as one radix-4 pass. With t = W_2S^j, the inputs at
// j + {0, S/2, S, 3S/2} become A, t^2 B, t C and t^3 D, and the outputs are
// A+B+C+D, A-B+i(C-D), A+B-(C+D) and A-B-i(C-D): three twiddle products where two
// radix-2 stages take four, and one pass over memory instead of two. quad holds
// t^2, t and t^3 for each four consecutive j.
static __forceinline void Radix4Pass(float* work,const float* quad,unsigned size,unsigned span) {
    const unsigned half = span/2;
    const __m256 sign = _mm256_castsi256_ps(_mm256_setr_epi32((int)0x80000000u,0,(int)0x80000000u,0,
                                                             (int)0x80000000u,0,(int)0x80000000u,0));
    for (unsigned at = 0; at < size; at += span*2) {
        const float* t = quad;
        for (unsigned j = 0; j < half; j += 4, t += 24) {
            float* p = work+(at+j)*2;
            __m256 a = _mm256_loadu_ps(p), b = ComplexMul(_mm256_loadu_ps(p+half*2),_mm256_loadu_ps(t));
            __m256 c = ComplexMul(_mm256_loadu_ps(p+span*2),_mm256_loadu_ps(t+8));
            __m256 d = ComplexMul(_mm256_loadu_ps(p+(span+half)*2),_mm256_loadu_ps(t+16));
            __m256 s0 = _mm256_add_ps(a,b), s1 = _mm256_sub_ps(a,b), s2 = _mm256_add_ps(c,d);
            __m256 s3 = _mm256_xor_ps(_mm256_permute_ps(_mm256_sub_ps(c,d),0xB1),sign);
            _mm256_storeu_ps(p,_mm256_add_ps(s0,s2));
            _mm256_storeu_ps(p+span*2,_mm256_sub_ps(s0,s2));
            _mm256_storeu_ps(p+half*2,_mm256_add_ps(s1,s3));
            _mm256_storeu_ps(p+(span+half)*2,_mm256_sub_ps(s1,s3));
        }
    }
}
// `size` is the FFT length in complex samples; work holds interleaved re/im pairs.
// After the fused first two stages, an odd remaining stage is radix-2 (stage-major roots,
// so each butterfly's twiddle is a contiguous load) and the rest are radix-4 passes.
void AudioFftAvx2(float* work,const float* roots,const float* quad,unsigned size,bool grouped) {
    if (size < 8) { FftSpans(work,roots,size,2,size); _mm256_zeroupper(); return; }
    if (!grouped) Fft4Groups(work,size);
    unsigned stages = 0, span = 8;
    for (unsigned s = 8; s <= size; s *= 2) ++stages;
    if (stages & 1) { FftSpans(work,roots,size,8,8); span = 16; }
    for (; span < size; span *= 4) { Radix4Pass(work,quad,size,span); quad += span*3; }
    _mm256_zeroupper();
}
// An M-point row FFT (M = 8, 16 or 32) held in registers: the fused first two stages (unless
// Grouped: the fold did them), then the radix-2 stages with FftSpans' arithmetic.
template<unsigned M,bool Grouped> static __forceinline void RowFft(float* row,const float* roots) {
    constexpr unsigned count = M/4;
    __m256 v[count];
    for (unsigned i = 0; i < count; ++i) v[i] = _mm256_loadu_ps(row+i*8);
    if constexpr (!Grouped) for (unsigned i = 0; i < count; i += 2) Fft4Pair(v[i],v[i+1]);
    for (unsigned span = 8; span <= M; span *= 2) {
        const unsigned half = span/8;
        for (unsigned at = 0; at < count; at += half*2) for (unsigned j = 0; j < half; ++j) {
            __m256 a = v[at+j], product = ComplexMul(v[at+j+half],_mm256_loadu_ps(roots+(span/2+j*4)*2));
            v[at+j] = _mm256_add_ps(a,product); v[at+j+half] = _mm256_sub_ps(a,product);
        }
    }
    for (unsigned i = 0; i < count; ++i) _mm256_storeu_ps(row+i*8,v[i]);
}
template<unsigned M,bool Grouped> static void RowFfts(float* work,const float* roots) {
    for (unsigned row = 0; row < 15; ++row) RowFft<M,Grouped>(work+row*M*2,roots);
}
// 15*m-point FFT for m >= 4: an m-point FFT along each of 15 rows, then twiddles and a
// 15-point prime-factor DFT (3 x 5, no inner twiddles) down each column of four lanes.
// grouped: each row's first two stages are done (m a multiple of 16).
void AudioFft15Avx2(float* work,float* out,const float* roots,const float* twiddles,unsigned m,bool grouped) {
    static constexpr unsigned char in15[5][3]={{0,5,10},{3,8,13},{6,11,1},{9,14,4},{12,2,7}};
    static constexpr unsigned char out15[3][5]={{0,6,12,3,9},{10,1,7,13,4},{5,11,2,8,14}};
    // Four-point rows are transformed in pairs; the caller leaves a sixteenth padding row.
    if (m == 4) Fft4Groups(work,64);
    else if (m == 8) RowFfts<8,false>(work,roots);
    else if (m == 16) grouped ? RowFfts<16,true>(work,roots) : RowFfts<16,false>(work,roots);
    else if (m == 32) grouped ? RowFfts<32,true>(work,roots) : RowFfts<32,false>(work,roots);
    else for (unsigned row = 0; row < 15; ++row) {
        if (!grouped) Fft4Groups(work+row*m*2,m);
        FftSpans(work+row*m*2,roots,m,8,m);
    }
    auto spread = [](float s) { return _mm256_setr_ps(-s,s,-s,s,-s,s,-s,s); };
    const __m256 half = _mm256_set1_ps(0.5f), h3 = spread(0.866025403784438647f);
    const __m256 c1 = _mm256_set1_ps(0.309016994374947424f), c2 = _mm256_set1_ps(-0.809016994374947424f);
    const __m256 s1 = spread(0.951056516295153572f), s2 = spread(0.587785252292473129f);
    for (unsigned k = 0; k < m; k += 4) {
        __m256 v[15], y[5][3];
        v[0] = _mm256_loadu_ps(work+k*2);
        for (unsigned row = 1; row < 15; ++row)
            v[row] = ComplexMul(_mm256_loadu_ps(work+(row*m+k)*2),_mm256_loadu_ps(twiddles+(row*m+k)*2));
        for (unsigned b = 0; b < 5; ++b) {
            __m256 x0 = v[in15[b][0]], s = _mm256_add_ps(v[in15[b][1]],v[in15[b][2]]);
            __m256 d = _mm256_sub_ps(v[in15[b][1]],v[in15[b][2]]);
            __m256 mid = _mm256_fnmadd_ps(half,s,x0), turn = _mm256_mul_ps(_mm256_permute_ps(d,0xB1),h3);
            y[b][0] = _mm256_add_ps(x0,s); y[b][1] = _mm256_add_ps(mid,turn); y[b][2] = _mm256_sub_ps(mid,turn);
        }
        for (unsigned c = 0; c < 3; ++c) {
            __m256 x0 = y[0][c], t1 = _mm256_add_ps(y[1][c],y[4][c]), t2 = _mm256_add_ps(y[2][c],y[3][c]);
            __m256 t3 = _mm256_permute_ps(_mm256_sub_ps(y[1][c],y[4][c]),0xB1);
            __m256 t4 = _mm256_permute_ps(_mm256_sub_ps(y[2][c],y[3][c]),0xB1);
            __m256 a1 = _mm256_fmadd_ps(c1,t1,_mm256_fmadd_ps(c2,t2,x0)), a2 = _mm256_fmadd_ps(c2,t1,_mm256_fmadd_ps(c1,t2,x0));
            __m256 b1 = _mm256_fmadd_ps(t3,s1,_mm256_mul_ps(t4,s2)), b2 = _mm256_fmsub_ps(t3,s2,_mm256_mul_ps(t4,s1));
            float* o = out+k*2;
            _mm256_storeu_ps(o+out15[c][0]*m*2,_mm256_add_ps(x0,_mm256_add_ps(t1,t2)));
            _mm256_storeu_ps(o+out15[c][1]*m*2,_mm256_add_ps(a1,b1));
            _mm256_storeu_ps(o+out15[c][4]*m*2,_mm256_sub_ps(a1,b1));
            _mm256_storeu_ps(o+out15[c][2]*m*2,_mm256_add_ps(a2,b2));
            _mm256_storeu_ps(o+out15[c][3]*m*2,_mm256_sub_ps(a2,b2));
        }
    }
    _mm256_zeroupper();
}

// Recursive even/odd DCT split, vectorized at every stage that is at least eight
// wide. factors[] holds the per-stage rotation constants.
template<unsigned N> __forceinline void Dct2Avx(const float* in,float* out,const float* factors) {
    if constexpr (N == 1) *out = *in;
    else {
        constexpr unsigned half = N/2;
        float sum[half],diff[half],even[half],odd[half];
        unsigned j = 0;
        if constexpr (half >= 8) for (; j+8 <= half; j += 8) {
            __m256 a = _mm256_loadu_ps(in+j);
            __m256 b = _mm256_permutevar8x32_ps(_mm256_loadu_ps(in+N-j-8),_mm256_setr_epi32(7,6,5,4,3,2,1,0));
            _mm256_storeu_ps(sum+j,_mm256_add_ps(a,b));
            _mm256_storeu_ps(diff+j,_mm256_mul_ps(_mm256_sub_ps(a,b),_mm256_loadu_ps(factors+half+j)));
        }
        for (; j < half; ++j) { sum[j] = in[j]+in[N-1-j]; diff[j] = (in[j]-in[N-1-j])*factors[half+j]; }
        Dct2Avx<half>(sum,even,factors); Dct2Avx<half>(diff,odd,factors);
        for (j = 0; j < half; ++j) { out[j*2] = even[j]; out[j*2+1] = odd[j]+(j+1 < half ? odd[j+1] : 0); }
    }
}
// Rotate the 32 DCT outputs into the 1024-sample history, duplicate that half,
// then run the 512-tap window as 32 overlapping dot products. `head` is the write
// position; the history has two identical halves so wrapped reads stay contiguous.
// The 64 new values are dct[16..31], 0, -dct[31..1] and -dct[0..15]; they are built in
// registers and stored whole, so the window's first loads can be forwarded from the stores.
static void MpegWindow(const float* dct,float* history,unsigned head,float* out) {
    float* v = history+head;
    const __m256 sign = _mm256_set1_ps(-0.0f);
    const __m256i mirror = _mm256_setr_epi32(0,7,6,5,4,3,2,1);
    __m256 d[4], p[4];
    for (unsigned i = 0; i < 4; ++i) { d[i] = _mm256_loadu_ps(dct+i*8); p[i] = _mm256_permutevar8x32_ps(d[i],mirror); }
    // p[i] holds dct[8i], then dct[8i+7] down to dct[8i+1].
    __m256 block[8] = {d[2],d[3],_mm256_blend_ps(_mm256_xor_ps(p[3],sign),_mm256_setzero_ps(),1),
        _mm256_xor_ps(_mm256_blend_ps(p[2],p[3],1),sign),_mm256_xor_ps(_mm256_blend_ps(p[1],p[2],1),sign),
        _mm256_xor_ps(_mm256_blend_ps(p[0],p[1],1),sign),_mm256_xor_ps(d[0],sign),_mm256_xor_ps(d[1],sign)};
    for (unsigned i = 0; i < 8; ++i) { _mm256_storeu_ps(v+i*8,block[i]); _mm256_storeu_ps(v+1024+i*8,block[i]); }
    for (unsigned j = 0; j < 32; j += 8) {
        __m256 a = _mm256_setzero_ps(),b=a,c=a,d=a;
        for (unsigned i = 0; i < 8; i+=2) {
            a = _mm256_fmadd_ps(_mm256_loadu_ps(history+head+i*128+j),_mm256_loadu_ps(mpeg::synthesisWindow+i*64+j),a);
            b = _mm256_fmadd_ps(_mm256_loadu_ps(history+head+i*128+96+j),_mm256_loadu_ps(mpeg::synthesisWindow+i*64+32+j),b);
            c = _mm256_fmadd_ps(_mm256_loadu_ps(history+head+i*128+128+j),_mm256_loadu_ps(mpeg::synthesisWindow+i*64+64+j),c);
            d = _mm256_fmadd_ps(_mm256_loadu_ps(history+head+i*128+224+j),_mm256_loadu_ps(mpeg::synthesisWindow+i*64+96+j),d);
        }
        _mm256_storeu_ps(out+j,_mm256_add_ps(_mm256_add_ps(a,b),_mm256_add_ps(c,d)));
    }
}
// One 32-subband polyphase step for Layer I/II; the Layer III entry point batches
// 18 of these.
void AudioMpegSynthesisAvx2(const float* in,float* history,unsigned head,const float* factors,float* out) {
    float dct[32]; Dct2Avx<32>(in,dct,factors);
    MpegWindow(dct,history,head,out);
    _mm256_zeroupper();
}
// 8x8 float transpose with unpack/shuffle moves only; no memory round-trip.
static void Transpose8(__m256* x) {
    __m256 a0=_mm256_unpacklo_ps(x[0],x[1]),a1=_mm256_unpackhi_ps(x[0],x[1]);
    __m256 a2=_mm256_unpacklo_ps(x[2],x[3]),a3=_mm256_unpackhi_ps(x[2],x[3]);
    __m256 a4=_mm256_unpacklo_ps(x[4],x[5]),a5=_mm256_unpackhi_ps(x[4],x[5]);
    __m256 a6=_mm256_unpacklo_ps(x[6],x[7]),a7=_mm256_unpackhi_ps(x[6],x[7]);
    __m256 b0=_mm256_shuffle_ps(a0,a2,0x44),b1=_mm256_shuffle_ps(a0,a2,0xEE);
    __m256 b2=_mm256_shuffle_ps(a1,a3,0x44),b3=_mm256_shuffle_ps(a1,a3,0xEE);
    __m256 b4=_mm256_shuffle_ps(a4,a6,0x44),b5=_mm256_shuffle_ps(a4,a6,0xEE);
    __m256 b6=_mm256_shuffle_ps(a5,a7,0x44),b7=_mm256_shuffle_ps(a5,a7,0xEE);
    x[0]=_mm256_permute2f128_ps(b0,b4,0x20);x[4]=_mm256_permute2f128_ps(b0,b4,0x31);
    x[1]=_mm256_permute2f128_ps(b1,b5,0x20);x[5]=_mm256_permute2f128_ps(b1,b5,0x31);
    x[2]=_mm256_permute2f128_ps(b2,b6,0x20);x[6]=_mm256_permute2f128_ps(b2,b6,0x31);
    x[3]=_mm256_permute2f128_ps(b3,b7,0x20);x[7]=_mm256_permute2f128_ps(b3,b7,0x31);
}
// Same recursion as Dct2Avx, but each "sample" is a YMM lane, so eight time slots
// are transformed together. factors are broadcast scalars.
template<unsigned N> __forceinline void Dct2Batch(const __m256* in,__m256* out,const float* factors) {
    if constexpr (N==1) *out=*in;
    else {
        constexpr unsigned half=N/2;
        __m256 sum[half],diff[half],even[half],odd[half];
        for (unsigned j=0;j<half;++j) {
            sum[j]=_mm256_add_ps(in[j],in[N-1-j]);
            diff[j]=_mm256_mul_ps(_mm256_sub_ps(in[j],in[N-1-j]),_mm256_set1_ps(factors[half+j]));
        }
        Dct2Batch<half>(sum,even,factors);Dct2Batch<half>(diff,odd,factors);
        for (unsigned j=0;j<half;++j) {
            out[j*2]=even[j];
            out[j*2+1]=j+1<half?_mm256_add_ps(odd[j],odd[j+1]):odd[j];
        }
    }
}
// Layer III calls this once per granule: 18 independent synthesis filterbanks,
// batched eight slots at a time with the last two done singly. The history head
// still advances in exact stream order so overlap-add stays correct.
void AudioMpegSynthesis18Avx2(const float* in,float* history,unsigned& head,const float* factors,float* out) {
    // Transform eight independent time slots in SIMD lanes. The stateful
    // polyphase window still advances in exact stream order.
    for (unsigned at=0;at<16;at+=8) {
        __m256 input[32],result[32];float dct[8][32];
        for (unsigned band=0;band<32;band+=8) {
            __m256 tile[8];
            for (unsigned t=0;t<8;++t) tile[t]=_mm256_loadu_ps(in+(at+t)*32+band);
            Transpose8(tile);
            for (unsigned j=0;j<8;++j) input[band+j]=tile[j];
        }
        Dct2Batch<32>(input,result,factors);
        for (unsigned band=0;band<32;band+=8) {
            __m256 tile[8];for (unsigned j=0;j<8;++j) tile[j]=result[band+j];
            Transpose8(tile);
            for (unsigned t=0;t<8;++t) _mm256_storeu_ps(dct[t]+band,tile[t]);
        }
        for (unsigned t=0;t<8;++t) {
            head=(head-64)&1023;MpegWindow(dct[t],history,head,out+(at+t)*32);
        }
    }
    for (unsigned t=16;t<18;++t) {
        float dct[32];Dct2Avx<32>(in+t*32,dct,factors);
        head=(head-64)&1023;MpegWindow(dct,history,head,out+t*32);
    }
    _mm256_zeroupper();
}
} // namespace nativeaudio

// cordinal's own codec (cord_codec.h): the residual loop with a fused multiply-add predictor.
// Products of 16-bit coefficients and 32-bit samples need 46 bits and a 32-term sum 51, so
// doubles hold every step exactly and the prediction is the integer path's to the bit.
namespace {
struct FusedPredictor {
    double* samples;          // the coded channel as doubles: kMaxOrder of history, then the frame
    const double* taps;       // 32-byte aligned: the coefficients reversed and zero-padded to 32
                              // taps, tap 31 - j meeting x[i - 1 - j]; the last four go unused
    const int32_t* x;         // the coded channel as decoded
    int64_t newest[4];        // the coefficients of the four newest samples, zero beyond the order
    unsigned shift;
    int64_t Predict(size_t i) const {
        // Lags 5 to 32 come from samples decoded long enough ago to be settled in memory: loading a
        // vector over a value stored a moment before would stall. The four newest lags are still
        // in flight, so they are added last, in integers.
        const double* w = samples + i;
        __m256d a = _mm256_mul_pd(_mm256_loadu_pd(w), _mm256_load_pd(taps));
        __m256d b = _mm256_mul_pd(_mm256_loadu_pd(w + 4), _mm256_load_pd(taps + 4));
        a = _mm256_fmadd_pd(_mm256_loadu_pd(w + 8), _mm256_load_pd(taps + 8), a);
        b = _mm256_fmadd_pd(_mm256_loadu_pd(w + 12), _mm256_load_pd(taps + 12), b);
        a = _mm256_fmadd_pd(_mm256_loadu_pd(w + 16), _mm256_load_pd(taps + 16), a);
        b = _mm256_fmadd_pd(_mm256_loadu_pd(w + 20), _mm256_load_pd(taps + 20), b);
        a = _mm256_fmadd_pd(_mm256_loadu_pd(w + 24), _mm256_load_pd(taps + 24), a);
        a = _mm256_add_pd(a, b);
        __m128d s = _mm_add_pd(_mm256_castpd256_pd128(a), _mm256_extractf128_pd(a, 1));
        s = _mm_add_sd(s, _mm_unpackhi_pd(s, s));
        const int32_t* y = x + i;
        return (_mm_cvtsd_si64(s) + newest[3] * y[-4] + newest[2] * y[-3] + newest[1] * y[-2] + newest[0] * y[-1]) >> shift;
    }
    void Decoded(size_t i, int32_t v) { samples[cord::kMaxOrder + i] = v; }
};
// Short predictors are as quick in plain integers.
struct IntegerPredictor {
    const int32_t* x;
    const int32_t* coefficients;
    unsigned order, shift;
    int64_t Predict(size_t i) const { return cord::Predict(x + i, coefficients, order, shift); }
    void Decoded(size_t, int32_t) const {}
};
// The cascade's dot products and adaptation sixteen taps at a time; integers, so the baseline's.
struct Avx2Kernel {
    int32_t Dot(const int16_t* w, const int16_t* h, unsigned taps) const {
        __m256i sum = _mm256_setzero_si256();
        for (unsigned j = 0; j < taps; j += 16)
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(_mm256_load_si256((const __m256i*)(w + j)),
                                                          _mm256_loadu_si256((const __m256i*)(h + j))));
        __m128i s = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
        return _mm_cvtsi128_si32(_mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1)));
    }
    void Adapt(int16_t* w, const int16_t* a, unsigned taps, int64_t error) const {
        const __m256i sign = _mm256_set1_epi16((int16_t)((error > 0) - (error < 0)));
        const __m256i newest = _mm256_loadu_si256((const __m256i*)cord::kStepFactor);
        for (unsigned j = 0; j < taps; j += 16) {
            __m256i step = _mm256_loadu_si256((const __m256i*)(a + j));
            if (j + 16 == taps) step = _mm256_mullo_epi16(step, newest);
            _mm256_store_si256((__m256i*)(w + j), _mm256_add_epi16(_mm256_load_si256((const __m256i*)(w + j)),
                                                                   _mm256_sign_epi16(step, sign)));
        }
    }
};
// The cascade's filters with each one's newest sixteen lags of history and steps held in registers.
// Each sample stores one 16-bit value into those rings, and a vector load over a value stored a
// moment before cannot be forwarded and waits for the store to retire; from registers the next
// sample's window is ready at once. Older lags are stored at least sixteen samples before they are
// loaded. A 16-tap filter lives in registers entirely. The weights' update by the error is fused
// with the next sample's dot product, so they are loaded once. The same integers as Lms, exactly.
// The filters keep their registers as 32-byte members, so their structs are padded to 32 bytes:
// intended, and not worth a warning.
#pragma warning(push)
#pragma warning(disable : 4324)
struct Lane {
    __m256i rh, ra, rw;  // the window's newest chunk of history and steps (lags 16..1); a 16-tap filter's weights
    int16_t *w, *h, *a;
    unsigned taps, shift, scale, cursor;
    uint32_t average;
    int32_t dot;
    cord::Lms* lms;
};
inline int32_t Sum(__m256i v) {
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    return _mm_cvtsi128_si32(_mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1)));
}
// The register window moved on by one: lanes drop toward lag 16 and `v` enters as lag 1.
// (A broadcast, because MSVC builds a cast from 128 bits through the stack, and the wider reload
// then waits for the narrower store.)
__forceinline __m256i Shift(__m256i window, int16_t v) {
    return _mm256_alignr_epi8(_mm256_permute2x128_si256(window, _mm256_set1_epi16(v), 0x21), window, 2);
}
struct FastCascade {
    Lane stage[cord::kMaxStages], cross;
    __m256i factor;
    cord::CascadeLink link;
    unsigned count;
    bool hasCross;

    explicit FastCascade(const cord::CascadeLink& l) : link(l) {
        factor = _mm256_loadu_si256((const __m256i*)cord::kStepFactor);
        count = l.cascade->Stages();
        for (unsigned s = 0; s < count; ++s) {
            Open(stage[s], l.cascade->Stage(s));
            stage[s].dot = WindowDot(stage[s]);
        }
        cord::Lms* c = l.cascade->Cross();
        hasCross = c != nullptr;
        if (hasCross) Open(cross, *c);
    }
    ~FastCascade() {
        for (unsigned s = 0; s < count; ++s) Close(stage[s]);
        if (hasCross) Close(cross);
    }
    static void Open(Lane& f, cord::Lms& lms) {
        const cord::Lms::Raw raw = lms.Expose();
        f.w = raw.w; f.h = raw.h; f.a = raw.a;
        f.taps = raw.taps; f.shift = raw.shift; f.scale = raw.scale; f.cursor = raw.cursor; f.average = raw.average;
        f.lms = &lms;
        f.rh = _mm256_loadu_si256((const __m256i*)(f.h + f.cursor + f.taps - 16));
        f.ra = _mm256_loadu_si256((const __m256i*)(f.a + f.cursor + f.taps - 16));
        f.rw = _mm256_load_si256((const __m256i*)(f.w + f.taps - 16));
    }
    static void Close(Lane& f) {
        _mm256_store_si256((__m256i*)(f.w + f.taps - 16), f.rw);
        f.lms->Settle(f.cursor, f.average);
    }
    __forceinline static int64_t Prediction(const Lane& f, int32_t dot) {
        return ((int64_t)dot * ((int64_t)1 << f.scale) + ((int64_t)1 << (f.shift - 1))) >> f.shift;
    }
    // The dot product over the current window, the newest chunk from registers.
    __forceinline static int32_t WindowDot(const Lane& f) {
        __m256i sum = _mm256_madd_epi16(f.rw, f.rh);
        for (unsigned j = 0; j + 16 < f.taps; j += 16)
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(_mm256_load_si256((const __m256i*)(f.w + j)),
                                                          _mm256_loadu_si256((const __m256i*)(f.h + f.cursor + j))));
        return Sum(sum);
    }
    // Lms::Push: the input joins the history and its step joins the steps. The stores into the rings
    // wait for Store: they replace the oldest lag, which the adaptation before them still reads.
    __forceinline static void Push(Lane& f, int64_t input) {
        const unsigned at = f.cursor;
        Advance(f, input);
        Store(f, at);
    }
    __forceinline static void Store(const Lane& f, unsigned at) {
        const int16_t h = (int16_t)_mm256_extract_epi16(f.rh, 15), a = (int16_t)_mm256_extract_epi16(f.ra, 15);
        f.h[at] = f.h[at + f.taps] = h;
        f.a[at] = f.a[at + f.taps] = a;
    }
    // Returns the value that joined the history.
    __forceinline static int16_t Advance(Lane& f, int64_t input) {
        const uint64_t m = (uint64_t)(input < 0 ? -input : input);
        const uint32_t magnitude = m > UINT32_MAX ? UINT32_MAX : (uint32_t)m;
        const int16_t step =
            (int16_t)(1 << ((magnitude > (uint64_t)f.average * 3) + (magnitude > (uint64_t)f.average + f.average / 3)));
        f.average = (uint32_t)((int64_t)f.average + ((int64_t)magnitude - (int64_t)f.average) / 16);
        const int64_t scaled = input >> f.scale;
        const int16_t h = (int16_t)(scaled < -32768 ? -32768 : scaled > 32767 ? 32767 : scaled);
        const int16_t a = input > 0 ? step : input < 0 ? (int16_t)-step : (int16_t)0;
        f.rh = Shift(f.rh, h);
        f.ra = Shift(f.ra, a);
        if (++f.cursor == f.taps) f.cursor = 0;
        return h;
    }
    __forceinline __m256i NewestSteps(const Lane& f, __m256i sign) const {
        return _mm256_sign_epi16(_mm256_mullo_epi16(f.ra, factor), sign);
    }
    __forceinline static __m256i Sign(int64_t error) { return _mm256_set1_epi16((int16_t)((error > 0) - (error < 0))); }

    // One stage's Lms::Restore, then the next sample's dot product over the weights it adapts. The
    // input this sample produces enters that product only as the newest lag's single term, so the
    // vector work runs on the error's sign alone, and the input waits for one scalar multiply and add
    // instead of a whole reduction: that is the chain from one sample to the next.
    __forceinline int64_t Restore(Lane& f, int64_t r) {
        const __m256i sign = Sign(r);
        f.rw = _mm256_add_epi16(f.rw, NewestSteps(f, sign));  // the steps before the push, as Adapt reads them
        // Locals, so the stores into the weights don't make the compiler reload the lane.
        int16_t* const w = f.w;
        const unsigned taps = f.taps, before = f.cursor, after = before + 1 == taps ? 0 : before + 1;
        const int16_t *a = f.a + before, *h = f.h + after;
        __m256i sum = _mm256_madd_epi16(f.rw, Shift(f.rh, 0)), odd = _mm256_setzero_si256();
        unsigned j = 0;
        for (; j + 32 < taps; j += 32) {
            const __m256i w0 = _mm256_add_epi16(_mm256_load_si256((const __m256i*)(w + j)),
                                                _mm256_sign_epi16(_mm256_loadu_si256((const __m256i*)(a + j)), sign));
            const __m256i w1 = _mm256_add_epi16(_mm256_load_si256((const __m256i*)(w + j + 16)),
                                                _mm256_sign_epi16(_mm256_loadu_si256((const __m256i*)(a + j + 16)), sign));
            _mm256_store_si256((__m256i*)(w + j), w0);
            _mm256_store_si256((__m256i*)(w + j + 16), w1);
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(w0, _mm256_loadu_si256((const __m256i*)(h + j))));
            odd = _mm256_add_epi32(odd, _mm256_madd_epi16(w1, _mm256_loadu_si256((const __m256i*)(h + j + 16))));
        }
        for (; j + 16 < taps; j += 16) {
            const __m256i w0 = _mm256_add_epi16(_mm256_load_si256((const __m256i*)(w + j)),
                                                _mm256_sign_epi16(_mm256_loadu_si256((const __m256i*)(a + j)), sign));
            _mm256_store_si256((__m256i*)(w + j), w0);
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(w0, _mm256_loadu_si256((const __m256i*)(h + j))));
        }
        const uint32_t rest = (uint32_t)Sum(_mm256_add_epi32(sum, odd));
        const int32_t newestWeight = (int16_t)_mm256_extract_epi16(f.rw, 15);
        const int64_t in = r + Prediction(f, f.dot);
        const int16_t joined = Advance(f, in);
        Store(f, before);
        f.dot = (int32_t)(rest + (uint32_t)(newestWeight * joined));
        return in;
    }
    // The cross filter's adaptation after its push, with no push of its own: Lms::Restore(cross).
    __forceinline void AdaptCross(Lane& f, int64_t r) {
        const __m256i sign = Sign(r);
        f.rw = _mm256_add_epi16(f.rw, NewestSteps(f, sign));
        const int16_t* a = f.a + f.cursor;
        for (unsigned j = 0; j + 16 < f.taps; j += 16)
            _mm256_store_si256((__m256i*)(f.w + j), _mm256_add_epi16(_mm256_load_si256((const __m256i*)(f.w + j)),
                                                                     _mm256_sign_epi16(_mm256_loadu_si256((const __m256i*)(a + j)), sign)));
    }
    __forceinline int64_t operator()(int64_t r, size_t i) {
        int32_t crossDot = 0;
        if (hasCross) {
            Push(cross, link.other[i]);
            crossDot = WindowDot(cross);
        }
        for (unsigned s = count; s-- > 0;) r = Restore(stage[s], r);
        if (hasCross) {
            AdaptCross(cross, r);
            r += Prediction(cross, crossDot);
        }
        if (link.record) link.record[i] = (int32_t)r;
        return r;
    }
};

// One filter whose length the compiler knows, so its registers stay registers: FastCascade's
// lanes sit in an array walked at run time, which the compiler keeps in memory. A 16-tap filter
// needs no rings at all while it runs; they are rewritten from the registers when it closes.
template<unsigned Taps> struct Fixed {
    static constexpr bool kRegisters = Taps == 16;
    __m256i rh, ra, rw;
    int16_t *w, *h, *a;
    unsigned shift, scale, cursor;
    uint32_t average;
    int32_t dot;
    cord::Lms* lms;

    void Open(cord::Lms& l) {
        const cord::Lms::Raw raw = l.Expose();
        w = raw.w; h = raw.h; a = raw.a;
        shift = raw.shift; scale = raw.scale; cursor = raw.cursor; average = raw.average;
        lms = &l;
        rh = _mm256_loadu_si256((const __m256i*)(h + cursor + Taps - 16));
        ra = _mm256_loadu_si256((const __m256i*)(a + cursor + Taps - 16));
        rw = _mm256_load_si256((const __m256i*)(w + Taps - 16));
    }
    void Close() {
        _mm256_store_si256((__m256i*)(w + Taps - 16), rw);
        if (kRegisters) {
            alignas(32) int16_t history[16], steps[16];
            _mm256_store_si256((__m256i*)history, rh);
            _mm256_store_si256((__m256i*)steps, ra);
            for (unsigned j = 0; j < 16; ++j) {
                const unsigned at = (cursor + j) & 15;
                h[at] = h[at + 16] = history[j];
                a[at] = a[at + 16] = steps[j];
            }
        }
        lms->Settle(cursor, average);
    }
    __forceinline int64_t Prediction(int32_t d) const {
        return ((int64_t)d * ((int64_t)1 << scale) + ((int64_t)1 << (shift - 1))) >> shift;
    }
    __forceinline int32_t WindowDot() const {
        __m256i sum = _mm256_madd_epi16(rw, rh);
        for (unsigned j = 0; j + 16 < Taps; j += 16)
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(_mm256_load_si256((const __m256i*)(w + j)),
                                                          _mm256_loadu_si256((const __m256i*)(h + cursor + j))));
        return Sum(sum);
    }
    // Lms::Push in registers; the rings are written by Store, after the reads of the lag it replaces.
    __forceinline int16_t Advance(int64_t input, int16_t& step) {
        const uint64_t m = (uint64_t)(input < 0 ? -input : input);
        const uint32_t magnitude = m > UINT32_MAX ? UINT32_MAX : (uint32_t)m;
        const int16_t size = (int16_t)(1 << ((magnitude > (uint64_t)average * 3) + (magnitude > (uint64_t)average + average / 3)));
        average = (uint32_t)((int64_t)average + ((int64_t)magnitude - (int64_t)average) / 16);
        const int64_t scaled = input >> scale;
        const int16_t value = (int16_t)(scaled < -32768 ? -32768 : scaled > 32767 ? 32767 : scaled);
        step = input > 0 ? size : input < 0 ? (int16_t)-size : (int16_t)0;
        rh = Shift(rh, value);
        ra = Shift(ra, step);
        cursor = (cursor + 1) & (Taps - 1);
        return value;
    }
    __forceinline void Store(unsigned at, int16_t value, int16_t step) {
        if (kRegisters) return;
        h[at] = h[at + Taps] = value;
        a[at] = a[at + Taps] = step;
    }
    __forceinline void Push(int64_t input) {
        const unsigned at = cursor;
        int16_t step;
        const int16_t value = Advance(input, step);
        Store(at, value, step);
    }
    __forceinline __m256i NewestSteps(__m256i factor, __m256i sign) const {
        return _mm256_sign_epi16(_mm256_mullo_epi16(ra, factor), sign);
    }
    // FastCascade::Restore for this length.
    // The decoder's direction: the coded residual r gives back the filter's input.
    __forceinline int64_t Restore(int64_t r, __m256i factor) {
        const int64_t in = r + Prediction(dot);
        Step(r, in, factor);
        return in;
    }
    // The encoder's: the input gives the residual left to code.
    __forceinline int64_t Filter(int64_t in, __m256i factor) {
        const int64_t r = in - Prediction(dot);
        Step(r, in, factor);
        return r;
    }
    // Both directions' shared step: the weights adapt to the residual's sign, the input joins the
    // history, and the next sample's dot product is taken.
    __forceinline void Step(int64_t r, int64_t in, __m256i factor) {
        const __m256i sign = FastCascade::Sign(r);
        rw = _mm256_add_epi16(rw, NewestSteps(factor, sign));
        const unsigned before = cursor, after = (before + 1) & (Taps - 1);
        const int16_t *as = a + before, *hs = h + after;
        __m256i sum = _mm256_madd_epi16(rw, Shift(rh, 0)), odd = _mm256_setzero_si256();
        for (unsigned j = 0; j + 16 < Taps; j += 32) {
            const __m256i w0 = _mm256_add_epi16(_mm256_load_si256((const __m256i*)(w + j)),
                                                _mm256_sign_epi16(_mm256_loadu_si256((const __m256i*)(as + j)), sign));
            _mm256_store_si256((__m256i*)(w + j), w0);
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(w0, _mm256_loadu_si256((const __m256i*)(hs + j))));
            if (j + 32 < Taps) {
                const __m256i w1 = _mm256_add_epi16(_mm256_load_si256((const __m256i*)(w + j + 16)),
                                                    _mm256_sign_epi16(_mm256_loadu_si256((const __m256i*)(as + j + 16)), sign));
                _mm256_store_si256((__m256i*)(w + j + 16), w1);
                odd = _mm256_add_epi32(odd, _mm256_madd_epi16(w1, _mm256_loadu_si256((const __m256i*)(hs + j + 16))));
            }
        }
        const uint32_t rest = (uint32_t)Sum(_mm256_add_epi32(sum, odd));
        const int32_t newestWeight = (int16_t)_mm256_extract_epi16(rw, 15);
        int16_t step;
        const int16_t joined = Advance(in, step);
        Store(before, joined, step);
        dot = (int32_t)(rest + (uint32_t)(newestWeight * joined));
    }
    __forceinline void AdaptCross(int64_t r, __m256i factor) {
        const __m256i sign = FastCascade::Sign(r);
        rw = _mm256_add_epi16(rw, NewestSteps(factor, sign));
        for (unsigned j = 0; j + 16 < Taps; j += 16)
            _mm256_store_si256((__m256i*)(w + j), _mm256_add_epi16(_mm256_load_si256((const __m256i*)(w + j)),
                                                                   _mm256_sign_epi16(_mm256_loadu_si256((const __m256i*)(a + cursor + j)), sign)));
    }
};

// The encoder's cascade (cord_encode.cpp, Encoder::Preset): 256 taps then 16, and for the second
// channel of a pair a 16-tap cross filter. Any other cascade takes FastCascade.
template<bool Cross> struct PresetCascade {
    Fixed<256> wide;
    Fixed<16> narrow, cross;
    __m256i factor;
    cord::CascadeLink link;
    static bool Fits(const cord::CascadeLink& l) {
        cord::Cascade& c = *l.cascade;
        return c.Stages() == 2 && c.Stage(0).Expose().taps == 256 && c.Stage(1).Expose().taps == 16 &&
               (c.Cross() ? Cross && c.Cross()->Expose().taps == 16 : !Cross);
    }
    explicit PresetCascade(const cord::CascadeLink& l) : link(l) {
        factor = _mm256_loadu_si256((const __m256i*)cord::kStepFactor);
        wide.Open(l.cascade->Stage(0));
        narrow.Open(l.cascade->Stage(1));
        wide.dot = wide.WindowDot();
        narrow.dot = narrow.WindowDot();
        if (Cross) cross.Open(*l.cascade->Cross());
    }
    ~PresetCascade() {
        wide.Close();
        narrow.Close();
        if (Cross) cross.Close();
    }
    __forceinline int64_t operator()(int64_t r, size_t i) {
        int32_t crossDot = 0;
        if (Cross) {
            cross.Push(link.other[i]);
            crossDot = cross.WindowDot();
        }
        r = narrow.Restore(r, factor);
        r = wide.Restore(r, factor);
        if (Cross) {
            cross.AdaptCross(r, factor);
            r += cross.Prediction(crossDot);
        }
        if (link.record) link.record[i] = (int32_t)r;
        return r;
    }
    // Cascade::Filter: the cross filter first, then the stages in order.
    __forceinline int64_t Filter(int64_t e, int32_t other) {
        if (Cross) {
            cross.Push(other);
            e -= cross.Prediction(cross.WindowDot());
            cross.AdaptCross(e, factor);
        }
        e = wide.Filter(e, factor);
        return narrow.Filter(e, factor);
    }
};

#pragma warning(pop)

template<bool Cross> void FilterPreset(const cord::CascadeLink& link, const uint32_t* source, size_t n, int32_t* record,
                                       const int32_t* other, uint32_t* out, bool& overflow) {
    PresetCascade<Cross> cascade(link);
    for (size_t i = 0; i < n; ++i) {
        const int64_t input = cord::Unfold(source[i]);
        if (record) record[i] = (int32_t)input;
        const int64_t e = cascade.Filter(input, Cross ? other[i] : 0);
        overflow |= e < INT32_MIN || e > INT32_MAX;
        out[i] = cord::Fold(std::clamp<int64_t>(e, INT32_MIN, INT32_MAX));
    }
}

template<class Predictor> bool Rebuild(const cord::PredictedChannel& c, Predictor& predictor) {
    if (!c.link.cascade) {
        cord::Unfiltered restore;
        return cord::Reconstruct(c.folded, c.x, c.n, c.low, c.high, predictor, restore);
    }
    if (PresetCascade<false>::Fits(c.link)) {
        PresetCascade<false> restore(c.link);
        return cord::Reconstruct(c.folded, c.x, c.n, c.low, c.high, predictor, restore);
    }
    if (PresetCascade<true>::Fits(c.link)) {
        PresetCascade<true> restore(c.link);
        return cord::Reconstruct(c.folded, c.x, c.n, c.low, c.high, predictor, restore);
    }
    FastCascade restore(c.link);
    return cord::Reconstruct(c.folded, c.x, c.n, c.low, c.high, predictor, restore);
}
// The fused predictor for a channel; `taps` is its 32-byte aligned coefficient table.
FusedPredictor Fused(const cord::PredictedChannel& c, double* taps) {
    using cord::kMaxOrder;
    std::fill_n(taps, kMaxOrder, 0.0);
    for (unsigned j = 4; j < c.order; ++j) taps[kMaxOrder - 1 - j] = c.coefficients[j];
    FusedPredictor predictor{c.scratch, taps, c.x, {}, c.shift};
    for (unsigned j = 0; j < 4 && j < c.order; ++j) predictor.newest[j] = c.coefficients[j];
    for (unsigned j = 0; j < kMaxOrder; ++j) c.scratch[j] = c.x[(int)j - (int)kMaxOrder];
    return predictor;
}
} // namespace

bool cord::ReconstructAvx2(const PredictedChannel& c) {
    bool ok;
    if (c.order > 8) {
        alignas(32) double taps[kMaxOrder];
        FusedPredictor predictor = Fused(c, taps);
        ok = Rebuild(c, predictor);
    } else {
        IntegerPredictor predictor{c.x, c.coefficients, c.order, c.shift};
        ok = Rebuild(c, predictor);
    }
    _mm256_zeroupper();
    return ok;
}

namespace {
template<class First, class Second>
bool RebuildPair(const cord::PredictedChannel& a, const cord::PredictedChannel& b, FusedPredictor& pa, FusedPredictor& pb) {
    First ra(a.link);
    Second rb(b.link);
    for (size_t i = 0; i < a.n; ++i) {
        const int64_t va = pa.Predict(i) + ra(cord::Unfold(a.folded[i]), i);
        const int64_t vb = pb.Predict(i) + rb(cord::Unfold(b.folded[i]), i);
        if (va < a.low || va > a.high || vb < b.low || vb > b.high) return false;
        a.x[i] = (int32_t)va;
        b.x[i] = (int32_t)vb;
        pa.Decoded(i, (int32_t)va);
        pb.Decoded(i, (int32_t)vb);
    }
    return true;
}
} // namespace

bool cord::ReconstructPairAvx2(const PredictedChannel& a, const PredictedChannel& b) {
    alignas(32) double tapsA[kMaxOrder], tapsB[kMaxOrder];
    FusedPredictor pa = Fused(a, tapsA), pb = Fused(b, tapsB);
    const bool ok = PresetCascade<false>::Fits(a.link) && PresetCascade<true>::Fits(b.link)
                        ? RebuildPair<PresetCascade<false>, PresetCascade<true>>(a, b, pa, pb)
                        : RebuildPair<FastCascade, FastCascade>(a, b, pa, pb);
    _mm256_zeroupper();
    return ok;
}

bool cord::FilterAvx2(Cascade& cascade, const uint32_t* source, size_t n, int32_t* record, const int32_t* other,
                      uint32_t* out, bool& overflow) {
    const CascadeLink link{&cascade, nullptr, nullptr};
    bool handled = true;
    if (PresetCascade<false>::Fits(link)) FilterPreset<false>(link, source, n, record, other, out, overflow);
    else if (PresetCascade<true>::Fits(link)) FilterPreset<true>(link, source, n, record, other, out, overflow);
    else handled = false;
    _mm256_zeroupper();
    return handled;
}

void cord::CorrelateAvx2(const double* x, size_t n, ptrdiff_t from, bool history, unsigned maxLag, double* out) {
    for (unsigned lag = 0; lag <= maxLag; ++lag) {
        ptrdiff_t i = history ? from : std::max<ptrdiff_t>(from, lag);
        __m256d a = _mm256_setzero_pd(), b = a;
        for (; i + 8 <= (ptrdiff_t)n; i += 8) {
            a = _mm256_add_pd(a, _mm256_mul_pd(_mm256_loadu_pd(x + i), _mm256_loadu_pd(x + i - lag)));
            b = _mm256_add_pd(b, _mm256_mul_pd(_mm256_loadu_pd(x + i + 4), _mm256_loadu_pd(x + i + 4 - lag)));
        }
        const __m256d c = _mm256_add_pd(a, b);
        const __m128d d = _mm_add_pd(_mm256_castpd256_pd128(c), _mm256_extractf128_pd(c, 1));
        double tail = 0;
        for (; i < (ptrdiff_t)n; ++i) tail += x[i] * x[i - lag];
        out[lag] = (_mm_cvtsd_f64(d) + _mm_cvtsd_f64(_mm_unpackhi_pd(d, d))) + tail;
    }
    _mm256_zeroupper();
}

// The encoder's search: every candidate predictor's residuals over a frame. With no sample waiting
// on the last, sixteen samples run at once in four accumulators, and doubles stay exact as above.
// Dividing by 2^shift is exact in doubles too, so its floor is the integer path's shift.
static bool FoldResiduals(const double* y, size_t n, const int32_t* coefficients, unsigned order, unsigned shift,
                          uint32_t* folded) {
    const double step = 1.0 / (double)(uint64_t(1) << shift);
    const __m256d scale = _mm256_set1_pd(step), limit = _mm256_set1_pd(2147483647.0), magnitude = _mm256_set1_pd(-0.0);
    auto fold = [&](__m256d acc, size_t i) {
        const __m256d e = _mm256_sub_pd(_mm256_loadu_pd(y + i), _mm256_floor_pd(_mm256_mul_pd(acc, scale)));
        if (_mm256_movemask_pd(_mm256_cmp_pd(_mm256_andnot_pd(magnitude, e), limit, _CMP_GT_OQ))) return false;
        const __m128i v = _mm256_cvttpd_epi32(e);
        _mm_storeu_si128((__m128i*)(folded + i), _mm_xor_si128(_mm_slli_epi32(v, 1), _mm_srai_epi32(v, 31)));
        return true;
    };
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256d a = _mm256_setzero_pd(), b = a, c = a, d = a;
        for (unsigned j = 0; j < order; ++j) {
            const __m256d k = _mm256_set1_pd(coefficients[j]);
            const double* w = y + i - 1 - j;
            a = _mm256_fmadd_pd(k, _mm256_loadu_pd(w), a);
            b = _mm256_fmadd_pd(k, _mm256_loadu_pd(w + 4), b);
            c = _mm256_fmadd_pd(k, _mm256_loadu_pd(w + 8), c);
            d = _mm256_fmadd_pd(k, _mm256_loadu_pd(w + 12), d);
        }
        if (!fold(a, i) || !fold(b, i + 4) || !fold(c, i + 8) || !fold(d, i + 12)) return false;
    }
    for (; i + 4 <= n; i += 4) {
        __m256d a = _mm256_setzero_pd();
        for (unsigned j = 0; j < order; ++j) a = _mm256_fmadd_pd(_mm256_set1_pd(coefficients[j]), _mm256_loadu_pd(y + i - 1 - j), a);
        if (!fold(a, i)) return false;
    }
    for (; i < n; ++i) {
        double acc = 0;
        for (unsigned j = 0; j < order; ++j) acc += coefficients[j] * y[i - 1 - j];
        const double e = y[i] - std::floor(acc * step);
        if (std::fabs(e) > 2147483647.0) return false;
        const int32_t v = (int32_t)e;
        folded[i] = (uint32_t)v << 1 ^ (uint32_t)(v >> 31);
    }
    return true;
}

bool cord::FoldResidualsAvx2(const double* y, size_t n, const int32_t* coefficients, unsigned order, unsigned shift,
                             uint32_t* folded) {
    const bool ok = FoldResiduals(y, n, coefficients, order, shift, folded);
    _mm256_zeroupper();
    return ok;
}
