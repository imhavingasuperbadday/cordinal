// Cordinal AAC-LC decoder, from ISO/IEC 14496-3 syntax: ADTS/MP4 input, Huffman spectrum,
// scalefactor/PNS quantization, M/S and intensity stereo, TNS and the long/short filterbank.
// Only the LC object type is supported.
#include "native_audio.h"
#include "audio_live.h"
#include "audio_bits.h"
#include "audio_huffman.h"
#include "audio_packets.h"
#include "audio_mdct.h"
#include "audio_aac_tables.h"
#include <array>
#include <span>

namespace nativeaudio {
namespace {
// Sampling frequencies indexed by sampling_frequency_index; index 15 escapes to an explicit rate.
constexpr unsigned rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
}
// ADTS framing: optionally skip an ID3v2 tag, then walk the fixed 7/9-byte headers. Every
// header must share one object/sampling-index/channel signature and the file must end cleanly.
bool PacketFile::OpenAdts(const std::wstring& path) {
    if (!file.Size() && !file.Open(path)) return false;
    uint64_t at=0;
    if (file.Size()>=10) {
        const uint8_t* p=file.View(0,10); if (!p) return false;
        if (!memcmp(p,"ID3",3)) {
            if ((p[6]|p[7]|p[8]|p[9])&128) return false;
            at=10+((p[6]<<21)|(p[7]<<14)|(p[8]<<7)|p[9])+((p[5]&16)?10:0);
        }
    }
    unsigned configuration=0;
    while (at+7<=file.Size()) {
        const uint8_t* p=file.View(at,7); if (!p || p[0]!=255 || (p[1]&0xF6)!=0xF0) return false;
        unsigned index=(p[2]>>2)&15, ch=((p[2]&1)<<2)|(p[3]>>6), object=(p[2]>>6)+1;
        unsigned n=((p[3]&3)<<11)|(p[4]<<3)|(p[5]>>5), head=(p[1]&1)?7:9;
        if (index>=13 || !ch || ch>7 || n<head || (p[6]&3) || n>file.Size()-at) return false;
        unsigned signature=(object<<8)|(index<<4)|ch;
        if (packets.empty()) {
            configuration=signature; rate=rates[index]; channels=ch==7?8:ch; codec=0x6D703461;
            config={(uint8_t)((object<<3)|(index>>1)),(uint8_t)((index<<7)|(ch<<3))};
        } else if (signature!=configuration) return false;
        packets.push_back({at+head,frames,n-head,1024}); frames+=1024; at+=n;
        if (packets.size()>8000000) return false;
    }
    return !packets.empty() && at==file.Size();
}
} // namespace nativeaudio

