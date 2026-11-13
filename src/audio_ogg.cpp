// Cordinal Ogg packet indexes for Opus and FLAC: CRC, page sequence, lacing, continuation and
// granules. Reassembles each codec's packets (Opus: the OpusHead/OpusTags headers and the final
// granule, so the decoder can trim the pre-skip and padding; FLAC: its metadata blocks).
#include "audio_packets.h"
#include "audio_opus_packet.h"
#include <array>

namespace nativeaudio {
// Slice-by-8: table k gives a byte's contribution followed by k zero bytes, so eight bytes
// fold per step. The tables are derived on first use into .bss.
uint32_t OggCrc(const uint8_t* p,size_t n) {
    static std::array<std::array<uint32_t,256>,8> table;
    static const bool ready=[] {
        for (unsigned i=0;i<256;++i) { uint32_t v=i<<24; for (int j=0;j<8;++j) v=(v<<1)^((v>>31)?0x04C11DB7:0); table[0][i]=v; }
        for (unsigned k=1;k<8;++k) for (unsigned i=0;i<256;++i) table[k][i]=(table[k-1][i]<<8)^table[0][table[k-1][i]>>24];
        return true;
    }();
    (void)ready;
    auto run=[&](uint32_t crc,const uint8_t* q,size_t count) {
        for (;count>=8;q+=8,count-=8)
            crc=table[7][q[0]^(crc>>24)]^table[6][q[1]^((crc>>16)&255)]^table[5][q[2]^((crc>>8)&255)]^table[4][q[3]^(crc&255)]^
                table[3][q[4]]^table[2][q[5]]^table[1][q[6]]^table[0][q[7]];
        while (count--) crc=(crc<<8)^table[0][(crc>>24)^*q++];
        return crc;
    };
    static constexpr uint8_t zero[4]{};
    if (n<26) return 0;
    return run(run(run(0,p,22),zero,4),p+26,n-26);
}
namespace {
// The page CRC, with the stored checksum field (bytes 22..25) read as zero.
uint32_t Crc(const uint8_t* p,size_t n) { return OggCrc(p,n); }

// Page structure is fixed: 27-byte header, segment table, then payload. A packet ends at a
// lacing value < 255; larger values continue into the next page. Walks one logical stream from
// its first page, checking CRCs, serial and sequence numbers, and appends each complete packet to
// f.packets before calling packet(), which may read it with f.Data() and drop it again. True when
// the stream ends with its end-of-stream page and no partial packet; lastGranule is the last
// page granule given (UINT64_MAX if none) and must never decrease.
template<class F> bool OggStream(PacketFile& f,uint64_t& lastGranule,F&& packet) {
    uint32_t serial=0,sequence=0; bool seen=false,ended=false;
    uint64_t at=0; uint32_t size=0,first=0;
    lastGranule=UINT64_MAX;
    while (at<f.file.Size()) {
        const uint8_t* p=f.file.View(at,27);
        if (!p || !Is(p,"OggS") || p[4]) return false;
        unsigned flags=p[5],laces=p[26];
        p=f.file.View(at,27+laces); if (!p) return false;
        size_t payload=0; for (unsigned i=0;i<laces;++i) payload+=p[27+i];
        size_t bytes=27+laces+payload;
        p=f.file.View(at,bytes); if (!p || Crc(p,bytes)!=U32(p+22)) return false;
        uint32_t stream=U32(p+14),seq=U32(p+18);
        if (ended || (seen && (stream!=serial || seq!=sequence+1 || (flags&2))) || (!seen && (!(flags&2) || (flags&1)))) return false;
        if (bool(flags&1)!=bool(size)) return false;
        seen=true; serial=stream; sequence=seq;
        uint64_t granule=U64(p+6);
        size_t offset=27+laces;
        for (unsigned i=0;i<laces;++i) {
            unsigned n=p[27+i];
            if (!size) first=(uint32_t)f.segments.size();
            if (size+n>16*1024*1024 || f.segments.size()>16000000) return false;
            f.segments.push_back({at+offset,n}); size+=n; offset+=n;
            if (n==255) continue;
            Packet out; out.bytes=size; out.first=first; out.pieces=(uint32_t)f.segments.size()-first;
            if (out.pieces==1) { out.offset=f.segments.back().offset; out.pieces=0; }
            f.packets.push_back(out);
            if (!packet() || f.packets.size()>8000000) return false;
            // Data() may remap the file. Reacquire the page before reading more laces.
            p=f.file.View(at,bytes); if (!p) return false;
            size=0;
        }
        // -1 marks a page on which no packet ends.
        if (granule!=UINT64_MAX) { if (lastGranule!=UINT64_MAX && granule<lastGranule) return false; lastGranule=granule; }
        ended=(flags&4)!=0; at+=bytes;
    }
    return ended && !size;
}
}

// Reassembles Opus packets: the first two are the OpusHead and OpusTags headers.
bool PacketFile::OpenOggOpus(const std::wstring& path) {
    if (!file.Open(path)) return false;
    if (IsMatroska()) return OpenMatroska(path) && codec==AudioOpus;
    unsigned number=0;
    uint64_t lastGranule=UINT64_MAX;
    const bool ended=OggStream(*this,lastGranule,[&] {
        const size_t index=packets.size()-1;
        const uint32_t size=packets[index].bytes;
        const uint8_t* data=Data(index);
        if (!data) return false;
        if (number==0) {
            if (size<19 || memcmp(data,"OpusHead",8) || data[8]>15 || !data[9]) return false;
            config.assign(data,data+size); channels=data[9]; skip=U16(data+10); rate=48000; codec=0x4F707573;
            if (data[18] || channels>2) return false;
            packets.pop_back();
        } else if (number==1) {
            if (size<8 || memcmp(data,"OpusTags",8)) return false;
            packets.pop_back();
        } else {
            OpusPacket framing;
            if (!framing.Parse({data,size})) return false;
            packets.back().start=frames; packets.back().frames=framing.samples; frames+=framing.samples;
        }
        ++number;
        return true;
    });
    // A final granule counts 48 kHz samples including pre-skip, and must lie inside the coded range.
    if (!ended || number<3 || lastGranule==UINT64_MAX || lastGranule<skip || lastGranule>frames) return false;
    valid=lastGranule-skip;
    return true;
}

// The first packet is 0x7F "FLAC", the mapping version (1.x), a count of header packets, then
// "fLaC" and the STREAMINFO block. Each further header packet is one metadata block, until the
// block marked last; every packet after that is one FLAC frame. FlacDecoder reads the frames'
// own headers for their sample positions, so granules are only checked for order.
bool PacketFile::OpenOggFlac(const std::wstring& path) {
    if (!file.Size() && !file.Open(path)) return false;
    unsigned number=0;
    bool headers=true;
    size_t lastBlock=0;  // config offset of the latest block header, to mark the last one
    uint64_t lastGranule=UINT64_MAX;
    const bool ended=OggStream(*this,lastGranule,[&] {
        const size_t index=packets.size()-1;
        const uint32_t size=packets[index].bytes;
        const uint8_t* data=Data(index);
        if (!data) return false;
        if (number++==0) {
            if (size<51 || data[0]!=0x7F || memcmp(data+1,"FLAC",4) || data[5]!=1 || memcmp(data+9,"fLaC",4) ||
                (data[13]&127)!=0) return false;
            config.assign(data+9,data+size);
            lastBlock=4; headers=!(data[13]&128);
            packets.pop_back();
            return true;
        }
        // A frame starts with its sync code, which no metadata block header can (type 127 is invalid).
        if (headers && size && data[0]!=0xFF) {
            lastBlock=config.size();
            config.insert(config.end(),data,data+size);
            headers=!(data[0]&128);
            packets.pop_back();
            return true;
        }
        if (headers) { config[lastBlock]|=128; headers=false; }  // a muxer that left the last flag unset
        if (!size) packets.pop_back();
        return true;
    });
    if (!ended || number<2 || packets.empty()) return false;
    if (headers) config[lastBlock]|=128;
    codec=AudioFlac;
    return true;
}
} // namespace nativeaudio
