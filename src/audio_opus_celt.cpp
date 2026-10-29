// Cordinal CELT reconstruction, RFC 6716 sections 4.3 and 4.1.
// Implements the CELT decoder behind OpusCelt: range-coded energy, band
// allocation and shape (PVQ/folding), the inverse MDCT, and the post-filter and
// deemphasis synthesis, all in the fixed-point scales the spec defines.
#include "audio_opus_celt.h"
#include "audio_opus_tables.h"
#include "audio_mdct.h"
#include <array>
#include <cmath>
#include <numeric>
#include <emmintrin.h>

namespace nativeaudio {
namespace {
namespace d=opusdata;
constexpr double pi=3.14159265358979323846;
// One value per CELT band (21 bands); the band edges live in d::bands.
using Bands=std::array<int,21>;
int Width(int band) { return d::bands[band+1]-d::bands[band]; }
int Ilog(unsigned n) { return n?32-std::countl_zero(n):0; }
// Q15 multiply with rounding: (a*b + 0.5) >> 15.
int RoundedProduct(int a,int b) { return (int)((int64_t(a)*b+16384)>>15); }
// Fixed-point cosine (Q15 out) using CELT's angle convention (16384 = pi/2).
int Cosine(int angle) {
    int x=(4096+angle*angle)>>13;
    int polynomial=8277+RoundedProduct(-626,x);
    polynomial=-7651+RoundedProduct(x,polynomial);
    return 32768-x+RoundedProduct(x,polynomial);
}
// Fixed-point log(sine/cosine), used to split bits between mid and side.
int LogTan(int sine,int cosine) {
    int s=Ilog(sine),c=Ilog(cosine); sine<<=15-s; cosine<<=15-c;
    return (s-c)*2048+RoundedProduct(sine,7932+RoundedProduct(sine,-2597))-
        RoundedProduct(cosine,7932+RoundedProduct(cosine,-2597));
}
// Decodes a signed Laplace-distributed value (the CELT energy residual): `zero`
// is the probability mass at zero and `decay` the geometric falloff of the tail.
int Laplace(OpusRange& range,unsigned zero,unsigned decay) {
    unsigned target=range.Frequency(32768);
    if (target<zero) { range.Update(0,zero,32768); return 0; }
    unsigned low=zero,mass=((32736-zero)*(16384-decay)>>15)+1,magnitude=1;
    while (mass>1 && target>=low+mass*2) {
        low+=mass*2; mass=(((mass*2-2)*decay)>>15)+1; ++magnitude;
    }
    if (mass==1) { unsigned jump=(target-low)/2; magnitude+=jump; low+=jump*2; }
    bool positive=target>=low+mass;
    if (positive) low+=mass;
    range.Update(low,std::min(32768u,low+mass),32768);
    return positive?(int)magnitude:-(int)magnitude;
}
int LogCost(unsigned count) {
    // Conservative base-2 log in eighth bits, as required for uniform symbol budgets.
    static constexpr int table[]={0,8,13,16,19,21,23,24,26,27,28,29,30,31,32,32,33,34,34,35,36,36,37,37};
    return count<std::size(table)?table[count]:(int)std::ceil(std::log2(count)*8);
}
// Result of the RFC 6716 band allocation: which bands are coded, the per-band
// shape/fine bit split, the stereo intensity threshold, the independent-stereo
// ("dual") flag, and the leftover `balance` carried into the band loop.
struct Allocation {
    Bands shape{},fine{},priority{};
    int coded=0,intensity=0,balance=0;
    bool dual=false;
};
// Chooses the band allocation by searching interpolation levels against the bit
// budget; `boost`/`caps` are the packet's boost and the static per-band caps.
Allocation Allocate(OpusRange& range,int first,int last,int channels,int lm,int trim,int budget,const Bands& boost,const Bands& caps) {
    Allocation result;
    int floor=channels*8,skipReserve=budget>=8?8:0;
    budget=std::max(0,budget)-skipReserve;
    int intensityReserve=channels==2?LogCost(last-first):0,dualReserve=0;
    if (intensityReserve>budget) intensityReserve=0;
    else { budget-=intensityReserve; if (channels==2 && budget>=8) { dualReserve=8; budget-=8; } }
    Bands threshold{},tilt{};
    int stop=first;
    for (int i=first;i<last;++i) {
        threshold[i]=std::max(floor,(3*Width(i)*(1<<lm)*8)>>4);
        tilt[i]=(channels*Width(i)*(trim-5-lm)*(last-i-1)*(1<<(lm+3)))>>6;
        if ((Width(i)<<lm)==1) tilt[i]-=floor;
        if (boost[i]) stop=i;
    }
    auto Constrain=[&](Bands values) {
        int active=first-1;
        for (int i=first;i<last;++i) if (values[i]>=threshold[i]) active=i;
        for (int i=first;i<last;++i) {
            if (i>active) values[i]=values[i]>=floor?floor:0;
            values[i]=std::min(values[i],caps[i]);
        }
        return values;
    };
    auto Sum=[&](const Bands& values) { return std::accumulate(values.begin()+first,values.begin()+last,0); };
    // Sum(Constrain(v)) without building the array: bands above the highest one reaching its
    // threshold keep at most the floor, and every band is capped.
    auto ConstrainedSum=[&](auto value) {
        int sum=0,i=last-1;
        for (;i>=first;--i) { int v=value(i); if (v>=threshold[i]) break; sum+=std::min(v>=floor?floor:0,caps[i]); }
        for (;i>=first;--i) sum+=std::min(value(i),caps[i]);
        return sum;
    };
    Bands scale{};
    for (int i=first;i<last;++i) scale[i]=(channels*Width(i))<<lm;
    auto Weight=[&](int quality,bool offset,int i) {
        int base=quality==11?caps[i]:(scale[i]*d::allocation[quality][i])>>2;
        if (base>0) base=std::max(0,base+tilt[i]);
        return base+(offset?boost[i]:0);
    };
    auto Weights=[&](int quality,bool offset) {
        Bands v{};
        for (int i=first;i<last;++i) v[i]=Weight(quality,offset,i);
        return v;
    };
    int lo=0,hi=11;
    while (lo+1<hi) {
        int mid=(lo+hi)/2;
        if (ConstrainedSum([&](int i) { return Weight(mid,true,i); })<=budget) lo=mid; else hi=mid;
    }
    Bands low=Weights(lo,lo!=0),high=Weights(hi,true),delta{};
    for (int i=first;i<last;++i) delta[i]=std::max(0,high[i]-low[i]);
    auto Interpolate=[&](int fraction) { Bands v=low; for (int i=first;i<last;++i) v[i]+=(fraction*delta[i])>>6; return Constrain(v); };
    lo=0;hi=64;
    for (int i=0;i<6;++i) {
        int mid=(lo+hi)/2;
        if (ConstrainedSum([&](int i) { return low[i]+((mid*delta[i])>>6); })<=budget) lo=mid; else hi=mid;
    }
    Bands bits=Interpolate(lo); int used=Sum(bits),coded=last;
    for (;;) {
        int band=coded-1;
        if (band<=stop) { budget+=skipReserve; break; }
        int left=budget-used,width=d::bands[coded]-d::bands[first];
        int per=left/width,remainder=left-width*per;
        int available=bits[band]+per*Width(band)+std::max(0,remainder-(d::bands[band]-d::bands[first]));
        if (available>=std::max(threshold[band],floor+8)) {
            if (range.Bit(1)) break;
            used+=8; available-=8;
        }
        used-=bits[band]+intensityReserve;
        if (intensityReserve) intensityReserve=LogCost(band-first);
        used+=intensityReserve;
        bits[band]=available>=floor?floor:0; used+=bits[band]; --coded;
    }
    result.coded=coded;
    result.intensity=intensityReserve?first+(int)range.Uniform(coded+1-first):0;
    if (result.intensity<=first) { budget+=dualReserve; dualReserve=0; }
    result.dual=dualReserve && range.Bit(1);
    int left=budget-used,width=d::bands[coded]-d::bands[first],per=left/width,remainder=left-width*per;
    for (int i=first;i<coded;++i) { int extra=std::min(remainder,Width(i)); bits[i]+=per*Width(i)+extra; remainder-=extra; }
    int balance=0;
    for (int i=first;i<coded;++i) {
        int n=Width(i)<<lm,total=bits[i]+balance,excess;
        if (n>1) {
            excess=std::max(0,total-caps[i]); bits[i]=total-excess;
            int degrees=channels*n+(channels==2 && n>2 && !result.dual && i<result.intensity);
            int log=degrees*(d::logN400[i]+8*lm),offset=(log>>1)-degrees*21;
            if (n==2) offset+=degrees*2;
            if (bits[i]+offset<degrees*16) offset+=log>>2;
            else if (bits[i]+offset<degrees*24) offset+=log>>3;
            int fine=std::max(0,(bits[i]+offset+degrees*4)/(degrees*8));
            fine=std::min({fine,8,bits[i]/(channels*8)});
            result.fine[i]=fine; result.priority[i]=fine*degrees*8>=bits[i]+offset;
            bits[i]-=fine*channels*8;
        } else {
            excess=std::max(0,total-floor); bits[i]=total-excess; result.priority[i]=1;
        }
        if (excess>0) {
            int extra=std::min(excess/(channels*8),8-result.fine[i]);
            result.fine[i]+=extra; int consumed=extra*channels*8;
            result.priority[i]=consumed>=excess-balance; excess-=consumed;
        }
        balance=excess;
    }
    result.balance=balance;
    for (int i=coded;i<last;++i) { result.fine[i]=bits[i]/(channels*8); result.priority[i]=result.fine[i]<1; bits[i]=0; }
    result.shape=bits;
    return result;
}
// Overlap-adds one inverse MDCT into `wave`, reading the DCT-IV output `dct` (block values)
// with the MDCT's mirrored order instead of materializing the 2*block time signal: CELT keeps
// only its middle block+120 samples, under the 120-sample window at each end.
void OverlapAdd(const float* dct,const float* window,float* w,unsigned block) {
    auto reversed=[](const float* p) { __m128 v=_mm_loadu_ps(p);return _mm_shuffle_ps(v,v,0x1B); };
    for (unsigned i=0;i<60;i+=4)
        _mm_storeu_ps(w+i,_mm_add_ps(_mm_loadu_ps(w+i),_mm_mul_ps(_mm_loadu_ps(dct+block-60+i),_mm_loadu_ps(window+i))));
    for (unsigned i=60;i<120;i+=4)
        _mm_storeu_ps(w+i,_mm_sub_ps(_mm_loadu_ps(w+i),_mm_mul_ps(reversed(dct+block+56-i),_mm_loadu_ps(window+i))));
    for (unsigned i=120;i<block;i+=4)
        _mm_storeu_ps(w+i,_mm_sub_ps(_mm_loadu_ps(w+i),reversed(dct+block+56-i)));
    for (unsigned i=block;i<block+60;i+=4)
        _mm_storeu_ps(w+i,_mm_sub_ps(_mm_loadu_ps(w+i),_mm_mul_ps(reversed(dct+block+56-i),reversed(window+block+116-i))));
    for (unsigned i=block+60;i<block+120;i+=4)
        _mm_storeu_ps(w+i,_mm_sub_ps(_mm_loadu_ps(w+i),_mm_mul_ps(_mm_loadu_ps(dct+i-60-block),reversed(window+block+116-i))));
}
// Deemphasis y[i] = x[i] + c*y[i-1] for both channels, interleaved and scaled to unit range;
// state holds each channel's last y. Four samples are combined with a two-step prefix (c and
// c^2), so the recursion carries one multiply-add per four samples instead of one per sample.
void Deemphasis(const float* left,const float* right,unsigned n,float* state,float* out) {
    constexpr float c=0.850006103515625f,c2=c*c;
    const __m128 k1=_mm_set1_ps(c),k2=_mm_set1_ps(c2),powers=_mm_setr_ps(c,c2,c2*c,c2*c2),scale=_mm_set1_ps(1.0f/32768);
    auto shift=[](__m128 v,int lanes) {
        return lanes==1?_mm_castsi128_ps(_mm_slli_si128(_mm_castps_si128(v),4)):_mm_castsi128_ps(_mm_slli_si128(_mm_castps_si128(v),8));
    };
    auto prefix=[&](__m128 x) { x=_mm_add_ps(x,_mm_mul_ps(k1,shift(x,1)));return _mm_add_ps(x,_mm_mul_ps(k2,shift(x,2))); };
    __m128 a=_mm_set1_ps(state[0]),b=_mm_set1_ps(state[1]);
    for (unsigned i=0;i<n;i+=4) {
        __m128 x=_mm_add_ps(prefix(_mm_loadu_ps(left+i)),_mm_mul_ps(powers,a));
        __m128 y=_mm_add_ps(prefix(_mm_loadu_ps(right+i)),_mm_mul_ps(powers,b));
        a=_mm_shuffle_ps(x,x,0xFF);b=_mm_shuffle_ps(y,y,0xFF);
        x=_mm_mul_ps(x,scale);y=_mm_mul_ps(y,scale);
        _mm_storeu_ps(out+i*2,_mm_unpacklo_ps(x,y));_mm_storeu_ps(out+i*2+4,_mm_unpackhi_ps(x,y));
    }
    state[0]=_mm_cvtss_f32(a);state[1]=_mm_cvtss_f32(b);
}
// Sums x*y and y*y in double precision, two lanes at a time.
void CrossPower(const float* x,const float* y,int n,double& cross,double& power) {
    __m128d c0=_mm_setzero_pd(),c1=c0,p0=c0,p1=c0;
    int i=0;
    for (;i+4<=n;i+=4) {
        __m128 a=_mm_loadu_ps(x+i),b=_mm_loadu_ps(y+i);
        __m128d al=_mm_cvtps_pd(a),ah=_mm_cvtps_pd(_mm_movehl_ps(a,a)),bl=_mm_cvtps_pd(b),bh=_mm_cvtps_pd(_mm_movehl_ps(b,b));
        c0=_mm_add_pd(c0,_mm_mul_pd(al,bl));c1=_mm_add_pd(c1,_mm_mul_pd(ah,bh));
        p0=_mm_add_pd(p0,_mm_mul_pd(bl,bl));p1=_mm_add_pd(p1,_mm_mul_pd(bh,bh));
    }
    __m128d c=_mm_add_pd(c0,c1),p=_mm_add_pd(p0,p1);
    cross=_mm_cvtsd_f64(_mm_add_sd(c,_mm_unpackhi_pd(c,c)));power=_mm_cvtsd_f64(_mm_add_sd(p,_mm_unpackhi_pd(p,p)));
    for (;i<n;++i) { cross+=double(x[i])*y[i];power+=double(y[i])*y[i]; }
}
// Rescales `v` to unit energy times `gain`, so band shape is gain-independent.
void Normalize(float* v,int n,float gain) {
    double energy=0; for (int i=0;i<n;++i) energy+=double(v[i])*v[i];
    float scale=gain/(float)std::sqrt(energy+1e-20);
    for (int i=0;i<n;++i) v[i]*=scale;
}
// In-place Haar butterfly over `count` points spaced by `stride` (CELT's TF transform).
void Haar(float* v,int count,int stride) {
    for (int i=0;i<stride;++i) for (int j=0;j<count/2;++j) {
        int a=(2*j)*stride+i,b=a+stride;
        float x=v[a]*0.7071067811865475f,y=v[b]*0.7071067811865475f;
        v[a]=x+y;v[b]=x-y;
    }
}
// Interleaves/deinterleaves transformed blocks, undoing the Hadamard ordering
// used when folding energy into long blocks.
void Reorder(float* v,int count,int stride,bool hadamard,bool inverse) {
    static constexpr int order[]={1,0,3,0,2,1,7,0,4,3,6,1,5,2,15,0,8,7,12,3,11,4,14,1,9,6,13,2,10,5};
    float copy[176];
    std::copy_n(v,count*stride,copy);
    for (int i=0;i<stride;++i) for (int j=0;j<count;++j) {
        int linear=(hadamard?order[stride-2+i]:i)*count+j,interleaved=j*stride+i;
        v[inverse?interleaved:linear]=copy[inverse?linear:interleaved];
    }
}
// Packs a per-block nonzero mask (chained 2-bit runs) to/from 1-bit-per-block form.
unsigned Compress(unsigned mask) { unsigned v=0; for (unsigned i=0;i<8;++i) if (mask&(3u<<(i*2))) v|=1u<<i; return v; }
unsigned Expand(unsigned mask) { unsigned v=0; for (unsigned i=0;i<8;++i) if (mask&(1u<<i)) v|=3u<<(2*i); return v; }
// CELT post-filter (comb) parameters: comb period, tap-set index and gain.
struct Filter { int period=15,taps=0; float gain=0; };
}

// Persistent CELT state: per-band log energy plus one/two-frame history for
// anti-collapse, and the time-domain overlap, post-filter and deemphasis memory.
struct OpusCelt::State {
    float energy[2][21]{},old[2][21]{},older[2][21]{};
    float overlap[2][120]{},post[2][2048]{},deemphasis[2]{};
    uint32_t random=0,position=0;
    Filter previous,latest;
};
struct OpusCelt::Impl : State {
    Mdct mdct[4];
    std::array<float,120> window{};
    float wave_[2][960+120];
    Impl() {
        for (unsigned lm=0;lm<4;++lm) mdct[lm].Init(240u<<lm);
        // 120-sample power-complementary overlap window, shared by all frame sizes.
        for (unsigned i=0;i<120;++i) { double s=std::sin(pi*(i+0.5)/240);window[i]=(float)std::sin(pi*0.5*s*s); }
        Reset();
    }
    // Resets to the stream-start sentinel: -28 (dB) log energy and zeroed memory.
    void Reset() {
        for (auto& v:energy) std::fill_n(v,21,0.0f);
        for (auto& v:old) std::fill_n(v,21,-28.0f);
        for (auto& v:older) std::fill_n(v,21,-28.0f);
        for (auto& v:overlap) std::fill_n(v,120,0.0f);
        for (auto& v:post) std::fill_n(v,2048,0.0f);
        deemphasis[0]=deemphasis[1]=0; random=position=0; previous=latest=Filter{};
    }
    // Per-packet decode cursor handed to Band(): band index, TF/spread decisions
    // and the remaining bit budget.
    struct Frame {
        Impl& owner; OpusRange& range;
        int channels,lm,first,last,spread=2,remaining=0,intensity=0,band=0;
        Bands tf{};
        unsigned Band(float*,float*,int,int,int,int,int,const float*,float*,float,unsigned,int);
        unsigned Pulses(float*,int,int,int,float);
    };
    bool Decode(OpusRange&,unsigned,unsigned,unsigned,unsigned,std::vector<float>&);
    void Synthesis(float spectrum[2][960],unsigned channels,unsigned lm,bool transient,Filter,std::vector<float>&);
};

namespace {
// Codewords are counted with U(n,k), symmetric in n and k: U(n,k) = U(n-1,k)+U(n,k-1)+U(n-1,k-1)
// for n,k > 0 and U(0,k) = 1 only for k = 0. A codebook of k pulses in n dimensions holds
// V(n,k) = U(n,k)+U(n,k+1) vectors, U(n,k) of them with a negative first value. The table is
// needed only once the first CELT band is decoded; building it at first use keeps its 126 KB
// out of the executable's read-only data (it lives in .bss, filled by 31,152 additions). Row n
// holds every count the decoder reads while n dimensions remain, so its searches stay in one
// row. Counts above 32 bits saturate to UINT32_MAX; no valid codebook reaches them.
using PulseTable=std::array<std::array<uint32_t,178>,177>;
PulseTable pulseCounts;
const PulseTable& PulseCounts() {
    static const bool filled=[] {
        pulseCounts[0][0]=1;
        for (int n=1;n<=176;++n) for (int k=1;k<=177;++k)
            pulseCounts[n][k]=(uint32_t)std::min<uint64_t>(UINT32_MAX,uint64_t(pulseCounts[n-1][k])+pulseCounts[n][k-1]+pulseCounts[n-1][k-1]);
        return true;
    }();
    (void)filled;
    return pulseCounts;
}
// Decodes codeword `index` of k pulses in n dimensions into y (the enumeration of RFC 6716
// 4.3.4.2, in the U form libopus uses) and returns the sum of squares. Codewords from
// U(n,k+1) on have a negative first value. The pulses left after this dimension are then the
// largest count whose U(n,count) the index still reaches; most dimensions hold fewer than three
// pulses, so the first three steps down are tested at once, without branches. The last two
// dimensions have closed forms.
uint32_t PulseVector(const PulseTable& table,int n,int k,uint32_t index,int* y) {
    uint32_t energy=0;
    auto put=[&](int value) { *y++=value;energy+=uint32_t(value*value); };
    if (n==1) { int sign=-int(index!=0);put((k+sign)^sign);return energy; }
    for (;n>2;--n) {
        // Row n increases with the count from U(n,0) = 0, so every search ends.
        const uint32_t* row=table[n].data();
        uint32_t q=row[k+1];int sign=-int(index>=q);index-=q&sign;
        int left=k-int(row[k]>index)-int(row[std::max(k-1,0)]>index)-int(row[std::max(k-2,0)]>index);
        if (left==k-3 && row[left]>index) {
            if (left>n && row[n]>index) left=n-1;
            while (row[left]>index) --left;
        }
        index-=row[left];put((k-left+sign)^sign);k=left;
    }
    uint32_t p=2*k+1;int sign=-int(index>=p);index-=p&sign;int before=k;
    k=int((index+1)>>1);if (k) index-=2*k-1;
    put((before-k+sign)^sign);
    sign=-int(index);put((k+sign)^sign);
    return energy;
}
// Givens rotation by (c,s) applied to element pairs `stride` apart, forwards then backwards.
// Each step rotates a value the previous step produced; with adjacent pairs that value is
// carried in a register rather than stored and reloaded.
void Rotate(float* x,int n,int stride,float c,float s) {
    if (stride==1 && n>=2) {
        float a=x[0];
        for (int i=0;i+1<n;++i) { float b=x[i+1];x[i]=c*a-s*b;a=c*b+s*a; }
        x[n-1]=a;
        if (n<3) return;
        float b=x[n-2];
        for (int i=n-3;i>=0;--i) { float v=x[i];x[i+1]=c*b+s*v;b=c*v-s*b; }
        x[0]=b;
        return;
    }
    auto turn=[&](int i) { float a=x[i],b=x[i+stride];x[i]=c*a-s*b;x[i+stride]=c*b+s*a; };
    for (int i=0;i<n-stride;++i) turn(i);
    for (int i=n-2*stride-1;i>=0;--i) turn(i);
}
// Applies the CELT "spread" (spectral whitening) rotation to a band.
void Spread(float* x,int n,int k,int blocks,int spread) {
    if (!spread || k*2>=n) return;
    static constexpr int factor[]={15,10,5};
    double g=double(n)/(n+factor[spread-1]*k),angle=pi*0.25*g*g;
    float c=(float)std::cos(angle),s=(float)std::sin(angle);
    int second=0;
    if (n>=8*blocks) { second=1; while ((second*second+second)*blocks+blocks/4<n) ++second; }
    int width=n/blocks;
    for (int b=0;b<blocks;++b) {
        if (second) Rotate(x+b*width,width,second,s,c);
        Rotate(x+b*width,width,1,c,s);
    }
}
// The RFC's cumulative bit-cost tables (one per band and split depth: a count, then up to 40
// increasing costs), with room after the last one so a band's whole table loads as vectors.
constexpr auto pulseCosts=[] {
    std::array<uint8_t,sizeof(d::cache_bits50)+48> table{};
    for (size_t i=0;i<sizeof(d::cache_bits50);++i) table[i]=d::cache_bits50[i];
    return table;
}();
// `cache` is a band's table in pulseCosts; PulseIndex picks the pulse count q whose cost is
// closest to the bit budget (ties round down).
int PulseCost(const uint8_t* cache,int q) { return q?cache[q]+1:0; }
int PulseIndex(const uint8_t* cache,int budget) {
    // The counts cheaper than the budget come first; all of a table is compared at once, so
    // no comparison waits on another.
    int entries=cache[0],cheaper=entries;
    if (budget<=1) cheaper=0;
    else if (budget<=256) {
        const __m128i limit=_mm_set1_epi8(char(budget-1));
        auto reached=[&](int at) {
            __m128i cost=_mm_loadu_si128((const __m128i*)(cache+1+at));
            return (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_max_epu8(cost,limit),cost));
        };
        uint64_t mask=reached(0)|uint64_t(reached(16))<<16|uint64_t(reached(32))<<32|1ull<<48;
        cheaper=std::min(std::countr_zero(mask),entries);
    }
    if (cheaper==entries) return entries;
    return budget-PulseCost(cache,cheaper)<=PulseCost(cache,cheaper+1)-budget?cheaper:cheaper+1;
}
}

