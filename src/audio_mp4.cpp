// ISO base media (MP4) sample tables: the moov box, track/sample descriptions and the
// stsz/stts/stsc/stco tables. Own parser, no operating-system or external demuxer.
#include "audio_packets.h"
#include <span>

namespace nativeaudio {
namespace {
using Bytes = std::span<const uint8_t>;
// Four-character box/codec type as an integer, plus big-endian field reads for the format.
constexpr uint32_t Tag(const char* p) { return uint32_t(uint8_t(p[0]))<<24 | uint32_t(uint8_t(p[1]))<<16 | uint32_t(uint8_t(p[2]))<<8 | uint8_t(p[3]); }
uint32_t Be(const uint8_t* p) { return U32(p,true); }
struct Box { uint32_t tag; Bytes bytes; };
// Read one box from the front of `list`, advancing past it. Size 1 selects a 64-bit length.
bool Next(Bytes& list,Box& box) {
    if (list.size() < 8) return false;
    uint64_t n = Be(list.data()); size_t head = 8;
    if (n == 1) { if (list.size() < 16) return false; n = U64(list.data()+8,true); head = 16; }
    if (!n) n = list.size();
    if (n < head || n > list.size()) return false;
    box = {Be(list.data()+4),list.subspan(head,(size_t)n-head)};
    list = list.subspan((size_t)n); return true;
}
// First child box of the given type, or empty; Path() walks a nested tag sequence.
Bytes Child(Bytes list,uint32_t type) {
    Box b{}; while (Next(list,b)) if (b.tag == type) return b.bytes;
    return {};
}
Bytes Path(Bytes list,std::initializer_list<uint32_t> path) { for (uint32_t t : path) list = Child(list,t); return list; }
// Full-box table header: version/flags plus a 32-bit entry count, validated against the size.
bool Table(Bytes b,size_t stride,uint32_t& n) {
    if (b.size() < 8) return false;
    n = Be(b.data()+4); return n <= (b.size()-8)/stride && n <= 8000000;
}
// ISO descriptor length (7-bit groups), limited to four bytes by the format.
bool Descriptor(Bytes& list,uint8_t& tag,Bytes& value) {
    if (list.empty()) return false;
    tag = list[0]; size_t n = 0, i = 1;
    for (; i <= 4; ++i) {
        if (i == list.size()) return false;
        uint8_t b = list[i]; n = n*128+(b&127);
        if (!(b&128)) { ++i; if (n > list.size()-i) return false; value=list.subspan(i,n); list=list.subspan(i+n); return true; }
    }
    return false;
}
// Walk the ES descriptor tree to the DecoderSpecificInfo (tag 5), the AudioSpecificConfig.
Bytes DecoderConfig(Bytes esds) {
    if (esds.size() < 4) return {};
    Bytes data = esds.subspan(4), value; uint8_t tag;
    if (!Descriptor(data,tag,value) || tag != 3 || value.size() < 3) return {};
    size_t skip = 3; uint8_t flags = value[2];
    if (flags&128) skip += 2;
    if (flags&64) { if (skip == value.size()) return {}; skip += 1+value[skip]; }
    if (flags&32) skip += 2;
    if (skip > value.size()) return {};
    data = value.subspan(skip);
    if (!Descriptor(data,tag,value) || tag != 4 || value.size() < 13) return {};
    data = value.subspan(13);
    while (Descriptor(data,tag,value)) if (tag == 5) return value;
    return {};
}
// Parse the first audio trak: sample description, codec config and the sample tables that
// link every packet to a file offset, size and start time.
bool Track(PacketFile& out,Bytes track,uint32_t movieScale) {
    Bytes mdia=Child(track,Tag("mdia")), handler=Child(mdia,Tag("hdlr"));
    if (handler.size()<12 || Be(handler.data()+8)!=Tag("soun")) return false;
    Bytes stbl=Path(mdia,{Tag("minf"),Tag("stbl")}), stsd=Child(stbl,Tag("stsd"));
    if (stsd.size()<8 || Be(stsd.data()+4)!=1) return false;
    Bytes entries=stsd.subspan(8); Box entry{};
    if (!Next(entries,entry) || entry.bytes.size()<28) return false;
    if (entry.tag!=Tag("alac") && entry.tag!=Tag("mp4a") && entry.tag!=Tag("Opus") && entry.tag!=Tag("ac-3")) return false;
    out.codec=entry.tag;
    out.channels=U16(entry.bytes.data()+16,true); out.rate=Be(entry.bytes.data()+24)>>16;
    unsigned version=U16(entry.bytes.data()+8,true);
    size_t extra=version==1?16:version==2?36:0;
    if (entry.bytes.size()<28+extra) return false;
    Bytes children=entry.bytes.subspan(28+extra), config;
    if (out.codec==Tag("alac")) { config=Child(children,Tag("alac")); if (config.size()<28) return false; config=config.subspan(4); }
    else if (out.codec==Tag("mp4a")) config=DecoderConfig(Child(children,Tag("esds")));
    else if (out.codec==Tag("Opus")) config=Child(children,Tag("dOps"));
    else config=Child(children,Tag("dac3"));
    if (config.empty() || config.size()>65536) return false;
    out.config.assign(config.begin(),config.end());
    if (out.codec==Tag("alac")) out.rate=Be(config.data()+20);

    // Sample sizes: either one constant size or a table of per-sample sizes.
    Bytes stsz=Child(stbl,Tag("stsz"));
    if (stsz.size()<12) return false;
    uint32_t constant=Be(stsz.data()+4), samples=Be(stsz.data()+8);
    if (!samples || samples>8000000 || samples>out.file.Size() ||
        (!constant && samples>(stsz.size()-12)/4)) return false;
    out.packets.assign(samples,{});
    for (uint32_t i=0;i<samples;++i) { uint32_t n=constant?constant:Be(stsz.data()+12+i*4); if (!n || n>16*1024*1024) return false; out.packets[i].bytes=n; }
    // Time-to-sample: run-length coded (count, duration) pairs give each packet its start.
    Bytes stts=Child(stbl,Tag("stts")); uint32_t count;
    if (!Table(stts,8,count)) return false;
    size_t sample=0; uint64_t time=0;
    for (uint32_t i=0;i<count;++i) {
        uint32_t n=Be(stts.data()+8+i*8), dur=Be(stts.data()+12+i*8);
        if (!dur || n>samples-sample) return false;
        for (uint32_t j=0;j<n;++j) { out.packets[sample].start=time; out.packets[sample++].frames=dur; time+=dur; }
    }
    if (sample!=samples) return false;
    out.frames=time;
    // Sample-to-chunk maps chunk runs to samples-per-chunk; the chunk offsets follow.
    Bytes stsc=Child(stbl,Tag("stsc")); uint32_t maps;
    if (!Table(stsc,12,maps) || !maps || Be(stsc.data()+8)!=1) return false;
    for (uint32_t i=0;i<maps;++i) {
        const uint8_t* p=stsc.data()+8+i*12;
        if (!Be(p+4) || Be(p+8)!=1 || (i && Be(p)<=Be(p-12))) return false;
    }
    // Chunk offsets are 32-bit (stco) or 64-bit (co64) when the former is absent.
    Bytes offsets=Child(stbl,Tag("stco")); bool wide=false;
    if (offsets.empty()) { wide=true; offsets=Child(stbl,Tag("co64")); }
    uint32_t chunks;
    if (!Table(offsets,wide?8:4,chunks) || !chunks) return false;
    sample=0; uint32_t map=0;
    for (uint32_t i=1;i<=chunks;++i) {
        while (map+1<maps && Be(stsc.data()+8+(map+1)*12)<=i) ++map;
        uint32_t n=Be(stsc.data()+12+map*12);
        if (n>samples-sample) return false;
        const uint8_t* p=offsets.data()+8+(i-1)*(wide?8:4);
        uint64_t at=wide?U64(p,true):Be(p);
        for (uint32_t j=0;j<n;++j) {
            auto& packet=out.packets[sample++];
            if (at>out.file.Size() || packet.bytes>out.file.Size()-at) return false;
            packet.offset=at; at+=packet.bytes;
        }
    }
    if (sample!=samples) return false;
    Bytes mdhd=Child(mdia,Tag("mdhd"));
    if (mdhd.size()<24 || (mdhd[0]==1 && mdhd.size()<36)) return false;
    uint32_t scale=Be(mdhd.data()+(mdhd[0]==1?20:12));
    // PCM codecs use their sample rate as media timescale. AAC follows the same convention.
    if (!scale) return false;
    if (!out.rate || out.codec==Tag("mp4a")) out.rate=scale;
    if (scale!=out.rate && out.codec!=Tag("alac")) return false;
    // Edit list: a leading empty edit becomes the start skip and its duration the valid length.
    Bytes elst=Path(track,{Tag("edts"),Tag("elst")});
    out.skip=out.valid=0;
    if (!elst.empty()) {
        bool large=elst[0]==1; uint32_t edits;
        if (!Table(elst,large?20:12,edits) || edits>2) return false;
        for (uint32_t i=0;i<edits;++i) {
            const uint8_t* p=elst.data()+8+i*(large?20:12);
            uint64_t dur=large?U64(p,true):Be(p);
            int64_t from=large?(int64_t)U64(p+8,true):(int32_t)Be(p+4);
            if (from<0) continue;
            if (U32(p+(large?16:8),true)!=0x10000 || out.valid) return false;
            out.skip=(uint64_t)from;
            if (movieScale && dur<=UINT64_MAX/scale) out.valid=dur*scale/movieScale;
        }
    }
    return true;
}
} // namespace

// Scan top-level boxes for moov (copied out, since the parser remaps regions), then take
// the first audio track.
bool PacketFile::OpenMp4(const std::wstring& path) {
    if (!file.Size() && !file.Open(path)) return false;
    if (IsMatroska()) return OpenMatroska(path);
    std::vector<uint8_t> storage;
    for (uint64_t at=0;at+8<=file.Size();) {
        const uint8_t* p=file.View(at,16); if (!p) p=file.View(at,8); if (!p) return false;
        uint64_t n=Be(p); uint32_t tag=Be(p+4); size_t head=8;
        if (n==1) { p=file.View(at,16); if (!p) return false; n=U64(p+8,true); head=16; }
        if (!n) n=file.Size()-at;
        if (n<head || n>file.Size()-at) return false;
        if (tag==Tag("moov")) {
            if (n-head>64*1024*1024) return false;
            p=file.View(at+head,(size_t)n-head); if (!p) return false;
            storage.assign(p,p+(size_t)n-head); break;
        }
        at+=n;
    }
    Bytes moov=storage, mvhd=Child(moov,Tag("mvhd"));
    if (mvhd.size()<24 || (mvhd[0]==1 && mvhd.size()<36)) return false;
    uint32_t movieScale=Be(mvhd.data()+(mvhd[0]==1?20:12));
    Box b{}; while (Next(moov,b)) if (b.tag==Tag("trak") && Track(*this,b.bytes,movieScale)) return true;
    return false;
}
} // namespace nativeaudio
