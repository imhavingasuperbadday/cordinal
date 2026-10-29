// Cordinal ALAC: bounded Golomb codes, adaptive integer prediction, stereo unmixing.
// The packet syntax is defined by Apple's ALAC format; no reference decoder is linked.
// Decodes the MP4 'alac' codec; a partial frame supplies only its final n samples.
#include "native_audio.h"
#include "audio_bits.h"
#include "audio_packets.h"
#include <bit>
#include <type_traits>
#include <array>

namespace {
using namespace nativeaudio;
// Wrap to `bits` signed bits, matching ALAC's modulo arithmetic on intermediates.
int Sign(int64_t x) { return (x>0)-(x<0); }
int64_t Wrap(int64_t x,unsigned bits) { return int64_t(uint64_t(x)<<(64-bits))>>(64-bits); }
unsigned Log(uint64_t x) { return x ? HighestBit(x) : 0; }

// Bounded Golomb-Rice: a unary prefix selects the group, the next k bits the offset; a
// prefix of 9 (escape) instead reads a raw value of `escape` bits. The result is unsigned.
__forceinline uint64_t Golomb(Bits& b,unsigned k,unsigned escape,uint32_t divisor) {
    unsigned prefix=0;
    if (k && k<=23 && b.Left()>=32) {
        uint32_t word=(uint32_t)b.Peek(32);
        // Leading ones, at most nine: bit 22 stops the count there.
        prefix=31-HighestBit(uint32_t(~word|(1u<<22)));
        if (prefix==9) { b.Skip(9); return b.Get(escape); }
        if (k==1 && divisor==1) { b.Skip(prefix+1); return prefix; }
        uint32_t tail=(word<<(prefix+1))>>(32-k);
        b.Skip(prefix+1+k-(tail<2));
        return uint64_t(prefix)*divisor+(tail<2?0:tail-1);
    }
    if (b.Left()>=9) {
        prefix=31-HighestBit(uint32_t(~((uint32_t)b.Peek(9)<<23)));
        b.Skip(prefix+(prefix<9));
    } else while (prefix<9 && b.Get(1)) ++prefix;
    if (prefix==9) return b.Get(escape);
    if (k==1 && divisor==1) return prefix;
    if (!k || k>31) { b.Fail(); return 0; }
    uint32_t tail=(uint32_t)b.Peek(k);
    b.Skip(tail<2?k-1:k);
    return uint64_t(prefix)*divisor+(tail<2?0:tail-1);
}

struct Predictor {
    unsigned order=0, shift=0, factor=0, mode=0;
    int16_t coefficients[31]{};
    // Per-channel predictor header: mode, shift, quantization factor and order/taps.
    void Header(Bits& b) {
        mode=(unsigned)b.Get(4); shift=(unsigned)b.Get(4);
        factor=(unsigned)b.Get(3); order=(unsigned)b.Get(5);
        for (unsigned i=0;i<order;++i) coefficients[i]=(int16_t)b.Signed(16);
    }
    // Reconstruct residuals into samples. `Narrow` allows 32-bit intermediates when the
    // wrapped sum provably survives the final width truncation; coefficients adapt in
    // place after every sample.
    template<unsigned Order,bool Narrow,class T> void RestoreOrder(std::vector<T>& samples,unsigned width) {
        const unsigned taps=Order?Order:order;
        const size_t n=samples.size();
        const unsigned predictionShift=shift;
        const int64_t rounding=predictionShift?int64_t(1)<<(predictionShift-1):0;
        for (size_t i=1;i<=taps && i<n;++i) samples[i]=(T)Wrap((int64_t)samples[i]+samples[i-1],width);
        for (size_t i=taps+1;i<n;++i) {
            const int64_t base=samples[i-taps-1], residual=samples[i];
            int64_t prediction=rounding;
            if constexpr (Narrow) {
                // Wrapping the sum before shifting changes it by a multiple
                // of 2^(32-shift), discarded by the final sample-width wrap.
                uint32_t sum=(uint32_t)prediction;
                for (unsigned j=0;j<taps;++j) sum+=(uint32_t)coefficients[j]*((uint32_t)samples[i-j-1]-(uint32_t)base);
                prediction=(int32_t)sum;
            } else for (unsigned j=0;j<taps;++j) prediction+=coefficients[j]*(samples[i-j-1]-base);
            samples[i]=(T)Wrap(base+(prediction>>predictionShift)+residual,width);
            const int direction=Sign(residual);
            using Adaptive=std::conditional_t<sizeof(T)==4,int32_t,int64_t>;
            Adaptive remaining=(Adaptive)(residual*direction);
            for (unsigned j=taps;direction && j-- && remaining>0;) {
                Adaptive difference=(Adaptive)(base-samples[i-j-1]);
                int adjustment=Sign(difference)*direction;
                coefficients[j]=(int16_t)(coefficients[j]-adjustment);
                // Arithmetic rounding is applied before multiplying by the tap distance.
                remaining-=direction*Adaptive(taps-j)*((adjustment*difference)>>predictionShift);
            }
        }
    }
    template<class T> void Restore(std::vector<T>& samples,unsigned width) {
        if (!order || samples.empty()) return;
        if (order==31) { for (size_t i=1;i<samples.size();++i) samples[i]=(T)Wrap((int64_t)samples[i]+samples[i-1],width); return; }
        if (width+shift<=32) RestoreSelected<true>(samples,width);
        else RestoreSelected<false>(samples,width);
    }
    template<bool Narrow,class T> void RestoreSelected(std::vector<T>& samples,unsigned width) {
        switch (order) {
        case 4: RestoreOrder<4,Narrow>(samples,width);break;
        case 6: RestoreOrder<6,Narrow>(samples,width);break;
        case 8: RestoreOrder<8,Narrow>(samples,width);break;
        default: RestoreOrder<0,Narrow>(samples,width);break;
        }
    }
};

class AlacDecoder final : public PacketDecoder {
public:
    bool Open(const std::wstring& path) {
        // The magic cookie is an ALACSpecificConfig: block size, sample width, history
        // parameters, Rice limit, channel count and sample rate.
        if (!media_.OpenMp4(path) || media_.codec!=0x616c6163 || media_.config.size()<24) return false;
        const auto* p=media_.config.data();
        block_=U32(p,true); bits_=p[5]; historyWeight_=p[6]; historyStart_=p[7]; riceLimit_=p[8]; channels_=p[9];
        unsigned rate=U32(p+20,true);
        if (p[4] || !block_ || block_>65536 || bits_<4 || bits_>32 || !riceLimit_ || riceLimit_>31 ||
            !channels_ || channels_>8 || !rate || rate>768000 || rate!=media_.rate) return false;
        return Ready(rate,true);
    }
private:
    // Decode residuals with an adaptive Golomb parameter; runs of zeros are coded at once.
    template<class T> bool Residuals(Bits& b,std::vector<T>& values,unsigned width,unsigned factor) {
        uint32_t mean=historyStart_, weight=historyWeight_*factor/4;
        unsigned signOffset=0;
        for (size_t i=0;i<values.size();) {
            unsigned k=std::min(Log((mean>>9)+3),riceLimit_);
            uint64_t code=Golomb(b,k,width,(1u<<k)-1), v=code+signOffset;
            if (!b.Good() || v>(uint64_t(1)<<width)) return false;
            values[i++]=(T)((v>>1) ^ (uint64_t(0)-(v&1)));
            mean=code>65535?65535:uint32_t(weight*v+mean-((uint64_t(weight)*mean)>>9));
            signOffset=0;
            if (mean<128 && i<values.size()) {
                k=mean?7-Log(mean)+((mean+16)>>6):8;
                uint64_t run=Golomb(b,k,16,((1u<<k)-1)&((1u<<riceLimit_)-1));
                if (!b.Good() || run>values.size()-i) return false;
                std::fill_n(values.begin()+i,(size_t)run,0);
                i+=(size_t)run; signOffset=run<65535; mean=0;
            }
        }
        return b.Good();
    }
    bool Decode(const uint8_t* p,size_t bytes,std::vector<float>& out) override {
        return bits_<=24?DecodeSamples<int32_t>(p,bytes,out):DecodeSamples<int64_t>(p,bytes,out);
    }
    // An ALAC frame is a sequence of 3-bit-tagged elements, one or two channels each, with
    // its own predictor header and optional stereo matrix parameters.
    template<class T> bool DecodeSamples(const uint8_t* p,size_t bytes,std::vector<float>& out) {
        Bits b(p,bytes);
        unsigned channel=0, count=0;
        auto& planar=planar_;
        auto& values=[&]() -> std::array<std::vector<T>,2>& {
            if constexpr (sizeof(T)==4) return samples32_;else return samples64_;
        }();
        while (b.Good() && b.Left()>=3) {
            unsigned tag=(unsigned)b.Get(3);
            if (tag==7) break;
            if (tag==6) { size_t n=(size_t)b.Get(4); if (n==15) n+=b.Get(8)-1; b.Skip(n*8); continue; }
            if (tag==4) { b.Skip(4); bool align=b.Get(1)!=0; size_t n=(size_t)b.Get(8); if (n==255) n+=b.Get(8); if (align) b.Align(); b.Skip(n*8); continue; }
            if (tag!=0 && tag!=1 && tag!=3) return false;
            unsigned pair=tag==1?2:1;
            b.Skip(4);
            if (b.Get(12) || channel+pair>channels_) return false;
            bool partial=b.Get(1)!=0;
            unsigned low=(unsigned)b.Get(2)*8; bool raw=b.Get(1)!=0;
            unsigned n=partial?(unsigned)b.Get(32):block_;
            if (!n || n>block_ || low>16 || low>=bits_ || (channel && n!=count)) return false;
            if (!channel) {
                count=n;
                if (channels_<=2) out.resize(size_t(n)*2);
                else planar.resize(size_t(n)*channels_);
            }
            for (unsigned c=0;c<pair;++c) values[c].resize(n);
            unsigned matrixShift=0; int matrixWeight=0;
            auto& lows=lows_;
            // Raw channels are stored as signed bits_-wide samples with no prediction.
            if (raw) {
                low=0;
                for (unsigned i=0;i<n;++i) for (unsigned c=0;c<pair;++c) values[c][i]=(T)b.Signed(bits_);
            } else {
                matrixShift=(unsigned)b.Get(8); matrixWeight=(int)b.Signed(8);
                if (matrixShift>31) return false;
                Predictor pred[2];
                for (unsigned c=0;c<pair;++c) { pred[c].Header(b); if (pred[c].mode>1) return false; }
                if (low) {
                    lows.resize(size_t(n)*pair);
                    // Seven 8-bit or three 16-bit low parts per read.
                    const unsigned per=56/low;size_t i=0;
                    for (;i+per<=lows.size();i+=per) {
                        uint64_t word=b.Get(per*low);
                        for (unsigned k=0;k<per;++k) lows[i+k]=uint32_t(word>>((per-1-k)*low))&((1u<<low)-1);
                    }
                    for (;i<lows.size();++i) lows[i]=(uint32_t)b.Get(low);
                }
                unsigned width=bits_-low+pair-1;
                for (unsigned c=0;c<pair;++c) {
                    if (!Residuals(b,values[c],width,pred[c].factor)) return false;
                    if (pred[c].mode) for (unsigned i=1;i<n;++i) values[c][i]=(T)Wrap((int64_t)values[c][i]+values[c][i-1],width);
                    pred[c].Restore(values[c],width);
                }
            }
            const double scale=std::ldexp(1.0,1-(int)bits_);
            // Stereo of up to 24 bits mixes exactly in 32-bit lanes, so the SIMD path is exact;
            // raw low parts (interleaved left, right) are appended after the matrix.
            unsigned first=0;
            if constexpr (sizeof(T)==4) if (channels_==2 && pair==2 && (matrixWeight==0 || matrixWeight==1)) {
                __m128i widthShift=_mm_cvtsi32_si128(32-bits_),matrixCount=_mm_cvtsi32_si128(matrixShift),lowCount=_mm_cvtsi32_si128(low);
                __m128 gain=_mm_set1_ps((float)scale);
                for (;first+4<=n;first+=4) {
                    __m128i l=_mm_loadu_si128((const __m128i*)(values[0].data()+first));
                    __m128i r=_mm_loadu_si128((const __m128i*)(values[1].data()+first));
                    if (matrixWeight) { __m128i side=r;r=_mm_sub_epi32(l,_mm_sra_epi32(r,matrixCount));l=_mm_add_epi32(r,side); }
                    if (low) {
                        __m128 a=_mm_castsi128_ps(_mm_loadu_si128((const __m128i*)(lows.data()+first*2)));
                        __m128 c=_mm_castsi128_ps(_mm_loadu_si128((const __m128i*)(lows.data()+first*2+4)));
                        l=_mm_or_si128(_mm_sll_epi32(l,lowCount),_mm_castps_si128(_mm_shuffle_ps(a,c,0x88)));
                        r=_mm_or_si128(_mm_sll_epi32(r,lowCount),_mm_castps_si128(_mm_shuffle_ps(a,c,0xDD)));
                    }
                    l=_mm_sra_epi32(_mm_sll_epi32(l,widthShift),widthShift);
                    r=_mm_sra_epi32(_mm_sll_epi32(r,widthShift),widthShift);
                    __m128 leftFloat=_mm_mul_ps(_mm_cvtepi32_ps(l),gain),rightFloat=_mm_mul_ps(_mm_cvtepi32_ps(r),gain);
                    _mm_storeu_ps(out.data()+first*2,_mm_unpacklo_ps(leftFloat,rightFloat));
                    _mm_storeu_ps(out.data()+first*2+4,_mm_unpackhi_ps(leftFloat,rightFloat));
                }
            }
            for (unsigned i=first;i<n;++i) {
                if (pair==2 && matrixWeight) {
                    int64_t r=values[0][i]-(((int64_t)values[1][i]*matrixWeight)>>matrixShift);
                    values[0][i]=(T)(r+values[1][i]); values[1][i]=(T)r;
                }
                for (unsigned c=0;c<pair;++c) {
                    int64_t v=values[c][i];
                    if (low) v=(int64_t)((uint64_t(v)<<low)|lows[i*pair+c]);
                    float sample=(float)(Wrap(v,bits_)*scale);
                    if (channels_==1) out[i*2]=out[i*2+1]=sample;
                    else if (channels_==2) out[i*2+channel+c]=sample;
                    else planar[size_t(channel+c)*n+i]=sample;
                }
            }
            channel+=pair;
        }
        if (!b.Good() || channel!=channels_ || !count) return false;
        if (channels_<=2) return true;
        out.resize(size_t(count)*2);
        // ALAC order: C,L,R,(Ls,Rs),(Cs),LFE. Normalize before Cordinal's downmix.
        static constexpr uint8_t order[9][8]={{},{0},{0,1},{1,2,0},{1,2,0,3},{1,2,0,3,4},
            {1,2,0,5,3,4},{1,2,0,6,3,4,5},{3,4,0,7,5,6,1,2}};
        for (unsigned i=0;i<count;++i) {
            float v[8]{}; for (unsigned c=0;c<channels_;++c) v[c]=planar[size_t(order[channels_][c])*count+i];
            Mix(v,channels_,out.data()+i*2);
        }
        return true;
    }
    unsigned block_=0,bits_=0,channels_=0,historyWeight_=0,historyStart_=0,riceLimit_=0;
    std::array<std::vector<int32_t>,2> samples32_;
    std::array<std::vector<int64_t>,2> samples64_;
    std::vector<uint32_t> lows_;
    std::vector<float> planar_;
};
} // namespace

std::unique_ptr<Decoder> OpenAlacDecoder(const std::wstring& path) {
    auto d=std::make_unique<AlacDecoder>();
    if (!d->Open(path)) return nullptr;
    return d;
}
