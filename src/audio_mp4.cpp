// ISO base media (MP4) sample tables: the moov box, track/sample descriptions and the
// stsz/stts/stsc/stco tables, or the moof/trun runs of a fragmented file. Own parser, no
// operating-system or external demuxer. ReadMp4Fragments below hands the same walk to the
// container rewriter in mp4_remux.cpp, so the rewrite parses nothing on its own.
#include "audio_packets.h"
#include "mp4_remux.h"
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
    if (flags&64) { if (skip >= value.size()) return {}; skip += 1+value[skip]; }
    if (flags&32) skip += 2;
    if (skip > value.size()) return {};
    data = value.subspan(skip);
    if (!Descriptor(data,tag,value) || tag != 4 || value.size() < 13) return {};
    data = value.subspan(13);
    while (Descriptor(data,tag,value)) if (tag == 5) return value;
    return {};
}
// Non-fragmented files: stsz gives each sample's size, stts its duration, and stsc with
// stco/co64 the file offset of every sample.
bool SampleTables(PacketFile& out,Bytes stbl,Bytes stsz,uint32_t constant,uint32_t samples) {
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
    return sample==samples;
}

// Fragmented files (moov with an empty sample table, then moof/mdat pairs): each traf's trun lists
// its samples' sizes, durations and data offsets. tfhd and trex supply defaults, tfdt the decode
// time, and a traf without tfdt continues from the previous run. Other tracks' runs are skipped.
bool ReadFragments(PacketFile& out,Bytes track,Bytes movie) {
    Bytes tkhd=Child(track,Tag("tkhd"));
    if (tkhd.size()<24) return false;
    const uint32_t trackId=Be(tkhd.data()+(tkhd[0]==1?20:12));
    uint32_t defaultDuration=0,defaultSize=0;
    Bytes mvex=Child(movie,Tag("mvex")); Box box{};
    while (Next(mvex,box)) {
        if (box.tag==Tag("trex") && box.bytes.size()>=20 && Be(box.bytes.data()+4)==trackId) {
            defaultDuration=Be(box.bytes.data()+12); defaultSize=Be(box.bytes.data()+16);
        }
    }
    uint64_t time=0;
    for (uint64_t at=0;at+8<=out.file.Size();) {
        const uint8_t* p=out.file.View(at,16); if (!p) p=out.file.View(at,8); if (!p) return false;
        uint64_t n=Be(p); uint32_t tag=Be(p+4); size_t head=8;
        if (n==1) { p=out.file.View(at,16); if (!p) return false; n=U64(p+8,true); head=16; }
        if (!n) n=out.file.Size()-at;
        if (n<head || n>out.file.Size()-at) break;
        if (tag==Tag("moof")) {
            if (n>16*1024*1024) return false;
            p=out.file.View(at,(size_t)n); if (!p) return false;
            const std::vector<uint8_t> copy(p,p+(size_t)n);  // the file view may move on the next call
            Bytes moof=Bytes(copy).subspan(head);
            uint64_t dataEnd=at;  // a traf without a base offset starts after the previous traf's data
            Box traf{};
            while (Next(moof,traf)) {
                if (traf.tag!=Tag("traf")) continue;
                Bytes header=Child(traf.bytes,Tag("tfhd"));
                if (header.size()<8) return false;
                const uint32_t flags=Be(header.data())&0xFFFFFF, id=Be(header.data()+4);
                const size_t need=8+((flags&1)?8:0)+((flags&2)?4:0)+((flags&8)?4:0)+((flags&0x10)?4:0)+((flags&0x20)?4:0);
                if (header.size()<need) return false;
                const bool ours=id==trackId;
                const bool explicitBase=(flags&1)!=0;
                uint64_t base=explicitBase?U64(header.data()+8,true):(flags&0x20000)?at:dataEnd;
                size_t field=explicitBase?16:8;
                if (flags&2) field+=4;
                uint32_t defDuration=defaultDuration,defSize=defaultSize;
                if (flags&8) { defDuration=Be(header.data()+field); field+=4; }
                if (flags&0x10) { defSize=Be(header.data()+field); field+=4; }
                Bytes decode=Child(traf.bytes,Tag("tfdt"));
                if (!decode.empty()) {
                    if (decode.size()<(decode[0]==1?12u:8u)) return false;
                    if (ours) time=decode[0]==1?U64(decode.data()+4,true):Be(decode.data()+4);
                }
                uint64_t cursor=base;
                Bytes runs=traf.bytes; Box run{};
                while (Next(runs,run)) {
                    if (run.tag!=Tag("trun")) continue;
                    const Bytes r=run.bytes;
                    if (r.size()<8) return false;
                    const uint32_t runFlags=Be(r.data())&0xFFFFFF, count=Be(r.data()+4);
                    size_t at3=8;
                    if (runFlags&1) {
                        if (r.size()<12) return false;
                        cursor=uint64_t(int64_t(base)+int32_t(Be(r.data()+8)));
                        at3=12;
                    }
                    if (runFlags&4) at3+=4;
                    const size_t stride=4*(((runFlags>>8)&1)+((runFlags>>9)&1)+((runFlags>>10)&1)+((runFlags>>11)&1));
                    if (r.size()<at3 || count>8000000 || (count && stride && count>(r.size()-at3)/stride)) return false;
                    for (uint32_t i=0;i<count;++i) {
                        uint32_t duration=defDuration,bytes=defSize;
                        if (runFlags&0x100) { duration=Be(r.data()+at3); at3+=4; }
                        if (runFlags&0x200) { bytes=Be(r.data()+at3); at3+=4; }
                        if (runFlags&0x400) at3+=4;
                        if (runFlags&0x800) at3+=4;
                        if (!duration || !bytes || bytes>16*1024*1024) return false;
                        if (ours) {
                            if (cursor>out.file.Size() || bytes>out.file.Size()-cursor || out.packets.size()>=8000000) return false;
                            if (!out.packets.empty() && time<out.packets.back().start) return false;
                            out.packets.push_back({cursor,time,bytes,duration});
                            time+=duration;
                        }
                        cursor+=bytes;
                    }
                }
                dataEnd=cursor;
            }
        }
        at+=n;
    }
    out.frames=time;
    return !out.packets.empty();
}

