// RFC 6716 packet framing, also used for Ogg duration/index validation.
// OpusPacket parses one self-contained Opus packet: TOC byte, optional frame
// count and padding, then the per-frame length coding (codes 0-3), exposing the
// individual frame payloads plus the packet's 48 kHz sample count.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace nativeaudio {
struct OpusPacket {
    // TOC byte: config in bits 3-7, stereo flag in bit 2, frame code in bits 0-1.
    uint8_t toc=0;
    // Total duration in 48 kHz samples across all frames.
    unsigned samples=0;
    // Payload span of each frame, in order.
    std::vector<std::span<const uint8_t>> frames;
    bool Parse(std::span<const uint8_t> bytes) {
        frames.clear(); if (bytes.empty()) return false;
        toc=bytes[0]; unsigned config=toc>>3;
        // Frame duration in 48 kHz samples from the config; 5760 (120 ms) is the per-frame max.
        unsigned duration=config>=16?120u<<(config&3):config>=12?480u<<(config&1):(config&3)==3?2880u:480u<<(config&3);
        // Codes 0/1/2 mean 1 frame / 2 CBR frames / 2 VBR frames; code 3 signals its own count.
        unsigned code=toc&3,count=code==0?1:2; bool vbr=code==2;
        size_t at=1,end=bytes.size();
        if (code==3) {
            if (at==end) return false;
            unsigned flags=bytes[at++]; count=flags&63; vbr=(flags&128)!=0;
            if (!count || count>48) return false;
            // Code 3 may pad at the end; padding bytes are stripped before framing.
            if (flags&64) {
                unsigned n;
                do {
                    if (at==end) return false;
                    n=bytes[at++]; unsigned pad=n==255?254:n;
                    if (pad>end-at) return false; end-=pad;
                } while (n==255);
            }
        }
        if (count*duration>5760) return false;
        std::vector<size_t> lengths(count); size_t total=0;
        if (vbr) {
            // VBR: every frame length except the last is coded; 252+ escapes to 2 bytes.
            for (unsigned i=0;i+1<count;++i) {
                if (at==end) return false;
                unsigned n=bytes[at++];
                if (n>=252) { if (at==end) return false; n+=bytes[at++]*4; }
                lengths[i]=n; total+=n;
            }
            if (total>end-at) return false;
            lengths.back()=end-at-total;
        } else {
            // CBR: the remaining payload is split evenly across the frames.
            if ((end-at)%count) return false;
            for (auto& n:lengths) n=(end-at)/count;
        }
        // RFC limit: each frame is at most 1275 bytes.
        for (size_t n:lengths) { if (n>1275 || n>end-at) return false; frames.push_back(bytes.subspan(at,n)); at+=n; }
        samples=count*duration;
        return at==end;
    }
};
} // namespace nativeaudio
