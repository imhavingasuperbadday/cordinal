// Cordinal AC-3 reconstruction, following ETSI TS 102 366 clauses 4 and 6.
// The entropy cursor, masking calculation and synthesis are Cordinal code.
// Decodes internally up to 5.1 channels and downmixes to Cordinal's stereo output.
#include "native_audio.h"
#include "audio_packets.h"
#include "audio_bits.h"
#include "audio_mdct.h"
#include "audio_ac3_tables.h"
#include <array>
#include <bit>
#include <numbers>
#include <emmintrin.h>
#ifdef CORDINAL_CODEC_TRACE
#include <cstdio>
#endif

namespace {
using namespace nativeaudio;
// Frame header tables: sample-rate code, bitrate code (kbit/s) and acmod channel count.
constexpr unsigned rates[]{48000,44100,32000};
constexpr unsigned bitrates[]{32,40,48,56,64,80,96,112,128,160,192,224,256,320,384,448,512,576,640};
constexpr unsigned channelCount[]{2,1,2,3,3,4,4,5};
// Mantissas and dither stay normal after scaling by the validated 0..24 exponents.
// Constructing the exact power of two avoids a library call for every spectral bin.
float Scale(float value,unsigned exponent) { return value*std::bit_cast<float>((127u-exponent)<<23); }
// Dither noise, uniform in about +-0.707. A counter (the frame's seed plus a Weyl step per draw)
// is hashed, so no draw waits on the one before it as a recurrence's would.
float Noise(uint32_t& r) {
    uint32_t h=r+=0x9E3779B9u;
    h^=h>>16;h*=0x7FEB352Du;h^=h>>15;h*=0x846CA68Bu;h^=h>>16;
    return (int32_t)h*(0.7071067811865475f/2147483648.0f);
}
struct Header { unsigned bytes=0,rate=0,shift=0,sampleRateCode=0; };
// Parse the 7-byte syncframe header; the frame-size code and bitstream id set length and rate.
Header Inspect(const uint8_t* p,size_t size) {
    if (size<7 || p[0]!=11 || p[1]!=119) return {};
    unsigned f=p[4]>>6,n=p[4]&63,id=p[5]>>3;
    if (f==3 || n>=38 || id>10) return {};
    unsigned bitrate=bitrates[n/2],words=f==0?bitrate*2:f==2?bitrate*3:bitrate*320/147+(n&1);
    unsigned shift=id>8?id-8:0;
    return {words*2,rates[f]>>shift,shift,f};
}
// CRC-1 over the frame, starting at the byte after the syncword.
bool ValidCrc(const uint8_t* p,size_t size) { return !Crc16(p+2,size-2); }
// 253-bin exponent band layout: band width grows above bin 73 as 1,3,6,12,24.
constexpr auto BandEdges() {
    std::array<unsigned,51> edges{};
    for (unsigned band=0;band<50;++band)
        edges[band+1]=edges[band]+(band<28?1:band<35?3:band<41?6:band<45?12:24);
    return edges;
}
constexpr auto edges=BandEdges();
unsigned Band(unsigned bin) { return (unsigned)(std::upper_bound(edges.begin(),edges.end(),bin)-edges.begin()-1); }
// PSD-minus-mask (in 1/32 units) mapped to the bit-allocation pointer for each mantissa.
constexpr uint8_t allocation[]{0,1,1,1,1,1,2,2,3,3,3,4,4,5,5,6,6,6,6,7,7,7,7,8,8,8,8,9,9,9,9,10,
    10,10,10,11,11,11,11,12,12,12,12,13,13,13,13,14,14,14,14,14,14,14,14,15,15,15,15,15,15,15,15,15};

// Per-channel decode state accumulated within a block: exponent set, bit allocation and
// the coupling coordinates that parallel the quantized spectrum.
struct Channel {
    unsigned first=0,end=0,strategy=0,fine=0,fastGain=0;
    bool switched=false,dither=false,coupled=false,coordinates=false;
    std::array<uint8_t,256> exponent{},bap{};
    std::array<float,18> coordinate{};
    std::array<int,50> delta{};
    unsigned deltaMode=2;
    std::array<float,256> spectrum{},overlap{};
    // bap[] must be recomputed: exponents or allocation parameters changed since it was.
    bool stale=true;
};
// Masking/threshold parameters shared by the block; Compute() turns each channel's
// exponents and PSD into the per-bin bit-allocation pointer bap[].
struct Allocation {
    int slowDecay=0,fastDecay=0,slowGain=0,knee=0,floor=0;
    unsigned coarse=0,fastLeak=0,slowLeak=0,rateCode=0,shift=0;
    void Parameters(Bits& b) {
        constexpr int slow[]{15,17,19,21},fast[]{63,83,103,123},gain[]{1344,1240,1144,1040};
        constexpr int knees[]{0,1792,2304,2816},floors[]{752,688,624,560,496,368,240,-2048};
        slowDecay=slow[b.Get(2)]>>shift;fastDecay=fast[b.Get(2)]>>shift;
        slowGain=gain[b.Get(2)];knee=knees[b.Get(2)];floor=floors[b.Get(3)];
    }
    void Compute(Channel& ch,bool zero) const {
        ch.bap.fill(0);if (zero) return;
        std::array<int,50> energy{},excitation{};
        const uint8_t* exponent=ch.exponent.data();
        auto psd=[&](unsigned bin) { return 3072-128*int(exponent[bin]); };
        unsigned firstBand=Band(ch.first),lastBand=Band(ch.end-1)+1;
        for (unsigned band=firstBand;band<std::min(lastBand,28u);++band) energy[band]=psd(band);
        // Above the single-bin bands, each band's log-additions form one serial chain. Bands of
        // one width (3, 6, 12 or 24 bins) are independent, so their chains advance side by side.
        static constexpr unsigned widths[]{28,35,41,45,50};
        for (unsigned g=0;g<4;++g) {
            unsigned lo=std::max(firstBand,widths[g]),hi=std::min(lastBand,widths[g+1]),start[7],end[7],longest=0;
            int value[7];
            for (unsigned k=0;k+lo<hi;++k) {
                start[k]=std::max(ch.first,edges[lo+k]);end[k]=std::min(ch.end,edges[lo+k+1]);
                value[k]=psd(start[k]);longest=std::max(longest,end[k]-start[k]);
            }
            for (unsigned j=1;j<longest;++j) for (unsigned k=0;k+lo<hi;++k) if (start[k]+j<end[k]) {
                int other=psd(start[k]+j),distance=std::min(std::abs(value[k]-other)>>1,255);
                value[k]=std::max(value[k],other)+ac3data::logAdd[distance];
            }
            for (unsigned k=0;k+lo<hi;++k) energy[lo+k]=value[k];
        }
        int low=0,fgain=128*(ch.fastGain+1),fast=256*fastLeak+768,slow=256*slowLeak+768;
        auto lowpass=[&](unsigned band) {
            if (band>=20) low=std::max(0,low-128);
            else if (energy[band+1]==energy[band]+256) low=band<7?384:320;
            else if (energy[band]>energy[band+1]) low=std::max(0,low-64);
        };
        unsigned begin=firstBand;
        if (!firstBand) {
            for (unsigned band=0;band<2;++band) { lowpass(band);excitation[band]=energy[band]-fgain-low; }
            begin=7;
            for (unsigned band=2;band<7;++band) {
                if (lastBand!=7 || band!=6) lowpass(band);
                fast=energy[band]-fgain;slow=energy[band]-slowGain;excitation[band]=fast-low;
                if ((lastBand!=7 || band!=6) && energy[band]<=energy[band+1]) { begin=band+1;break; }
            }
        }
        for (unsigned band=begin;band<lastBand;++band) {
            fast=std::max(fast-fastDecay,energy[band]-fgain);
            slow=std::max(slow-slowDecay,energy[band]-slowGain);
            if (!firstBand && band<22) {
                if (lastBand!=7 || band!=6) lowpass(band);
                excitation[band]=std::max(fast-low,slow);
            } else excitation[band]=std::max(fast,slow);
        }
        const uint16_t* hearing=rateCode==0?ac3data::hearing0:rateCode==1?ac3data::hearing1:ac3data::hearing2;
        int snr=(int(coarse)*16+int(ch.fine)-240)*4;
        // A pointer is allocation[clamp((psd-mask)>>5,0,63)]. As 128*exponent is a multiple of 32,
        // the index is ((3072-mask)>>5)-4*exponent, so a wide band takes sixteen bins at a time.
        // allocation[] steps up at 1, 6, 8, 11 and 13, every fourth index from 15 to 47, and at
        // 55; a vector pointer counts the steps its index reaches. A vector may run into the next
        // band, which overwrites it, but never past the channel.
        const __m128i none=_mm_setzero_si128();
        auto reached=[](__m128i index,char step) { return _mm_cmpgt_epi8(index,_mm_set1_epi8(char(step-1))); };
        for (unsigned band=firstBand;band<lastBand;++band) {
            int excite=excitation[band]+std::max(0,knee-energy[band])/4;
            int mask=std::max(excite,int(hearing[band>>shift]));
            if (ch.deltaMode<2) mask+=ch.delta[band];
            mask=((std::max(0,mask-snr-floor)&0x1fe0)+floor);
            unsigned bin=std::max(ch.first,edges[band]),stop=std::min(ch.end,edges[band+1]);
            if (stop-bin>=12) {
                const __m128i base=_mm_set1_epi16((short)std::clamp((3072-mask)>>5,-1,160));
                for (;bin<stop && bin+16<=ch.end;bin+=16) {
                    __m128i e=_mm_loadu_si128((const __m128i*)(exponent+bin));
                    __m128i lo=_mm_sub_epi16(base,_mm_slli_epi16(_mm_unpacklo_epi8(e,none),2));
                    __m128i hi=_mm_sub_epi16(base,_mm_slli_epi16(_mm_unpackhi_epi8(e,none),2));
                    __m128i index=_mm_min_epu8(_mm_packus_epi16(lo,hi),_mm_set1_epi8(63));
                    __m128i quarters=_mm_and_si128(_mm_srli_epi16(_mm_subs_epu8(index,_mm_set1_epi8(15)),2),_mm_set1_epi8(0x3F));
                    __m128i pointer=_mm_min_epu8(quarters,_mm_set1_epi8(8));
                    pointer=_mm_sub_epi8(pointer,reached(index,1));pointer=_mm_sub_epi8(pointer,reached(index,6));
                    pointer=_mm_sub_epi8(pointer,reached(index,8));pointer=_mm_sub_epi8(pointer,reached(index,11));
                    pointer=_mm_sub_epi8(pointer,reached(index,13));pointer=_mm_sub_epi8(pointer,reached(index,15));
                    pointer=_mm_sub_epi8(pointer,reached(index,55));
                    _mm_storeu_si128((__m128i*)(ch.bap.data()+bin),pointer);
                }
            }
            for (;bin<stop;++bin) ch.bap[bin]=allocation[std::clamp((psd(bin)-mask)>>5,0,63)];
        }
    }
};
// Differential exponents. The first value is 4-bit (twice as wide for coupling); the rest
// arrive as groups of three base-5 digits packed into a 7-bit code.
// Each 7-bit code's three base-5 digits minus two, most significant first; codes 125 and up
// are invalid.
constexpr auto exponentDeltas=[] {
    std::array<std::array<int8_t,3>,128> table{};
    for (unsigned code=0;code<125;++code) table[code]={int8_t(code/25-2),int8_t(code/5%5-2),int8_t(code%5-2)};
    return table;
}();
bool Exponents(Bits& b,Channel& ch,bool coupling) {
    // A local cursor stays in registers across the exponent stores.
    Bits cursor=b;
    unsigned group=1u<<(ch.strategy-1),step=3*group,at=ch.first;
    int previous=(int)cursor.Get(4)*(coupling?2:1);
    if (!coupling) ch.exponent[at++]=(uint8_t)previous;
    unsigned groups=(ch.end-at+step-1)/step;
    // Groups past the 256 bins make the frame invalid.
    if (at+groups*step>ch.exponent.size()) return false;
    bool bad=false;
    uint8_t* out=ch.exponent.data()+at;
    // Each exponent covers 1, 2 or 4 bins: one store of that many copies.
    for (unsigned i=0;i<groups;++i) {
        unsigned code=(unsigned)cursor.Get(7);bad|=code>=125;
        for (int8_t delta:exponentDeltas[code]) {
            previous+=delta;bad|=unsigned(previous)>24;
            uint32_t copies=uint32_t(uint8_t(previous))*0x01010101u;
            if (group==1) *out=uint8_t(copies);
            else if (group==2) { uint16_t pair=uint16_t(copies);memcpy(out,&pair,2); }
            else memcpy(out,&copies,4);
            out+=group;
        }
    }
    b=cursor;
    return !bad && b.Good();
}
// Optional band-by-band delta adjustments applied to the bit allocation.
bool Delta(Bits& b,Channel& ch) {
    if (ch.deltaMode!=1) return ch.deltaMode<3;
    unsigned count=(unsigned)b.Get(3)+1,band=Band(ch.first);ch.delta.fill(0);
    for (unsigned i=0;i<count;++i) {
        band+=(unsigned)b.Get(5);unsigned length=(unsigned)b.Get(4),value=(unsigned)b.Get(3);
        if (band>50 || length>50-band) return false;
        int delta=(int(value)-(value>=4?3:4))*128;
        for (unsigned j=0;j<length;++j) ch.delta[band++]=delta;
    }
    return b.Good();
}
// Dequantized mantissas, (2*digit-(levels-1))/levels, for every code of the grouped
// (3, 5 and 11 levels; most significant digit first) and plain (7 and 15 levels) quantizers.
// The plain tables have a spare last entry, so an invalid code can be looked up harmlessly.
struct Mantissas { float three[27][3],five[125][3],eleven[121][2],seven[8],fifteen[16]; };
const Mantissas& MantissaTable() {
    static const Mantissas table=[] {
        Mantissas t{};
        auto value=[](unsigned digit,unsigned levels) { return (2.0f*digit-float(levels-1))/levels; };
        for (unsigned code=0;code<125;++code) for (unsigned i=0;i<3;++i) {
            if (code<27) t.three[code][i]=value(code/(i==0?9:i==1?3:1)%3,3);
            t.five[code][i]=value(code/(i==0?25:i==1?5:1)%5,5);
            if (code<121 && i<2) t.eleven[code][i]=value(code/(i==0?11:1)%11,11);
        }
        for (unsigned v=0;v<15;++v) { if (v<7) t.seven[v]=value(v,7); t.fifteen[v]=value(v,15); }
        return t;
    }();
    return table;
}
// Reads one block's quantized mantissas, channel by channel in `order` (bitstream order).
// Odd allocation pointers group several symbols into one codeword, and a group continues into
// the next channel; pointer 0 emits dither noise, or silence when dithering is off. The cursor,
// groups and noise state are locals, so they stay in registers. `data` is padded: a channel
// may read up to 512 bytes past `limit` before the next channel's check rejects the block.
bool ReadMantissas(const uint8_t* data,size_t& position,size_t limit,Channel* const* order,unsigned count,uint32_t& random) {
    const Mantissas& t=MantissaTable();
    size_t at=position;uint32_t r=random;
    const float* group[3]{};unsigned left[3]{};
    bool bad=false;
    auto get=[&](unsigned n) { unsigned v=unsigned((U64(data+(at>>3),true)<<(at&7))>>(64-n));at+=n;return v; };
    // A new grouped codeword when the previous one is used up; an invalid code fails the block.
    auto grouped=[&](unsigned g,unsigned width,unsigned capacity,unsigned size,const float* values) {
        if (!left[g]) {
            unsigned code=get(width);bad|=code>=capacity;
            group[g]=values+(code<capacity?code:0)*size;left[g]=size;
        }
        return group[g][size-left[g]--];
    };
    for (unsigned c=0;c<count;++c) {
        if (at>limit) return false;
        Channel& ch=*order[c];float* out=ch.spectrum.data();
        memset(out,0,ch.first*sizeof(float));memset(out+ch.end,0,(256-ch.end)*sizeof(float));
        for (unsigned bin=ch.first;bin<ch.end;++bin) {
            float v;
            switch (unsigned bap=ch.bap[bin]) {
                case 0: {
                    // A run of unallocated bins (often the whole top of the band) is noise or
                    // silence; filling it in one loop skips the per-bin dispatch.
                    unsigned end=bin+1;
                    while (end<ch.end && !ch.bap[end]) ++end;
                    if (ch.dither) for (;bin<end;++bin) out[bin]=Scale(Noise(r),ch.exponent[bin]);
                    else for (;bin<end;++bin) out[bin]=0;
                    --bin;
                    continue;
                }
                case 1: v=grouped(0,5,27,3,t.three[0]);break;
                case 2: v=grouped(1,7,125,3,t.five[0]);break;
                case 4: v=grouped(2,7,121,2,t.eleven[0]);break;
                case 3: { unsigned code=get(3);bad|=code>=7;v=t.seven[code];break; }
                case 5: { unsigned code=get(4);bad|=code>=15;v=t.fifteen[code];break; }
                default: {
                    unsigned width=bap==14?14:bap==15?16:bap-1;
                    v=Scale((float)(int32_t(get(width)<<(32-width))>>(32-width)),width-1);
                }
            }
            out[bin]=Scale(v,ch.exponent[bin]);
        }
    }
    position=at;random=r;
    return !bad && at<=limit;
}

class Ac3Decoder final : public PacketDecoder {
public:
    bool Open(const std::wstring& path) {
        if (!media_.file.Open(path)) return false;
        if (media_.IsMatroska()) {
            if (!media_.OpenMatroska(path) || media_.codec!=AudioAc3) return false;
        } else {
        uint64_t at=0;
        const uint8_t* p=media_.file.View(0,10);if (!p) return false;
        if (!memcmp(p,"ID3",3)) {
            if ((p[6]|p[7]|p[8]|p[9])&128) return false;
            at=10+((uint64_t)p[6]<<21)+((uint64_t)p[7]<<14)+((uint64_t)p[8]<<7)+p[9]+((p[5]&16)?10:0);
        }
        // Walk syncframes; each must decode fully and share one sample rate.
        while (at+7<=media_.file.Size()) {
            p=media_.file.View(at,7);if (!p) return false;
            Header h=Inspect(p,7);
            if (!h.bytes || h.bytes>media_.file.Size()-at || (media_.rate && h.rate!=media_.rate)) return false;
            media_.rate=h.rate;media_.packets.push_back({at,media_.frames,h.bytes,1536});media_.frames+=1536;at+=h.bytes;
            if (media_.packets.size()>8000000) return false;
        }
        if (at!=media_.file.Size() || media_.packets.empty()) return false;
        }
        transform_[0].Init(512);transform_[1].Init(256);(void)MantissaTable();
        // Kaiser-Bessel derived window, alpha=5. Derive full precision values at setup.
        double total=0,cumulative[256]{};
        for (unsigned i=0;i<=256;++i) {
            double x=2.0*i/256-1,z=std::numbers::pi*5*std::sqrt(std::max(0.0,1-x*x));
            double sum=1,term=1;
            for (unsigned k=1;k<40;++k) { term*=z*z/(4.0*k*k);sum+=term;if (term<sum*1e-16) break; }
            total+=sum;if (i<256) cumulative[i]=total;
        }
        for (unsigned i=0;i<256;++i) window_[i]=(float)std::sqrt(cumulative[i]/total);
        return Ready(media_.rate);
    }
private:
    void Reset() override { for (auto& ch:channel_) ch.overlap.fill(0); }
    size_t Preroll() const override { return 1; }
    // Inverse filterbank. Unswitched channels use one 512-point transform; block-switched
    // channels interleave two 256-point transforms to rebuild the alternating windows.
    // Either way the 512-sample block is (first[0..127], -first[127..0], -tail[127..0],
    // -tail[0..127]); the windowing reads those halves in place of building the block.
    void Synthesize(Channel& ch,float* pcm) {
        float dct[256],second[128];
        const float* first=dct;
        if (!ch.switched) { transform_[0].Dct(ch.spectrum.data(),dct);first=dct+128; }
        else {
            float input[128];
            for (unsigned i=0;i<128;++i) input[i]=ch.spectrum[i*2];
            transform_[1].Dct(input,dct);
            for (unsigned i=0;i<128;++i) input[i]=ch.spectrum[i*2+1];
            transform_[1].Dct(input,second);
        }
        const float* tail=ch.switched?second:dct;
        const float* w=window_.data();float* o=ch.overlap.data();
        auto reversed=[](const float* p) { __m128 v=_mm_loadu_ps(p);return _mm_shuffle_ps(v,v,0x1B); };
        const __m128 minus2=_mm_set1_ps(-2.0f),sign=_mm_set1_ps(-0.0f);
        for (unsigned i=0;i<128;i+=4)
            _mm_storeu_ps(pcm+i,_mm_mul_ps(minus2,_mm_add_ps(_mm_loadu_ps(o+i),_mm_mul_ps(_mm_loadu_ps(first+i),_mm_loadu_ps(w+i)))));
        for (unsigned i=128;i<256;i+=4)
            _mm_storeu_ps(pcm+i,_mm_mul_ps(minus2,_mm_sub_ps(_mm_loadu_ps(o+i),_mm_mul_ps(reversed(first+252-i),_mm_loadu_ps(w+i)))));
        for (unsigned i=0;i<128;i+=4)
            _mm_storeu_ps(o+i,_mm_xor_ps(sign,_mm_mul_ps(reversed(tail+124-i),reversed(w+252-i))));
        for (unsigned i=128;i<256;i+=4)
            _mm_storeu_ps(o+i,_mm_xor_ps(sign,_mm_mul_ps(_mm_loadu_ps(tail+i-128),reversed(w+252-i))));
    }
    // Each syncframe carries six 256-sample blocks. Parse per-block side info, derive the
    // masking curves, read mantissas, then combine couplers, matrix and downmix to stereo.
    bool Decode(const uint8_t* p,size_t bytes,std::vector<float>& out) override {
        Header h=Inspect(p,bytes);if (h.bytes!=bytes || h.rate!=rate_ || !ValidCrc(p,bytes)) return false;
        // At most 3,840 bytes; the copy gives the mantissa reader its padding.
        memcpy(frame_.data(),p,bytes);
        Bits b(p,bytes-2);b.Skip(48);unsigned mode=(unsigned)b.Get(3),channels=channelCount[mode];
        unsigned center=(mode&1) && mode!=1?(unsigned)b.Get(2):0,surround=mode&4?(unsigned)b.Get(2):0;
        if (mode==2) b.Skip(2);
        bool lfe=b.Get(1)!=0;
        for (unsigned i=0;i<(mode==0?2u:1u);++i) {
            b.Skip(5);if (b.Get(1)) b.Skip(8);if (b.Get(1)) b.Skip(8);if (b.Get(1)) b.Skip(7);
        }
        b.Skip(2);for (int i=0;i<2;++i) if (b.Get(1)) b.Skip(14);
        if (b.Get(1)) { size_t length=(size_t)b.Get(6)+1;b.Skip(length*8); }
        if (!b.Good()) return false;
        for (auto& ch:channel_) { ch.first=ch.end=0;ch.coupled=ch.coordinates=false;ch.deltaMode=2;ch.delta.fill(0);ch.stale=true; }
        auto invalidate=[&] { for (auto& ch:channel_) ch.stale=true; };
        bool wasZero=false;
        Allocation params;params.rateCode=h.sampleRateCode;params.shift=h.shift;
        bool coupling=false,phase=false;unsigned cplStart=0,cplEnd=0,bands=0,bandForSub[18]{};bool rematrix[4]{};
        // Dither depends only on the frame, so a seek reproduces it. The frame's two CRC words
        // already summarize its content.
        uint32_t random=uint32_t(p[2])<<24|uint32_t(p[3])<<16|uint32_t(p[bytes-2])<<8|p[bytes-1];
        out.resize(3072);
        for (unsigned block=0;block<6;++block) {
            for (unsigned c=0;c<channels;++c) channel_[c].switched=b.Get(1)!=0;
#ifdef CORDINAL_CODEC_TRACE
            fprintf(stderr,"ac3dither %zu %u\n",b.Position(),channels);
#endif
            for (unsigned c=0;c<channels;++c) channel_[c].dither=b.Get(1)!=0;
            if (b.Get(1)) b.Skip(8);if (mode==0 && b.Get(1)) b.Skip(8);
            bool strategy=b.Get(1)!=0;if (!block && !strategy) return false;
            if (strategy) {
                invalidate();
                bool previous=coupling;coupling=b.Get(1)!=0;
                unsigned coupled=0;
                for (unsigned c=0;c<channels;++c) {
                    bool was=channel_[c].coupled;channel_[c].coupled=coupling && b.Get(1);
                    if (!was || !previous) channel_[c].coordinates=false;
                    coupled+=channel_[c].coupled;
                }
                if (coupling) {
                    if (coupled<2 || mode<2) return false;
                    phase=mode==2 && b.Get(1);
                    unsigned first=(unsigned)b.Get(4),last=(unsigned)b.Get(4)+3;
                    if (first>=last || last>18) return false;
                    cplStart=37+first*12;cplEnd=37+last*12;bands=1;
                    bandForSub[0]=0;
                    for (unsigned sub=1;sub<last-first;++sub) { if (!b.Get(1)) ++bands;bandForSub[sub]=bands-1; }
                    channel_[6].first=cplStart;channel_[6].end=cplEnd;
                }
            }
            // Coupling: each coupled channel stores coordinates for the shared high band.
            if (coupling) {
                bool updated=false;
                for (unsigned c=0;c<channels;++c) if (channel_[c].coupled) {
                    auto& ch=channel_[c];bool exists=b.Get(1)!=0;
                    if (!exists && !ch.coordinates) return false;
                    if (exists) {
                        unsigned master=(unsigned)b.Get(2)*3;updated=true;ch.coordinates=true;
                        for (unsigned band=0;band<bands;++band) {
                            unsigned exponent=(unsigned)b.Get(4),mantissa=(unsigned)b.Get(4);
                            ch.coordinate[band]=(float)std::ldexp(exponent==15?mantissa/16.0:(mantissa+16)/32.0,3-int(exponent+master));
                        }
                    }
                }
                if (mode==2 && phase && updated) for (unsigned band=0;band<bands;++band) {
                    float magnitude=std::abs(channel_[1].coordinate[band]);channel_[1].coordinate[band]=b.Get(1)?-magnitude:magnitude;
                }
            }
            // Stereo rematrix flags select which frequency bands are summed/differenced.
            if (mode==2) {
                bool exists=b.Get(1)!=0;if (!block && !exists) return false;
                if (exists) {
                    unsigned count=!coupling || cplStart>61?4:cplStart>37?3:2;
                    for (unsigned band=0;band<count;++band) rematrix[band]=b.Get(1)!=0;
                    for (unsigned band=count;band<4;++band) rematrix[band]=false;
                }
            }
            if (coupling) channel_[6].strategy=(unsigned)b.Get(2);
            for (unsigned c=0;c<channels;++c) channel_[c].strategy=(unsigned)b.Get(2);
            if (lfe) { channel_[5].strategy=(unsigned)b.Get(1);channel_[5].end=7;channel_[5].switched=false;channel_[5].dither=false; }
            for (unsigned c=0;c<channels;++c) if (channel_[c].strategy) {
                unsigned bandwidth=channel_[c].coupled?0:(unsigned)b.Get(6);if (bandwidth>60) return false;
                channel_[c].end=channel_[c].coupled?cplStart:73+3*bandwidth;
            }
            if (coupling) {
                if (!block && !channel_[6].strategy) return false;
                if (channel_[6].strategy) { channel_[6].stale=true;if (!Exponents(b,channel_[6],true)) return false; }
            }
            for (unsigned c=0;c<channels;++c) {
                auto& ch=channel_[c];if ((!block && !ch.strategy) || !ch.end) return false;
                if (ch.strategy) { ch.stale=true;if (!Exponents(b,ch,false)) return false;b.Skip(2); }
            }
            if (lfe) {
                if (!block && !channel_[5].strategy) return false;
                if (channel_[5].strategy) { channel_[5].stale=true;if (!Exponents(b,channel_[5],false)) return false; }
            }
            // New allocation parameters, SNR offsets, leak values or deltas change every
            // channel's masking curve; otherwise reused exponents keep their pointers.
            bool parameters=b.Get(1)!=0;if (!block && !parameters) return false;if (parameters) { params.Parameters(b);invalidate(); }
            bool snr=b.Get(1)!=0;if (!block && !snr) return false;
            auto snrChannel=[&](unsigned c) { channel_[c].fine=(unsigned)b.Get(4);channel_[c].fastGain=(unsigned)b.Get(3); };
            if (snr) { params.coarse=(unsigned)b.Get(6);if (coupling) snrChannel(6);for (unsigned c=0;c<channels;++c) snrChannel(c);if (lfe) snrChannel(5);invalidate(); }
            if (coupling) {
                bool leak=b.Get(1)!=0;if (!block && !leak) return false;
                if (leak) { params.fastLeak=(unsigned)b.Get(3);params.slowLeak=(unsigned)b.Get(3);invalidate(); }
            }
            if (b.Get(1)) {
                invalidate();
                if (coupling) channel_[6].deltaMode=(unsigned)b.Get(2);
                for (unsigned c=0;c<channels;++c) channel_[c].deltaMode=(unsigned)b.Get(2);
                if (coupling && !Delta(b,channel_[6])) return false;
                for (unsigned c=0;c<channels;++c) if (!Delta(b,channel_[c])) return false;
            }
            if (b.Get(1)) { size_t length=(size_t)b.Get(9);b.Skip(length*8); }
            if (!b.Good()) return false;
            bool zero=params.coarse==0;
            for (unsigned c=0;c<channels;++c) zero=zero && !channel_[c].fine;
            if (coupling) zero=zero && !channel_[6].fine;if (lfe) zero=zero && !channel_[5].fine;
            if (zero!=wasZero) { invalidate();wasZero=zero; }
            auto allocate=[&](Channel& ch) { if (ch.stale) { params.Compute(ch,zero);ch.stale=false; } };
            if (coupling) allocate(channel_[6]);
            for (unsigned c=0;c<channels;++c) allocate(channel_[c]);
            if (lfe) allocate(channel_[5]);
            // Mantissas follow in channel order; the coupling channel's come after the first
            // coupled channel's, and the LFE's last.
            Channel* order[7];unsigned count=0;bool readCoupling=false;
            for (unsigned c=0;c<channels;++c) {
                order[count++]=&channel_[c];
                if (channel_[c].coupled && !readCoupling) { order[count++]=&channel_[6];readCoupling=true; }
            }
            if (lfe) order[count++]=&channel_[5];
            size_t at=b.Position();
            if (!ReadMantissas(frame_.data(),at,(bytes-2)*8,order,count,random)) return false;
            b.Skip(at-b.Position());
            if (!b.Good()) return false;
            if (coupling) for (unsigned c=0;c<channels;++c) if (channel_[c].coupled) {
                auto& ch=channel_[c];const auto& coupled=channel_[6];
                // Coupling sub-bands are 12 bins wide.
                for (unsigned bin=cplStart,sub=0;bin<cplEnd;bin+=12,++sub) {
                    const __m128 coordinate=_mm_set1_ps(ch.coordinate[bandForSub[sub]]);
                    for (unsigned i=bin;i<bin+12;i+=4)
                        _mm_storeu_ps(&ch.spectrum[i],_mm_mul_ps(_mm_loadu_ps(&coupled.spectrum[i]),coordinate));
                }
                // With dither on, this channel draws its own noise for the unallocated coupled bins.
                if (ch.dither) for (unsigned bin=cplStart,sub=0;bin<cplEnd;bin+=12,++sub) {
                    const float coordinate=ch.coordinate[bandForSub[sub]];
                    for (unsigned i=bin;i<bin+12;++i)
                        if (!coupled.bap[i]) ch.spectrum[i]=Scale(Noise(random),coupled.exponent[i])*coordinate;
                }
            }
            if (mode==2) {
                constexpr unsigned boundaries[]{13,25,37,61,253};unsigned end=std::min(channel_[0].end,channel_[1].end);
                float* l=channel_[0].spectrum.data();float* r=channel_[1].spectrum.data();
                for (unsigned band=0;band<4;++band) if (rematrix[band]) {
                    unsigned bin=boundaries[band],stop=std::min(end,boundaries[band+1]);
                    for (;bin+4<=stop;bin+=4) {
                        __m128 a=_mm_loadu_ps(l+bin),v=_mm_loadu_ps(r+bin);
                        _mm_storeu_ps(l+bin,_mm_add_ps(a,v));_mm_storeu_ps(r+bin,_mm_sub_ps(a,v));
                    }
                    for (;bin<stop;++bin) { float a=l[bin],v=r[bin];l[bin]=a+v;r[bin]=a-v; }
                }
            }
            // Every channel the downmix reads is synthesized first, so no clearing is needed.
            std::array<std::array<float,256>,5> pcm;
            for (unsigned c=0;c<channels;++c) Synthesize(channel_[c],pcm[c].data());
            // Center/surround mix levels plus a normalizing gain keep the stereo sum in range.
            constexpr float centers[]{0.70710678f,0.59460356f,0.5f,0.59460356f},surrounds[]{0.70710678f,0.5f,0,0.5f};
            bool front=(mode&1) && mode!=1,rear=mode>=4;
            float cg=front?centers[center]:0,sg=rear?surrounds[surround]*((mode==4 || mode==5)?0.70710678f:1):0;
            float gain=1/(1+cg+sg);
            float* o=out.data()+block*512;
            if (!front && !rear) {
                // Plain stereo or mono: interleave, with the gain applied and the finite check
                // made on the way.
                const float* l=pcm[0].data();const float* r=pcm[mode==1?0:1].data();
                const __m128 g=_mm_set1_ps(gain);
                const __m128i exponent=_mm_set1_epi32(0x7F800000);
                __m128i bad=_mm_setzero_si128();
                for (unsigned i=0;i<256;i+=4) {
                    __m128 x=_mm_mul_ps(_mm_loadu_ps(l+i),g),y=_mm_mul_ps(_mm_loadu_ps(r+i),g);
                    bad=_mm_or_si128(bad,_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(x),exponent),exponent));
                    bad=_mm_or_si128(bad,_mm_cmpeq_epi32(_mm_and_si128(_mm_castps_si128(y),exponent),exponent));
                    _mm_storeu_ps(o+i*2,_mm_unpacklo_ps(x,y));_mm_storeu_ps(o+i*2+4,_mm_unpackhi_ps(x,y));
                }
                if (_mm_movemask_epi8(bad)) return false;
                continue;
            } else for (unsigned i=0;i<256;++i) {
                float left=pcm[0][i],right=pcm[mode==1?0:front?2:1][i];
                if (front) { left+=cg*pcm[1][i];right+=cg*pcm[1][i]; }
                if (rear) { unsigned s=front?3:2;left+=sg*pcm[s][i];right+=sg*pcm[s+(mode>=6?1:0)][i]; }
                o[i*2]=left*gain;o[i*2+1]=right*gain;
            }
            // The gain is at most one, so a non-finite input sample stays non-finite here.
            if (!AllFinite(o,512)) return false;
        }
        return b.Good();
    }
    Channel channel_[7];Mdct transform_[2];std::array<float,256> window_{};
    std::array<uint8_t,3840+520> frame_{};
};
}
std::unique_ptr<Decoder> OpenAc3Decoder(const std::wstring& path) {
    auto decoder=std::make_unique<Ac3Decoder>();if (!decoder->Open(path)) return nullptr;return decoder;
}