// Decodes a PVQ codeword of k pulses over n samples using the combinadic pulse
// counts, then normalises and spreads; returns the per-block nonzero mask.
unsigned OpusCelt::Impl::Frame::Pulses(float* out,int n,int k,int blocks,float gain) {
    const auto& table=PulseCounts();
    uint64_t total=n>=1 && n<=176 && k>=1 && k<=128?uint64_t(table[n][k])+table[n][k+1]:UINT32_MAX;
    if (total>=UINT32_MAX) { range.Fail(); return 0; }
    int y[176];
    uint32_t squares=PulseVector(table,n,k,range.Uniform((uint32_t)total),y);
    // A block's mask bit says it received a pulse (a single block always does); a ragged tail
    // belongs to the last block.
    unsigned mask=blocks==1;
    if (blocks>1) for (int b=0,perBlock=n/blocks;b<blocks;++b)
        for (int i=b*perBlock,end=b+1<blocks?i+perBlock:n;i<end;++i) if (y[i]) { mask|=1u<<b;break; }
    // The squared pulse counts sum exactly, so this matches Normalize's double sum.
    float scale=gain/(float)std::sqrt(squares+1e-20);
    for (int i=0;i<n;++i) out[i]=(float)y[i]*scale;
    Spread(out,n,k,blocks,spread);
    return mask;
}