namespace {
using namespace nativeaudio;
using namespace nativeaudio::aacdata;
constexpr double pi=3.14159265358979323846;

// The 12 spectral Huffman codebooks, built once as prefix trees at first use.
const std::array<PrefixBook,12>& Books() {
    static const std::array<PrefixBook,12> books={PrefixBook(codes0),PrefixBook(codes1),PrefixBook(codes2),PrefixBook(codes3),PrefixBook(codes4),PrefixBook(codes5),
        PrefixBook(codes6),PrefixBook(codes7),PrefixBook(codes8),PrefixBook(codes9),PrefixBook(codes10),PrefixBook(codes11)};
    return books;
}
// Scalefactor-band edges depend on the sampling-rate index and on short vs long windows.
std::span<const uint16_t> Bands(unsigned index,bool shortBlock) {
    if (shortBlock) {
        if (index<3) return bands139;
        if (index<6) return bands130;
        if (index<8) return bands137;
        if (index<11) return bands135;
        return bands133;
    }
    if (index<2) return bands140;
    if (index==2) return bands138;
    if (index<5) return bands129;
    if (index==5) return bands131;
    if (index<8) return bands136;
    if (index<11) return bands134;
    return bands132;
}
// Individual channel stream: window sequence, short-block grouping and the band layout.
struct Ics {
    unsigned sequence=0,shape=0,max=0,groups=1,group[8]={1};
    uint8_t book[8][64]{};
    int sf[8][64]{};
    std::span<const uint16_t> bands;
    bool Info(Bits& b,unsigned rate) {
        if (b.Get(1)) return false;
        sequence=(unsigned)b.Get(2); shape=(unsigned)b.Get(1); groups=1; std::fill_n(group,8,0); group[0]=1;
        if (sequence==2) {
            max=(unsigned)b.Get(4); unsigned mask=(unsigned)b.Get(7);
            for (int i=6;i>=0;--i) { if (mask&(1<<i)) ++group[groups-1]; else group[groups++]=1; }
        } else { max=(unsigned)b.Get(6); if (b.Get(1)) return false; }
        bands=Bands(rate,sequence==2);
        return b.Good() && max<bands.size();
    }
};
// One temporal-noise-shaping filter: order, direction and reflection coefficients.
struct TnsFilter { unsigned length=0,order=0; bool reverse=false; float a[20]{}; };
// One decoded channel: its ICS info, current spectral lines and the saved overlap tail.
struct Channel {
    Ics info;
    std::array<float,1024> spectrum{},overlap{};
    unsigned previousShape=0;
    std::vector<TnsFilter> tns[8];
};
float Scale(int n) {
    // Every syntax scale is an integral quarter-power of two.
    static const auto table=[] {
        std::array<float,2049> v{};
        for (int i=-1024;i<=1024;++i) v[i+1024]=(float)std::exp2(i*0.25);
        return v;
    }();
    return table[n+1024];
}
// Expand each codebook symbol into base-5/base-33 digits, offset per book so the digits
// become signed quantized values. Built once on first use.
const auto& SpectralTuples() {
    static const auto table=[] {
        std::array<std::array<std::array<int8_t,4>,289>,12> v{};
        for (unsigned book=1;book<12;++book) {
            unsigned dim=book<5?4:2, base=book<5?3:book<7?9:book<9?8:book<11?13:17;
            int offset=book<3?1:book<7 && book>=5?4:0;
            unsigned count=dim==4?81:base*base;
            for (unsigned symbol=0;symbol<count;++symbol) {
                unsigned index=symbol;
                for (unsigned j=dim;j--;) { v[book][symbol][j]=(int8_t)(int(index%base)-offset); index/=base; }
            }
        }
        return v;
    }();
    return table;
}
const std::array<float,8192>& Powers();
const auto& SpectralPowers() {
    static const auto table=[] {
        std::array<std::array<std::array<float,4>,289>,11> v{};
        const auto& tuples=SpectralTuples(); const auto& powers=Powers();
        for (unsigned book=1;book<11;++book) for (unsigned symbol=0;symbol<289;++symbol)
            for (unsigned j=0;j<4;++j) {
                int q=tuples[book][symbol][j];v[book][symbol][j]=(q<0?-1:1)*powers[std::abs(q)];
            }
        return v;
    }();
    return table;
}
struct SignedPowerTables {
    std::array<std::array<float,4>,625> quads;
    std::array<std::array<float,2>,1089> pairs;
};
// Pre-multiplied |value|^(4/3) pairs and quads for the signed codebooks' fast path.
const SignedPowerTables& SignedPowers() {
    static const auto tables=[] {
        SignedPowerTables v{};const auto& powers=Powers();
        for (unsigned i=0;i<v.quads.size();++i) {
            unsigned code=i;
            for (unsigned j=4;j--;) { int q=int(code%5)-2;code/=5;v.quads[i][j]=(q<0?-1:1)*powers[std::abs(q)]; }
        }
        for (unsigned i=0;i<v.pairs.size();++i) {
            int a=int(i/33)-16,b=int(i%33)-16;
            v.pairs[i]={(a<0?-1:1)*powers[std::abs(a)],(b<0?-1:1)*powers[std::abs(b)]};
        }
        return v;
    }();
    return tables;
}
// 12-bit prefix lookup per signed codebook: packs the symbol, its sign bits and the consumed
// bit count so DirectSpectrum can decode without re-walking the tree.
const auto& SignedSpectralLookup() {
    static const auto tables=[] {
        std::array<std::array<uint32_t,4096>,11> v{};
        const auto& books=Books(); const auto& tuples=SpectralTuples();
        for (unsigned book=1;book<11;++book) for (unsigned prefix=0;prefix<4096;++prefix) {
            int node=0; unsigned bits=0;
            while (node>=0 && bits<12) node=books[book].nodes[node].child[(prefix>>(11-bits++))&1];
            if (node>=0) continue;
            unsigned symbol=-node-1,dim=book<5?4:2;
            int values[4];for (unsigned j=0;j<dim;++j) values[j]=tuples[book][symbol][j];
            if (book==3 || book==4 || book>=7) for (unsigned j=0;j<dim;++j) if (values[j]) {
                if (bits==12) { bits=13;break; }
                if ((prefix>>(11-bits++))&1) values[j]=-values[j];
            }
            unsigned tuple=0;
            for (unsigned j=0;j<dim;++j) tuple=tuple*(dim==4?5:33)+values[j]+(dim==4?2:16);
            if (bits<=12) v[book][prefix]=(tuple<<4)|bits;
        }
        return v;
    }();
    return tables;
}
// Decode count spectral lines with codebook book. The lookup short-circuits the common
// case; otherwise the tree walk reads any sign bits in a separate pass.
template<unsigned Dim,bool Signs> __forceinline bool DirectSpectrum(Bits& b,const PrefixBook& book,
    const std::array<std::array<float,4>,289>& powers,const std::array<uint32_t,4096>& lookup,
    const SignedPowerTables& signedPowers,float scale,float* out,unsigned count) {
    if (count%Dim) return false;
    for (unsigned at=0;at<count;at+=Dim) {
        if (b.Left()>=12) if (uint32_t entry=lookup[(size_t)b.Peek(12)]) {
            b.Skip(entry&15);
            __m128 tuple;
            if constexpr (Dim==4) tuple=_mm_loadu_ps(signedPowers.quads[entry>>4].data());
            else tuple=_mm_castsi128_ps(_mm_loadl_epi64((const __m128i*)signedPowers.pairs[entry>>4].data()));
            __m128 scaled=_mm_mul_ps(tuple,_mm_set1_ps(scale));
            if constexpr (Dim==4) _mm_storeu_ps(out+at,scaled);
            else _mm_storel_epi64((__m128i*)(out+at),_mm_castps_si128(scaled));
            continue;
        }
        const auto& tuple=powers[book.Read(b)];
        unsigned flags=0,used=0;
        bool peek=Signs && b.Left()>=Dim;
        if (peek) flags=(unsigned)b.Peek(Dim);
        for (unsigned j=0;j<Dim;++j) {
            float v=tuple[j];
            if constexpr (Signs) if (v) {
                bool negative=peek?((flags>>(Dim-1-used++))&1)!=0:b.Get(1)!=0;
                if (negative) v=-v;
            }
            out[at+j]=v*scale;
        }
        if (peek) b.Skip(used);
    }
    return b.Good();
}
// x^(4/3), the AAC inverse-quantization nonlinearity, evaluated once for 0..8191.
const std::array<float,8192>& Powers() {
    static const auto table=[] { std::array<float,8192> v{}; for (unsigned i=0;i<v.size();++i) v[i]=(float)std::pow((double)i,4.0/3); return v; }();
    return table;
}
// Filterbank window: sine for the low-overlap mode, else a Kaiser-Bessel-derived window
// (alpha 6 for 128 points, 4 for 1024) accumulated to full double precision.
std::vector<float> Window(unsigned n,bool kbd) {
    std::vector<float> w(n);
    if (!kbd) { for (unsigned i=0;i<n;++i) w[i]=(float)std::sin(pi*(i+0.5)/(n*2)); return w; }
    std::vector<double> sum(n+1); double total=0;
    for (unsigned i=0;i<=n;++i) {
        double x=2.0*i/n-1, q=pi*(n==128?6:4)*std::sqrt(std::max(0.0,1-x*x)), term=1, value=1;
        for (int j=1;j<40;++j) { term*=q*q/(4*j*j); value+=term; if (term<1e-16*value) break; }
        sum[i]=total+=value;
    }
    for (unsigned i=0;i<n;++i) w[i]=(float)std::sqrt(sum[i]/total);
    return w;
}

class AacDecoder final : public PacketDecoder {
public:
    bool Open(const std::wstring& path) {
        // Inspect the signature before selecting a parser; both share one bounded file mapping.
        if (!media_.file.Open(path)) return false;
        const uint8_t* head=media_.file.View(0,(size_t)std::min<uint64_t>(8,media_.file.Size()));
        if (!head || media_.file.Size()<7) return false;
        bool adts=(head[0]==255 && (head[1]&0xF6)==0xF0) || !memcmp(head,"ID3",3);
        if (!(adts?media_.OpenAdts(path):media_.OpenMp4(path)) || media_.codec!=0x6D703461) return false;
        return Configure(false);
    }
    bool OpenLive(std::shared_ptr<LiveAudioSource> source) {
        if (!source) return false;
        live_=std::move(source);
        uint8_t head[7];
        if (!ReadLiveBytes(*live_,head,sizeof(head)) || !SkipLiveId3(*live_,head,sizeof(head))) return false;
        unsigned index=0,layout=0,bytes=0,header=0;
        for (size_t skipped=0;;++skipped) {
            if (LiveHeader(head,index,layout,bytes,header)) break;
            if (skipped>=65536) return false;
            memmove(head,head+1,6);
            if (!ReadLiveBytes(*live_,head+6,1)) return false;
        }
        liveSignature_=(index<<4)|layout;
        media_.rate=rates[index]; media_.channels=layout==7?8:layout; media_.codec=AudioAac;
        media_.config={(uint8_t)((2<<3)|(index>>1)),(uint8_t)((index<<7)|(layout<<3))};
        liveFrame_.resize(bytes);
        memcpy(liveFrame_.data(),head,sizeof(head));
        if (!ReadLiveBytes(*live_,liveFrame_.data()+7,bytes-7)) return false;
        liveHeader_=header; liveFirst_=true;
        return Configure(true);
    }
    size_t Read(float* out,size_t frames) override {
        if (!live_) return PacketDecoder::Read(out,frames);
        if (liveFailed_) return 0;
        size_t done=0;
        while (done<frames) {
            if (liveUsed_==livePcm_.size()/2) {
                if (!LiveFrame()) { liveFailed_=true; break; }
            }
            size_t n=std::min(frames-done,livePcm_.size()/2-liveUsed_);
            memcpy(out+done*2,livePcm_.data()+liveUsed_*2,n*2*sizeof(float));
            liveUsed_+=n; done+=n;
        }
        return done;
    }
    bool Seek(double seconds) override { return live_ ? false : PacketDecoder::Seek(seconds); }
    bool FastSeek() const override { return !live_ && PacketDecoder::FastSeek(); }
    double Duration() const override { return live_ ? 0 : PacketDecoder::Duration(); }
    bool Failed() const override { return live_ ? liveFailed_ : PacketDecoder::Failed(); }
    void Abort() override { if (live_) live_->Abort(); }
    std::wstring Problem() const override { return live_ ? live_->Problem() : std::wstring{}; }
private:
    static bool LiveHeader(const uint8_t* p,unsigned& index,unsigned& layout,unsigned& bytes,unsigned& header) {
        if (p[0]!=255 || (p[1]&0xF6)!=0xF0 || (p[2]>>6)!=1 || (p[6]&3)) return false;
        index=(p[2]>>2)&15; layout=((p[2]&1)<<2)|(p[3]>>6);
        bytes=((p[3]&3)<<11)|(p[4]<<3)|(p[5]>>5); header=(p[1]&1)?7:9;
        return index<13 && layout && layout<=7 && bytes>=header;
    }
    bool LiveFrame() {
        if (liveFirst_) liveFirst_=false;
        else {
            uint8_t head[7]; unsigned index=0,layout=0,bytes=0,header=0;
            if (!ReadLiveBytes(*live_,head,sizeof(head)) || !LiveHeader(head,index,layout,bytes,header) ||
                ((index<<4)|layout)!=liveSignature_) return false;
            liveFrame_.resize(bytes);
            memcpy(liveFrame_.data(),head,sizeof(head));
            if (!ReadLiveBytes(*live_,liveFrame_.data()+7,bytes-7)) return false;
            liveHeader_=header;
        }
        livePcm_.clear(); liveUsed_=0;
        return Decode(liveFrame_.data()+liveHeader_,liveFrame_.size()-liveHeader_,livePcm_) && !livePcm_.empty();
    }
    bool Configure(bool live) {
        // config is an AudioSpecificConfig: object type, sampling index and channel layout.
        Bits b(media_.config.data(),media_.config.size());
        unsigned object=(unsigned)b.Get(5); if (object==31) object=32+(unsigned)b.Get(6);
        index_=(unsigned)b.Get(4); unsigned rate=index_==15?(unsigned)b.Get(24):index_<13?rates[index_]:0;
        unsigned layout=(unsigned)b.Get(4);
        // LC only: other AAC tools require additional reconstruction stages.
        if (object!=2 || !rate || b.Get(1)) return false;
        if (b.Get(1)) b.Skip(14);
        if (b.Get(1) || !b.Good()) return false;
        // MPEG-4 channel configurations 1..7, 11, 12 and 14 map to 1..8 channels; 13 is 22.2
        // (24 channels), beyond this decoder's eight channel slots.
        static constexpr uint8_t kLayoutChannels[16] = {0, 1, 2, 3, 4, 5, 6, 8, 0, 0, 0, 7, 8, 0, 8, 0};
        unsigned mapped = layout ? (layout < std::size(kLayoutChannels) ? kLayoutChannels[layout] : 0)
                                 : media_.channels;
        if (!mapped || mapped > 8) return false;
        channels_=mapped;
        if (index_==15) { index_=0; while (index_<13 && rates[index_]!=rate) ++index_; if (index_==13) return false; }
        if (!media_.mp4TimeScale && rate!=media_.rate) return false;
        transform_[0].Init(2048); transform_[1].Init(256);
        for (int shortBlock=0;shortBlock<2;++shortBlock) for (int k=0;k<2;++k) window_[shortBlock][k]=Window(shortBlock?128:1024,k!=0);
        simd_=FastAudioKernels();
        (void)Books(); (void)Powers(); (void)SpectralTuples(); (void)SpectralPowers(); (void)SignedPowers(); (void)SignedSpectralLookup(); (void)Scale(0);
        if (live) { rate_=rate; return true; }
        return Ready(rate,true);
    }
    void Reset() override { for (auto& c:channel_) { c.overlap.fill(0); c.previousShape=0; } }
    size_t Preroll() const override { return 2; }
    // Scalefactor-band sections: each run assigns one codebook to a span of bands.
    bool Sections(Bits& b,Ics& info) {
        unsigned bits=info.sequence==2?3:5, escape=(1u<<bits)-1;
        for (unsigned g=0;g<info.groups;++g) for (unsigned band=0;band<info.max;) {
            unsigned book=(unsigned)b.Get(4), len=0, part;
            if (book==12) return false;
            do { part=(unsigned)b.Get(bits); len+=part; if (!b.Good() || len>info.max-band) return false; } while (part==escape);
            if (!len) return false;
            std::fill_n(info.book[g]+band,len,(uint8_t)book); band+=len;
        }
        return b.Good();
    }
    // Scalefactors per section, differentially coded per codebook class. Intensity (>=14)
    // and noise (13) bands carry their own running prediction across bands.
    bool Factors(Bits& b,Ics& info,int gain) {
        int normal=gain,intensity=0,noise=gain-90; bool firstNoise=true;
        for (unsigned g=0;g<info.groups;++g) for (unsigned s=0;s<info.max;++s) {
            unsigned book=info.book[g][s]; int& v=info.sf[g][s]; v=0;
            if (!book) continue;
            if (book>=14) { intensity+=Books()[0].Read(b)-60; v=intensity; if (v<-1024 || v>1024) return false; }
            else if (book==13) { noise+=firstNoise?(int)b.Get(9)-256:Books()[0].Read(b)-60; firstNoise=false; v=noise; if (v<-100 || v>255) return false; }
            else { normal+=Books()[0].Read(b)-60; if (normal<0 || normal>255) return false; v=normal; }
        }
        return b.Good();
    }
    // TNS: per-window filters whose integer coefficients are mapped through a sine curve.
    bool Tns(Bits& b,Channel& c) {
        bool shortBlock=c.info.sequence==2;
        for (unsigned w=0;w<(shortBlock?8u:1u);++w) {
            unsigned count=(unsigned)b.Get(shortBlock?1:2);
            unsigned resolution=count?(unsigned)b.Get(1)+3:0;
            for (unsigned f=0;f<count;++f) {
                TnsFilter filter; filter.length=(unsigned)b.Get(shortBlock?4:6); filter.order=(unsigned)b.Get(shortBlock?3:5);
                if (filter.order>(shortBlock?7u:12u)) return false;
                if (filter.order) {
                    filter.reverse=b.Get(1)!=0; unsigned width=resolution-(unsigned)b.Get(1);
                    for (unsigned j=0;j<filter.order;++j) {
                        int v=(int)b.Signed(width);
                        float r=(float)std::sin(pi*v/((1u<<resolution)+(v<0?1:-1)));
                        float next[20];
                        for (unsigned k=0;k<j;++k) next[k]=filter.a[k]+r*filter.a[j-1-k];
                        std::copy_n(next,j,filter.a); filter.a[j]=r;
                    }
                }
                c.tns[w].push_back(filter);
            }
        }
        return b.Good();
    }
    // Quantized spectrum. Books <11 use the direct fast path; 11 adds escape codes; 13 (PNS)
    // synthesizes random lines normalized to the band's energy.
    bool Spectrum(Bits& b,Channel& c,const std::vector<std::pair<unsigned,unsigned>>& pulses) {
        const Ics& info=c.info; unsigned window=0, size=info.sequence==2?128:1024;
        // Only bands decoded through the integer path (escape book or pulse data)
        // read quant, and every such line is written before the second pass. A pulse
        // may still point at a band that was skipped (zero or noise codebook), so
        // clear the array whenever pulses can touch it.
        std::array<int,1024> quant;
        if (!pulses.empty()) quant.fill(0);
        const auto& books=Books(); const auto& tuples=SpectralTuples(); const auto& powers=Powers();
        const auto& directPowers=SpectralPowers();
        const auto& signedLookup=SignedSpectralLookup();
        const auto& signedPowers=SignedPowers();
        c.spectrum.fill(0);
        for (unsigned g=0;g<info.groups;++g) {
            for (unsigned s=0;s<info.max;++s) {
                unsigned book=info.book[g][s]; if (!book || book>=13) continue;
                if (book<11 && pulses.empty()) {
                    unsigned start=info.bands[s],count=info.bands[s+1]-start;
                    float scale=Scale(info.sf[g][s]-100)*(1.0f/32768);
                    for (unsigned w=0;w<info.group[g];++w) {
                        float* dst=c.spectrum.data()+(window+w)*size+start;
                        bool ok;
                        if (book<3) ok=DirectSpectrum<4,false>(b,books[book],directPowers[book],signedLookup[book],signedPowers,scale,dst,count);
                        else if (book<5) ok=DirectSpectrum<4,true>(b,books[book],directPowers[book],signedLookup[book],signedPowers,scale,dst,count);
                        else if (book<7) ok=DirectSpectrum<2,false>(b,books[book],directPowers[book],signedLookup[book],signedPowers,scale,dst,count);
                        else ok=DirectSpectrum<2,true>(b,books[book],directPowers[book],signedLookup[book],signedPowers,scale,dst,count);
                        if (!ok) return false;
                    }
                    continue;
                }
                unsigned dim=book<5?4:2;
                bool signs=book==3 || book==4 || book>=7;
                for (unsigned w=0;w<info.group[g];++w) for (unsigned at=info.bands[s];at<info.bands[s+1];at+=dim) {
                    int index=books[book].Read(b), v[4];
                    for (unsigned j=0;j<4;++j) v[j]=tuples[book][index][j];
                    if (signs) for (unsigned j=0;j<dim;++j) if (v[j] && b.Get(1)) v[j]=-v[j];
                    if (book==11) for (unsigned j=0;j<dim;++j) if (v[j]==16 || v[j]==-16) {
                        unsigned n=4; while (b.Get(1) && n<13) ++n;
                        if (n>12) return false;
                        int value=(1<<n)+(int)b.Get(n); v[j]=v[j]<0?-value:value;
                    }
                    if (!b.Good() || at+dim>info.bands[s+1]) return false;
                    for (unsigned j=0;j<dim;++j) quant[(window+w)*size+at+j]=v[j];
                }
            }
            window+=info.group[g];
        }
        for (auto pulse:pulses) { int& q=quant[pulse.first]; q+=q>0?(int)pulse.second:-(int)pulse.second; }
        window=0;
        for (unsigned g=0;g<info.groups;++g) {
            for (unsigned s=0;s<info.max;++s) {
                unsigned book=info.book[g][s]; if (!book || book>=14) continue;
                if (book<11 && pulses.empty()) continue;
                float scale=Scale(info.sf[g][s]-100)*(1.0f/32768);
                for (unsigned w=0;w<info.group[g];++w) {
                    float* dst=c.spectrum.data()+(window+w)*size;
                    if (book==13) {
                        double energy=0;
                        for (unsigned i=info.bands[s];i<info.bands[s+1];++i) {
                            random_=random_*1664525u+1013904223u;
                            dst[i]=(float)(int32_t)random_; energy+=double(dst[i])*dst[i];
                        }
                        float norm=scale/(float)std::sqrt(std::max(energy,1.0));
                        for (unsigned i=info.bands[s];i<info.bands[s+1];++i) dst[i]*=norm;
                    } else for (unsigned i=info.bands[s];i<info.bands[s+1];++i) {
                        int v=quant[(window+w)*size+i]; if (std::abs(v)>8191) return false;
                        dst[i]=(v<0?-1:1)*powers[std::abs(v)]*scale;
                    }
                }
            }
            window+=info.group[g];
        }
        return b.Good();
    }
    // One channel's raw stream: ICS info (unless shared), sections, scalefactors, pulses,
    // TNS and the spectrum.
    bool Stream(Bits& b,Channel& c,bool common) {
        int gain=(int)b.Get(8);
        for (auto& filters:c.tns) filters.clear();
        if (!common && !c.info.Info(b,index_)) return false;
        if (!Sections(b,c.info) || !Factors(b,c.info,gain)) return false;
        std::vector<std::pair<unsigned,unsigned>> pulses;
        if (b.Get(1)) {
            if (c.info.sequence==2) return false;
            unsigned n=(unsigned)b.Get(2)+1, start=(unsigned)b.Get(6);
            if (start+1>=c.info.bands.size()) return false;
            unsigned at=c.info.bands[start];
            for (unsigned i=0;i<n;++i) { at+=(unsigned)b.Get(5); unsigned value=(unsigned)b.Get(4); if (at>=1024) return false; pulses.emplace_back(at,value); }
        }
        if (b.Get(1) && !Tns(b,c)) return false;
        if (b.Get(1)) return false; // SSR gain control is not an LC syntax tool.
        return Spectrum(b,c,pulses);
    }
    // Apply M/S or intensity stereo. Intensity/noise bands copy the left lines scaled by the
    // right-channel factors; normal M/S replaces the pair by their sum and difference.
    void Stereo(Channel& left,Channel& right,unsigned mask,const bool used[8][64]) {
        const Ics& info=left.info; unsigned window=0,size=info.sequence==2?128:1024;
        for (unsigned g=0;g<info.groups;++g) {
            for (unsigned s=0;s<info.max;++s) {
                unsigned lb=info.book[g][s], rb=right.info.book[g][s]; bool ms=mask==2 || used[g][s];
                bool intensity=rb>=14,noise=lb==13 && rb==13 && ms;
                if (!intensity && !noise && !(lb!=13 && rb!=13 && ms)) continue;
                float factor=intensity?Scale(-right.info.sf[g][s])*(rb==14?-1:1)*(mask==1 && ms?-1:1):
                             noise?Scale(right.info.sf[g][s]-info.sf[g][s]):1;
                for (unsigned w=0;w<info.group[g];++w) {
                    float* l=left.spectrum.data()+(window+w)*size;
                    float* r=right.spectrum.data()+(window+w)*size;
                    unsigned i=info.bands[s],end=info.bands[s+1];
                    if (intensity || noise) {
                        for (;i+4<=end;i+=4) _mm_storeu_ps(r+i,_mm_mul_ps(_mm_loadu_ps(l+i),_mm_set1_ps(factor)));
                        for (;i<end;++i) r[i]=l[i]*factor;
                    } else {
                        for (;i+4<=end;i+=4) {
                            __m128 a=_mm_loadu_ps(l+i),b=_mm_loadu_ps(r+i);
                            _mm_storeu_ps(l+i,_mm_add_ps(a,b));_mm_storeu_ps(r+i,_mm_sub_ps(a,b));
                        }
                        for (;i<end;++i) { float v=l[i];l[i]=v+r[i];r[i]=v-r[i]; }
                    }
                }
            }
            window+=info.group[g];
        }
    }
    // Run each TNS filter in place as an all-pole synthesis filter, forward or reversed.
    void ApplyTns(Channel& c) {
        static constexpr unsigned maxLong[]={31,31,34,40,42,51,46,46,42,42,42,39,39};
        static constexpr unsigned maxShort[]={9,9,10,14,14,14,14,14,14,14,14,14,14};
        const Ics& info=c.info; bool shortBlock=info.sequence==2; unsigned size=shortBlock?128:1024;
        for (unsigned w=0;w<(shortBlock?8u:1u);++w) {
            unsigned bottom=(unsigned)info.bands.size()-1;
            for (const auto& f:c.tns[w]) {
                unsigned top=bottom; bottom-=std::min(bottom,f.length);
                unsigned limit=std::min(info.max,shortBlock?maxShort[index_]:maxLong[index_]);
                unsigned from=info.bands[std::min(bottom,limit)],to=info.bands[std::min(top,limit)];
                float history[20]{};
                for (unsigned i=0;f.order && i<to-from;++i) {
                    unsigned at=w*size+(f.reverse?to-i-1:from+i); float v=c.spectrum[at];
                    for (unsigned j=0;j<f.order;++j) v-=history[j]*f.a[j];
                    for (unsigned j=f.order-1;j>0;--j) history[j]=history[j-1];
                    history[0]=v; c.spectrum[at]=v;
                }
            }
        }
    }
    // Inverse filterbank with overlap-add. Long blocks use one DCT, short blocks run eight
    // 128-point transforms, and the two mixed sequences splice long and short windows.
    void Synthesize(Channel& c,float* out) {
        const auto& info=c.info;
        if (info.sequence==0) {
            // Window the four symmetric DCT quarters directly into output and
            // overlap. No expanded IMDCT or intermediate windowed block is needed,
            // so this path allocates neither of the shared work blocks below.
            float transformed[1024];
            transform_[0].Dct(c.spectrum.data(),transformed);
            const float* left=window_[0][c.previousShape].data();
            const float* right=window_[0][info.shape].data();
            if (simd_) AudioAacLongWindowAvx2(transformed,left,right,c.overlap.data(),out,c.overlap.data());
            else {
                const __m128 scale=_mm_set1_ps(1.0f/1024),negative=_mm_set1_ps(-1.0f/1024);
                for (unsigned i=0;i<512;i+=4) {
                    __m128 a=_mm_loadu_ps(transformed+512+i),b=_mm_shuffle_ps(_mm_loadu_ps(transformed+1020-i),_mm_loadu_ps(transformed+1020-i),0x1B);
                    __m128 d=_mm_loadu_ps(transformed+i),e=_mm_shuffle_ps(_mm_loadu_ps(transformed+508-i),_mm_loadu_ps(transformed+508-i),0x1B);
                    _mm_storeu_ps(out+i,_mm_add_ps(_mm_loadu_ps(c.overlap.data()+i),_mm_mul_ps(a,_mm_mul_ps(_mm_loadu_ps(left+i),scale))));
                    _mm_storeu_ps(out+512+i,_mm_add_ps(_mm_loadu_ps(c.overlap.data()+512+i),_mm_mul_ps(b,_mm_mul_ps(_mm_loadu_ps(left+512+i),negative))));
                    __m128 r0=_mm_shuffle_ps(_mm_loadu_ps(right+1020-i),_mm_loadu_ps(right+1020-i),0x1B);
                    __m128 r1=_mm_shuffle_ps(_mm_loadu_ps(right+508-i),_mm_loadu_ps(right+508-i),0x1B);
                    _mm_storeu_ps(c.overlap.data()+i,_mm_mul_ps(e,_mm_mul_ps(r0,negative)));
                    _mm_storeu_ps(c.overlap.data()+512+i,_mm_mul_ps(d,_mm_mul_ps(r1,negative)));
                }
            }
            c.previousShape=info.shape;return;
        }
        float block[2048],transformed[2048];
        if (info.sequence==2) {
            std::fill_n(block,2048,0.0f);
            for (unsigned w=0;w<8;++w) {
                transform_[1].Dct(c.spectrum.data()+w*128,transformed);
                const auto& left=window_[1][w?info.shape:c.previousShape]; const auto& right=window_[1][info.shape];
                float* dst=block+448+w*128;
                const __m128 scale=_mm_set1_ps(1.0f/128),negative=_mm_set1_ps(-1.0f/128);
                for (unsigned i=0;i<64;i+=4) {
                    __m128 a=_mm_loadu_ps(transformed+64+i),b=_mm_shuffle_ps(_mm_loadu_ps(transformed+124-i),_mm_loadu_ps(transformed+124-i),0x1B);
                    __m128 d=_mm_loadu_ps(transformed+i),e=_mm_shuffle_ps(_mm_loadu_ps(transformed+60-i),_mm_loadu_ps(transformed+60-i),0x1B);
                    __m128 r0=_mm_shuffle_ps(_mm_loadu_ps(right.data()+124-i),_mm_loadu_ps(right.data()+124-i),0x1B);
                    __m128 r1=_mm_shuffle_ps(_mm_loadu_ps(right.data()+60-i),_mm_loadu_ps(right.data()+60-i),0x1B);
                    _mm_storeu_ps(dst+i,_mm_add_ps(_mm_loadu_ps(dst+i),_mm_mul_ps(a,_mm_mul_ps(_mm_loadu_ps(left.data()+i),scale))));
                    _mm_storeu_ps(dst+64+i,_mm_add_ps(_mm_loadu_ps(dst+64+i),_mm_mul_ps(b,_mm_mul_ps(_mm_loadu_ps(left.data()+64+i),negative))));
                    _mm_storeu_ps(dst+128+i,_mm_add_ps(_mm_loadu_ps(dst+128+i),_mm_mul_ps(e,_mm_mul_ps(r0,negative))));
                    _mm_storeu_ps(dst+192+i,_mm_add_ps(_mm_loadu_ps(dst+192+i),_mm_mul_ps(d,_mm_mul_ps(r1,negative))));
                }
            }
        } else {
            transform_[0].Run(c.spectrum.data(),transformed);
            const auto& longL=window_[0][c.previousShape]; const auto& longR=window_[0][info.shape];
            const auto& shortL=window_[1][c.previousShape]; const auto& shortR=window_[1][info.shape];
            for (unsigned i=0;i<1024;++i) {
                float l=longL[i],r=longR[1023-i];
                if (info.sequence==3) l=i<448?0:i<576?shortL[i-448]:1;
                if (info.sequence==1) r=i<448?1:i<576?shortR[575-i]:0;
                block[i]=transformed[i]*l*(1.0f/1024); block[i+1024]=transformed[i+1024]*r*(1.0f/1024);
            }
        }
        for (unsigned i=0;i<1024;++i) { out[i]=block[i]+c.overlap[i]; c.overlap[i]=block[i+1024]; }
        c.previousShape=info.shape;
    }
    // Program config element; only its channel count is checked against the header.
    bool Pce(Bits& b) {
        b.Skip(4+2+4); unsigned front=(unsigned)b.Get(4),side=(unsigned)b.Get(4),back=(unsigned)b.Get(4),lfe=(unsigned)b.Get(2);
        unsigned assoc=(unsigned)b.Get(3),coupled=(unsigned)b.Get(4);
        if (b.Get(1)) b.Skip(4); if (b.Get(1)) b.Skip(4); if (b.Get(1)) b.Skip(3);
        unsigned count=lfe;
        for (unsigned i=0;i<front+side+back;++i) { count+=b.Get(1)?2:1; b.Skip(4); }
        b.Skip(lfe*4+assoc*4+coupled*5); b.Align(); size_t n=(size_t)b.Get(8); b.Skip(n*8);
        return b.Good() && count==channels_;
    }
    bool Decode(const uint8_t* p,size_t bytes,std::vector<float>& out) override {
        // A raw_data_block is a 3-bit-tagged sequence of elements that fill channels in order.
        Bits b(p,bytes); unsigned channels=0;
        // Packet-derived PNS seed makes seeks reproducible without a song-sized state cache.
        random_=2166136261u; for (size_t i=0;i<bytes;++i) random_=(random_^p[i])*16777619u;
        while (b.Good() && b.Left()>=3) {
            unsigned tag=(unsigned)b.Get(3); if (tag==7) break;
            if (tag==6) {
                size_t n=(size_t)b.Get(4); if (n==15) n+=b.Get(8)-1;
                if (n && b.Left()>=4 && (b.Peek(4)==13 || b.Peek(4)==14)) return false; // SBR requires its own synthesis stage.
                b.Skip(n*8); continue;
            }
            if (tag==4) { b.Skip(4); bool align=b.Get(1)!=0; size_t n=(size_t)b.Get(8); if (n==255) n+=b.Get(8); if (align) b.Align(); b.Skip(n*8); continue; }
            if (tag==5) { if (!Pce(b)) return false; continue; }
            if (tag!=0 && tag!=1 && tag!=3) return false;
            b.Skip(4); unsigned n=tag==1?2:1; if (channels+n>channels_) return false;
            bool common=n==2 && b.Get(1); unsigned mask=0; bool used[8][64]{};
            if (common) {
                if (!channel_[channels].info.Info(b,index_)) return false;
                channel_[channels+1].info=channel_[channels].info;
                mask=(unsigned)b.Get(2); if (mask==3) return false;
                if (mask==1) for (unsigned g=0;g<channel_[channels].info.groups;++g)
                    for (unsigned s=0;s<channel_[channels].info.max;++s) used[g][s]=b.Get(1)!=0;
            }
            for (unsigned j=0;j<n;++j) if (!Stream(b,channel_[channels+j],common)) return false;
            if (common) Stereo(channel_[channels],channel_[channels+1],mask,used);
            channels+=n;
        }
        if (!b.Good() || channels!=channels_) return false;
        std::array<std::array<float,1024>,8> pcm;
        for (unsigned c=0;c<channels_;++c) { ApplyTns(channel_[c]); Synthesize(channel_[c],pcm[c].data()); }
        // Map each channel layout from AAC order to Cordinal's downmix order.
        static constexpr uint8_t order[9][8]={{},{0},{0,1},{1,2,0},{1,2,0,3},{1,2,0,3,4},
            {1,2,0,5,3,4},{1,2,0,6,3,4,5},{1,2,0,7,5,6,3,4}};
        out.resize(2048);
        if (channels_<=2) return InterleaveStereo(pcm[0].data(),pcm[channels_-1].data(),out.data(),1024);
        for (unsigned i=0;i<1024;++i) {
            float v[8]{};
            for (unsigned c=0;c<channels_;++c) { v[c]=pcm[order[channels_][c]][i]; if (!std::isfinite(v[c])) return false; }
            Mix(v,channels_,out.data()+i*2);
        }
        return true;
    }
    unsigned channels_=0,index_=0;
    std::shared_ptr<LiveAudioSource> live_;
    std::vector<uint8_t> liveFrame_;
    std::vector<float> livePcm_;
    size_t liveUsed_=0;
    unsigned liveSignature_=0,liveHeader_=0;
    bool liveFirst_=false,liveFailed_=false;
    bool simd_=false;
    uint32_t random_=0;
    Channel channel_[8];
    Mdct transform_[2];
    std::vector<float> window_[2][2];
};
} // namespace

std::unique_ptr<Decoder> OpenAacDecoder(const std::wstring& path) {
    auto d=std::make_unique<AacDecoder>(); if (!d->Open(path)) return nullptr; return d;
}

std::unique_ptr<Decoder> OpenLiveAac(std::shared_ptr<LiveAudioSource> source) {
    auto d=std::make_unique<AacDecoder>(); if (!d->OpenLive(std::move(source))) return nullptr; return d;
}
