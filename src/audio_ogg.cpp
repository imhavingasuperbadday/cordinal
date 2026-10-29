// Cordinal Ogg Opus packet index: CRC, page sequence, lacing, continuation and granules.
// Reassembles Opus packets (plus the OpusHead/OpusTags headers) and records the final granule
// so the decoder can trim the pre-skip and padding.
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
}
// Page structure is fixed: 27-byte header, segment table, then payload. A packet ends at a
// lacing value < 255; larger values continue into the next page.
bool PacketFile::OpenOggOpus(const std::wstring& path) {
    if (!file.Open(path)) return false;
    if (IsMatroska()) return OpenMatroska(path) && codec==AudioOpus;
    uint32_t serial=0,sequence=0; bool seen=false,ended=false;
    uint64_t at=0,lastGranule=UINT64_MAX;
    unsigned number=0; uint32_t size=0,first=0;
    std::vector<uint8_t> header;
    while (at<file.Size()) {
        const uint8_t* p=file.View(at,27);
        if (!p || !Is(p,"OggS") || p[4]) return false;
        unsigned flags=p[5],laces=p[26];
        p=file.View(at,27+laces); if (!p) return false;
        size_t payload=0; for (unsigned i=0;i<laces;++i) payload+=p[27+i];
        size_t bytes=27+laces+payload;
        p=file.View(at,bytes); if (!p || Crc(p,bytes)!=U32(p+22)) return false;
        uint32_t stream=U32(p+14),seq=U32(p+18);
        if (ended || (seen && (stream!=serial || seq!=sequence+1 || (flags&2))) || (!seen && (!(flags&2) || (flags&1)))) return false;
        if (bool(flags&1)!=bool(size)) return false;
        seen=true; serial=stream; sequence=seq;
        uint64_t granule=U64(p+6);
        size_t offset=27+laces;
        for (unsigned i=0;i<laces;++i) {
            unsigned n=p[27+i];
            if (!size) { first=(uint32_t)segments.size(); header.clear(); }
            if (size+n>16*1024*1024 || segments.size()>16000000) return false;
            if (number<2) header.insert(header.end(),p+offset,p+offset+n);
            segments.push_back({at+offset,n}); size+=n; offset+=n;
            if (n==255) continue;
            // The first two packets are the OpusHead and OpusTags headers.
            if (number==0) {
                if (header.size()<19 || memcmp(header.data(),"OpusHead",8) || header[8]>15 || !header[9]) return false;
                config=header; channels=header[9]; skip=U16(header.data()+10); rate=48000; codec=0x4F707573;
                if (header[18] || channels>2) return false;
            } else if (number==1) {
                if (header.size()<8 || memcmp(header.data(),"OpusTags",8)) return false;
            } else {
                Packet packet; packet.bytes=size; packet.first=first; packet.pieces=(uint32_t)segments.size()-first; packet.start=frames;
                if (packet.pieces==1) { packet.offset=segments.back().offset; packet.pieces=0; }
                packets.push_back(packet);
                const uint8_t* data=Data(packets.size()-1); OpusPacket framing;
                if (!data || !framing.Parse({data,size})) return false;
                packets.back().frames=framing.samples; frames+=framing.samples;
                if (packets.size()>8000000) return false;
                // Data() may remap the file. Reacquire the page before reading more laces.
                p=file.View(at,bytes); if (!p) return false;
            }
            ++number; size=0;
        }
        // A final granule counts 48 kHz samples including pre-skip; -1 marks a page with none.
        if (granule!=UINT64_MAX) { if (lastGranule!=UINT64_MAX && granule<lastGranule) return false; lastGranule=granule; }
        ended=(flags&4)!=0; at+=bytes;
    }
    // Require a clean end-of-stream flag, no partial packet, and a granule inside the coded range.
    if (!ended || size || number<3 || lastGranule==UINT64_MAX || lastGranule<skip || lastGranule>frames) return false;
    valid=lastGranule-skip;
    return true;
}
} // namespace nativeaudio
