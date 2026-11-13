// Cordinal DFPWM1a decoder (Dynamic Filter Pulse Width Modulation, Ben Russell's 1-bit codec that
// ComputerCraft speakers play), written from the format's published reference algorithm. Each bit
// steers a "charge" towards +127 or -128 at an adaptive "strength"; the decoder then smooths the
// result with an anti-jerk step and a fixed low-pass filter. Bits are read low bit first. Raw
// .dfpwm files carry no header: they are 48 kHz mono, as ComputerCraft writes them. WAV files with
// DFPWM's GUID give their own rate. Mono only.
#include "native_audio.h"
#include "audio_coded.h"

namespace {
using namespace nativeaudio;

constexpr int kPrecision = 10;      // strength is a fraction of 1 << kPrecision
constexpr int kFilterStrength = 140;  // low-pass, out of 256

class DfpwmDecoder final : public PacketDecoder {
public:
    bool Open(const std::wstring& path, const CodedStream* s) {
        if (!media_.file.Open(path)) return false;
        uint64_t start = 0, bytes = media_.file.Size(), frames = UINT64_MAX;
        unsigned rate = 48000;
        if (s) {
            if (s->channels != 1) return false;
            start = s->start; bytes = s->bytes; frames = s->frames; rate = s->rate;
        }
        if (!rate || rate > 1048575) return false;
        // 4 KB packets (32,768 samples); checkpoints keep seeks short.
        return FixedPackets(media_, start, bytes, 4096, 4096 * 8, frames) && Ready(rate);
    }

private:
    struct Charge : State { int charge = 0, strength = 0, filtered = 0; bool last = false; };
    Charge c_;
    bool Decode(const uint8_t* p, size_t bytes, std::vector<float>& out) override {
        out.resize(bytes * 16);
        float* o = out.data();
        int charge = c_.charge, strength = c_.strength, filtered = c_.filtered;
        bool last = c_.last;
        for (size_t i = 0; i < bytes; ++i) {
            unsigned d = p[i];
            for (int b = 0; b < 8; ++b, d >>= 1) {
                const bool bit = d & 1;
                const int target = bit ? 127 : -128;
                // Move the charge towards the target by the strength, at least one step.
                int next = charge + ((strength * (target - charge) + (1 << (kPrecision - 1))) >> kPrecision);
                if (next == charge && next != target) next += bit ? 1 : -1;
                const int previous = charge;
                charge = next;
                // Strength rises while the bits repeat and falls when they alternate.
                const int aim = bit == last ? (1 << kPrecision) - 1 : 0;
                if (strength != aim) strength += aim ? 1 : -1;
                if (strength < 2 << (kPrecision - 8)) strength = 2 << (kPrecision - 8);  // DFPWM1a's floor
                // Anti-jerk: halfway between the two charges where the bit changed; then low-pass.
                const int value = bit != last ? (next + previous + 1) >> 1 : next;
                filtered += (kFilterStrength * (value - filtered) + 0x80) >> 8;
                o[0] = o[1] = (float)filtered * (1.0f / 128);
                o += 2;
                last = bit;
            }
        }
        c_.charge = charge; c_.strength = strength; c_.filtered = filtered; c_.last = last;
        return true;
    }
    void Reset() override { c_ = Charge{}; }
    size_t Preroll() const override { return SIZE_MAX; }  // the state never settles: checkpoint it
    std::shared_ptr<const State> Capture() const override { return std::make_shared<Charge>(c_); }
    bool Restore(const State& s) override { c_ = static_cast<const Charge&>(s); return true; }
};
}  // namespace

std::unique_ptr<Decoder> OpenDfpwmDecoder(const std::wstring& path, const CodedStream* s) {
    auto d = std::make_unique<DfpwmDecoder>();
    return d->Open(path, s) ? std::move(d) : nullptr;
}
