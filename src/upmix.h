// Stereo to surround upmixing for outputs with more than two speakers (5.1, 7.1 and the like).
// The front pair keeps the original left and right untouched, so a speaker that is missing or
// unlinked loses nothing; the other speakers are derived from them, as a passive matrix decoder
// does: the center and subwoofer from what both sides share, the surrounds from what differs.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class Speaker : uint8_t { FrontLeft, FrontRight, Center, Lfe, BackLeft, BackRight, SideLeft, SideRight, Other };

constexpr size_t kMaxSpeakers = 8;

// A device's channels in the order its frames interleave them.
struct SpeakerLayout {
    uint8_t count = 0;
    std::array<Speaker, kMaxSpeakers> at{};
    bool Has(Speaker s) const;
    // True when the layout has front left and right and at least one speaker the upmixer feeds.
    bool Upmixes() const;
    bool operator==(const SpeakerLayout& o) const;
};

// From a WAVEFORMATEXTENSIBLE dwChannelMask (speakers in ascending bit order). A zero mask takes
// Windows' default layout for that many channels. Channels past kMaxSpeakers make an empty layout.
SpeakerLayout LayoutFromMask(uint32_t mask, unsigned channels);
// From PipeWire channel names ("FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"; anything else is Other).
SpeakerLayout LayoutFromNames(const std::vector<std::string>& names);
// PipeWire's name for a speaker ("FL" ...), and PulseAudio's channel position number.
const char* SpeakerName(Speaker s);
uint8_t PulsePosition(Speaker s);
// PulseAudio channel position number to PipeWire's name ("" for positions the upmixer does not know).
const char* PulsePositionName(uint8_t position);
// What to call the layout: "5.1", "7.1", "4.0", "3.1"... counting fronts plus center plus surrounds.
std::string LayoutLabel(const SpeakerLayout& layout);

// Runs on the render thread: Configure may be called there too and never allocates.
class Upmixer {
public:
    Upmixer();
    void Configure(const SpeakerLayout& layout, unsigned rate);
    void Reset();  // clears filter and delay state (after a flush)
    // `frames` interleaved stereo frames in, the same frames in the layout's channels out.
    void Process(const float* stereo, size_t frames, float* out);
    const SpeakerLayout& Layout() const { return layout_; }
    unsigned Rate() const { return rate_; }

    // Mixing levels, exposed for the tests.
    static constexpr float kCenterGain = 0.5f;     // of (L+R)/2, so a centered voice gains about 1 dB overall
    static constexpr float kLfeGain = 0.5f;        // of (L+R)/2 below the crossover; receivers add 10 dB
    static constexpr float kSurroundGain = 0.7071f;
    static constexpr double kLfeCutoff = 120, kSurroundLow = 100, kSurroundHigh = 7000;
    static constexpr double kSurroundDelayMs = 12;

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double Run(double x);
    };
    SpeakerLayout layout_;
    unsigned rate_ = 0;
    Biquad lfe_[2], surroundLow_, surroundHigh_;
    std::vector<float> delay_;
    size_t delayFrames_ = 0, delayAt_ = 0;
    float surroundGain_ = kSurroundGain;  // shared between side and back pairs when both exist
};
