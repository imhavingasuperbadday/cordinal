// Stereo to surround upmixing (see upmix.h): layouts from Windows masks and PipeWire/PulseAudio
// channel names, and the matrix itself.
#include "upmix.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;
// The longest delay the surrounds can need: kSurroundDelayMs at 768 kHz, with room to spare.
constexpr size_t kDelayCapacity = 16384;

}  // namespace

bool SpeakerLayout::Has(Speaker s) const {
    for (uint8_t i = 0; i < count; ++i)
        if (at[i] == s) return true;
    return false;
}

bool SpeakerLayout::Upmixes() const {
    if (!Has(Speaker::FrontLeft) || !Has(Speaker::FrontRight)) return false;
    for (Speaker s : {Speaker::Center, Speaker::Lfe, Speaker::BackLeft, Speaker::BackRight, Speaker::SideLeft,
                      Speaker::SideRight})
        if (Has(s)) return true;
    return false;
}

bool SpeakerLayout::operator==(const SpeakerLayout& o) const {
    return count == o.count && std::equal(at.begin(), at.begin() + count, o.at.begin());
}

SpeakerLayout LayoutFromMask(uint32_t mask, unsigned channels) {
    if (!mask) {
        // Windows' defaults (KSAUDIO_SPEAKER_*) for a format without a mask.
        switch (channels) {
        case 2: mask = 0x3; break;
        case 3: mask = 0x7; break;
        case 4: mask = 0x33; break;   // quad
        case 6: mask = 0x3F; break;   // 5.1 with back speakers
        case 8: mask = 0x63F; break;  // 7.1 surround
        default: break;
        }
    }
    SpeakerLayout l;
    if (channels > kMaxSpeakers) return l;
    for (uint32_t bit = 0; bit < 32 && l.count < channels; ++bit) {
        if (!(mask & (1u << bit))) continue;
        Speaker s = Speaker::Other;
        switch (1u << bit) {
        case 0x1: s = Speaker::FrontLeft; break;
        case 0x2: s = Speaker::FrontRight; break;
        case 0x4: s = Speaker::Center; break;
        case 0x8: s = Speaker::Lfe; break;
        case 0x10: s = Speaker::BackLeft; break;
        case 0x20: s = Speaker::BackRight; break;
        case 0x200: s = Speaker::SideLeft; break;
        case 0x400: s = Speaker::SideRight; break;
        default: break;
        }
        l.at[l.count++] = s;
    }
    // A mask naming fewer speakers than there are channels leaves the rest unassigned.
    while (l.count < channels) l.at[l.count++] = Speaker::Other;
    return l;
}

SpeakerLayout LayoutFromNames(const std::vector<std::string>& names) {
    SpeakerLayout l;
    if (names.size() > kMaxSpeakers) return l;
    for (const std::string& n : names) {
        Speaker s = Speaker::Other;
        for (uint8_t i = 0; i < (uint8_t)Speaker::Other; ++i)
            if (n == SpeakerName((Speaker)i)) s = (Speaker)i;
        l.at[l.count++] = s;
    }
    return l;
}

const char* SpeakerName(Speaker s) {
    switch (s) {
    case Speaker::FrontLeft: return "FL";
    case Speaker::FrontRight: return "FR";
    case Speaker::Center: return "FC";
    case Speaker::Lfe: return "LFE";
    case Speaker::BackLeft: return "RL";
    case Speaker::BackRight: return "RR";
    case Speaker::SideLeft: return "SL";
    case Speaker::SideRight: return "SR";
    default: return "AUX0";
    }
}

// PulseAudio's pa_channel_position_t: 1 front left, 2 front right, 3 front center, 5/6 rear left
// and right, 7 LFE, 10/11 side left and right.
uint8_t PulsePosition(Speaker s) {
    switch (s) {
    case Speaker::FrontLeft: return 1;
    case Speaker::FrontRight: return 2;
    case Speaker::Center: return 3;
    case Speaker::Lfe: return 7;
    case Speaker::BackLeft: return 5;
    case Speaker::BackRight: return 6;
    case Speaker::SideLeft: return 10;
    case Speaker::SideRight: return 11;
    default: return 12;  // aux0
    }
}

const char* PulsePositionName(uint8_t position) {
    for (uint8_t i = 0; i < (uint8_t)Speaker::Other; ++i)
        if (PulsePosition((Speaker)i) == position) return SpeakerName((Speaker)i);
    return "";
}