// Decodes one band (or a split half) into x/y: stereo or intensity split shape,
// folding from lower bands, PVQ or noise fill, then the inverse Haar/reorder.
// Returns the per-block collapsed mask; `level` drives recursive split depth.
unsigned OpusCelt::Impl::Frame::Band(float* x,float* y,int n,int budget,int blocks,int mode,int level,
    const float* low,float* fold,float gain,unsigned fill,int tfChange) {
    bool stereo=y!=nullptr;
    if (n==1) {
        for (int ch=0;ch<(stereo?2:1);++ch) {
            bool negative=false;
            if (remaining>=8) { negative=range.Raw(1)!=0; remaining-=8; }
            (ch?y:x)[0]=negative?-1.0f:1.0f;
        }
        if (fold) *fold=*x;
        return 1;
    }
    int originalN=n,originalBlocks=blocks,width=n/blocks,recombine=0,divide=0;
    bool longBlock=blocks==1;
    // A band holds at most 22 << 3 coefficients.
    float lowCopy[176];
    if (!stereo && level==0) {
        recombine=std::max(0,tfChange);
        if (low && (recombine || (!(width&1) && tfChange<0) || blocks>1)) { std::copy_n(low,n,lowCopy); low=lowCopy; }
        for (int i=0;i<recombine;++i) {
            if (low) Haar(lowCopy,n>>i,1<<i);
            fill=Compress(fill);
        }
        blocks>>=recombine; width<<=recombine;
        while (!(width&1) && tfChange<0) {
            if (low) Haar(lowCopy,width,blocks);
            fill|=fill<<blocks; blocks*=2; width/=2; ++divide;++tfChange;
        }
        if (low && blocks>1) Reorder(lowCopy,width>>recombine,blocks<<recombine,longBlock,false);
    }
    int transformedBlocks=blocks,transformedWidth=width;
    bool split=stereo;
    int key=(mode+1)*21+band;
    if (key<0 || key>=105 || d::cache_index50[key]<0) { range.Fail();return 0; }
    const uint8_t* cache=pulseCosts.data()+d::cache_index50[key];
    if (!stereo && mode>=0 && n>2 && budget>PulseCost(cache,cache[0])+11) {
        n/=2; y=x+n; split=true; --mode;
        if (blocks==1) fill=(fill&1)|(fill<<1);
        blocks=(blocks+1)/2;
    }
    unsigned collapse=0;
    bool invert=false;
    float mid=1,side=0;
    if (split) {
        int pulseCap=d::logN400[band]+mode*8;
        int offset=(pulseCap>>1)-(stereo && n==2?16:4),degrees=2*n-1-int(stereo && n==2);
        int precision=std::min(std::min(budget-pulseCap-32,(budget+degrees*offset)/degrees),64);
        static constexpr int exponential[]={16384,17866,19483,21247,23170,25267,27554,30048};
        int steps=precision<4?1:((exponential[precision&7]>>(14-(precision>>3)))+1)/2*2;
        if (stereo && band>=intensity) steps=1;
        int before=range.TellFraction(),theta=0;
        if (steps>1) {
            int index;
            if (stereo && n>2) {
                unsigned half=steps/2,total=4*half+3,frequency=range.Frequency(total);
                unsigned pivot=3*(half+1),symbol=frequency<pivot?frequency/3:half+1+frequency-pivot;
                unsigned start=symbol<=half?symbol*3:pivot+symbol-half-1;
                range.Update(start,start+(symbol<=half?3:1),total);index=(int)symbol;
            } else if (transformedBlocks>1 || stereo) index=(int)range.Uniform(steps+1);
            else {
                // Triangular pdf min(i+1,steps+1-i): with h=steps/2 the total is (h+1)^2 and
                // the cumulative count below i <= h is i(i+1)/2, so the symbol is a square root.
                unsigned h=steps/2,total=(h+1)*(h+1),frequency=range.Frequency(total),symbol,below,size;
                auto root=[](unsigned v) { unsigned r=(unsigned)std::sqrt((double)v); while (r*r>v) --r; while ((r+1)*(r+1)<=v) ++r; return r; };
                if (frequency<(h+1)*(h+2)/2) { symbol=(root(8*frequency+1)-1)/2; below=symbol*(symbol+1)/2; size=symbol+1; }
                else {
                    unsigned r=(root(8*(total-1-frequency)+1)+1)/2;
                    symbol=steps+1-r; below=total-r*(r+1)/2; size=r;
                }
                range.Update(below,below+size,total);index=(int)symbol;
            }
            theta=index*16384/steps;
        } else if (stereo && budget>16 && remaining>16) invert=range.Bit(2);
        int cost=range.TellFraction()-before;
        budget-=cost; remaining-=cost;
        unsigned originalFill=fill;
        int imid,iside,delta;
        if (!theta) { imid=32767;iside=0;delta=-16384;fill&=(1u<<blocks)-1; }
        else if (theta==16384) { imid=0;iside=32767;delta=16384;fill&=((1u<<blocks)-1)<<blocks; }
        else { imid=Cosine(theta);iside=Cosine(16384-theta);delta=RoundedProduct((n-1)*128,LogTan(iside,imid)); }
        mid=imid*(1.0f/32768);side=iside*(1.0f/32768);
        if (stereo && n==2) {
            int sideBits=theta && theta!=16384?8:0; remaining-=sideBits;
            bool swapped=theta>8192; float* major=swapped?y:x;float* minor=swapped?x:y;
            int sign=sideBits && range.Raw(1)?-1:1;
            collapse=Band(major,nullptr,n,budget-sideBits,blocks,mode,level,low,fold,gain,originalFill,tfChange);
            minor[0]=-sign*major[1];minor[1]=sign*major[0];
            for (int i=0;i<2;++i) { float a=x[i]*mid,b=y[i]*side;x[i]=a-b;y[i]=a+b; }
        } else {
            if (transformedBlocks>1 && !stereo && theta && theta!=16384) {
                if (theta>8192) delta-=delta>>(4-mode);
                else delta=std::min(0,delta+((n*8)>>(5-mode)));
            }
            int left=std::clamp((budget-delta)/2,0,std::max(0,budget)),right=budget-left;
            const float* secondLow=low && !stereo?low+n:nullptr;
            int childLevel=stereo?0:level+1;
            auto firstHalf=[&] { return Band(x,nullptr,n,left,blocks,mode,childLevel,low,stereo?fold:nullptr,stereo?1:gain*mid,fill,tfChange); };
            auto secondHalf=[&] { return Band(y,nullptr,n,right,blocks,mode,childLevel,secondLow,nullptr,gain*side,fill>>blocks,tfChange); };
            unsigned a,b;
            int saved=remaining;
            if (left>=right) {
                a=firstHalf();int spare=left-(saved-remaining);
                if (spare>24 && theta) right+=spare-24;
                b=secondHalf();
            } else {
                b=secondHalf();int spare=right-(saved-remaining);
                if (spare>24 && theta!=16384) left+=spare-24;
                a=firstHalf();
            }
            collapse=a|(b<<(stereo?0:transformedBlocks/2));
        }
    } else {
        int q=PulseIndex(cache,budget),cost=PulseCost(cache,q);
        remaining-=cost;
        while (q && remaining<0) { remaining+=cost;cost=PulseCost(cache,--q);remaining-=cost; }
        if (q) {
            int k=q<8?q:(8+(q&7))<<((q>>3)-1);
            collapse=Pulses(x,n,k,blocks,gain);
        } else {
            unsigned mask=(1u<<blocks)-1;fill&=mask;
            if (!fill) memset(x,0,n*sizeof(float));
            else {
                uint32_t seed=owner.random;
                for (int i=0;i<n;++i) {
                    seed=seed*1664525u+1013904223u;
                    x[i]=low?low[i]+((seed&0x8000)?1.0f/256:-1.0f/256):(float)((int32_t)seed>>20);
                }
                owner.random=seed;collapse=low?fill:mask;Normalize(x,n,gain);
            }
        }
    }
    if (stereo) {
        if (n!=2) {
            double cross,power;
            CrossPower(x,y,n,cross,power);
            double left=mid*mid+power-2*mid*cross,right=mid*mid+power+2*mid*cross;
            if (left<0.0006 || right<0.0006) std::copy_n(x,n,y);
            else {
                float a=(float)(1/std::sqrt(left)),b=(float)(1/std::sqrt(right));
                const __m128 m=_mm_set1_ps(mid),va=_mm_set1_ps(a),vb=_mm_set1_ps(b);
                int i=0;
                for (;i+4<=n;i+=4) {
                    __m128 l=_mm_mul_ps(_mm_loadu_ps(x+i),m),r=_mm_loadu_ps(y+i);
                    _mm_storeu_ps(x+i,_mm_mul_ps(_mm_sub_ps(l,r),va));_mm_storeu_ps(y+i,_mm_mul_ps(_mm_add_ps(l,r),vb));
                }
                for (;i<n;++i) { float l=x[i]*mid,r=y[i];x[i]=(l-r)*a;y[i]=(l+r)*b; }
            }
        }
        if (invert) for (int i=0;i<n;++i) y[i]=-y[i];
    } else if (!level) {
        if (transformedBlocks>1) Reorder(x,transformedWidth>>recombine,transformedBlocks<<recombine,longBlock,true);
        width=transformedWidth;blocks=transformedBlocks;
        for (int i=0;i<divide;++i) { blocks/=2;width*=2;collapse|=collapse>>blocks;Haar(x,width,blocks); }
        for (int i=0;i<recombine;++i) { collapse=Expand(collapse);Haar(x,originalN>>i,1<<i); }
        if (fold) { float scale=(float)std::sqrt(originalN);for (int i=0;i<originalN;++i) fold[i]=x[i]*scale; }
        collapse&=(1u<<originalBlocks)-1;
    }
    return collapse;
}

