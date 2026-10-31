// Cordinal's WavPack 4/5 PCM decoder. Independent blocks, adaptive Rice intervals,
// integer decorrelation and extended integer/IEEE reconstruction.
// Hybrid/lossy blocks are supported and multichannel groups are downmixed to stereo.
#include "native_audio.h"
#include "audio_file.h"
#include "audio_kernels.h"
#include "audio_lsb.h"
#include <array>
#include <bit>
#include <cmath>
#include <span>
#ifdef CORDINAL_CODEC_TRACE
#include <cstdio>
#endif

namespace {
using namespace nativeaudio;
using Bytes=std::span<const uint8_t>;
// Block flags used by this decoder; the rest of the 32-bit flag word is layout metadata.
constexpr uint32_t mono=4,hybrid=8,joint=16,floating=128,extended=256,falseStereo=0x40000000;
constexpr unsigned rates[]{6000,8000,9600,11025,12000,16000,22050,24000,32000,44100,48000,64000,88200,96000,192000};
// log2/exp lookup pairs that recreate WavPack's fixed-point log/exponent transforms.
struct Logs {
    std::array<uint8_t,256> logarithm{},exponent{};
    Logs() { for (unsigned i=0;i<256;++i) {
        logarithm[i]=(uint8_t)std::lround(256*std::log2(1+i/256.0));
        exponent[i]=(uint8_t)std::lround(256*(std::exp2(i/256.0)-1));
    } }
};
const Logs& Tables() { static const Logs logs;return logs; }
uint32_t Exponent(unsigned log) {
    uint32_t value=256|Tables().exponent[log&255];unsigned power=log>>8;
    return power<=9?value>>(9-power):value<<((power-9)&31);
}
int32_t SignedExponent(int log) { return (int32_t)(log<0?0u-Exponent((unsigned)-log):Exponent(log)); }
unsigned Logarithm(uint32_t value) {
    value+=value>>9;unsigned width=32-std::countl_zero(value);
    unsigned fraction=width<9?(value<<(9-width))&255:(value>>(width-9))&255;
    return width*256+Tables().logarithm[fraction];
}
// Weights are stored as signed bytes scaled by 8; Product applies the >>10 fixed point.
int Weight(int8_t v) { int w=v*8;return w>0?w+((w+64)>>7):w; }
int32_t Product(int weight,int32_t value) { return (int32_t)((int64_t(weight)*value+512)>>10); }
// Nudge a weight toward (or away from) the source sign; `clip` bounds the negative terms.
// The signs are data, so the step is selected arithmetically rather than by branches.
void Adapt(int& weight,int32_t source,int32_t residual,unsigned step,bool clip) {
    int direction=((source^residual)>>31)|1;
    weight+=source && residual?direction*(int)step:0;
    if (clip) weight=std::clamp(weight,-1024,1024);
}

struct Header {
    uint64_t index=0,total=0;
    uint32_t bytes=0,samples=0,flags=0,crc=0;
    unsigned version=0;
    // Validate the 32-byte 'wvpk' block header; bytes is the in-file size including the header.
    bool Read(const uint8_t* p) {
        if (!p || !Is(p,"wvpk")) return false;
        bytes=U32(p+4)+8;version=U16(p+8);samples=U32(p+20);flags=U32(p+24);crc=U32(p+28);
        index=U32(p+16)|(uint64_t(p[10])<<32);
        total=U32(p+12)|(uint64_t(p[11])<<32);
        return bytes>=32 && bytes<=16*1024*1024 && !(bytes&1) && version>=0x402 && version<=0x410 && samples<=131072 && !(flags&0x80000000);
    }
    unsigned Channels() const { return flags&mono?1:2; }
    unsigned Coded() const { return flags&(mono|falseStereo)?1:2; }
};
// Split a block into its length-tagged metadata items; id bit 5 marks odd size, bit 6 an
// optional item, and key 0x2f is the block checksum.
struct Metadata {
    std::array<Bytes,64> item{};
    bool Read(Bytes block,uint32_t flags) {
        bool checksum=false;
        for (size_t at=32;at<block.size();) {
            size_t begin=at;
            if (block.size()-at<2) return false;
            unsigned id=block[at++];size_t words=block[at++];
            if (id&128) { if (block.size()-at<2) return false;words|=size_t(block[at])<<8|size_t(block[at+1])<<16;at+=2; }
            size_t padded=words*2;
            if (padded>block.size()-at || (!padded && (id&64))) return false;
            Bytes data=block.subspan(at,padded-((id>>6)&1));
            unsigned key=id&63;
            if (key==0x2f) {
                if ((id&64) || (data.size()!=2 && data.size()!=4)) return false;
                uint32_t sum=UINT32_MAX;
                for (size_t i=0;i<begin;i+=2) sum=sum*3+U16(block.data()+i);
                if (data.size()==2) { sum^=sum>>16;if ((uint16_t)sum!=U16(data.data())) return false; }
                else if (sum!=U32(data.data())) return false;
                checksum=true;
            }
            if (!(id&32) && key!=0 && key!=2 && key!=3 && key!=4 && key!=5 && key!=6 && key!=7 && key!=8 && key!=9 && key!=10 && key!=11 && key!=12 && key!=13 && key!=0x1f) return false;
            if (!item[key].empty()) return false;
            item[key]=data;at+=padded;
        }
        return !(flags&0x10000000) || checksum;
    }
};
// One decorrelation pass: a term (tap pattern), adaptive weight and the sample memory ring.
struct Pass {
    int term=0,weight[2]{};
    unsigned step=0;
    int32_t memory[2][8]{};
};
// Adaptive Rice entropy coder: per-channel medians, the zero-run suppression state, and
// the slow/error limit used by hybrid blocks.
struct Entropy {
    struct Channel { uint32_t median[3]{},slow=0,error=0; } ch[2];
    uint32_t flags=0,accumulator[2]{},delta[2]{},zeros=0;
    unsigned holdOne=0,holdZero=0;
    bool Setup(const Header& h,const Metadata& meta) {
        flags=h.flags;Bytes data=meta.item[5];unsigned count=h.Coded();
        if (data.size()!=count*6) return false;
        for (unsigned c=0;c<count;++c) for (unsigned i=0;i<3;++i) {
            unsigned value=U16(data.data()+c*6+i*2);if (value>8447) return false;
            ch[c].median[i]=Exponent(value);
        }
        if (!(flags&hybrid)) return true;
        data=meta.item[6];size_t at=0;
        auto take=[&] { if (data.size()-at<2) { at=data.size()+1;return 0u; } unsigned v=U16(data.data()+at);at+=2;return v; };
        if (flags&512) for (unsigned c=0;c<count;++c) { unsigned v=take();if (v>8447) return false;ch[c].slow=Exponent(v); }
        for (unsigned c=0;c<count;++c) accumulator[c]=take()<<16;
        if (at<data.size()) for (unsigned c=0;c<count;++c) delta[c]=(uint32_t)SignedExponent((int16_t)take());
        return at==data.size();
    }
    uint32_t Variable(LittleBits& bits) {
        unsigned width=bits.Ones(33);
        if (width>32) { bits.Fail();return 0; }
        return width<2?width:bits.Get(width-1)|(1u<<(width-1));
    }
    uint32_t Truncated(LittleBits& bits,uint32_t maximum) {
        if (maximum<2) return bits.Get(maximum);
        unsigned width=32-std::countl_zero(maximum);
        // Values below the cutoff take width-1 bits and the rest one more. Which applies is
        // data, so peek both and select without a branch.
        uint32_t cutoff=(uint32_t)((uint64_t(1)<<width)-maximum-1),window=bits.Peek(width);
        uint32_t value=window&((1u<<(width-1))-1),big=value>=cutoff;
        bits.Skip(width-1+big);
        return big?value*2-cutoff+(window>>(width-1)):value;
    }
    void ErrorLimit(unsigned count) {
        int target[2]{};
        for (unsigned c=0;c<count;++c) { accumulator[c]+=delta[c];target[c]=(int)(accumulator[c]>>16); }
        if ((flags&512) && count==2 && (flags&1024)) {
            int balance=((int)((ch[1].slow+128)>>8)-(int)((ch[0].slow+128)>>8)+target[1]+1)>>1;
            balance=std::clamp(balance,-target[0],target[0]);
            target[1]=target[0]+balance;target[0]-=balance;
        }
        for (unsigned c=0;c<count;++c) {
            int log=flags&512?(int)((ch[c].slow+128)>>8)-target[c]+256:target[c];
            ch[c].error=log>0?Exponent((unsigned)std::min(log,8447)):0;
        }
    }
    // Decode one residual. Medians select the magnitude range; the value is read either
    // truncated or by binary search against the error limit.
    int32_t Word(LittleBits& bits,unsigned channel,unsigned count) {
        bits.Prepare();
        auto& c=ch[channel];
        if (ch[0].median[0]<2 && ch[1].median[0]<2 && !holdOne && !holdZero) {
            if (zeros) {
                if (--zeros) { c.slow-=(c.slow+128)>>8;return 0; }
            } else {
                zeros=Variable(bits);
                if (zeros) {
                    for (auto& state:ch) for (auto& m:state.median) m=0;
                    c.slow-=(c.slow+128)>>8;return 0;
                }
            }
        }
        uint32_t category=0;
        if (holdZero) holdZero=0;
        else {
            uint32_t ones=bits.Ones(17);
            if (ones==16) ones+=Variable(bits);
            category=(ones>>1)+holdOne;holdOne=ones&1;holdZero=1-holdOne;
        }
        if ((flags&hybrid) && !channel) ErrorLimit(count);
        uint32_t low=0,high=0;
        for (unsigned m=0;m<3;++m) {
            unsigned shift=7-m;uint32_t span=(c.median[m]>>4)+1,divisor=1u<<shift;
            if (category==m) {
                high=low+span-1;c.median[m]-=((c.median[m]+divisor-2)>>shift)*2;break;
            }
            c.median[m]+=((c.median[m]+divisor)>>shift)*5;
            if (m==2) { low+=(category-2)*span;high=low+span-1; }
            else low+=span;
        }
        low&=0x7fffffff;high=std::max(low,high&0x7fffffff);
        uint32_t value=(uint32_t)((uint64_t(high)+low+1)/2);
        if (!c.error) value=low+Truncated(bits,high-low);
        else while (high-low>c.error) {
            if (bits.Get(1)) low=value;else high=value-1;
            value=(uint32_t)((uint64_t(high)+low+1)/2);
        }
        // A set sign bit stores the ones' complement; the bit is random, so flip arithmetically.
        uint32_t sign=0u-bits.Get(1);
        if (flags&512) { c.slow-=(c.slow+128)>>8;c.slow+=Logarithm(value); }
        return (int32_t)(value^sign);
    }
};

// Terms, weights and sample history for every decorrelation pass, stored in reverse order.
bool ReadPasses(const Header& h,const Metadata& meta,std::vector<Pass>& passes) {
    Bytes terms=meta.item[2];if (terms.size()>16) return false;
    unsigned count=h.Coded();passes.resize(terms.size());
    for (unsigned i=0;i<terms.size();++i) {
        auto& pass=passes[i];unsigned value=terms[terms.size()-i-1];
        pass.term=(value&31)-5;pass.step=value>>5;
        if (!pass.term || pass.term< -3 || (pass.term>8 && pass.term<17) || pass.term>18 || (count==1 && pass.term<0)) return false;
    }
    Bytes weights=meta.item[3];if (weights.size()%count || weights.size()/count>passes.size()) return false;
    for (unsigned i=0;i<weights.size()/count;++i) for (unsigned c=0;c<count;++c)
        passes[passes.size()-i-1].weight[c]=Weight((int8_t)weights[i*count+c]);
    Bytes data=meta.item[4];size_t at=0;
    if (h.version==0x402 && (h.flags&hybrid)) at=count*2;
    for (size_t i=passes.size();i && at<data.size();) {
        auto& pass=passes[--i];
        auto take=[&](int32_t& value) {
            if (at>data.size() || data.size()-at<2) return false;
            int log=(int16_t)U16(data.data()+at);at+=2;
            if (std::abs(log)>8447) return false;
            value=SignedExponent(log);return true;
        };
        if (pass.term>=17) for (unsigned c=0;c<count;++c) { if (!take(pass.memory[c][0]) || !take(pass.memory[c][1])) return false; }
        else if (pass.term<0) { if (!take(pass.memory[0][0]) || !take(pass.memory[1][0])) return false; }
        else for (unsigned j=0;j<(unsigned)pass.term;++j) for (unsigned c=0;c<count;++c) if (!take(pass.memory[c][j])) return false;
    }
    return at==data.size();
}

// Positive terms keep each channel to itself; a stereo block runs both channels in one loop
// so their dependency chains overlap.
template<unsigned Channels> void Predict(int32_t* v,size_t frames,Pass& pass) {
    // Locals, since the sample stores could otherwise alias the pass's fields.
    int weight[Channels];
    for (unsigned c=0;c<Channels;++c) weight[c]=pass.weight[c];
    const unsigned step=pass.step;
    const int kind=pass.term;
    auto apply=[&](size_t i,unsigned c,int32_t source) {
        int32_t residual=v[i*Channels+c];
        v[i*Channels+c]=(int32_t)(uint32_t(residual)+uint32_t(Product(weight[c],source)));
        Adapt(weight[c],source,residual,step,false);
    };
    if (kind<=8) {
        const size_t term=(size_t)kind,head=std::min(term,frames);
        for (size_t i=0;i<head;++i) for (unsigned c=0;c<Channels;++c) apply(i,c,pass.memory[c][i]);
        for (size_t i=term;i<frames;++i) for (unsigned c=0;c<Channels;++c) apply(i,c,v[(i-term)*Channels+c]);
    } else {
        int32_t last[Channels],before[Channels];
        for (unsigned c=0;c<Channels;++c) { last[c]=pass.memory[c][0];before[c]=pass.memory[c][1]; }
        for (size_t i=0;i<frames;++i) for (unsigned c=0;c<Channels;++c) {
            int32_t source=kind==17?(int32_t)(uint32_t(last[c])*2-uint32_t(before[c])):
                (int32_t)(uint32_t(last[c])+uint32_t((int32_t)(uint32_t(last[c])-uint32_t(before[c]))>>1));
            apply(i,c,source);
            before[c]=last[c];last[c]=v[i*Channels+c];
        }
    }
    for (unsigned c=0;c<Channels;++c) pass.weight[c]=weight[c];
}
// Undo each decorrelation pass. Blocks are independent and a pass's history ends with the
// block, so each pass runs over the block on its own: terms 1-8 predict from the value
// `term` samples back (the stored history before the block starts), 17 and 18 extrapolate
// the last two values, and the negative terms mix the two channels of a stereo block.
void Decorrelate(std::vector<int32_t>& samples,unsigned channels,std::vector<Pass>& passes) {
    const size_t frames=samples.size()/channels;
    const bool fast=FastAudioKernels();
    for (auto& pass:passes) {
        if (pass.term>0) {
            if (channels==2 && fast) WavPackPredictAvx2(samples.data(),frames,pass.term,pass.weight,(int)pass.step,pass.memory);
            else if (channels==2) Predict<2>(samples.data(),frames,pass);
            else Predict<1>(samples.data(),frames,pass);
            continue;
        }
        for (size_t i=0;i<frames;++i) {
            int32_t* value=samples.data()+i*channels;
            unsigned first=pass.term== -2?1:0,second=1-first;
            int32_t source=pass.memory[first][0],residual=value[first];
            value[first]=(int32_t)(uint32_t(residual)+uint32_t(Product(pass.weight[first],source)));
            Adapt(pass.weight[first],source,residual,pass.step,true);
            source=pass.term== -3?pass.memory[second][0]:value[first];residual=value[second];
            value[second]=(int32_t)(uint32_t(residual)+uint32_t(Product(pass.weight[second],source)));
            Adapt(pass.weight[second],source,residual,pass.step,true);
            if (pass.term== -3) { pass.memory[0][0]=value[1];pass.memory[1][0]=value[0]; }
            else pass.memory[first][0]=value[second];
        }
    }
}

// Turn decorrelated values into PCM. Floating blocks rebuild IEEE bit patterns (and their
// CRC); integer blocks restore sent bits, zero/one/duplicate runs and the hybrid shift.
bool Reconstruct(const Header& h,const Metadata& meta,std::vector<int32_t>& values,std::vector<float>& result) {
    Bytes x=meta.item[12].empty()?meta.item[31]:meta.item[12];
    if (!x.empty() && x.size()<4) return false;
    LittleBits extra(x.empty()?Bytes{}:x.subspan(4));
    bool extraPresent=!x.empty(),modern=!meta.item[31].empty();
    uint32_t crc=UINT32_MAX;
    result.resize(values.size());
    if (h.flags&floating) {
        Bytes info=meta.item[8];if (info.size()!=4) return false;
        unsigned mode=info[0],shift=info[1],maxExponent=info[2];
        if (shift>31) return false;
        unsigned minimumZeros=modern?extra.Get(5):0,maximumOnes=modern?extra.Get(5):0;
        for (size_t i=0;i<values.size();++i) {
            uint32_t bits=0,raw=(uint32_t)values[i];unsigned exponent=maxExponent;
            if (!raw) {
                if (extraPresent && (mode&8)) {
                    if (extra.Get(1)) {
                        bits=extra.Get(23);
                        if (exponent>=25) bits|=extra.Get(8)<<23;
                        bits|=extra.Get(1)<<31;
                    } else if (mode&16) bits=extra.Get(1)<<31;
                }
            } else {
                raw<<=shift;
                uint32_t sign=(raw>>31)<<31;if (sign) raw=0u-raw;
                unsigned missing=0;
                if (raw==0x1000000 && extraPresent) {
                    bits=sign|0x7f800000;if (extra.Get(1)) bits|=extra.Get(23);
                } else {
                    if (raw>=0x1000000) while (raw&0xf000000) { raw>>=1;++exponent; }
                    else while (exponent && !(raw&0x800000)) { if (!--exponent) break;raw<<=1;++missing; }
                    missing&=31;
                    if (missing) {
                        if ((mode&1) || (extraPresent && (mode&2) && extra.Get(1))) raw|=(1u<<missing)-1;
                        else if (extraPresent && (mode&4)) {
                            unsigned zeros=maximumOnes && missing>maximumOnes?missing-maximumOnes:0;
                            zeros=std::max(zeros,std::min(minimumZeros,missing));
                            raw|=extra.Get(missing-zeros)<<zeros;
                        }
                    }
                    if (exponent>255) return false;
                    bits=sign|(exponent<<23)|(raw&0x7fffff);
                }
            }
            crc=crc*27+(bits&0x7fffff)*9+((bits>>23)&255)*3+(bits>>31);
            float sample=std::bit_cast<float>(bits);result[i]=std::isfinite(sample)?sample:0;
        }
    } else {
        unsigned sent=0,zeros=0,ones=0,duplicates=0,width=0,shift=(h.flags>>13)&31,bits=((h.flags&3)+1)*8;
        if (h.flags&extended) {
            Bytes info=meta.item[9];if (info.size()!=4) return false;
            sent=info[0];zeros=info[1];ones=info[2];duplicates=info[3];
            if (std::max({sent,zeros,ones,duplicates})>31 || sent+zeros+ones+duplicates>31) return false;
            if (modern) width=extra.Get(5);
        }
        if (!extraPresent && sent) { shift+=sent+zeros+ones+duplicates;sent=zeros=ones=duplicates=0; }
        if (shift>31) return false;
        // The normalization is constant for the block; double multiplication preserves
        // ldexp's rounding even for 32-bit integers that do not fit exactly in a float.
        double scale=std::ldexp(1.0,1-(int)bits);
        // Plain lossless blocks restore nothing, and their unused checksum is not computed.
        if (!extraPresent && !(sent|zeros|ones|duplicates) && !(h.flags&hybrid)) {
            for (size_t i=0;i<values.size();++i) result[i]=(float)((int32_t)((uint32_t)values[i]<<shift)*scale);
            return true;
        }
        for (size_t i=0;i<values.size();++i) {
            uint32_t raw=(uint32_t)values[i];
            if (sent) {
                int n=sent;
                if (width) n=std::min(n,(int)width-(int)(32-std::countl_zero(values[i]<0?~raw:raw)));
                unsigned tail=n>0?extra.Get((unsigned)n):0;
                raw=n>0?((raw<<n)|tail)<<(sent-n):raw<<sent;
            }
            if (zeros) raw<<=zeros;
            else if (ones) raw=((raw+1)<<ones)-1;
            else if (duplicates) { unsigned parity=raw&1;raw=((raw+parity)<<duplicates)-parity; }
            crc=crc*9+(raw&65535)*3+(raw>>16);
            int64_t sample=(int32_t)raw;
            if (h.flags&hybrid) {
                if (shift>=bits) return false;
                sample=std::clamp(sample,-(int64_t(1)<<(bits-1-shift)),(int64_t(1)<<(bits-1-shift))-1);
            }
            raw=(uint32_t)sample<<shift;
            result[i]=(float)((int32_t)raw*scale);
        }
    }
#ifdef CORDINAL_CODEC_TRACE
    if (extraPresent && crc!=U32(x.data())) fprintf(stderr,"wv extra CRC %08x expected %08x\n",crc,U32(x.data()));
#endif
    return !extraPresent || (extra.Good() && crc==U32(x.data()));
}

// One block: metadata, entropy setup, passes, residual decode, joint stereo and
// reconstruction, all gated by the block CRC.
// `values` is scratch, reused between blocks.
bool DecodeBlock(const Header& h,Bytes data,std::vector<float>& out,std::vector<int32_t>& values) {
    Metadata meta;if (!meta.Read(data,h.flags)) return false;
    Entropy entropy;if (!entropy.Setup(h,meta)) return false;
    std::vector<Pass> passes;if (!ReadPasses(h,meta,passes)) return false;
    Bytes compressed=meta.item[10];if (compressed.empty() || (compressed.size()&1)) return false;
    unsigned count=h.Coded();values.resize(h.samples*count);
    LittleBits bits(compressed);
    if (count==2) for (size_t i=0;i<values.size();i+=2) { values[i]=entropy.Word(bits,0,2);values[i+1]=entropy.Word(bits,1,2); }
    else for (size_t i=0;i<values.size();++i) values[i]=entropy.Word(bits,0,1);
    if (!bits.Good()) return false;
    Decorrelate(values,count,passes);
    if (count==2 && (h.flags&joint)) for (size_t i=0;i<values.size();i+=2) {
        int32_t right=(int32_t)(uint32_t(values[i+1])-uint32_t(values[i]>>1));
        values[i]=(int32_t)(uint32_t(values[i])+uint32_t(right));values[i+1]=right;
    }
    uint32_t crc=UINT32_MAX;for (int32_t v:values) crc=crc*3+(uint32_t)v;
#ifdef CORDINAL_CODEC_TRACE
    if (crc!=h.crc) fprintf(stderr,"wv core CRC %08x expected %08x\n",crc,h.crc);
#endif
    if (crc!=h.crc || !Reconstruct(h,meta,values,out)) return false;
    if (h.flags&falseStereo) {
        out.resize(h.samples*2);
        for (size_t i=h.samples;i--;) out[i*2]=out[i*2+1]=out[i];
    }
    return true;
}

class WavPackDecoder final : public Decoder {
    struct Group { uint64_t frame=0;unsigned samples=0;std::vector<uint64_t> blocks; };
public:
    bool Open(const std::wstring& path) {
        if (!file_.Open(path)) return false;
        file_.Sequential();
        // Blocks sharing an index form one multichannel group; the first block repeats
        // (flag 2048) at each new group, and flag 4096 marks the group's last block.
        uint64_t at=0,base=0,total=0;unsigned groupChannels=0,stored=0;bool complete=true;
        while (at+32<=file_.Size()) {
            const uint8_t* p=file_.View(at,32);if (!p) return false;
            if (!Is(p,"wvpk")) {
                if (!groups_.empty()) break;
                if (at>=1024*1024) return false;++at;continue;
            }
            Header h;if (!h.Read(p)) return false;
            p=file_.View(at,h.bytes);if (!p) return false;
            Metadata metadata;if (!metadata.Read({p,h.bytes},h.flags)) return false;
            if (!h.samples) { at+=h.bytes;continue; }
            // Rates come from a 4-bit index, or from the sample-rate metadata item when 15.
            unsigned rateIndex=(h.flags>>23)&15,rate=rateIndex<15?rates[rateIndex]:0;
            if (!rate) {
                auto data=metadata.item[0x27];
                if (data.empty()) rate=rate_;
                else {
                    if (data.size()<3 || data.size()>4) return false;
                    rate=U16(data.data())|(uint32_t(data[2])<<16);if (data.size()==4) rate|=uint32_t(data[3])<<24;
                }
            }
            if (!rate || rate>768000) return false;
            if (h.flags&2048) {
                if (!complete) return false;
                if (groups_.empty()) { base=h.index;rate_=rate;stored=h.flags&(floating|3); }
                if (h.index!=base+total) return false;
                groups_.push_back({total,h.samples,{}});groupChannels=0;complete=false;
            } else if (complete || groups_.empty() || h.index!=base+groups_.back().frame || h.samples!=groups_.back().samples) return false;
            if (rate!=rate_ || (h.flags&(floating|3))!=stored) return false;
            auto& group=groups_.back();group.blocks.push_back(at);groupChannels+=h.Channels();
            if (groupChannels>32 || groups_.size()>8000000) return false;
            if (h.flags&4096) {
                if (channels_ && groupChannels!=channels_) return false;
                channels_=groupChannels;total+=h.samples;complete=true;
            }
            at+=h.bytes;
        }
        if (!complete || groups_.empty() || !total) return false;
        frames_=total;return Seek(0);
    }
    size_t Read(float* out,size_t frames) override {
        if (failed_) return 0;size_t done=0;
        while (done<frames && position_<frames_) {
            if (used_==pcm_.size()/2) {
                if (next_==groups_.size()) break;
                const Group& group=groups_[next_++];
                pcm_.clear();used_=0;
                bool direct=group.blocks.size()==1 && channels_<=2;
                if (!direct) interleaved_.resize(size_t(group.samples)*channels_);
                unsigned channel=0;
                for (uint64_t offset:group.blocks) {
                    Header h;if (!h.Read(file_.View(offset,32))) { failed_=true;return done; }
                    const uint8_t* data=file_.View(offset,h.bytes);
                    auto& block=direct?pcm_:block_;
                    if (!data || !DecodeBlock(h,{data,h.bytes},block,values_) || block.size()!=size_t(group.samples)*h.Channels()) { failed_=true;return done; }
                    if (!direct) for (unsigned i=0;i<group.samples;++i) for (unsigned c=0;c<h.Channels();++c)
                        interleaved_[i*channels_+channel+c]=block[i*h.Channels()+c];
                    channel+=h.Channels();
                }
                // A single mono/stereo block already has playback order; only channel groups
                // need a separate interleaved buffer and downmix.
                pcm_.resize(group.samples*2);
                if (direct) {
                    if (channels_==1) for (size_t i=group.samples;i--;) pcm_[i*2]=pcm_[i*2+1]=pcm_[i];
                } else for (unsigned i=0;i<group.samples;++i) Mix(interleaved_.data()+i*channels_,channels_,pcm_.data()+i*2);
                used_=(size_t)(position_-group.frame);
            }
            size_t count=(size_t)std::min<uint64_t>({frames-done,pcm_.size()/2-used_,frames_-position_});
            memcpy(out+done*2,pcm_.data()+used_*2,count*2*sizeof(float));
            done+=count;used_+=count;position_+=count;
        }
        return done;
    }
    bool Seek(double seconds) override {
        if (!std::isfinite(seconds)) return false;
        position_=(uint64_t)std::min<double>((double)frames_,std::max(0.0,std::floor(seconds*rate_)));
        auto at=std::upper_bound(groups_.begin(),groups_.end(),position_,[](uint64_t v,const Group& g){return v<g.frame;});
        next_=position_==frames_?groups_.size():at==groups_.begin()?0:(size_t)(at-groups_.begin()-1);
        pcm_.clear();used_=0;failed_=false;return true;
    }
    unsigned Rate() const override { return rate_; }
    bool FastSeek() const override { return true; }
    bool Failed() const override { return failed_; }
    double Duration() const override { return (double)frames_/rate_; }
private:
    File file_;std::vector<Group> groups_;std::vector<float> pcm_,interleaved_,block_;std::vector<int32_t> values_;
    size_t next_=0,used_=0;uint64_t frames_=0,position_=0;unsigned channels_=0,rate_=0;bool failed_=false;
};
}
std::unique_ptr<Decoder> OpenWavPackDecoder(const std::wstring& path) {
    auto decoder=std::make_unique<WavPackDecoder>();if (!decoder->Open(path)) return nullptr;return decoder;
}
