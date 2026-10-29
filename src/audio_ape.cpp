// Cordinal Monkey's Audio decoder: bounded frames, arithmetic residuals,
// adaptive FIR stages, channel prediction and PCM CRC verification.
// Supports compressed levels fast/normal/high/extra for versions 3.95 through 3.99.
#include "native_audio.h"
#include "audio_file.h"
#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <vector>

namespace {
using namespace nativeaudio;
using Bytes=std::span<const uint8_t>;
int Opposite(int64_t value) { return (value<0)-(value>0); }
int32_t Wrap(int64_t value) { return (int32_t)(uint32_t)value; }

// Fixed arithmetic-symbol boundaries, part of the 3.97/3.98 bitstream models.
constexpr uint16_t olderModel[]{0,14824,28224,39348,47855,53994,58171,60926,62682,63786,64463,
    64878,65126,65276,65365,65419,65450,65469,65480,65487,65491,65493};
constexpr uint16_t newerModel[]{0,19578,36160,48417,56323,60899,63265,64435,64971,65232,65351,
    65416,65447,65466,65476,65482,65485,65488,65490,65491,65492,65493};

// Reads the APE byte stream. The logical stream is big-endian but is stored in little-endian
// DWORDs, so the low address bits are flipped on every byte access.
class Cursor {
public:
    Cursor(Bytes bytes,size_t offset):bytes_(bytes),at_(offset) {}
    uint8_t Byte() {
        // APE stores the logical big-endian stream in little-endian DWORDs.
        size_t index=at_++^3;
        if (index>=bytes_.size()) { good_=false;return 0; }
        return bytes_[index];
    }
    uint32_t Word() { uint32_t v=0;for (int i=0;i<4;++i) v=(v<<8)|Byte();return v; }
    bool Good() const { return good_; }
private:
    Bytes bytes_;size_t at_=0;bool good_=true;
};
// Arithmetic decoder over the fixed symbol models. Frequency() renormalizes the range and
// pulls fresh bytes as needed; a zero range or out-of-range value fails the stream.
class Arithmetic {
public:
    explicit Arithmetic(Cursor& cursor):cursor_(cursor) { previous_=cursor_.Byte();value_=previous_>>1; }
    uint32_t Uniform(uint32_t count) {
        if (count<=1) return 0;
        uint32_t symbol=Frequency(count);Update(symbol,1);return symbol;
    }
    unsigned Symbol(bool modern) {
        uint32_t value=Frequency(65536);
        if (value>=65493) { Update(value,1);return value-65472; }
        const auto& model=modern?newerModel:olderModel;
        unsigned symbol=(unsigned)(std::upper_bound(std::begin(model),std::end(model),value)-std::begin(model)-1);
        Update(model[symbol],model[symbol+1]-model[symbol]);return symbol;
    }
    bool Good() const { return good_ && cursor_.Good(); }
private:
    uint32_t Frequency(uint32_t total) {
        while (range_<=0x800000) {
            uint8_t next=cursor_.Byte();
            value_=(value_<<8)|((previous_&1)<<7)|(next>>1);previous_=next;range_<<=8;
        }
        unit_=total?range_/total:0;
        if (!unit_) { good_=false;unit_=1; }
        uint32_t value=value_/unit_;
        if (value>=total) { good_=false;return 0; }
        return value;
    }
    void Update(uint32_t start,uint32_t frequency) { value_-=unit_*start;range_=unit_*frequency; }
    Cursor& cursor_;
    uint32_t value_=0,range_=128,unit_=0;uint8_t previous_=0;bool good_=true;
};
// Adaptive residual decoder. k tracks the running sum's magnitude; 3.99 resurrected the
// pivot-based split while earlier versions use a plain bit-width split.
struct Residual {
    uint32_t sum=16384;unsigned k=10;
    int32_t Read(Arithmetic& r,unsigned version) {
        uint32_t value=0,overflow=r.Symbol(version>=3990);
        if (version>=3990) {
            uint32_t pivot=std::max(sum>>5,1u);
            if (overflow==63) { unsigned high=r.Uniform(65536);overflow=(high<<16)|r.Uniform(65536); }
            uint32_t remainder;
            if (pivot<65536) remainder=r.Uniform(pivot);
            else {
                unsigned extra=32-std::countl_zero(pivot)-16;
                uint32_t high=r.Uniform((pivot>>extra)+1),low=r.Uniform(1u<<extra);
                remainder=(high<<extra)+low;
            }
            value=remainder+overflow*pivot;
        } else {
            unsigned width=k?k-1:0;
            if (overflow==63) { width=r.Uniform(32);overflow=0; }
            if (width<=16) value=r.Uniform(1u<<width);
            else {
                uint32_t low=r.Uniform(65536),high=r.Uniform(1u<<(width-16));
                value=low|(high<<16);
            }
            value+=overflow<<width;
        }
        sum+=(value+1u)/2-((sum+16u)>>5);
        uint32_t lower=k?1u<<(k+4):0;
        if (sum<lower) --k;
        else if (k<24 && sum>=(1u<<(k+5))) ++k;
        return value&1?Wrap((int64_t)(value>>1)+1):Wrap(-(int64_t)(value>>1));
    }
};

// Mirrored rings keep the entire filter input contiguous at every cursor.
// Prediction and adaptation share one SIMD walk through the coefficients.
class Fir {
public:
    Fir(unsigned order,unsigned precision,unsigned version):order_(order),precision_(precision),version_(version),
        weights_(order),history_(order*2),adaptation_(order*2) {}
    int32_t Process(int32_t residual) {
        __m128i sum=_mm_setzero_si128();int direction=Opposite(residual);
        for (unsigned j=0;j<order_;j+=8) {
            auto weight=_mm_loadu_si128((const __m128i*)(weights_.data()+j));
            auto input=_mm_loadu_si128((const __m128i*)(history_.data()+cursor_+j));
            sum=_mm_add_epi32(sum,_mm_madd_epi16(weight,input));
            if (direction) {
                auto adjustment=_mm_loadu_si128((const __m128i*)(adaptation_.data()+cursor_+j));
                weight=direction>0?_mm_add_epi16(weight,adjustment):_mm_sub_epi16(weight,adjustment);
                _mm_storeu_si128((__m128i*)(weights_.data()+j),weight);
            }
        }
        sum=_mm_add_epi32(sum,_mm_srli_si128(sum,8));sum=_mm_add_epi32(sum,_mm_srli_si128(sum,4));
        int32_t dot=_mm_cvtsi128_si32(sum);
        int32_t value=Wrap(residual+((int64_t(dot)+(int64_t(1)<<(precision_-1)))>>precision_));
        auto store=[&](std::vector<int16_t>& ring,unsigned at,int v) { ring[at]=ring[at+order_]=(int16_t)v; };
        store(history_,cursor_,std::clamp(value,-32768,32767));
        if (version_<3980) store(adaptation_,cursor_,value?((value>>28)&8)-4:0);
        else {
            uint32_t magnitude=value<0?0u-(uint32_t)value:(uint32_t)value;
            unsigned scale=8u<<((magnitude>uint64_t(average_)*3)+(magnitude>average_+average_/3));
            store(adaptation_,cursor_,Opposite(value)*(int)scale);
            average_+=(int32_t)(magnitude-average_)/16;
        }
        auto halve=[&](unsigned back) { unsigned at=(cursor_+order_-back)%order_;store(adaptation_,at,adaptation_[at]>>1); };
        if (version_<3980) { halve(4);halve(8); }
        else { halve(1);halve(2);halve(8); }
        if (++cursor_==order_) cursor_=0;
        return value;
    }
private:
    unsigned order_,precision_,version_,cursor_=0;uint32_t average_=0;
    std::vector<int16_t> weights_,history_,adaptation_;
};

// Cascade of adaptive predictors: a 4-tap self predictor and a 5-tap cross-channel one,
// each with its own coefficient set that tracks recent residuals.
struct Predictor {
    bool good=true;
    struct Lane {
        int64_t previous=0,output=0,previousSelf=0,previousOther=0,previousCross=0;
        std::array<int64_t,3> selfDifferences{};
        std::array<int64_t,4> crossDifferences{};
        std::array<int64_t,4> a{360,317,-109,98};
        std::array<int64_t,5> b{};
    } lane[2];
    int32_t Apply(int32_t residual,unsigned channel,bool mono,bool wide) {
        Lane& p=lane[channel];
        constexpr int64_t limit=1ll<<34;
        if (!good || p.previous< -limit || p.previous>limit || p.output< -limit || p.output>limit ||
            p.previousOther< -limit || p.previousOther>limit || lane[1-channel].output< -limit || lane[1-channel].output>limit) {
            good=false;return 0;
        }
        std::array<int64_t,4> self{p.previous,p.previous-p.previousSelf,p.selfDifferences[0],p.selfDifferences[1]};
        int64_t crossValue=lane[1-channel].output-((p.previousOther*31)>>5);
        std::array<int64_t,5> cross{crossValue,crossValue-p.previousCross,p.crossDifferences[0],p.crossDifferences[1],p.crossDifferences[2]};
        int64_t selfPrediction=0,crossPrediction=0;
        for (unsigned i=0;i<4;++i) selfPrediction+=self[i]*p.a[i];
        if (!mono) for (unsigned i=0;i<5;++i) crossPrediction+=cross[i]*p.b[i];
        int64_t predicted;
        if (mono) predicted=Wrap(selfPrediction)>>10;
        else if (!wide) predicted=Wrap(Wrap(selfPrediction)+(int64_t)(Wrap(crossPrediction)>>1))>>10;
        else predicted=(selfPrediction+(crossPrediction>>1))>>10;
        int64_t current=mono || !wide?Wrap(residual+predicted):residual+predicted;
        int sign=Opposite(residual);
        for (unsigned i=0;i<4;++i) p.a[i]+=Opposite(self[i])*sign;
        if (!mono) for (unsigned i=0;i<5;++i) p.b[i]+=Opposite(cross[i])*sign;
        p.previousSelf=p.previous;p.previous=current;p.output=current+((p.output*31)>>5);
        p.previousCross=crossValue;p.previousOther=lane[1-channel].output;
        std::move_backward(p.selfDifferences.begin(),p.selfDifferences.end()-1,p.selfDifferences.end());p.selfDifferences[0]=self[1];
        std::move_backward(p.crossDifferences.begin(),p.crossDifferences.end()-1,p.crossDifferences.end());p.crossDifferences[0]=cross[1];
        return Wrap(p.output);
    }
};

constexpr auto CrcTable() {
    std::array<uint32_t,256> table{};
    for (unsigned i=0;i<256;++i) { uint32_t value=i;for (int k=0;k<8;++k) value=(value>>1)^((value&1)?0xEDB88320:0);table[i]=value; }
    return table;
}
constexpr auto crcTable=CrcTable();
// Standard CRC-32 (reflected) over the little-endian PCM samples; APE stores ~crc>>1.
uint32_t PcmCrc(const std::vector<int32_t>& values,unsigned bits) {
    uint32_t crc=UINT32_MAX;
    for (int32_t sample:values) {
        uint32_t value=(uint32_t)sample+(bits==8?128:0);
        for (unsigned byte=0;byte<bits/8;++byte) { crc=(crc>>8)^crcTable[(crc^value)&255];value>>=8; }
    }
    return ~crc>>1;
}

class ApeDecoder final : public Decoder {
    struct Frame { uint64_t offset=0,end=0;unsigned skip=0,samples=0; };
public:
    bool Open(const std::wstring& path) {
        if (!file_.Open(path)) return false;
        file_.Sequential();
        uint64_t origin=0;
        const uint8_t* p=file_.View(0,32);
        if (p && !memcmp(p,"ID3",3)) {
            if ((p[6]|p[7]|p[8]|p[9])&128) return false;
            origin=10+((uint64_t)p[6]<<21)+((uint64_t)p[7]<<14)+((uint64_t)p[8]<<7)+p[9]+((p[5]&16)?10:0);
            p=file_.View(origin,32);
        }
        if (!p || !Is(p,"MAC ")) return false;
        version_=U16(p+4);if (version_<3950 || version_>3990) return false;
        uint64_t seekAt=0,first=0,end=0;uint32_t frames=0,last=0,seekBytes=0;
        // 3.98+ stores a descriptor header, an APE_HEADER and a seek table; 3.95-3.97 pack
        // the same fields directly into the leading bytes.
        if (version_>=3980) {
            p=file_.View(origin,52);if (!p) return false;
            uint32_t descriptor=U32(p+8),header=U32(p+12),wave=U32(p+20);
            uint64_t audio=U64(p+24);seekBytes=U32(p+16);
            if (descriptor<52 || header<24 || descriptor>1048576 || header>1048576) return false;
            seekAt=origin+descriptor+header;first=seekAt+seekBytes+wave;
            if (audio>file_.Size() || first>file_.Size()-audio) return false;
            end=audio?first+audio:file_.Size();
            p=file_.View(origin+descriptor,24);if (!p) return false;
            level_=U16(p);flags_=U16(p+2);perFrame_=U32(p+4);last=U32(p+8);frames=U32(p+12);
            bits_=U16(p+16);channels_=U16(p+18);rate_=U32(p+20);
        } else {
            level_=U16(p+6);flags_=U16(p+8);channels_=U16(p+10);rate_=U32(p+12);
            uint32_t wave=U32(p+16),tail=U32(p+20);frames=U32(p+24);last=U32(p+28);
            uint64_t at=origin+32;
            if (flags_&4) at+=4;
            seekBytes=frames*4;
            if (flags_&16) { p=file_.View(at,4);if (!p) return false;seekBytes=U32(p)*4;at+=4; }
            seekAt=at+((flags_&32)?0:wave);first=seekAt+seekBytes;
            end=file_.Size()-std::min<uint64_t>(tail,file_.Size());
            perFrame_=294912;bits_=(flags_&1)?8:(flags_&8)?24:16;
        }
        if (level_<1000 || level_>5000 || level_%1000 || (channels_!=1 && channels_!=2) ||
            (bits_!=8 && bits_!=16 && bits_!=24) || !rate_ || rate_>768000 || !perFrame_ || perFrame_>1179648 ||
            !last || last>perFrame_ || !frames || frames>8000000 || frames>seekBytes/4 || first>=end || end>file_.Size()) return false;
        // The seek table holds byte offsets of frame starts; align each to a DWORD and
        // derive per-frame ends, then total the sample count.
        p=file_.View(seekAt,seekBytes);if (!p) return false;
        index_.resize(frames);
        for (unsigned i=0;i<frames;++i) {
            uint64_t position=i?origin+U32(p+i*4):first;
            if (position<first || position>=end || (i && position<=origin+U32(p+(i-1)*4))) return false;
            unsigned skip=(unsigned)((position-first)&3);
            index_[i]={position-skip,0,skip,i+1==frames?last:perFrame_};
            if (i) index_[i-1].end=position;
        }
        index_.back().end=end;
        for (auto& frame:index_) {
            uint64_t size=(frame.end-frame.offset+3)&~uint64_t(3);
            if (size>32*1024*1024 || frame.offset+size>file_.Size()) return false;
            frame.end=frame.offset+size;
        }
        frames_=uint64_t(frames-1)*perFrame_+last;
        return Seek(0);
    }
    size_t Read(float* out,size_t count) override {
        if (failed_) return 0;
        size_t done=0;
        while (done<count && position_<frames_) {
            if (used_==pcm_.size()/2) {
                if (next_==index_.size()) break;
                size_t frame=next_++;pcm_.clear();used_=0;
                if (!Decode(index_[frame],pcm_)) { pcm_.clear();failed_=true;break; }
                used_=(size_t)(position_-uint64_t(frame)*perFrame_);
            }
            size_t take=(size_t)std::min<uint64_t>({count-done,pcm_.size()/2-used_,frames_-position_});
            memcpy(out+done*2,pcm_.data()+used_*2,take*2*sizeof(float));
            used_+=take;position_+=take;done+=take;
        }
        return done;
    }
    bool Seek(double seconds) override {
        if (!std::isfinite(seconds)) return false;
        position_=(uint64_t)std::min<double>((double)frames_,std::max(0.0,std::floor(seconds*rate_)));
        next_=position_==frames_?index_.size():(size_t)(position_/perFrame_);
        pcm_.clear();used_=0;failed_=false;return true;
    }
    unsigned Rate() const override { return rate_; }
    bool Failed() const override { return failed_; }
    double Duration() const override { return (double)frames_/rate_; }
private:
    // Decode one frame: optional header, arithmetic residuals, FIR stages and channel
    // prediction, then PCM CRC verification.
    bool Decode(const Frame& frame,std::vector<float>& pcm) {
        const uint8_t* bytes=file_.View(frame.offset,(size_t)(frame.end-frame.offset));if (!bytes) return false;
        Cursor cursor({bytes,(size_t)(frame.end-frame.offset)},frame.skip);
        uint32_t stored=cursor.Word(),code=0;
        if (stored&0x80000000) { stored&=0x7fffffff;code=cursor.Word(); }
        bool silent=channels_==1?(code&3)!=0:(code&3)==3,pseudo=channels_==2 && (code&4);
        unsigned count=channels_==1 || pseudo?1:2;
        std::vector<int32_t> residual(size_t(frame.samples)*count),values(size_t(frame.samples)*channels_);
        if (!silent) {
            cursor.Byte();Arithmetic range(cursor);Residual state[2];
            // Per-quality FIR orders and weight precisions; index 0 is "fast" (no filters).
            constexpr unsigned orders[5][3]={{0,0,0},{16,0,0},{64,0,0},{32,256,0},{16,256,1280}};
            constexpr unsigned fractions[5][3]={{0,0,0},{11,0,0},{11,0,0},{10,13,0},{11,13,15}};
            std::vector<Fir> filters[2];unsigned quality=level_/1000-1;
            for (unsigned c=0;c<count;++c) for (unsigned i=0;i<3 && orders[quality][i];++i)
                filters[c].emplace_back(orders[quality][i],fractions[quality][i],version_);
            for (unsigned i=0;i<frame.samples;++i) for (unsigned c=0;c<count;++c) {
                int32_t value=state[c].Read(range,version_);
                for (auto& filter:filters[c]) value=filter.Process(value);
                residual[i*count+c]=value;
            }
            if (!range.Good()) return false;
        }
        // Recover stereo from the mono predictor plus a cross-channel residual; `wide`
        // selects whether the sum is wrapped before the shift.
        auto predict=[&](bool wide) {
            Predictor predictor;
            for (unsigned i=0;i<frame.samples;++i) {
                int32_t first=silent?0:predictor.Apply(residual[i*count],0,count==1,wide);
                if (count==1) {
                    values[i*channels_]=first;if (channels_==2) values[i*2+1]=first;
                } else {
                    int32_t second=silent?0:predictor.Apply(residual[i*2+1],1,false,wide);
                    int32_t left=Wrap(second-(int64_t)(first/2));
                    values[i*2]=left;values[i*2+1]=Wrap((int64_t)left+first);
                }
                if (!predictor.good) return false;
            }
            return true;
        };
        bool valid=predict(false);
        if (!valid || PcmCrc(values,bits_)!=stored) {
            // 24-bit streams exist with either intermediate prediction width.
            // The PCM CRC selects the matching arithmetic without guessing.
            if (bits_!=24 || count!=2 || silent) return false;
            if (!predict(true) || PcmCrc(values,bits_)!=stored) return false;
        }
        if (!cursor.Good()) return false;
        pcm.resize(size_t(frame.samples)*2);unsigned shift=32-bits_;
        for (unsigned i=0;i<frame.samples;++i) for (unsigned c=0;c<2;++c) {
            int32_t value=(int32_t)((uint32_t)values[i*channels_+(channels_==1?0:c)]<<shift)>>shift;
            pcm[i*2+c]=(float)std::ldexp((double)value,1-(int)bits_);
        }
        return true;
    }
    File file_;std::vector<Frame> index_;std::vector<float> pcm_;
    uint64_t frames_=0,position_=0;size_t next_=0,used_=0;
    unsigned version_=0,level_=0,flags_=0,perFrame_=0,bits_=0,channels_=0,rate_=0;bool failed_=false;
};
}
std::unique_ptr<Decoder> OpenApeDecoder(const std::wstring& path) {
    auto decoder=std::make_unique<ApeDecoder>();if (!decoder->Open(path)) return nullptr;return decoder;
}
