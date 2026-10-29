// Cordinal Matroska/WebM audio demuxer. EBML and block lacing follow RFC 8794/9559.
// Selects the first preferred enabled audio track, computes per-packet timestamps and
// decodes codec-private headers to samples; PCM tracks are handled directly here.
#include "native_audio.h"
#include "audio_packets.h"
#include "audio_bits.h"
#include "audio_opus_packet.h"
#include <array>
#include <span>

namespace nativeaudio {
namespace {
// One EBML element; `first`/`end` bound its payload and `unknown` marks an open-ended size.
struct Element { uint32_t id=0;uint64_t first=0,end=0;bool unknown=false; };
// Decode an EBML variable-length integer. Identifiers keep the leading marker bit; sizes
// strip it. An all-ones pattern is the "unknown size" case handled by the caller.
bool Vint(std::span<const uint8_t> bytes,size_t& at,uint64_t& value,unsigned& length,bool identifier=false) {
    if (at==bytes.size() || !bytes[at]) return false;
    uint8_t head=bytes[at++],marker=128;length=1;
    while (!(head&marker)) { marker>>=1;++length; }
    if (length>(identifier?4u:8u) || length-1>bytes.size()-at) return false;
    value=identifier?head:head&(marker-1);
    for (unsigned i=1;i<length;++i) value=(value<<8)|bytes[at++];
    return true;
}
// Read the next element at `at`, resolving unknown-sized elements to the parent's end.
bool Next(File& file,uint64_t at,uint64_t end,Element& e) {
    if (at>=end) return false;
    const uint8_t* p=file.View(at,(size_t)std::min<uint64_t>(12,end-at));if (!p) return false;
    std::span bytes(p,(size_t)std::min<uint64_t>(12,end-at));size_t used=0;uint64_t id,size;unsigned a,b;
    if (!Vint(bytes,used,id,a,true) || !Vint(bytes,used,size,b)) return false;
    bool unknown=size==((uint64_t(1)<<(b*7))-1);
    if (!unknown && size>end-at-used) return false;
    e={(uint32_t)id,at+used,unknown?end:at+used+size,unknown};return true;
}
// Element value readers; any failure flips `good` instead of throwing.
uint64_t Unsigned(File& file,const Element& e,bool& good) {
    uint64_t n=e.end-e.first;if (!n || n>8) { good=false;return 0; }
    const uint8_t* p=file.View(e.first,(size_t)n);if (!p) { good=false;return 0; }
    uint64_t value=0;for (unsigned i=0;i<n;++i) value=(value<<8)|p[i];return value;
}
int64_t Signed(File& file,const Element& e,bool& good) {
    uint64_t value=Unsigned(file,e,good);unsigned bits=(unsigned)(e.end-e.first)*8;
    return good?(bits==64?(int64_t)value:(int64_t)(value<<(64-bits))>>(64-bits)):0;
}
double Float(File& file,const Element& e,bool& good) {
    uint64_t n=e.end-e.first;if (n!=4 && n!=8) { good=false;return 0; }
    const uint8_t* p=file.View(e.first,(size_t)n);if (!p) { good=false;return 0; }
    if (n==4) { uint32_t bits=U32(p,true);float value;memcpy(&value,&bits,4);return value; }
    uint64_t bits=U64(p,true);double value;memcpy(&value,&bits,8);return value;
}
bool Binary(File& file,const Element& e,std::vector<uint8_t>& out,size_t maximum) {
    if (e.end-e.first>maximum) return false;
    const uint8_t* p=file.View(e.first,(size_t)(e.end-e.first));if (!p) return false;
    out.assign(p,p+(size_t)(e.end-e.first));return true;
}
// Iterate a parent's children in order; a rejected child or bad size stops the walk.
bool Children(File& file,const Element& parent,auto&& visit) {
    uint64_t at=parent.first;
    while (at<parent.end) {
        Element e;if (!Next(file,at,parent.end,e) || e.unknown || !visit(e)) return false;
        at=e.end;
    }
    return true;
}
// Map a Matroska CodecID string to a Cordinal codec (0x160 keeps the WMA format tag).
uint32_t Codec(const std::string& id) {
    if (id=="A_AAC" || id=="A_AAC/MPEG2/LC" || id=="A_AAC/MPEG4/LC") return AudioAac;
    if (id=="A_ALAC") return AudioAlac;
    if (id=="A_OPUS") return AudioOpus;
    if (id=="A_FLAC") return AudioFlac;
    if (id=="A_VORBIS") return AudioVorbis;
    if (id=="A_MPEG/L1" || id=="A_MPEG/L2" || id=="A_MPEG/L3") return AudioMpeg;
    if (id=="A_AC3") return AudioAc3;
    if (id=="A_PCM/INT/LIT") return AudioPcmLE;
    if (id=="A_PCM/INT/BIG") return AudioPcmBE;
    if (id=="A_PCM/FLOAT/IEEE") return AudioPcmFloat;
    if (id=="A_MS/ACM") return 0x160;
    return 0;
}
// Selected audio track: codec, layout and the codec-private config/prefix bytes.
struct Track {
    uint64_t number=0,type=0,delay=0,scope=1,compression=UINT64_MAX;
    uint32_t codec=0,channels=1,bits=0,rate=8000;
    bool enabled=true,preferred=true,encoded=false,encrypted=false;
    std::vector<uint8_t> config,prefix;
};
// Parse a TrackEntry. Encrypted or non-headerless compressed entries invalidate the codec.
bool ReadTrack(File& file,const Element& element,Track& t) {
    bool good=true;
    auto integer=[&](const Element& e) { return Unsigned(file,e,good); };
    auto audio=[&](const Element& e) {
        if (e.id==0xb5 || e.id==0x78b5) {
            double value=Float(file,e,good);if (!std::isfinite(value) || value<1 || value>768000 || value!=std::floor(value)) return false;
            t.rate=(uint32_t)value;
        } else if (e.id==0x9f) { uint64_t v=integer(e);if (!v || v>32) return false;t.channels=(uint32_t)v; }
        else if (e.id==0x6264) { uint64_t v=integer(e);if (!v || v>64) return false;t.bits=(uint32_t)v; }
        return good;
    };
    auto encoding=[&](const Element& e) {
        if (e.id==0x5031 && integer(e)!=0) return false;
        if (e.id==0x5032) t.scope=integer(e);
        if (e.id==0x5033 && integer(e)) t.encrypted=true;
        if (e.id==0x5035) t.encrypted=true;
        if (e.id==0x5034) {
            t.compression=0;
            if (!Children(file,e,[&](const Element& item) {
                if (item.id==0x4254) t.compression=integer(item);
                if (item.id==0x4255 && !Binary(file,item,t.prefix,65536)) return false;
                return good;
            })) return false;
        }
        return good;
    };
    bool parsed=Children(file,element,[&](const Element& e) {
        switch (e.id) {
        case 0xd7:t.number=integer(e);break;
        case 0x83:t.type=integer(e);break;
        case 0xb9:t.enabled=integer(e)!=0;break;
        case 0x88:t.preferred=integer(e)!=0;break;
        case 0x56aa:t.delay=integer(e);if (t.delay>1000000000000000000ull) return false;break;
        case 0x23314f:if (Float(file,e,good)!=1) return false;break;
        case 0x86: {
            std::vector<uint8_t> id;if (!Binary(file,e,id,128)) return false;
            t.codec=Codec(std::string(id.begin(),id.end()));break;
        }
        case 0x63a2:if (!Binary(file,e,t.config,16777216)) return false;break;
        case 0xe1:if (!Children(file,e,audio)) return false;break;
        case 0x6d80:
            if (!Children(file,e,[&](const Element& item) {
                if (item.id!=0x6240) return true;
                if (t.encoded) return false;t.encoded=true;return Children(file,item,encoding);
            })) return false;
            break;
        }
        return good;
    });
    if (t.encoded && (t.encrypted || t.compression!=3 || t.scope!=1)) t.codec=0;
    return parsed && good && t.number;
}
// Read one (Simple)Block: track, signed cluster offset, lacing mode and payload sizes.
// Sizes split into packets and any attached padding becomes start/end trim.
bool Block(PacketFile& out,const Element& element,uint64_t track,int64_t cluster,uint64_t scale,int64_t padding,
           bool& seenPadding,int64_t& lastTime) {
    if (element.end-element.first>32*1024*1024 || element.end-element.first<4) return false;
    const uint8_t* p=out.file.View(element.first,(size_t)(element.end-element.first));if (!p) return false;
    std::span bytes(p,(size_t)(element.end-element.first));size_t at=0;uint64_t number;unsigned length;
    if (!Vint(bytes,at,number,length) || bytes.size()-at<3) return false;
    if (number!=track) return true;
    if (seenPadding) return false;
    int64_t relative=(int16_t)U16(p+at,true);unsigned lacing=(p[at+2]>>1)&3;at+=3;
    if (cluster>INT64_MAX-std::max<int64_t>(relative,0)) return false;
    int64_t time=cluster+relative;
    if (time<lastTime || time>INT64_MAX/(int64_t)scale) return false;lastTime=time;
    out.timestamps.emplace_back(out.packets.size(),time*(int64_t)scale);
    unsigned count=1;
    if (lacing) { if (at==bytes.size()) return false;count=p[at++]+1;if (count<2) return false; }
    std::array<uint32_t,256> sizes{};uint64_t sum=0;
    if (lacing==2) {
        if ((bytes.size()-at)%count) return false;
        for (unsigned i=0;i<count;++i) sizes[i]=(uint32_t)((bytes.size()-at)/count);
    } else {
        int64_t previous=0;
        for (unsigned i=0;i+1<count;++i) {
            uint64_t size=0;
            if (lacing==1) {
                unsigned part;
                do { if (at==bytes.size()) return false;part=p[at++];size+=part; } while (part==255);
            } else if (lacing==3) {
                uint64_t value;if (!Vint(bytes,at,value,length)) return false;
                int64_t current=i?previous+(int64_t)value-((int64_t(1)<<(length*7-1))-1):(int64_t)value;
                if (current<0) return false;size=(uint64_t)current;previous=current;
            }
            if (size>16777216) return false;sizes[i]=(uint32_t)size;sum+=size;
        }
        if (sum>bytes.size()-at) return false;sizes[count-1]=(uint32_t)(bytes.size()-at-sum);
    }
    for (unsigned i=0;i<count;++i) {
        if (sizes[i]>16777216-out.prefix.size() || sizes[i]>bytes.size()-at || out.packets.size()>=8000000) return false;
        out.packets.push_back({element.first+at,0,sizes[i]+(uint32_t)out.prefix.size(),0});at+=sizes[i];
    }
    if (padding) {
        if (padding>0) { out.trimEndNs=(uint64_t)padding;seenPadding=true; }
        else {
            if (out.packets.size()!=count || padding==INT64_MIN) return false;
            out.trimStartNs=(uint64_t)-padding;
        }
        if (out.trimStartNs>1000000000000000000ull || out.trimEndNs>1000000000000000000ull) return false;
    }
    return at==bytes.size();
}
}
// Walk the EBML header, Segment, Tracks and Clusters. With index=false the walk stops
// after the track is known (used to probe the codec).
bool PacketFile::OpenMatroska(const std::wstring& path,bool index) {
    if (!file.Size() && !file.Open(path)) return false;
    Element header;if (!Next(file,0,file.Size(),header) || header.id!=0x1a45dfa3 || header.unknown) return false;
    bool good=true,document=false;
    if (!Children(file,header,[&](const Element& e) {
        if (e.id==0x4282) { std::vector<uint8_t> value;if (!Binary(file,e,value,16)) return false;std::string name(value.begin(),value.end());document=name=="matroska" || name=="webm"; }
        if (e.id==0x42f7 && Unsigned(file,e,good)>1) return false;
        if (e.id==0x42f2 && Unsigned(file,e,good)>4) return false;
        if (e.id==0x42f3 && Unsigned(file,e,good)>8) return false;
        return good;
    }) || !document) return false;
    uint64_t at=header.end;Element segment;
    while (at<file.Size()) { if (!Next(file,at,file.Size(),segment)) return false;if (segment.id==0x18538067) break;if (segment.unknown) return false;at=segment.end; }
    if (segment.id!=0x18538067) return false;
    Track selected;std::vector<Element> clusters;uint64_t scale=1000000;
    at=segment.first;
    while (at<segment.end) {
        Element e;if (!Next(file,at,segment.end,e)) return false;
        if (e.id==0x1654ae6b) {
            unsigned tracks=0;
            if (!Children(file,e,[&](const Element& entry) {
                if (entry.id!=0xae) return true;if (++tracks>256) return false;
                Track t;if (!ReadTrack(file,entry,t)) return false;
                if (t.type==2 && t.enabled && t.codec && (!selected.number || (t.preferred && !selected.preferred))) selected=std::move(t);
                return true;
            })) return false;
        } else if (e.id==0x1549a966) {
            if (!Children(file,e,[&](const Element& item) { if (item.id==0x2ad7b1) scale=Unsigned(file,item,good);return good && scale && scale<=1000000000; })) return false;
        } else if (e.id==0x1f43b675) {
            if (!index && selected.number) break;
            if (clusters.size()>=1000000) return false;
            if (e.unknown) {
                // Unknown-sized clusters end at the next Segment child.
                uint64_t scan=e.first;
                while (scan<e.end) {
                    Element child;if (!Next(file,scan,e.end,child)) return false;
                    if (child.id==0x1f43b675 || child.id==0x1654ae6b || child.id==0x1549a966 || child.id==0x1c53bb6b || child.id==0x1254c367 || child.id==0x114d9b74) break;
                    if (child.unknown) return false;scan=child.end;
                }
                e.end=scan;
            }
            clusters.push_back(e);
        } else if (e.unknown) return false;
        at=e.end;
    }
    if (!selected.number) return false;
    codec=selected.codec;rate=selected.rate;channels=selected.channels;sampleBits=selected.bits;
    config=std::move(selected.config);prefix=std::move(selected.prefix);delayNs=selected.delay;matroska=true;timestampScale=scale;
    if (codec==0x160) {
        if (config.size()<18 || U16(config.data()+16)>config.size()-18) return false;
        codec=U16(config.data());channels=U16(config.data()+2);rate=U32(config.data()+4);
        if (codec!=0x160 && codec!=0x161) return false;
    }
    if (codec==AudioAlac) {
        if (config.size()>=36 && Is(config.data()+4,"alac")) config.erase(config.begin(),config.begin()+12);
        if (config.size()<24) return false;rate=U32(config.data()+20,true);channels=config[9];
    }
    if (codec==AudioOpus) {
        if (config.size()!=19 || memcmp(config.data(),"OpusHead",8) || config[8]>15 || !config[9] || config[9]>2 || config[18]) return false;
        rate=48000;channels=config[9];if (!delayNs) skip=U16(config.data()+10);
    }
    if (!rate || rate>768000 || !channels || channels>32) return false;
    if (!index) return true;
    // Index clusters: a Timestamp element sets the cluster base, then each Block is decoded.
    bool padded=false;int64_t previous=INT64_MIN;
    for (const auto& cluster:clusters) {
        int64_t timestamp=-1;
        if (!Children(file,cluster,[&](const Element& e) {
            if (e.id==0xe7) { uint64_t n=Unsigned(file,e,good);if (n>uint64_t(INT64_MAX)) return false;timestamp=(int64_t)n;return good; }
            if (e.id==0xa3) return timestamp>=0 && Block(*this,e,selected.number,timestamp,scale,0,padded,previous);
            if (e.id==0xa0) {
                Element block;int64_t padding=0;
                if (!Children(file,e,[&](const Element& item) {
                    if (item.id==0xa1) { if (block.id) return false;block=item; }
                    if (item.id==0x75a2) padding=Signed(file,item,good);
                    return good;
                })) return false;
                return block.id && timestamp>=0 && Block(*this,block,selected.number,timestamp,scale,padding,padded,previous);
            }
            return true;
        })) return false;
    }
    if (packets.empty()) return false;
    // Compute samples per packet. Fixed-frame codecs are constants, ALAC/Opus parse their
    // frame header, and PCM divides the byte count by the frame alignment.
    unsigned align=channels*((sampleBits+7)/8);
    for (size_t i=0;i<packets.size();++i) {
        auto& packet=packets[i];unsigned samples=0;
        if (codec==AudioAac) samples=1024;
        else if (codec==AudioAlac) {
            const uint8_t* p=Data(i);if (!p) return false;Bits b(p,packet.bytes);b.Skip(19);bool partial=b.Get(1)!=0;b.Skip(3);
            samples=partial?(unsigned)b.Get(32):U32(config.data(),true);if (!b.Good() || !samples || samples>65536) return false;
        } else if (codec==AudioOpus) {
            const uint8_t* p=Data(i);OpusPacket opus;if (!p || !opus.Parse({p,packet.bytes})) return false;samples=opus.samples;
        } else if (codec==AudioAc3) samples=1536;
        else if (codec==AudioPcmLE || codec==AudioPcmBE || codec==AudioPcmFloat) {
            if (!align || packet.bytes%align) return false;samples=packet.bytes/align;
            if (samples>65536) return false;
        }
        packet.start=frames;packet.frames=samples;frames+=samples;
    }
    return true;
}
} // namespace nativeaudio

