// Bounded ASF packet demuxing for Cordinal audio codecs. Microsoft ASF specification.
// Walks the header object list for the audio stream, then splits data packets into codec
// blocks; every offset and length is checked against the mapped file.
#include "audio_packets.h"
#include <span>

namespace nativeaudio {
namespace {
using Bytes=std::span<const uint8_t>;
// 16-byte ASF object/stream GUIDs, stored in the wire (little-endian) byte order.
bool Guid(const uint8_t* p,const char* id) { return memcmp(p,id,16)==0; }
constexpr char header[]="\x30\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C";
constexpr char fileProps[]="\xA1\xDC\xAB\x8C\x47\xA9\xCF\x11\x8E\xE4\x00\xC0\x0C\x20\x53\x65";
constexpr char streamProps[]="\x91\x07\xDC\xB7\xB7\xA9\xCF\x11\x8E\xE6\x00\xC0\x0C\x20\x53\x65";
constexpr char audio[]="\x40\x9E\x69\xF8\x4D\x5B\xCF\x11\xA8\xFD\x00\x80\x5F\x5C\x44\x2B";
constexpr char dataObject[]="\x36\x26\xB2\x75\x8E\x66\xCF\x11\xA6\xD9\x00\xAA\x00\x62\xCE\x6C";
// Little-endian reader with bounds checking; a short read fails the whole packet.
class Cursor {
public:
    explicit Cursor(Bytes bytes):bytes_(bytes) {}
    uint32_t Get(unsigned n) {
        if (n>4 || n>Left()) { good_=false; return 0; }
        uint32_t v=0; for (unsigned i=0;i<n;++i) v|=uint32_t(bytes_[pos_++])<<(i*8);
        return v;
    }
    uint32_t Variable(unsigned kind) { return Get(kind==3?4:kind); }
    void Skip(size_t n) { if (n>Left()) good_=false; else pos_+=n; }
    size_t Position() const { return pos_; }
    size_t Left() const { return bytes_.size()-pos_; }
    bool Good() const { return good_; }
private:
    Bytes bytes_; size_t pos_=0; bool good_=true;
};
}

bool PacketFile::OpenAsf(const std::wstring& path) {
    if (!file.Open(path) || file.Size()<30) return false;
    if (IsMatroska()) return OpenMatroska(path);
    // The header object holds nested objects; the first audio Stream Properties defines the
    // codec, channel count, rate and block size.
    const uint8_t* p=file.View(0,30);
    if (!p || !Guid(p,header)) return false;
    uint64_t headerBytes=U64(p+16);
    if (headerBytes<30 || headerBytes>file.Size() || headerBytes>64*1024*1024) return false;
    p=file.View(0,(size_t)headerBytes); if (!p) return false;
    std::vector<uint8_t> storage(p,p+(size_t)headerBytes); Bytes list=storage;
    unsigned stream=0,maxPacket=0,block=0;
    // Header objects are length-prefixed at offset 16 and must fit inside the header.
    for (size_t at=30;at+24<=list.size();) {
        const uint8_t* box=list.data()+at; uint64_t n=U64(box+16);
        if (n<24 || n>list.size()-at) return false;
        Bytes b(box+24,(size_t)n-24);
        if (Guid(box,fileProps)) {
            if (b.size()<80) return false;
            maxPacket=U32(b.data()+72);
            if (!maxPacket || maxPacket>16*1024*1024) return false;
        } else if (Guid(box,streamProps) && !stream) {
            if (b.size()<54) return false;
            if (Guid(b.data(),audio)) {
                unsigned flags=U16(b.data()+48),nformat=U32(b.data()+40);
                if ((flags&0x8000) || nformat<18 || nformat>b.size()-54) return false;
                Bytes format=b.subspan(54,nformat);
                codec=U16(format.data()); channels=U16(format.data()+2); rate=U32(format.data()+4);
                if ((codec!=0x160 && codec!=0x161) || !channels || channels>2 || rate<4000 || rate>50000) return false;
                block=U16(format.data()+12);
                unsigned extra=U16(format.data()+16); if (!block || extra>format.size()-18) return false;
                config.assign(format.begin(),format.end()); stream=flags&127;
                if (!stream) return false;
            }
        }
        at+=(size_t)n;
    }
    if (!stream || !maxPacket) return false;
    // Locate the Data Object; its payload begins 50 bytes in (24-byte header + file id + count).
    uint64_t dataStart=0,dataEnd=0;
    for (uint64_t at=headerBytes;at+24<=file.Size();) {
        p=file.View(at,24); if (!p) return false;
        uint64_t n=U64(p+16); bool data=Guid(p,dataObject);
        if (n<24 || n>file.Size()-at) return false;
        if (data) { if (n<50) return false; dataStart=at+50; dataEnd=at+n; break; }
        at+=n;
    }
    if (!dataStart) return false;
    uint32_t objectId=0,objectSize=0,collected=0,first=0;
    // Assemble one codec block from its (possibly split) payload pieces. Offsets must be
    // contiguous and the pieces must sum to the block size before a packet is emitted.
    auto part=[&](uint32_t id,uint32_t offset,uint32_t size,uint64_t source,uint32_t bytes) {
        if (!offset) {
            if (collected) return false;
            objectId=id; objectSize=size?size:block; first=(uint32_t)segments.size();
        }
        if (id!=objectId || offset!=collected || !objectSize || objectSize>16*1024*1024 || bytes>objectSize-collected) return false;
        segments.push_back({source,bytes}); collected+=bytes;
        if (collected==objectSize) {
            Packet packet; packet.bytes=objectSize; packet.first=first; packet.pieces=(uint32_t)segments.size()-first;
            if (packet.pieces==1) { packet.offset=segments.back().offset; packet.pieces=0; }
            packets.push_back(packet); collected=0;
        }
        return packets.size()<8000000 && segments.size()<16000000;
    };
    // Walk fixed-size ASF packets, parsing their variable-length payload descriptors.
    for (uint64_t at=dataStart;at<dataEnd;) {
        size_t available=(size_t)std::min<uint64_t>(maxPacket,dataEnd-at);
        p=file.View(at,available); if (!p || !available) return false;
        Cursor c(Bytes(p,available)); unsigned firstByte=c.Get(1),flags;
        if (firstByte&128) { if (firstByte&0x70) return false; c.Skip(firstByte&15); flags=c.Get(1); }
        else flags=firstByte;
        unsigned properties=c.Get(1),packetBytes=c.Variable((flags>>5)&3);
        c.Variable((flags>>1)&3); unsigned padding=c.Variable((flags>>3)&3);
        if (!packetBytes) packetBytes=maxPacket;
        if (packetBytes>available || padding>packetBytes || packetBytes-padding<c.Position()+6) return false;
        c.Skip(6);
        unsigned payloads=1,lengthKind=0;
        if (flags&1) { unsigned v=c.Get(1); payloads=v&63; lengthKind=v>>6; if (!payloads || !lengthKind) return false; }
        for (unsigned i=0;i<payloads;++i) {
            unsigned sid=c.Variable(properties>>6)&127;
            uint32_t id=c.Variable((properties>>4)&3),offset=c.Variable((properties>>2)&3),replicated=c.Variable(properties&3);
            size_t replicateAt=c.Position();
            if (replicated>c.Left()) return false;
            uint32_t size=replicated>=8?U32(p+replicateAt):0;
            c.Skip(replicated);
            uint32_t bytes=(flags&1)?c.Variable(lengthKind):packetBytes-padding-(uint32_t)c.Position();
            if (!c.Good() || c.Position()>packetBytes-padding || bytes>packetBytes-padding-c.Position()) return false;
            size_t source=c.Position();
            if (sid==stream) {
                // A 1-byte replicated length means the payload is a run of sub-blocks.
                if (replicated==1) {
                    size_t end=source+bytes;
                    while (c.Position()<end) {
                        unsigned sub=c.Get(1); if (!sub || sub>end-c.Position()) return false;
                        if (!part(id++,0,sub,at+c.Position(),sub)) return false;
                        c.Skip(sub);
                    }
                } else {
                    if (replicated && replicated<8) return false;
                    // An ASF audio media object can contain multiple complete codec blocks.
                    if (!offset && !size && bytes>block && bytes%block==0) {
                        for (unsigned j=0;j<bytes;j+=block) if (!part(id++,0,block,at+source+j,block)) return false;
                    } else if (!part(id,offset,size,at+source,bytes)) return false;
                    c.Skip(bytes);
                }
            } else c.Skip(bytes);
        }
        if (!c.Good()) return false;
        at+=packetBytes;
    }
    return !collected && !packets.empty();
}
} // namespace nativeaudio