// Extra facts Track() records for ReadMp4Fragments(): the raw sample description to
// copy verbatim, and the edit list's empty edit, which playback ignores but a
// faithful rewrite has to carry over.
struct FragmentsCapture {
    std::vector<uint8_t> sampleDesc;
    uint64_t emptyEdit = 0;
    int emptyEdits = 0;
    bool sawEdit = false, fragmented = false;
};

// Parse the first audio trak: sample description, codec config and the sample tables that
// link every packet to a file offset, size and start time.
bool Track(PacketFile& out,Bytes track,Bytes movie,uint32_t movieScale,FragmentsCapture* capture=nullptr) {
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
    // The whole sample description box, header included: a rewrite copies it as it is,
    // so the codec setup stays exactly what the source stated.
    if (capture) {
        const uint8_t* head=entry.bytes.data()-(U32(entry.bytes.data()-4,true)==1?16:8);
        capture->sampleDesc.assign(head,entry.bytes.data()+entry.bytes.size());
    }

    // Sample sizes: either one constant size or a table of per-sample sizes. A fragmented file
    // has no samples here; its runs come from the moof boxes instead.
    Bytes stsz=Child(stbl,Tag("stsz"));
    if (stsz.size()<12) return false;
    uint32_t constant=Be(stsz.data()+4), samples=Be(stsz.data()+8);
    if (samples>8000000 || samples>out.file.Size() || (!constant && samples>(stsz.size()-12)/4)) return false;
    if (!samples && capture) capture->fragmented=true;
    if (samples ? !SampleTables(out,stbl,stsz,constant,samples) : !ReadFragments(out,track,movie)) return false;
    Bytes mdhd=Child(mdia,Tag("mdhd"));
    if (mdhd.size()<24 || (mdhd[0]==1 && mdhd.size()<36)) return false;
    uint32_t scale=Be(mdhd.data()+(mdhd[0]==1?20:12));
    // The media clock need not be the sample rate. Keep its ticks until the codec is configured.
    if (!scale) return false;
    if (!out.rate || out.codec==Tag("mp4a")) out.rate=scale;
    // The nonempty edit gives the start skip and playable duration; a leading empty edit is ignored.
    Bytes elst=Path(track,{Tag("edts"),Tag("elst")});
    out.skip=out.valid=0;
    uint64_t editDuration=0; bool sawEdit=false;
    if (!elst.empty()) {
        bool large=elst[0]==1; uint32_t edits;
        if (!Table(elst,large?20:12,edits) || edits>2) return false;
        for (uint32_t i=0;i<edits;++i) {
            const uint8_t* p=elst.data()+8+i*(large?20:12);
            uint64_t dur=large?U64(p,true):Be(p);
            int64_t from=large?(int64_t)U64(p+8,true):(int32_t)Be(p+4);
            if (from<0) {
                // An empty edit is leading silence; playback ignores it, a rewrite copies it.
                if (capture && !capture->emptyEdits) capture->emptyEdit=dur;
                if (capture) ++capture->emptyEdits;
                continue;
            }
            if (U32(p+(large?16:8),true)!=0x10000 || sawEdit) return false;
            sawEdit=true;
            if (capture) capture->sawEdit=true;
            out.skip=(uint64_t)from;
            editDuration=dur;
        }
    }
    out.mp4TimeScale=scale; out.mp4MovieScale=movieScale; out.mp4EditDuration=editDuration;
    return true;
}

