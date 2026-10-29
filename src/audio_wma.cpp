// Cordinal WMA Standard (versions 1/2). Owned entropy, envelope, overlap and packet state.
// Decodes ASF/Matroska input with per-frame varying transform sizes and an optional bit
// reservoir; both the LSP and the VLC exponent modes are supported.
#include "native_audio.h"
#include "audio_packets.h"
#include "audio_bits.h"
#include "audio_huffman.h"
#include "audio_mdct.h"
#include "audio_aac_tables.h"
#include "audio_wma_tables.h"
#include <bit>
#include <span>
#include <emmintrin.h>

namespace {
using namespace nativeaudio;
namespace w=wmadata;
unsigned Log(unsigned n) { return n?31-std::countl_zero(n):0; }
constexpr double pi=3.14159265358979323846;
// A run/level codebook: prefix codewords plus, per codeword length, the number of symbols
// at that length. Built from the raw table at first use.
struct Codebook {
    PrefixBook prefix;
    std::vector<uint16_t> run,level;
    Codebook(std::span<const uint16_t> codes,std::span<const uint16_t> counts):prefix(codes),run(codes.size()),level(codes.size()) {
        size_t at=2;
        for (unsigned l=0;l<counts.size();++l) for (unsigned r=0;r<counts[l];++r) { run[at]=(uint16_t)r; level[at++]=(uint16_t)(l+1); }
    }
};
const std::array<Codebook,6>& Codebooks() {
    static const std::array<Codebook,6> books={Codebook(w::coefficients0,w::levels0),Codebook(w::coefficients1,w::levels1),
        Codebook(w::coefficients2,w::levels2),Codebook(w::coefficients3,w::levels3),Codebook(w::coefficients4,w::levels4),Codebook(w::coefficients5,w::levels5)};
    return books;
}
const PrefixBook& ExponentBook() { static const PrefixBook b(aacdata::codes0); return b; }
const PrefixBook& GainBook() { static const PrefixBook b(w::highGain); return b; }
// VLC exponents are bounded integers; reuse the same rounded powers across all blocks.
const std::array<float,156>& ExponentPowers() {
    static const auto powers=[] {
        std::array<float,156> values{};
        for (unsigned i=0;i<values.size();++i) values[i]=(float)std::pow(10.0,(int(i)-60)/16.0);
        return values;
    }();
    return powers;
}

// Append-only bit buffer used to join bytes carried across packets by the bit reservoir.
struct BitString {
    std::vector<uint8_t> bytes;
    size_t bits=0;
    bool Append(Bits& input,size_t count) {
        if (count>input.Left() || bits+count>1024*1024*8) return false;
        while (count) {
            unsigned take=(unsigned)std::min<size_t>(count,8-(bits&7));
            if (!(bits&7)) bytes.push_back(0);
            bytes.back()|=(uint8_t)(input.Get(take)<<(8-(bits&7)-take));
            bits+=take; count-=take;
        }
        return input.Good();
    }
    void Clear() { bytes.clear(); bits=0; }
};
// One transform size: the scalefactor-band widths, the coded end/high subbands and the window.
struct Block {
    std::vector<unsigned> bands,highBands;
    std::vector<float> window;
    Mdct mdct;
    unsigned end=0,high=0;
};
// Per-channel decoder state: the current envelope, overlap history and next-exponent index.
struct Channel {
    std::vector<float> exponent,history;
    unsigned exponentSize=0;
    float maxExponent=1;
    bool initialized=false;
};

class WmaDecoder final : public PacketDecoder {
public:
    bool Open(const std::wstring& path) {
        if (!media_.OpenAsf(path) || media_.config.size()<18) return false;
        const uint8_t* f=media_.config.data();
        // ASF gives a WAVEFORMATEX; the format tag implies the version and the extra bytes
        // select the exponent coding plus the reservoir/variable-size flags.
        version_=media_.codec==0x160?1:2; channels_=media_.channels;
        unsigned rate=media_.rate,bitrate=U32(f+8)*8,align=U16(f+12);
        size_t flagAt=version_==1?20:22;
        if (media_.config.size()<flagAt+2 || !bitrate || !align) return false;
        unsigned flags=U16(f+flagAt);
        exponentVlc_=(flags&1)!=0; reservoir_=(flags&2)!=0; variable_=(flags&4)!=0;
        // Transform-block count per frame (512/1024/2048 samples) is implied by the rate.
        frameBits_=rate<=16000?9:rate<=22050 || (version_==1 && rate<=32000)?10:11;
        frame_=1u<<frameBits_;
        unsigned sizes=variable_?std::min(frameBits_-7,((flags>>3)&3)+1+(bitrate/channels_>=32000?2:0))+1:1;
        double bps=double(bitrate)/(rate*channels_),scaled=bps*(channels_==2?1.6:1);
        offsetBits_=Log((unsigned)(bps*frame_/8+0.5))+5;
        if (offsetBits_>28) return false;
        book_=(rate>=32000 && scaled<1.16)?(scaled<0.72?0:2):4;
        unsigned normalized=rate;
        if (version_==2) for (unsigned r:{44100u,22050u,16000u,11025u,8000u}) if (rate>=r) { normalized=r; break; }
        double high=1;
        noise_=true;
        switch (normalized) {
            case 44100: noise_=scaled<0.61; high=noise_?0.4:1; break;
            case 22050: noise_=scaled<1.16; high=noise_?(scaled<0.72?0.6:0.7):1; break;
            case 16000: high=bps>0.5?0.5:0.3; break;
            case 11025: high=0.7; break;
            case 8000: noise_=bps<=0.75; high=bps<=0.625?0.5:noise_?0.65:1; break;
            default: high=bps>=0.8?0.75:bps>=0.6?0.6:0.5; break;
        }
        // Each coarser block derives scalefactor bands from these critical-band edges, unless
        // version 2 supplies a fixed band layout instead.
        static constexpr unsigned critical[]={100,200,300,400,510,630,770,920,1080,1270,1480,1720,2000,
            2320,2700,3150,3700,4400,5300,6400,7700,9500,12000,15500,24500};
        blocks_.resize(sizes);
        for (unsigned b=0;b<sizes;++b) {
            auto& block=blocks_[b]; unsigned n=frame_>>b;
            std::span<const uint16_t> fixed;
            unsigned power=frameBits_-b-7;
            if (version_==2 && power<3) {
                if (rate>=44100) fixed=power==0?std::span<const uint16_t>(w::bands44100_0):power==1?std::span<const uint16_t>(w::bands44100_1):std::span<const uint16_t>(w::bands44100_2);
                else if (rate>=32000) fixed=power==0?std::span<const uint16_t>(w::bands32000_0):power==1?std::span<const uint16_t>(w::bands32000_1):std::span<const uint16_t>(w::bands32000_2);
                else if (rate>=22050) fixed=power==0?std::span<const uint16_t>(w::bands22050_0):power==1?std::span<const uint16_t>(w::bands22050_1):std::span<const uint16_t>(w::bands22050_2);
            }
            if (!fixed.empty()) block.bands.assign(fixed.begin(),fixed.end());
            else {
                unsigned previous=0;
                for (unsigned frequency:critical) {
                    unsigned position=version_==1?(n*2*frequency+rate/2)/rate:((n*2*frequency+rate*2)/(rate*4))*4;
                    position=std::min(position,n);
                    if (position>previous || version_==1) block.bands.push_back(position-previous);
                    previous=position; if (position==n) break;
                }
                if (previous!=n) return false;
            }
            block.end=(frame_-frame_*9/100)>>b;
            block.high=(unsigned)(n*high+0.5);
            unsigned bin=0;
            for (unsigned width:block.bands) {
                unsigned lo=std::max(bin,block.high),hi=std::min(bin+width,block.end);
                if (hi>lo) block.highBands.push_back(hi-lo);
                bin+=width;
            }
            block.window.resize(n); for (unsigned i=0;i<n;++i) block.window[i]=(float)std::sin(pi*(i+0.5)/(2*n));
            block.mdct.Init(n*2);
        }
        for (auto& c:channel_) { c.exponent.resize(frame_); c.history.resize(frame_*2); }
        // With a bit reservoir the packet's first nibble encodes how many frames it holds.
        media_.frames=0; bool previous=false;
        for (size_t i=0;i<media_.packets.size();++i) {
            auto& packet=media_.packets[i];
            if (packet.bytes<align) return false;
            if (packet.bytes!=align) return false;
            unsigned frames=1;
            if (reservoir_) {
                const uint8_t* p=media_.Data(i); if (!p) return false;
                unsigned n=p[0]&15;
                frames=n? n-1+unsigned(previous):0;
                previous=true;
            }
            packet.start=media_.frames; packet.frames=frames*frame_; media_.frames+=packet.frames;
        }
        // Two decoder frames of startup latency; emit the final overlap at EOF.
        Packet tail; tail.start=media_.frames; tail.frames=frame_;
        media_.packets.push_back(tail); media_.frames+=frame_; media_.skip=frame_*(media_.matroska?1:2);
        if (media_.matroska) media_.consumedDelay=frame_;
        (void)Codebooks(); (void)ExponentBook();
        if (exponentVlc_) (void)ExponentPowers();
        return Ready(rate);
    }
private:
    struct History : State {
        Channel channel[2];BitString carry;
        unsigned previous,current,next,noisePosition;
        uint32_t noiseSeed;
        bool resetLengths;
    };
    std::shared_ptr<const State> Capture() const override {
        if (carry_.bytes.size()>65536) return {};
        auto history=std::make_shared<History>();
        for (unsigned c=0;c<2;++c) history->channel[c]=channel_[c];
        history->carry=carry_;history->previous=previous_;history->current=current_;history->next=next_;
        history->noisePosition=noisePosition_;history->noiseSeed=noiseSeed_;history->resetLengths=resetLengths_;return history;
    }
    bool Restore(const State& state) override {
        const auto& history=static_cast<const History&>(state);
        for (unsigned c=0;c<2;++c) channel_[c]=history.channel[c];
        carry_=history.carry;previous_=history.previous;current_=history.current;next_=history.next;
        noisePosition_=history.noisePosition;noiseSeed_=history.noiseSeed;resetLengths_=history.resetLengths;return true;
    }
    size_t Preroll() const override { return SIZE_MAX; }
    void Reset() override {
        carry_.Clear(); resetLengths_=true; previous_=current_=next_=0; noisePosition_=0; noiseSeed_=1;
        for (auto& c:channel_) { std::fill(c.history.begin(),c.history.end(),0.0f); c.initialized=false; c.maxExponent=1; c.exponentSize=0; }
    }
    // Deterministic noise for the high-band and PNS paths; reseeded every 8192 samples.
    float Noise() {
        if (noisePosition_==8192) { noisePosition_=0; noiseSeed_=1; }
        noiseSeed_=noiseSeed_*314159u+1; ++noisePosition_;
        return (int32_t)noiseSeed_*(float)(std::sqrt(3.0)/2147483648.0)*(exponentVlc_?0.02f:0.04f);
    }
    // Envelope for a block: VLC differential exponents, or LSP-interpolated line spectrum
    // values converted to a per-bin power envelope.
    bool Exponents(Bits& bits,unsigned ch,unsigned block) {
        auto& c=channel_[ch]; unsigned n=frame_>>block;
        if (exponentVlc_) {
            const auto& powers=ExponentPowers();
            int exponent=version_==1?(int)bits.Get(5)+10:36;
            unsigned at=0,band=0; c.maxExponent=0;
            for (unsigned width:blocks_[block].bands) {
                if (version_!=1 || band) exponent+=ExponentBook().Read(bits)-60;
                if (exponent<-60 || exponent>95 || width>n-at) return false;
                float value=powers[exponent+60];
                std::fill_n(c.exponent.begin()+at,width,value); at+=width; ++band;
                c.maxExponent=std::max(c.maxExponent,value);
            }
            if (at!=n) return false;
        } else {
            float points[10];
            for (unsigned i=0;i<10;++i) points[i]=w::lsp[i][(size_t)bits.Get(i==0 || i>=8?3:4)];
            c.maxExponent=0;
            for (unsigned i=0;i<n;++i) {
                double angle=2*std::cos(pi*i/n),p=0.5,q=0.5;
                for (unsigned j=0;j<10;j+=2) { p*=angle-points[j+1]; q*=angle-points[j]; }
                double power=p*p*(2-angle)+q*q*(2+angle);
                if (power<=0) return false;
                c.exponent[i]=(float)std::pow(power,-0.25); c.maxExponent=std::max(c.maxExponent,c.exponent[i]);
            }
        }
        c.exponentSize=block; c.initialized=true;
        return bits.Good() && Finite(c.maxExponent) && c.maxExponent>0;
    }
    // Run/level codewords for one channel; a symbol of 1 terminates the list early.
    bool Levels(Bits& bits,std::vector<float>& values,unsigned count,unsigned escapeBits,unsigned which) {
        const auto& table=Codebooks()[which]; unsigned at=0;
        while (at<count && bits.Good()) {
            unsigned symbol=table.prefix.Read(bits); if (symbol==1) return true;
            unsigned run,level;
            if (!symbol) { level=(unsigned)bits.Get(escapeBits); run=(unsigned)bits.Get(frameBits_); }
            else { run=table.run[symbol]; level=table.level[symbol]; }
            if (run>=count-at) return false;
            // The sign bit is random, so set it arithmetically instead of branching on it.
            at+=run; values[at++]=std::bit_cast<float>(std::bit_cast<uint32_t>((float)level)^uint32_t(bits.Get(1)^1)<<31);
        }
        return bits.Good();
    }
    // Transform one block and overlap-add it. Neighbouring sizes change how the window is
    // substituted at the edges, letting long and short blocks butt against each other.
    // A smaller neighbour narrows the slope to its own window, centred, with the block's
    // outer part zero and its inner part unit weight; each region is a separate loop.
    void Overlap(unsigned ch,const std::vector<float>& spectrum,unsigned position) {
        const unsigned n=frame_>>current_,prev=frame_>>previous_,next=frame_>>next_;
        wave_.resize(n*2); blocks_[current_].mdct.Run(spectrum.data(),wave_.data());
        float* h=channel_[ch].history.data()+frame_/2+position-n/2;
        const float* w=wave_.data();
        auto add=[](float* h,const float* w,unsigned count) {
            for (unsigned i=0;i<count;i+=4) _mm_storeu_ps(h+i,_mm_add_ps(_mm_loadu_ps(h+i),_mm_loadu_ps(w+i)));
        };
        // h[i] += w[i]*window[i], or with the window read backwards from window[last].
        auto weigh=[](float* h,const float* w,const float* window,unsigned count) {
            for (unsigned i=0;i<count;i+=4)
                _mm_storeu_ps(h+i,_mm_add_ps(_mm_loadu_ps(h+i),_mm_mul_ps(_mm_loadu_ps(w+i),_mm_loadu_ps(window+i))));
        };
        auto weighBack=[](float* h,const float* w,const float* last,unsigned count) {
            for (unsigned i=0;i<count;i+=4) {
                __m128 v=_mm_loadu_ps(last-3-i);
                _mm_storeu_ps(h+i,_mm_add_ps(_mm_loadu_ps(h+i),_mm_mul_ps(_mm_loadu_ps(w+i),_mm_shuffle_ps(v,v,0x1B))));
            }
        };
        if (prev>=n) weigh(h,w,blocks_[current_].window.data(),n);
        else {
            unsigned pad=(n-prev)/2;
            weigh(h+pad,w+pad,blocks_[previous_].window.data(),prev);
            add(h+pad+prev,w+pad+prev,n-pad-prev);
        }
        h+=n;w+=n;
        if (next>=n) weighBack(h,w,blocks_[current_].window.data()+n-1,n);
        else {
            unsigned pad=(n-next)/2;
            add(h,w,pad);
            weighBack(h+pad,w+pad,blocks_[next_].window.data()+next-1,next);
        }
    }
    // Decode one frame as a sequence of transform blocks, then shift the overlap history.
    bool Frame(Bits& bits,std::vector<float>& out) {
        for (unsigned position=0;position<frame_;) {
            // Variable-size frames code the previous/current/next block sizes in-line.
            if (variable_) {
                unsigned width=Log((unsigned)blocks_.size()-1)+1;
                if (resetLengths_) { previous_=(unsigned)bits.Get(width); current_=(unsigned)bits.Get(width); resetLengths_=false; }
                else { previous_=current_; current_=next_; }
                next_=(unsigned)bits.Get(width);
                if (std::max({previous_,current_,next_})>=blocks_.size()) return false;
            } else previous_=current_=next_=0;
            unsigned n=frame_>>current_;
            if (n>frame_-position) return false;
            bool midSide=channels_==2 && bits.Get(1),coded[2]{};
            for (unsigned ch=0;ch<channels_;++ch) coded[ch]=bits.Get(1)!=0;
            // These buffers are scratch, not packet history. Keep their storage across
            // blocks while resetting every coefficient before decoding the next block.
            auto& spectrum=spectrum_; for (auto& v:spectrum) v.assign(n,0);
            if (coded[0] || coded[1]) {
                unsigned gain=1,part;
                do { part=(unsigned)bits.Get(7); gain+=part; if (!bits.Good() || gain>512) return false; } while (part==127);
                unsigned escape=gain<15?13:gain<32?12:gain<40?11:gain<45?10:9;
                unsigned start=version_==1?3:0,end=blocks_[current_].end;
                auto& highs=blocks_[current_].highBands;
                bool highNoise[2][32]{}; int noiseGain[2][32]{}; unsigned count[2]={end-start,end-start};
                if (highs.size()>32) return false;
                // High bands may be flagged as noise; their levels replace quantized lines.
                if (noise_) {
                    for (unsigned ch=0;ch<channels_;++ch) if (coded[ch]) for (unsigned j=0;j<highs.size();++j) {
                        highNoise[ch][j]=bits.Get(1)!=0; if (highNoise[ch][j]) count[ch]-=highs[j];
                    }
                    for (unsigned ch=0;ch<channels_;++ch) if (coded[ch]) {
                        bool first=true; int value=0;
                        for (unsigned j=0;j<highs.size();++j) if (highNoise[ch][j]) {
                            value+=first?(int)bits.Get(7)-19:int(w::highGainSymbols[GainBook().Read(bits)])-18;
                            first=false; noiseGain[ch][j]=value;
                        }
                    }
                }
                bool fresh=current_==0 || bits.Get(1);
                for (unsigned ch=0;ch<channels_;++ch) if (coded[ch] && fresh && !Exponents(bits,ch,current_)) return false;
                auto& quant=quant_; for (auto& v:quant) v.assign(n,0);
                for (unsigned ch=0;ch<channels_;++ch) {
                    if (coded[ch] && (!channel_[ch].initialized || !Levels(bits,quant[ch],count[ch],escape,book_+unsigned(ch==1 && midSide)))) return false;
                    if (version_==1 && channels_==2) bits.Align();
                }
                double norm=-(version_==1?std::sqrt(n/2.0):1)*(2.0/n)/32768;
                double gainPower=std::pow(10.0,gain/20.0);
                for (unsigned ch=0;ch<channels_;++ch) if (coded[ch]) {
                    auto& c=channel_[ch];
                    auto envelope=[&](unsigned i) { return c.exponent[(i<<current_)>>c.exponentSize]; };
                    float scale=(float)(gainPower*norm/c.maxExponent);
                    unsigned q=0;
                    if (noise_) {
                        unsigned bin=blocks_[current_].high,last=0; float energy[32]{};
                        for (unsigned j=0;j<highs.size();++j) {
                            if (highNoise[ch][j]) {
                                double sum=0; for (unsigned i=0;i<highs[j];++i) { double e=envelope(bin+i); sum+=e*e; }
                                energy[j]=(float)(sum/highs[j]); last=j;
                            }
                            bin+=highs[j];
                        }
                        for (unsigned i=0;i<start;++i) spectrum[ch][i]=Noise()*envelope(i)*scale;
                        bin=start;
                        for (int group=-1;group<(int)highs.size();++group) {
                            unsigned width=group<0?blocks_[current_].high-start:highs[group];
                            if (group>=0 && highNoise[ch][group]) {
                                double ratio=energy[last]>0?std::sqrt(energy[group]/energy[last]):0;
                                float s=(float)(ratio*std::pow(10.0,noiseGain[ch][group]/20.0)*norm/(c.maxExponent*(exponentVlc_?0.02:0.04)));
                                for (unsigned i=0;i<width;++i) { spectrum[ch][bin]=Noise()*envelope(bin)*s; ++bin; }
                            } else for (unsigned i=0;i<width;++i) { spectrum[ch][bin]=(quant[ch][q++]+Noise())*envelope(bin)*scale; ++bin; }
                        }
                        float top=envelope(end-1)*scale;
                        for (unsigned i=end;i<n;++i) spectrum[ch][i]=Noise()*top;
                    } else if (current_==c.exponentSize) {
                        // The envelope was coded at this block size, so it maps bin for bin.
                        const float* e=c.exponent.data();const float* v=quant[ch].data();float* s=spectrum[ch].data();
                        for (unsigned i=start;i<end;++i) s[i]=v[i-start]*e[i]*scale;
                    } else for (unsigned i=start;i<end;++i) spectrum[ch][i]=quant[ch][q++]*envelope(i)*scale;
                }
                if (midSide) {
                    if (coded[1]) for (unsigned i=0;i<n;++i) { float l=spectrum[0][i],r=spectrum[1][i]; spectrum[0][i]=l+r; spectrum[1][i]=l-r; }
                    else spectrum[1]=spectrum[0];
                }
            }
            if (!bits.Good()) return false;
            for (unsigned ch=0;ch<channels_;++ch) Overlap(ch,spectrum[ch],position);
            position+=n;
        }
        size_t at=out.size(); out.resize(at+frame_*2);
        const float* left=channel_[0].history.data();const float* right=channel_[channels_-1].history.data();
        float* o=out.data()+at;
        for (unsigned i=0;i<frame_;i+=4) {
            __m128 l=_mm_loadu_ps(left+i),r=_mm_loadu_ps(right+i);
            _mm_storeu_ps(o+i*2,_mm_unpacklo_ps(l,r));_mm_storeu_ps(o+i*2+4,_mm_unpackhi_ps(l,r));
        }
        if (!AllFinite(o,frame_*2)) return false;
        for (auto& c:channel_) { std::copy(c.history.begin()+frame_,c.history.end(),c.history.begin()); std::fill(c.history.begin()+frame_,c.history.end(),0.0f); }
        return true;
    }
    // Empty input flushes the final overlap. With a reservoir, the current packet's bytes
    // are joined with the carry from the previous one before any frame is decoded.
    bool Decode(const uint8_t* data,size_t bytes,std::vector<float>& out) override {
        if (!bytes) {
            out.resize(frame_*2);
            for (unsigned i=0;i<frame_;++i) { out[i*2]=channel_[0].history[i]; out[i*2+1]=channel_[channels_-1].history[i]; }
            return true;
        }
        Bits bits(data,bytes);
        if (!reservoir_) return Frame(bits,out);
        bits.Skip(4); unsigned count=(unsigned)bits.Get(4);
        if (!count) return carry_.bits && carry_.Append(bits,bits.Left());
        size_t boundary=(size_t)bits.Get(offsetBits_);
        if (boundary>bits.Left()) return false;
        if (carry_.bits) {
            if (!carry_.Append(bits,boundary)) return false;
            Bits joined(carry_.bytes.data(),carry_.bytes.size(),carry_.bits);
            if (!Frame(joined,out)) return false;
        } else bits.Skip(boundary);
        carry_.Clear(); resetLengths_=true;
        for (unsigned i=1;i<count;++i) if (!Frame(bits,out)) return false;
        return carry_.Append(bits,bits.Left());
    }
    unsigned channels_=0,version_=0,frame_=0,frameBits_=0,book_=0,offsetBits_=0,previous_=0,current_=0,next_=0;
    bool exponentVlc_=false,reservoir_=false,variable_=false,noise_=false,resetLengths_=true;
    unsigned noisePosition_=0; uint32_t noiseSeed_=1;
    std::vector<Block> blocks_;
    Channel channel_[2];
    std::vector<float> wave_,spectrum_[2],quant_[2];
    BitString carry_;
};
} // namespace

std::unique_ptr<Decoder> OpenWmaDecoder(const std::wstring& path) {
    auto d=std::make_unique<WmaDecoder>(); if (!d->Open(path)) return nullptr; return d;
}