bool OpusCelt::Impl::Decode(OpusRange& range,unsigned channels,unsigned lm,unsigned first,unsigned last,std::vector<float>& out) {
    if (!channels || channels>2 || lm>3 || first>=last || last>21) return false;
    const int bits=(int)range.Bytes()*8,multiple=1<<lm;
    if (channels==1) for (int i=0;i<21;++i) energy[0][i]=std::max(energy[0][i],energy[1][i]);
    // Silence when the coder is already at/over budget; a lone flag may state it explicitly.
    bool silence=range.Tell()>=bits;
    if (!silence && range.Tell()==1) silence=range.Bit(15);
    Filter filter;
    bool transient=false;
    // Every coded coefficient is written by its band; the rest of the spectrum stays zero.
    float spectrum[2][960];
    if (!silence) {
        for (auto& c:spectrum) {
            memset(c,0,d::bands[first]*multiple*sizeof(float));
            memset(c+d::bands[last]*multiple,0,(960-d::bands[last]*multiple)*sizeof(float));
        }
        // Optional pitch post-filter, coded only into the first band of a frame.
        if (!first && range.Tell()+16<=bits && range.Bit(1)) {
            unsigned octave=range.Uniform(6);
            filter.period=(16<<octave)+(int)range.Raw(4+octave)-1;
            filter.gain=0.09375f*(range.Raw(3)+1);
            if (range.Tell()+2<=bits) filter.taps=(int)range.Icdf(d::tapset_icdf,2);
        }
        // Transient selects short MDCT blocks; intra disables inter-frame energy prediction.
        if (lm && range.Tell()+3<=bits) transient=range.Bit(3);
        bool intra=range.Tell()+3<=bits && range.Bit(3);
        static constexpr float prediction[]={29440.0f/32768,26112.0f/32768,21248.0f/32768,16384.0f/32768};
        static constexpr float memory[]={30147.0f/32768,22282.0f/32768,12124.0f/32768,6554.0f/32768};
        float coefficient=intra?0:prediction[lm],beta=intra?4915.0f/32768:memory[lm],coarseHistory[2]{};
        // Decode each band's log energy as a Laplace residual off the predicted value.
        const uint8_t* model=d::energyModels[lm*2+unsigned(intra)];
        for (unsigned i=first;i<last;++i) for (unsigned ch=0;ch<channels;++ch) {
            int room=bits-range.Tell(),value;
            if (room>=15) value=Laplace(range,model[i*2]<<7,model[i*2+1]<<6);
            else if (room>=2) { static constexpr uint8_t smallEnergy[]={2,1,0};unsigned q=range.Icdf(smallEnergy,2);value=int(q>>1)^-int(q&1); }
            else value=room>=1?-int(range.Bit(1)):-1;
            energy[ch][i]=coefficient*std::max(-9.0f,energy[ch][i])+coarseHistory[ch]+value;
            coarseHistory[ch]+=(1-beta)*value;
        }
        Frame frame{*this,range,(int)channels,(int)lm,(int)first,(int)last};
        // Per-band TF-change flags, differentially coded (finer step when transient).
        int logp=transient?2:4;
        bool reserve=lm && range.Tell()+logp+1<=bits,change=false,current=false;
        int tfBudget=bits-int(reserve);
        for (unsigned i=first;i<last;++i) {
            if (range.Tell()+logp<=tfBudget) { current^=range.Bit(logp);change|=current; }
            frame.tf[i]=current;logp=transient?4:5;
        }
        int select=0,index=4*int(transient);
        if (reserve && d::tf[lm][index+int(change)]!=d::tf[lm][index+2+int(change)]) select=range.Bit(1);
        for (unsigned i=first;i<last;++i) frame.tf[i]=d::tf[lm][index+2*select+frame.tf[i]];
        if (range.Tell()+4<=bits) frame.spread=(int)range.Icdf(d::spread_icdf,5);
        // Static per-band coding caps, plus the packet's optional per-band boost.
        Bands caps{},boost{};
        for (unsigned i=first;i<last;++i) caps[i]=(d::cache_caps50[(lm*2+channels-1)*21+i]+64)*channels*(Width(i)<<lm)/4;
        int logBoost=6,total=bits*8;
        for (unsigned i=first;i<last;++i) {
            int width=channels*(Width(i)<<lm),quantum=std::min(width*8,std::max(48,width)),probability=logBoost;
            while (range.TellFraction()+probability*8<total && boost[i]<caps[i]) {
                if (!range.Bit(probability)) break;
                boost[i]+=quantum;total-=quantum;probability=1;
            }
            if (boost[i]) logBoost=std::max(2,logBoost-1);
        }
        // `trim` biases allocation between low and high bands.
        int trim=range.TellFraction()+48<=total?(int)range.Icdf(d::trim_icdf,7):5;
        int available=bits*8-range.TellFraction()-1;
        int antiReserve=transient && lm>=2 && available>=int(lm+2)*8?8:0;
        Allocation allocation=Allocate(range,first,last,channels,lm,trim,available-antiReserve,boost,caps);
        frame.intensity=allocation.intensity;
        // Fine energy refinement: one uniform value per coded band, in units of 2^-fine.
        for (unsigned i=first;i<last;++i) if (allocation.fine[i]) for (unsigned ch=0;ch<channels;++ch)
            energy[ch][i]+=(range.Raw(allocation.fine[i])+0.5f)/float(1<<allocation.fine[i])-0.5f;
        float fold[2][960]{};
        unsigned masks[21][2]{};
        int lowBand=0,balance=allocation.balance;
        bool update=true,dual=allocation.dual;
        int blocks=transient?multiple:1;
        // Reconstruct bands low to high, folding energy from already-decoded bands;
        // `balance` redistributes leftover bits to higher bands.
        for (unsigned i=first;i<last;++i) {
            frame.band=i;
            int begin=d::bands[i]*multiple,width=Width(i)*multiple,tell=range.TellFraction();
            if (i!=first) balance-=tell;
            frame.remaining=bits*8-antiReserve-tell-1;
            int budget=0;
            if ((int)i<allocation.coded) budget=std::clamp(allocation.shape[i]+balance/std::min(3,allocation.coded-(int)i),0,
                std::max(0,std::min(16383,frame.remaining+1)));
            if ((begin-width>=d::bands[first]*multiple || i==first+1) && (update || !lowBand)) lowBand=i;
            if (i==first+1 && Width(i)>Width(first)) {
                int from=d::bands[first]*multiple,a=Width(first)*multiple,b=Width(i)*multiple;
                for (unsigned ch=0;ch<channels;++ch) for (int j=0;j<b-a;++j) fold[ch][from+a+j]=fold[ch][from+2*a-b+j];
            }
            int low=-1;unsigned leftMask=(1u<<blocks)-1,rightMask=leftMask;
            if (lowBand && (frame.spread!=3 || blocks>1 || frame.tf[i]<0)) {
                low=std::max(int(d::bands[first])*multiple,int(d::bands[lowBand])*multiple-width);
                leftMask=rightMask=0;
                for (unsigned j=first;j<i;++j) if (d::bands[j+1]*multiple>low && d::bands[j]*multiple<low+width) {
                    leftMask|=masks[j][0];rightMask|=masks[j][channels-1];
                }
            }
            if (dual && (int)i==allocation.intensity) {
                dual=false;
                for (int j=d::bands[first]*multiple;j<begin;++j) fold[0][j]=(fold[0][j]+fold[1][j])*0.5f;
            }
            if (dual) {
                for (unsigned ch=0;ch<2;++ch) masks[i][ch]=frame.Band(spectrum[ch]+begin,nullptr,width,budget/2,blocks,lm,0,
                    low<0?nullptr:fold[ch]+low,fold[ch]+begin,1,ch?rightMask:leftMask,frame.tf[i]);
            } else {
                masks[i][0]=frame.Band(spectrum[0]+begin,channels==2?spectrum[1]+begin:nullptr,width,budget,blocks,lm,0,
                    low<0?nullptr:fold[0]+low,fold[0]+begin,1,leftMask|rightMask,frame.tf[i]);
                masks[i][1]=masks[i][0];
            }
            balance+=allocation.shape[i]+tell;update=budget>width*8;
        }
        // Anti-collapse: fill blocks left empty with band-limited noise to avoid
        // audible "birdies" in sparse high bands.
        bool anti=antiReserve && range.Raw(1);
        int room=bits-range.Tell();
        for (int priority=0;priority<2;++priority) for (unsigned i=first;i<last && room>=(int)channels;++i)
            if (allocation.fine[i]<8 && allocation.priority[i]==priority) {
                for (unsigned ch=0;ch<channels;++ch) energy[ch][i]+=(range.Raw(1)-0.5f)/float(1<<(allocation.fine[i]+1));
                room-=channels;
            }
        if (anti) {
            uint32_t seed=random;
            for (unsigned i=first;i<last;++i) {
                int width=Width(i)*multiple,depth=(1+allocation.shape[i])/width;
                float threshold=0.5f*(float)std::exp2(-0.125*depth);
                for (unsigned ch=0;ch<channels;++ch) {
                    float a=old[ch][i],b=older[ch][i];
                    if (channels==1) { a=std::max(a,old[1][i]);b=std::max(b,older[1][i]); }
                    float difference=std::max(0.0f,energy[ch][i]-std::min(a,b));
                    float level=std::min(threshold,2.0f*(float)std::exp2(-difference)*(lm==3?1.41421356237f:1.0f))/(float)std::sqrt(width);
                    float* x=spectrum[ch]+d::bands[i]*multiple;bool changed=false;
                    for (int block=0;block<multiple;++block) if (!(masks[i][ch]&(1u<<block))) {
                        for (int j=0;j<Width(i);++j) { seed=seed*1664525u+1013904223u;x[j*multiple+block]=(seed&0x8000)?level:-level; }
                        changed=true;
                    }
                    if (changed) Normalize(x,width,1);
                }
            }
        }
        if (!range.Good()) return false;
        // Apply the decoded log energies (band mean + residual) as band amplitudes.
        for (unsigned ch=0;ch<channels;++ch) for (unsigned i=first;i<last;++i) {
            float amplitude=(float)std::exp2(std::min(32.0f,energy[ch][i]+d::means[i]/16.0f));
            for (int j=d::bands[i]*multiple;j<d::bands[i+1]*multiple;++j) spectrum[ch][j]*=amplitude;
        }
    } else {
        for (auto& c:energy) std::fill_n(c,21,-28.0f);
        memset(spectrum,0,sizeof spectrum);
    }
    Synthesis(spectrum,channels,lm,transient,filter,out);
    // Advance the energy history (only non-transient frames keep a 2-frame record).
    if (channels==1) std::copy_n(energy[0],21,energy[1]);
    for (unsigned ch=0;ch<2;++ch) for (unsigned i=0;i<21;++i) {
        if (!transient) { older[ch][i]=old[ch][i];old[ch][i]=energy[ch][i]; }
        else old[ch][i]=std::min(old[ch][i],energy[ch][i]);
        if (i<first || i>=last) { energy[ch][i]=0;old[ch][i]=older[ch][i]=-28; }
    }
    random=range.FinalRange();
    return AllFinite(out.data(),out.size());
}

