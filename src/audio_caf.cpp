// Cordinal CAF packet index for packet codecs (ALAC). Apple's Core Audio Format: 'desc' gives the
// format, 'kuki' the codec's magic cookie, 'pakt' the size of every packet plus the priming and
// remainder frames to trim, and 'data' the packets themselves. PCM, G.711 and IMA4 in CAF are
// constant-size and play through audio_pcm.cpp instead.
#include "audio_packets.h"

namespace nativeaudio {
// pakt entries are big-endian base-128 integers: 7 bits a byte, the top bit set on all but the last.
static bool PaktInteger(const uint8_t* p, size_t n, size_t& at, uint64_t& value) {
    value = 0;
    for (int i = 0; i < 9 && at < n; ++i) {
        const uint8_t b = p[at++];
        value = value << 7 | (b & 127);
        if (!(b & 128)) return true;
    }
    return false;
}

bool PacketFile::OpenCaf(const std::wstring& path) {
    if (!file.Size() && !file.Open(path)) return false;
    const uint8_t* p = file.View(0, 8);
    if (!p || !Is(p, "caff") || U16(p + 4, true) != 1) return false;
    const uint64_t limit = file.Size();
    uint64_t dataStart = 0, dataBytes = 0, packetBytes = 0, packetFrames = 0;
    uint64_t declaredPackets = 0, validFrames = 0, priming = 0, remainder = 0;
    std::vector<uint8_t> table;
    bool desc = false, paktFound = false;
    for (uint64_t at = 8; at + 12 <= limit;) {
        if (!(p = file.View(at, 12))) return false;
        uint64_t n = U64(p + 4, true);
        const uint64_t body = at + 12;
        const bool isData = Is(p, "data");
        if (isData && n == UINT64_MAX) n = limit - body;
        if (n > limit - body) return false;
        if (Is(p, "desc")) {
            if (n < 32 || !(p = file.View(body, 32))) return false;
            const uint64_t bits = U64(p, true);
            double sampleRate;
            memcpy(&sampleRate, &bits, 8);
            if (!std::isfinite(sampleRate) || sampleRate < 1 || sampleRate > 768000) return false;
            rate = (uint32_t)std::llround(sampleRate);
            codec = U32(p + 8, true);
            packetBytes = U32(p + 16, true);
            packetFrames = U32(p + 20, true);
            channels = U32(p + 24, true);
            sampleBits = U32(p + 28, true);
            desc = true;
        } else if (Is(p, "kuki")) {
            if (n > 65536 || !(p = file.View(body, (size_t)n))) return false;
            config.assign(p, p + n);
        } else if (Is(p, "pakt")) {
            if (n < 24 || n > (64u << 20) || !(p = file.View(body, (size_t)n))) return false;
            declaredPackets = U64(p, true);
            validFrames = U64(p + 8, true);
            priming = U32(p + 16, true);
            remainder = U32(p + 20, true);
            table.assign(p + 24, p + n);
            paktFound = true;
        } else if (isData) {
            if (n < 4) return false;
            dataStart = body + 4;
            dataBytes = n - 4;
        }
        at = body + n;
    }
    if (!desc || !dataStart || !channels || channels > 8) return false;
    // Packets lie back to back from the start of the data. Fixed sizes or frame counts are in
    // 'desc'; a zero there means each pakt entry gives it.
    if (!packetBytes && !paktFound) return false;
    if (!packetFrames && !paktFound) return false;
    const uint64_t count = paktFound ? declaredPackets : dataBytes / packetBytes;
    if (count > 8000000) return false;
    packets.reserve((size_t)count);
    uint64_t offset = dataStart;
    size_t at = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t bytes = packetBytes, length = packetFrames;
        if (!bytes && !PaktInteger(table.data(), table.size(), at, bytes)) return false;
        if (!length && !PaktInteger(table.data(), table.size(), at, length)) return false;
        if (!bytes || bytes > 16 * 1024 * 1024 || !length || length > 65536 ||
            bytes > dataStart + dataBytes - offset) return false;
        Packet packet;
        packet.offset = offset;
        packet.bytes = (uint32_t)bytes;
        packet.start = frames;
        packet.frames = (uint32_t)length;
        packets.push_back(packet);
        offset += bytes;
        frames += length;
    }
    if (packets.empty()) return false;
    // A declared length must fit inside what the packets hold.
    if (paktFound) {
        if (priming > frames || remainder > frames - priming) return false;
        if (validFrames && validFrames > frames - priming) return false;
        skip = priming;
        valid = validFrames ? validFrames : frames - priming - remainder;
    }
    return true;
}
} // namespace nativeaudio
