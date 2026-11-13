// Speech and game codecs whose bytes sit in a WAV or AU file, or make up a raw file of their own:
// G.722, G.726, GSM 06.10 and DFPWM. audio_pcm.cpp finds such a stream in a container (CodedStream);
// each codec's file decodes it. Their state runs on from one block to the next, so they are
// PacketDecoders over fixed-size packets, and seeks restore a checkpoint and decode forward.
#pragma once
#include "audio_packets.h"

namespace nativeaudio {
// WAVE format tags. AU encodings 24 (G.722) and 23, 25, 26 (G.726 at 4, 3 and 5 bits) map onto them.
constexpr unsigned kWaveGsm = 0x31, kWaveG726 = 0x45, kWaveG722 = 0x028F;
constexpr unsigned kWaveDfpwm = 0x10001;  // WAVE_FORMAT_EXTENSIBLE with DFPWM's GUID; above 16 bits

struct CodedStream {
    unsigned tag = 0;
    unsigned rate = 0, channels = 0, align = 0, bits = 0;  // bits: per coded sample, as the header says
    unsigned blockFrames = 0;                              // GSM in WAV: samples per block
    uint64_t start = 0, bytes = 0;
    uint64_t frames = UINT64_MAX;                          // the declared length (WAV fact), if any
    bool lowBitsFirst = false;                             // G.726 code packing: AU low bits first, WAV high
};

// Splits [start, start + bytes) into packets of packetBytes, each packetFrames long; a short last
// packet keeps the frames its bytes hold. `frames` (if known) trims the end.
inline bool FixedPackets(PacketFile& f, uint64_t start, uint64_t bytes, uint32_t packetBytes, uint32_t packetFrames,
                         uint64_t frames = UINT64_MAX) {
    if (!packetBytes || !packetFrames || bytes / packetBytes > 8000000) return false;
    for (uint64_t at = 0; at < bytes; at += packetBytes) {
        Packet p;
        p.offset = start + at;
        p.bytes = (uint32_t)std::min<uint64_t>(packetBytes, bytes - at);
        p.frames = (uint32_t)((uint64_t)packetFrames * p.bytes / packetBytes);
        p.start = f.frames;
        if (!p.frames) break;
        f.packets.push_back(p);
        f.frames += p.frames;
    }
    if (frames != UINT64_MAX) f.valid = std::min(frames, f.frames);
    return !f.packets.empty();
}
}  // namespace nativeaudio

// Each codec opens a stream found in a container, or a raw file of its own (by extension).
std::unique_ptr<Decoder> OpenCodedStream(const std::wstring& path, const nativeaudio::CodedStream& s);
std::unique_ptr<Decoder> OpenG722Decoder(const std::wstring& path, const nativeaudio::CodedStream* s = nullptr);
std::unique_ptr<Decoder> OpenG726Decoder(const std::wstring& path, const nativeaudio::CodedStream& s);
std::unique_ptr<Decoder> OpenGsmDecoder(const std::wstring& path, const nativeaudio::CodedStream* s = nullptr);
std::unique_ptr<Decoder> OpenDfpwmDecoder(const std::wstring& path, const nativeaudio::CodedStream* s = nullptr);