// Rescale without overflowing when a long file's tick count is multiplied by its sample rate.
bool Samples(uint64_t ticks,uint32_t scale,unsigned rate,bool nearest,uint64_t& samples,bool* exact=nullptr) {
    if (!scale || !rate || rate>768000) return false;
    uint64_t whole=ticks/scale, part=(ticks%scale)*rate;
    if (whole>UINT64_MAX/rate) return false;
    uint64_t base=whole*rate, fraction=part/scale, remainder=part%scale;
    if (nearest && remainder>=(uint64_t(scale)+1)/2) ++fraction;
    if (fraction>UINT64_MAX-base) return false;
    samples=base+fraction;
    if (exact) *exact=remainder==0;
    return true;
}
} // namespace

bool PacketFile::NormalizeMp4(unsigned sampleRate) {
    if (!mp4TimeScale) return true;
    if (!sampleRate || sampleRate>768000) return false;
    if (mp4TimeScale!=sampleRate) {
        // An inexact packet start would create a gap or overlap in the sample timeline.
        for (auto& packet:packets) {
            uint64_t start,end; bool exact;
            if (!Samples(packet.start,mp4TimeScale,sampleRate,false,start,&exact) || !exact ||
                !Samples(packet.start+packet.frames,mp4TimeScale,sampleRate,true,end) ||
                end<=start || end-start>UINT32_MAX) return false;
            packet.start=start; packet.frames=(uint32_t)(end-start);
        }
        if (!Samples(frames,mp4TimeScale,sampleRate,true,frames) ||
            !Samples(skip,mp4TimeScale,sampleRate,true,skip)) return false;
    }
    if (mp4MovieScale && mp4EditDuration &&
        !Samples(mp4EditDuration,mp4MovieScale,sampleRate,false,valid)) return false;
    rate=sampleRate; mp4TimeScale=0;
    return true;
}

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
    const Bytes movie=moov;
    Box b{}; while (Next(moov,b)) if (b.tag==Tag("trak") && Track(*this,b.bytes,movie,movieScale)) return true;
    return false;
}

bool IsFragmentedMp4(const std::wstring& path) {
    PacketFile pf;
    if (!pf.file.Open(path) || pf.IsMatroska()) return false;
    // The box-header walk OpenMp4 uses, stopping at the first fragment marker.
    for (uint64_t at=0;at+8<=pf.file.Size();) {
        const uint8_t* p=pf.file.View(at,16); if (!p) p=pf.file.View(at,8); if (!p) return false;
        uint64_t n=Be(p); uint32_t tag=Be(p+4); size_t head=8;
        if (n==1) { p=pf.file.View(at,16); if (!p) return false; n=U64(p+8,true); head=16; }
        if (!n) n=pf.file.Size()-at;
        if (n<head || n>pf.file.Size()-at) return false;
        if (tag==Tag("moof")) return true;
        at+=n;
    }
    return false;
}

