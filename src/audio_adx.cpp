// Cordinal CRI ADX decoder (Sega/CRI Middleware game audio), written from the format's public
// descriptions. A big-endian header gives the channels, rate, sample count and a high-pass cutoff;
// the audio starts after the "(c)CRI" mark. Each channel takes 18-byte blocks in turn: a scale,
// then 32 four-bit samples, high nibble first. Every sample adds the scaled nibble to a two-tap
// prediction from the previous two, with coefficients derived from the cutoff. Standard ADX
// (encoding 3) only: no exponential-scale or fixed-coefficient variants, no encrypted files; loops
// play once.
#include "native_audio.h"
#include "audio_packets.h"
#include <cmath>
#include <numbers>

namespace {
using namespace nativeaudio;

class AdxDecoder final : public PacketDecoder {
public:
    bool Open(const std::wstring& path) {
        if (!media_.file.Open(path)) return false;
        const uint8_t* p = media_.file.View(0, 20);
        if (!p || U16(p, true) != 0x8000) return false;
        const unsigned offset = U16(p + 2, true), encoding = p[4], block = p[5], bits = p[6];
        channels_ = p[7];
        const unsigned rate = U32(p + 8, true), cutoff = U16(p + 16, true), version = p[18], flags = p[19];
        const uint64_t samples = U32(p + 12, true);
        if (encoding != 3 || block != 18 || bits != 4 || !channels_ || channels_ > 8 || !rate || rate > 1048575 ||
            (version != 3 && version != 4) || flags || offset < 6 || offset + 4 > media_.file.Size()) return false;
        // The copyright mark ends where the audio begins.
        const uint8_t* mark = media_.file.View(offset - 2, 6);
        if (!mark || memcmp(mark, "(c)CRI", 6)) return false;
        // The prediction: a two-pole high-pass at the cutoff, as 12-bit fixed point.
        const double a = std::numbers::sqrt2 - std::cos(2 * std::numbers::pi * cutoff / rate), b = std::numbers::sqrt2 - 1;
        const double c = (a - std::sqrt((a + b) * (a - b))) / b;
        coef1_ = (int)std::lrint(c * 8192);
        coef2_ = (int)std::lrint(-(c * c) * 4096);
        // 64 blocks a channel per packet (2048 frames).
        const uint32_t frameBytes = 18 * channels_;
        const uint64_t start = offset + 4, bytes = (media_.file.Size() - start) / frameBytes * frameBytes;
        for (uint64_t at = 0; at < bytes && media_.frames < samples; at += frameBytes * 64) {
            Packet packet;
            packet.offset = start + at;
            packet.bytes = (uint32_t)std::min<uint64_t>(frameBytes * 64, bytes - at);
            packet.frames = packet.bytes / frameBytes * 32;
            packet.start = media_.frames;
            media_.packets.push_back(packet);
            media_.frames += packet.frames;
        }
        if (media_.packets.empty()) return false;
        media_.valid = std::min(samples, media_.frames);
        return Ready(rate);
    }

private:
    struct History : State { int s1[8]{}, s2[8]{}; };
    History h_;
    unsigned channels_ = 0;
    int coef1_ = 0, coef2_ = 0;
    bool Decode(const uint8_t* p, size_t bytes, std::vector<float>& out) override {
        const size_t blocks = bytes / (18 * channels_);
        out.resize(blocks * 32 * 2);
        float planar[8][32];
        for (size_t k = 0; k < blocks; ++k) {
            for (unsigned c = 0; c < channels_; ++c, p += 18) {
                // A scale with its top bit set marks the end of the stream: it holds silence.
                const unsigned scale = U16(p, true);
                int s1 = h_.s1[c], s2 = h_.s2[c];
                for (unsigned i = 0; i < 32; ++i) {
                    int nibble = (p[2 + i / 2] >> ((i & 1) ? 0 : 4)) & 15;
                    if (nibble & 8) nibble -= 16;
                    int s = (scale & 0x8000) ? 0 : nibble * (int)scale + ((coef1_ * s1 + coef2_ * s2) >> 12);
                    s = std::clamp(s, -32768, 32767);
                    s2 = s1;
                    s1 = s;
                    planar[c][i] = s * (1.0f / 32768);
                }
                h_.s1[c] = s1;
                h_.s2[c] = s2;
            }
            float* o = out.data() + k * 64;
            for (unsigned i = 0; i < 32; ++i) {
                float frame[8];
                for (unsigned c = 0; c < channels_; ++c) frame[c] = planar[c][i];
                Mix(frame, channels_, o + i * 2);
            }
        }
        return true;
    }
    void Reset() override { h_ = History{}; }
    size_t Preroll() const override { return SIZE_MAX; }
    std::shared_ptr<const State> Capture() const override { return std::make_shared<History>(h_); }
    bool Restore(const State& s) override { h_ = static_cast<const History&>(s); return true; }
};
}  // namespace

std::unique_ptr<Decoder> OpenAdxDecoder(const std::wstring& path) {
    auto d = std::make_unique<AdxDecoder>();
    return d->Open(path) ? std::move(d) : nullptr;
}
