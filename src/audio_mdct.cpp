// Shared inverse MDCT: a windowed DCT-IV evaluated through a quarter-size complex
// FFT. This translation unit is compiled with the hot arithmetic optimized; the
// object is linked once and used by every codec that needs an MDCT.
#include "audio_mdct.h"

namespace nativeaudio {
namespace {
unsigned Reverse(unsigned v,unsigned size) {
    unsigned r=0;
    for (unsigned k=size;k>1;k>>=1) { r=r*2+(v&1); v>>=1; }
    return r;
}
// 15-point prime-factor DFT (Good-Thomas, 3 x 5): input n=(5a+3b)%15, output k=(10c+6d)%15,
// so the 3- and 5-point stages need no twiddles between them.
constexpr unsigned char kIn15[5][3]={{0,5,10},{3,8,13},{6,11,1},{9,14,4},{12,2,7}};
constexpr unsigned char kOut15[3][5]={{0,6,12,3,9},{10,1,7,13,4},{5,11,2,8,14}};
}

// Precompute rotation/twiddle tables for a transform of `size`. Power-of-two
// quarter sizes use one radix-2 FFT; 15*2^k sizes (Opus) use 15 radix-2 rows and
// a 15-point column DFT. Tables are shared by all channels and packets of a stream.
void Mdct::Init(unsigned size) {
    n_ = size;
    unsigned q = size / 4;
    radixTwo_ = q && !(q & (q-1));
    rows15_ = !radixTwo_ && q%15==0 && !((q/15) & (q/15-1));
    rowSize_ = rows15_ ? q/15 : q;
    // The vector kernels handle four complex values at a time.
    fast_ = (radixTwo_ || (rows15_ && rowSize_>=4)) && FastAudioKernels();
    pre_.resize(q); post_.resize(q); roots_.resize(rowSize_); reverse_.resize(q);
    // A 4-point row is transformed two rows at a time, so leave room for a sixteenth.
    work_.resize(rows15_ ? q+rowSize_ : q); mixed_.resize(rows15_?q:0); dct_.resize(size / 2);
    constexpr double pi = 3.14159265358979323846;
    auto phase = [](double x) { return Complex{(float)std::cos(x), (float)std::sin(x)}; };
    for (unsigned j = 0; j < q; ++j) {
        pre_[j] = phase(2*pi*j/size);
        post_[j] = phase(2*pi*(j+0.25)/size);
        // A 15-row transform stores x[15*n2+n1] at row n1, bit-reversed within the row.
        reverse_[j] = rows15_ ? (j%15)*rowSize_+Reverse(j/15,rowSize_) : Reverse(j,q);
    }
    // Stage-major twiddles: root (half+j) serves butterfly j of the stage whose span is
    // 2*half, so every stage reads contiguous roots.
    for (unsigned half=1;half<rowSize_;half*=2)
        for (unsigned j=0;j<half;++j) roots_[half+j]=phase(2*pi*(j*(rowSize_/(half*2)))/rowSize_);
    // The AVX2 FFT runs pairs of radix-2 stages as radix-4 passes (after its fused first two
    // stages and one radix-2 stage when the remaining count is odd). For the pass over spans
    // (S,2S) and t = W_2S^j, it reads t^2, t and t^3 for each four consecutive j.
    quad_.clear();
    if (fast_ && radixTwo_ && q >= 8) {
        unsigned stages = 0;
        for (unsigned s = 8; s <= q; s *= 2) ++stages;
        for (unsigned span = stages & 1 ? 16 : 8; span < q; span *= 4)
            for (unsigned j = 0; j < span/2; j += 4)
                for (unsigned power : {2u, 1u, 3u})
                    for (unsigned k = 0; k < 4; ++k) quad_.push_back(phase(2*pi*power*(j+k)/(span*2)));
    }
    if (rows15_) {
        twiddles_.resize(q);
        for (unsigned row=0;row<15;++row) for (unsigned k=0;k<rowSize_;++k) twiddles_[row*rowSize_+k]=phase(2*pi*row*k/q);
    }
}

// Windowed DCT-IV: fold the input with the floor curve, run the quarter-size
// complex FFT, and post-rotate back to real output.
// Unity selects the overload that is used when every floor coefficient is one;
// the multiplication is then compiled out of the load loop.
template<bool Unity> void Mdct::DctCore(const float* in, const float* curve, float* out) {
    unsigned m = n_ / 2, q = n_ / 4;
    // The vector kernels need at least four coefficients; smaller transforms keep the scalar path.
    const bool avx = fast_ && q >= 4;
    // Apply the floor while loading the transform, avoiding an extra full
    // spectral read/write pass for each channel and packet. When q is a multiple of 16 the
    // fold also does the first two stages of the FFT (or of each 15-row FFT row).
    const bool grouped = avx && q % 16 == 0;
    if (avx) AudioMdctFoldAvx2((float*)work_.data(), in, Unity ? nullptr : curve, (const float*)pre_.data(),
                               reverse_.data(), q, m, grouped);
    else if constexpr (Unity) for (unsigned j = 0; j < q; ++j)
        work_[reverse_[j]] = Mul({in[j*2], -in[m-1-j*2]}, pre_[j]);
    else for (unsigned j = 0; j < q; ++j)
        work_[reverse_[j]] = Mul({in[j*2]*curve[j*2], -in[m-1-j*2]*curve[m-1-j*2]}, pre_[j]);
    const Complex* result = work_.data();
    if (rows15_) {
        if (fast_) AudioFft15Avx2((float*)work_.data(),(float*)mixed_.data(),(const float*)roots_.data(),
                                  (const float*)twiddles_.data(),rowSize_,grouped);
        else Fft15();
        result = mixed_.data();
    } else if (fast_) AudioFftAvx2((float*)work_.data(),(const float*)roots_.data(),(const float*)quad_.data(),q,grouped);
    else for (unsigned span = 2; span <= q; span *= 2) {
        unsigned half = span / 2;
        for (unsigned at = 0; at < q; at += span) {
            Complex first = work_[at], second = work_[at+half];
            work_[at] = {first.r+second.r,first.i+second.i};
            work_[at+half] = {first.r-second.r,first.i-second.i};
            for (unsigned j = 1; j < half; ++j) {
                Complex a = work_[at+j], b = Mul(work_[at+j+half], roots_[half+j]);
                work_[at+j] = {a.r+b.r, a.i+b.i};
                work_[at+j+half] = {a.r-b.r, a.i-b.i};
            }
        }
    }
    if (avx) AudioMdctPostAvx2((const float*)result, (const float*)post_.data(), out, q, m);
    else for (unsigned j = 0; j < q; ++j) {
        Complex v = Mul(result[j], post_[j]);
        out[j*2] = v.r; out[m-1-j*2] = v.i;
    }
}
void Mdct::Dct(const float* in, const float* curve, float* out) { DctCore<false>(in,curve,out); }
void Mdct::Dct(const float* in, float* out) { DctCore<true>(in,nullptr,out); }

// Full inverse MDCT: Dct followed by the sign-mirrored reordering that turns the
// DCT-IV output into the time-domain block.
static inline void Reorder(const float* dct,float* out,unsigned q,unsigned n) {
    if (q % 4) {
        for (unsigned j = 0; j < q; ++j) out[j] = dct[j+q];
        for (unsigned j = q; j < q*3; ++j) out[j] = -dct[q*3-1-j];
        for (unsigned j = q*3; j < n; ++j) out[j] = -dct[j-q*3];
        return;
    }
    const __m128 sign = _mm_set1_ps(-0.0f);
    for (unsigned j = 0; j < q; j += 4) _mm_storeu_ps(out+j, _mm_loadu_ps(dct+j+q));
    for (unsigned j = q; j < q*3; j += 4) {
        __m128 v = _mm_loadu_ps(dct+q*3-4-j);
        _mm_storeu_ps(out+j, _mm_xor_ps(_mm_shuffle_ps(v,v,0x1B),sign));
    }
    for (unsigned j = q*3; j < n; j += 4) _mm_storeu_ps(out+j, _mm_xor_ps(_mm_loadu_ps(dct+j-q*3),sign));
}
void Mdct::Run(const float* in, const float* curve, float* out) {
    unsigned q = n_/4;
    Dct(in,curve,dct_.data());
    Reorder(dct_.data(),out,q,n_);
}
void Mdct::Run(const float* in, float* out) {
    unsigned q = n_/4;
    Dct(in,dct_.data());
    Reorder(dct_.data(),out,q,n_);
}

// Portable 15-row transform: X[k2+m*k1] = sum over rows n1 of W15^(n1*k1) * W_q^(n1*k2) * Y_n1[k2],
// where Y_n1 is the m-point FFT of row n1 = x[15*n2+n1].
void Mdct::Fft15() {
    const unsigned m=rowSize_;
    for (unsigned row=0;row<15;++row) {
        Complex* w=work_.data()+row*m;
        for (unsigned span=2;span<=m;span*=2) {
            unsigned half=span/2;
            for (unsigned at=0;at<m;at+=span) for (unsigned j=0;j<half;++j) {
                Complex a=w[at+j],b=Mul(w[at+j+half],roots_[half+j]);
                w[at+j]={a.r+b.r,a.i+b.i}; w[at+j+half]={a.r-b.r,a.i-b.i};
            }
        }
    }
    // The transform uses positive exponents: W = exp(+2*pi*i/N).
    constexpr float h3=0.866025403784438647f,c1=0.309016994374947424f,c2=-0.809016994374947424f,
        s1=0.951056516295153572f,s2=0.587785252292473129f;
    for (unsigned k=0;k<m;++k) {
        Complex v[15];
        v[0]=work_[k];
        for (unsigned row=1;row<15;++row) v[row]=Mul(work_[row*m+k],twiddles_[row*m+k]);
        Complex y[5][3];
        for (unsigned b=0;b<5;++b) {
            Complex x0=v[kIn15[b][0]],x1=v[kIn15[b][1]],x2=v[kIn15[b][2]];
            Complex s{x1.r+x2.r,x1.i+x2.i},d{x1.r-x2.r,x1.i-x2.i},mid{x0.r-0.5f*s.r,x0.i-0.5f*s.i};
            y[b][0]={x0.r+s.r,x0.i+s.i};
            y[b][1]={mid.r-h3*d.i,mid.i+h3*d.r};
            y[b][2]={mid.r+h3*d.i,mid.i-h3*d.r};
        }
        for (unsigned c=0;c<3;++c) {
            Complex x0=y[0][c],t1{y[1][c].r+y[4][c].r,y[1][c].i+y[4][c].i},t2{y[2][c].r+y[3][c].r,y[2][c].i+y[3][c].i},
                t3{y[1][c].r-y[4][c].r,y[1][c].i-y[4][c].i},t4{y[2][c].r-y[3][c].r,y[2][c].i-y[3][c].i};
            Complex a1{x0.r+c1*t1.r+c2*t2.r,x0.i+c1*t1.i+c2*t2.i},a2{x0.r+c2*t1.r+c1*t2.r,x0.i+c2*t1.i+c1*t2.i};
            // i*b1 and i*b2, with b1 = s1*t3+s2*t4 and b2 = s2*t3-s1*t4.
            Complex ib1{-(s1*t3.i+s2*t4.i),s1*t3.r+s2*t4.r},ib2{-(s2*t3.i-s1*t4.i),s2*t3.r-s1*t4.r};
            Complex* o=mixed_.data()+k;
            o[kOut15[c][0]*m]={x0.r+t1.r+t2.r,x0.i+t1.i+t2.i};
            o[kOut15[c][1]*m]={a1.r+ib1.r,a1.i+ib1.i};
            o[kOut15[c][4]*m]={a1.r-ib1.r,a1.i-ib1.i};
            o[kOut15[c][2]*m]={a2.r+ib2.r,a2.i+ib2.i};
            o[kOut15[c][3]*m]={a2.r-ib2.r,a2.i-ib2.i};
        }
    }
}
} // namespace nativeaudio