namespace {
using namespace nativeaudio;
// Direct PCM decoder for Matroska tracks; handles integer (LE/BE) and IEEE float.
class MatroskaPcm final : public PacketDecoder {
public:
    bool Open(const std::wstring& path) {
        if (!media_.OpenMatroska(path)) return false;
        bits_=media_.sampleBits;channels_=media_.channels;big_=media_.codec==AudioPcmBE;floating_=media_.codec==AudioPcmFloat;
        if (!channels_ || channels_>8 || (floating_?(bits_!=32 && bits_!=64):(bits_!=8 && bits_!=16 && bits_!=24 && bits_!=32))) return false;
        return Ready(media_.rate);
    }
private:
    bool Decode(const uint8_t* p,size_t bytes,std::vector<float>& out) override {
        unsigned width=bits_/8,align=width*channels_;if (bytes%align) return false;
        size_t frames=bytes/align;out.resize(frames*2);
        for (size_t i=0;i<frames;++i) {
            float values[8]{};
            for (unsigned c=0;c<channels_;++c,p+=width) {
                float value=0;
                if (floating_) {
                    if (bits_==32) memcpy(&value,p,4);
                    else { double d;memcpy(&d,p,8);value=(float)d; }
                    if (!std::isfinite(value)) value=0;
                } else if (bits_==8) value=(int(p[0])-128)/128.0f;
                else {
                    uint32_t sample=0;for (unsigned j=0;j<width;++j) sample|=uint32_t(p[big_?width-1-j:j])<<(j*8);
                    value=(int32_t)(sample<<(32-bits_))*(1.0f/2147483648.0f);
                }
                values[c]=value;
            }
            Mix(values,channels_,out.data()+i*2);
        }
        return true;
    }
    unsigned bits_=0,channels_=0;bool big_=false,floating_=false;
};
}
std::unique_ptr<Decoder> OpenMatroskaDecoder(const std::wstring& path) {
    nativeaudio::PacketFile probe;if (!probe.OpenMatroska(path,false)) return nullptr;
    using namespace nativeaudio;
    switch (probe.codec) {
    case AudioAac:return OpenAacDecoder(path);
    case AudioAlac:return OpenAlacDecoder(path);
    case AudioOpus:return OpenOpusDecoder(path);
    case AudioFlac:return OpenFlacDecoder(path);
    case AudioVorbis:return OpenVorbisDecoder(path);
    case AudioMpeg:return OpenMpegDecoder(path);
    case AudioAc3:return OpenAc3Decoder(path);
    case 0x160:case 0x161:return OpenWmaDecoder(path);
    case AudioPcmLE:case AudioPcmBE:case AudioPcmFloat: {
        auto decoder=std::make_unique<MatroskaPcm>();if (!decoder->Open(path)) return nullptr;return decoder;
    }
    }
    return nullptr;
}