// Inverse MDCT + overlap-add, post-filter mixing and deemphasis; writes n frames
// of interleaved 48 kHz output. `position` indexes the rolling post-filter buffer.
void OpusCelt::Impl::Synthesis(float spectrum[2][960],unsigned channels,unsigned lm,bool transient,Filter filter,std::vector<float>& out) {
    unsigned n=120u<<lm,blocks=transient?1u<<lm:1,block=n/blocks;
    out.resize(n*2);
    static constexpr float taps[3][3]={{0.306640625f,0.217041015625f,0.129638671875f},
        {0.4638671875f,0.26806640625f,0},{0.7998046875f,0.10009765625f,0}};
    const bool comb=previous.gain || latest.gain || filter.gain;
    float filtered[2][960];
    for (unsigned ch=0;ch<2;++ch) {
        float* wave=wave_[ch];
        // A mono frame feeds both outputs the same transform; only the histories differ.
        if (ch && channels==1) std::copy_n(wave_[0],n+120,wave);
        else {
            memset(wave,0,(n+120)*sizeof(float));
            const float* source=spectrum[channels==1?0:ch];
            for (unsigned b=0;b<blocks;++b) {
                float input[960],dct[960];
                if (blocks>1) for (unsigned i=0;i<block;++i) input[i]=source[i*blocks+b];
                mdct[transient?0:lm].Dct(blocks>1?input:source,dct);
                OverlapAdd(dct,window.data(),wave+b*block,block);
            }
        }
        float* result=filtered[ch];
        for (unsigned i=0;i<120;++i) { result[i]=wave[i]+overlap[ch][i];overlap[ch][i]=wave[n+i]; }
        std::copy_n(wave+120,n-120,result+120);
        if (!comb) {
            // The ring holds 2048 samples, so a frame wraps at most once.
            unsigned at=position&2047,head=std::min(n,2048-at);
            std::copy_n(result,head,post[ch]+at);std::copy_n(result+head,n-head,post[ch]);
            continue;
        }
        // Post-filter output at sample `at`, from the 3-tap comb around the period.
        auto contribution=[&](const Filter& f,uint32_t at) {
            if (!f.gain) return 0.0f;
            float value=post[ch][(at-std::max(15,f.period))&2047]*taps[f.taps][0];
            for (unsigned j=1;j<3;++j) value+=(post[ch][(at-std::max(15,f.period)-j)&2047]+post[ch][(at-std::max(15,f.period)+j)&2047])*taps[f.taps][j];
            return value*f.gain;
        };
        for (unsigned i=0;i<n;++i) {
            float value=result[i];
            // Cross-fade between the previous/latest/current post-filter settings
            // over the first two 120-sample halves of the frame.
            if (i<120) { float mix=window[i]*window[i];value+=(1-mix)*contribution(previous,position+i)+mix*contribution(latest,position+i); }
            else if (i<240) { float mix=window[i-120]*window[i-120];value+=(1-mix)*contribution(latest,position+i)+mix*contribution(filter,position+i); }
            else value+=contribution(filter,position+i);
            post[ch][(position+i)&2047]=result[i]=value;
        }
    }
    Deemphasis(filtered[0],filtered[1],n,deemphasis,out.data());
    // A short (transient) frame collapses the post-filter history to the current one.
    previous=latest;latest=filter;if (lm) previous=latest;
    position+=n;
}

OpusCelt::OpusCelt():impl_(std::make_unique<Impl>()) { (void)PulseCounts(); }  // build the combinadics once, off the decode path
OpusCelt::~OpusCelt()=default;
void OpusCelt::Reset() { impl_->Reset(); }
std::shared_ptr<const OpusCelt::State> OpusCelt::Save() const { return std::make_shared<State>(*impl_); }
void OpusCelt::Load(const State& state) { static_cast<State&>(*impl_)=state; }
bool OpusCelt::Decode(OpusRange& range,unsigned channels,unsigned lm,unsigned first,unsigned last,std::vector<float>& out) {
    return impl_->Decode(range,channels,lm,first,last,out);
}
} // namespace nativeaudio
