// Decodes one of the official Opus conformance vectors (RFC 6716 section 6, updated by RFC 8251)
// with cordinal's own Opus decoder, for tools/opus-conformance.sh. Not part of the player.
//
//   cordinal-opus-conformance VECTOR.bit OUT.pcm
//
// A .bit file is what libopus's opus_demo reads: per packet a 32-bit big-endian length, the
// encoder's final range-coder state (32-bit big-endian), then the packet. The packets are wrapped in
// an Ogg Opus stream (two channels, no pre-skip, no gain) and decoded through OpenDecoder at 48 kHz,
// as the player plays a .opus file; OUT.pcm gets 16-bit little-endian stereo, rounded and clamped as
// opus_demo writes it, for the official opus_compare to judge against the vector's .dec files.
// Like opus_demo, the decoder's final range state after each packet must equal the encoder's: that
// proves the entropy decoding bit-exact. This program is built with CORDINAL_CODEC_TRACE, which has
// src/audio_opus.cpp print each frame's state; the frames are matched to packets by their count.
#include "audio_opus_packet.h"
#include "audio_packets.h"
#include "decoder.h"
#include "library.h"

#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

void Le(std::string& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out += (char)(v >> 8 * i);
}

// One Ogg page holding one whole packet (lacing allows any length below 255 * 255 bytes).
void Page(std::string& out, const std::string& packet, uint32_t sequence, uint64_t granule, uint8_t flags) {
    std::string page = "OggS";
    page += '\0';
    page += (char)flags;
    Le(page, granule, 8);
    Le(page, 0x0c0d1a1, 4);  // serial
    Le(page, sequence, 4);
    Le(page, 0, 4);          // CRC, filled below
    const size_t laces = packet.size() / 255 + 1;
    page += (char)laces;
    for (size_t i = 0; i + 1 < laces; ++i) page += (char)255;
    page += (char)(packet.size() % 255);
    page += packet;
    const uint32_t crc = nativeaudio::OggCrc((const uint8_t*)page.data(), page.size());
    for (int i = 0; i < 4; ++i) page[22 + i] = (char)(crc >> 8 * i);
    out += page;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: cordinal-opus-conformance VECTOR.bit OUT.pcm\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::string bits((std::istreambuf_iterator<char>(in)), {});
    if (!in && !in.eof()) {
        fprintf(stderr, "can't read %s\n", argv[1]);
        return 2;
    }

    // ---- The packets, and an Ogg Opus stream of them ----
    struct Expect { unsigned frames; uint32_t range; };
    std::vector<Expect> expected;
    std::string ogg, head = "OpusHead";
    head += (char)1;  // version
    head += (char)2;  // channels
    Le(head, 0, 2);   // pre-skip
    Le(head, 48000, 4);
    Le(head, 0, 2);   // output gain
    head += (char)0;  // mapping family
    Page(ogg, head, 0, 0, 2);
    // OpusTags: the vendor string's length and the string, then no comments.
    Page(ogg, std::string("OpusTags\x08\0\0\0cordinal\0\0\0\0", 24), 1, 0, 0);
    uint64_t granule = 0;
    uint32_t sequence = 2;
    std::vector<std::string> packets;
    for (size_t at = 0; at < bits.size();) {
        if (bits.size() - at < 8) { fprintf(stderr, "%s: a packet header is cut short\n", argv[1]); return 1; }
        const uint8_t* p = (const uint8_t*)bits.data() + at;
        const uint32_t length = Be32(p), range = Be32(p + 4);
        if (length == 0 || length > bits.size() - at - 8) {
            fprintf(stderr, "%s: packet %zu has length %u, which this check does not handle\n", argv[1], packets.size(), length);
            return 1;
        }
        packets.emplace_back(bits.data() + at + 8, length);
        nativeaudio::OpusPacket packet;
        if (!packet.Parse({(const uint8_t*)packets.back().data(), packets.back().size()})) {
            fprintf(stderr, "%s: packet %zu is not a valid Opus packet\n", argv[1], packets.size() - 1);
            return 1;
        }
        expected.push_back({(unsigned)packet.frames.size(), range});
        granule += packet.samples;
        at += 8 + length;
    }
    // Each packet on its own page, whose granule is the samples up to its end; the last ends the stream.
    uint64_t end = 0;
    for (size_t i = 0; i < packets.size(); ++i) {
        nativeaudio::OpusPacket packet;
        packet.Parse({(const uint8_t*)packets[i].data(), packets[i].size()});
        end += packet.samples;
        Page(ogg, packets[i], sequence++, end, i + 1 == packets.size() ? 4 : 0);
    }
    const std::string oggPath = std::string(argv[2]) + ".opus";
    {
        std::ofstream o(oggPath, std::ios::binary);
        o.write(ogg.data(), (std::streamsize)ogg.size());
        if (!o) { fprintf(stderr, "can't write %s\n", oggPath.c_str()); return 2; }
    }

    // ---- Decode, with the decoder's range trace going to a file ----
    const std::string tracePath = std::string(argv[2]) + ".ranges";
    fflush(stderr);
    const int savedErr = dup(2);
    FILE* trace = fopen(tracePath.c_str(), "w");
    if (!trace || savedErr < 0) { fprintf(stderr, "can't write %s\n", tracePath.c_str()); return 2; }
    dup2(fileno(trace), 2);
    std::wstring error;
    std::string pcm;
    uint64_t frames = 0;
    bool opened = false, rateOk = false;
    {
        auto d = OpenDecoder(FromUtf8(oggPath), 48000, error);
        if (d) {
            opened = true;
            rateOk = d->Rate() == 48000;
            std::vector<float> block(4096 * 2);
            for (size_t got; (got = d->Read(block.data(), 4096)) > 0;) {
                for (size_t i = 0; i < got * 2; ++i) {
                    const float v = std::fmax(-32768.f, std::fmin(32767.f, block[i] * 32768.f));
                    Le(pcm, (uint16_t)(int16_t)std::lrint(v), 2);
                }
                frames += got;
            }
        }
    }
    fflush(stderr);
    dup2(savedErr, 2);
    close(savedErr);
    fclose(trace);
    if (!opened) { fprintf(stderr, "%s: the decoder refused the stream: %ls\n", argv[1], error.c_str()); return 1; }
    if (!rateOk) { fprintf(stderr, "%s: the decoder did not open at 48 kHz\n", argv[1]); return 1; }
    {
        std::ofstream o(argv[2], std::ios::binary);
        o.write(pcm.data(), (std::streamsize)pcm.size());
        if (!o) { fprintf(stderr, "can't write %s\n", argv[2]); return 2; }
    }
    if (frames != granule) {
        fprintf(stderr, "%s: decoded %llu samples a channel where the packets hold %llu\n", argv[1],
                (unsigned long long)frames, (unsigned long long)granule);
        return 1;
    }

    // ---- Each packet's final range state against the encoder's ----
    std::vector<uint32_t> ranges;
    {
        std::ifstream t(tracePath);
        std::string line;
        while (std::getline(t, line)) {
            unsigned v = 0;
            if (sscanf(line.c_str(), "range %x", &v) == 1) ranges.push_back(v);
        }
    }
    size_t frame = 0, mismatches = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        frame += expected[i].frames;
        if (frame > ranges.size()) {
            fprintf(stderr, "%s: the decoder traced %zu frames where the packets hold more\n", argv[1], ranges.size());
            return 1;
        }
        if (ranges[frame - 1] != expected[i].range && mismatches++ < 5)
            fprintf(stderr, "%s: packet %zu ends with range state %08x where the encoder's is %08x\n", argv[1], i,
                    ranges[frame - 1], expected[i].range);
    }
    if (frame != ranges.size()) {
        fprintf(stderr, "%s: the decoder traced %zu frames where the packets hold %zu\n", argv[1], ranges.size(), frame);
        return 1;
    }
    unlink(tracePath.c_str());
    unlink(oggPath.c_str());
    if (mismatches) {
        fprintf(stderr, "%s: %zu of %zu packets end in the wrong range state\n", argv[1], mismatches, expected.size());
        return 1;
    }
    printf("%s: %zu packets, %llu samples, every final range state matches\n", argv[1], expected.size(),
           (unsigned long long)frames);
    return 0;
}