std::string LayoutLabel(const SpeakerLayout& l) {
    int main = 0, lfe = 0;
    for (uint8_t i = 0; i < l.count; ++i) {
        if (l.at[i] == Speaker::Lfe) ++lfe;
        else if (l.at[i] != Speaker::Other) ++main;
    }
    return std::to_string(main) + "." + std::to_string(lfe);
}

double Upmixer::Biquad::Run(double x) {
    double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

namespace {

// RBJ cookbook low and high pass at Q = 1/sqrt(2) (Butterworth).
void Design(double& b0, double& b1, double& b2, double& a1, double& a2, double hz, unsigned rate, bool high) {
    const double w = 2 * kPi * std::min(hz, rate * 0.45) / rate;
    const double cw = std::cos(w), alpha = std::sin(w) / (2 * 0.70710678118654752);
    const double a0 = 1 + alpha;
    if (high) {
        b0 = (1 + cw) / 2 / a0;
        b1 = -(1 + cw) / a0;
    } else {
        b0 = (1 - cw) / 2 / a0;
        b1 = (1 - cw) / a0;
    }
    b2 = b0;
    a1 = -2 * cw / a0;
    a2 = (1 - alpha) / a0;
}

}  // namespace

Upmixer::Upmixer() : delay_(kDelayCapacity, 0.0f) {}

void Upmixer::Configure(const SpeakerLayout& layout, unsigned rate) {
    if (layout == layout_ && rate == rate_) return;
    layout_ = layout;
    rate_ = rate ? rate : 48000;
    // Two Butterworth sections make a fourth-order Linkwitz-Riley crossover for the subwoofer.
    for (Biquad& b : lfe_) Design(b.b0, b.b1, b.b2, b.a1, b.a2, kLfeCutoff, rate_, false);
    Design(surroundLow_.b0, surroundLow_.b1, surroundLow_.b2, surroundLow_.a1, surroundLow_.a2, kSurroundLow, rate_, true);
    Design(surroundHigh_.b0, surroundHigh_.b1, surroundHigh_.b2, surroundHigh_.a1, surroundHigh_.a2, kSurroundHigh,
           rate_, false);
    delayFrames_ = std::min<size_t>((size_t)std::lround(kSurroundDelayMs * rate_ / 1000), kDelayCapacity - 1);
    // Both a side and a back pair share the surround signal, each 3 dB lower, so the room gets the
    // same surround power as from one pair.
    const bool sides = layout.Has(Speaker::SideLeft) || layout.Has(Speaker::SideRight);
    const bool backs = layout.Has(Speaker::BackLeft) || layout.Has(Speaker::BackRight);
    surroundGain_ = sides && backs ? kSurroundGain * 0.70710678f : kSurroundGain;
    Reset();
}

void Upmixer::Reset() {
    for (Biquad& b : lfe_) b.z1 = b.z2 = 0;
    surroundLow_.z1 = surroundLow_.z2 = surroundHigh_.z1 = surroundHigh_.z2 = 0;
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    delayAt_ = 0;
}

void Upmixer::Process(const float* stereo, size_t frames, float* out) {
    const size_t n = layout_.count;
    for (size_t f = 0; f < frames; ++f) {
        const float left = stereo[f * 2], right = stereo[f * 2 + 1];
        const double mid = 0.5 * ((double)left + right), side = 0.5 * ((double)left - right);
        const double low = lfe_[1].Run(lfe_[0].Run(mid));
        // The surrounds hear the difference signal band-limited and late, so the precedence effect
        // keeps the image at the front while ambience and reverb spread around the listener.
        const double band = surroundHigh_.Run(surroundLow_.Run(side));
        const float surround = delay_[delayAt_];
        delay_[delayAt_] = (float)band;
        if (++delayAt_ > delayFrames_) delayAt_ = 0;
        float* o = out + f * n;
        for (size_t c = 0; c < n; ++c) {
            float v = 0;
            switch (layout_.at[c]) {
            case Speaker::FrontLeft: v = left; break;
            case Speaker::FrontRight: v = right; break;
            case Speaker::Center: v = (float)(mid * kCenterGain); break;
            case Speaker::Lfe: v = (float)(low * kLfeGain); break;
            // Opposite polarity left and right, as a matrix decoder's surround channel.
            case Speaker::BackLeft:
            case Speaker::SideLeft: v = surround * surroundGain_; break;
            case Speaker::BackRight:
            case Speaker::SideRight: v = -surround * surroundGain_; break;
            default: break;
            }
            o[c] = v;
        }
    }
}