bool ReadMp4Fragments(const std::wstring& path, Mp4Fragments& out, std::wstring& error) {
    PacketFile pf;
    if (!pf.file.Open(path) || pf.IsMatroska()) {
        error=L"the file could not be read as MP4";
        return false;
    }
    // Top-level boxes up to moov: the brand is kept verbatim, moov is copied out because
    // the parser remaps regions. A fragmented file always puts its moov before the runs.
    std::vector<uint8_t> storage;
    for (uint64_t at=0;at+8<=pf.file.Size();) {
        const uint8_t* p=pf.file.View(at,16); if (!p) p=pf.file.View(at,8); if (!p) break;
        uint64_t n=Be(p); uint32_t tag=Be(p+4); size_t head=8;
        if (n==1) { p=pf.file.View(at,16); if (!p) break; n=U64(p+8,true); head=16; }
        if (!n) n=pf.file.Size()-at;
        if (n<head || n>pf.file.Size()-at) break;
        if (tag==Tag("ftyp") && n<=4096) {
            p=pf.file.View(at,(size_t)n);
            if (p) out.ftyp.assign(p,p+(size_t)n);
        } else if (tag==Tag("moov")) {
            if (n-head>64*1024*1024) break;
            p=pf.file.View(at+head,(size_t)n-head); if (!p) break;
            storage.assign(p,p+(size_t)n-head);
            break;
        }
        at+=n;
    }
    if (storage.empty()) {
        error=L"no moov atom: not an MP4 audio file";
        return false;
    }
    Bytes moov=storage, mvhd=Child(moov,Tag("mvhd"));
    if (mvhd.size()<24 || (mvhd[0]==1 && mvhd.size()<36)) {
        error=L"the file's movie header is damaged";
        return false;
    }
    uint32_t movieScale=Be(mvhd.data()+(mvhd[0]==1?20:12));
    if (!movieScale) {
        error=L"the file's movie header has no timescale";
        return false;
    }
    const Bytes movie=moov;
    FragmentsCapture cap;
    Box b{};
    bool parsed=false;
    while (Next(moov,b)) {
        if (b.tag!=Tag("trak")) continue;
        // Only the trak that parses leaves its capture behind, so reset it per attempt.
        cap=FragmentsCapture{};
        if (Track(pf,b.bytes,movie,movieScale,&cap)) { parsed=true; break; }
    }
    if (!parsed) {
        error=L"the file has no audio track the rewrite could read";
        return false;
    }
    if (!cap.fragmented) {
        error=L"not a fragmented MP4";
        return false;
    }
    // An encrypted sample description (sinf) means the stream's bytes are not plain
    // compressed audio this rewrite could copy; save must fall back, not break the file.
    {
        const std::vector<uint8_t>& desc=cap.sampleDesc;
        unsigned version=U16(desc.data()+16,true);
        size_t children=8+28+(version==1?16:version==2?36:0);
        if (children>desc.size()) children=desc.size();
        Bytes kids{desc.data()+children,desc.size()-children};
        Box k{};
        while (Next(kids,k)) if (k.tag==Tag("sinf")) {
            error=L"the stream's audio is encrypted, so it cannot be saved as a plain file";
            return false;
        }
    }
    if (cap.emptyEdits>1) {
        error=L"the track's edit list is too unusual to rewrite";
        return false;
    }
    out.sampleDesc=std::move(cap.sampleDesc);
    out.movieScale=movieScale;
    out.mediaScale=pf.mp4TimeScale;
    out.codec=pf.codec; out.channels=pf.channels; out.rate=pf.rate;
    out.config=pf.config;
    out.hasEdit=cap.sawEdit;
    out.emptyEditDuration=cap.emptyEdit;
    out.editMediaTime=(int64_t)pf.skip;
    out.editDuration=pf.mp4EditDuration;
    out.lead=pf.packets.front().start;
    out.runs.reserve(pf.packets.size());
    for (const Packet& p:pf.packets) {
        out.runs.push_back({p.offset,p.bytes,p.frames});
        out.mediaDuration+=p.frames;
    }
    // The rewrite starts its sample table at zero, so the first fragment must sit at or
    // before the edit list's playable start, or the first samples would move.
    if ((uint64_t)out.editMediaTime<out.lead) {
        error=L"the stream's timeline starts after its edit list, which the rewrite cannot express";
        return false;
    }
    return true;
}
} // namespace nativeaudio
