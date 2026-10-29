// Game Boy APU: two pulses (the first with sweep), a 32-step wave channel and noise, rendered
// event-driven at their timer periods and driven by a 512 Hz frame sequencer for length, sweep
// and envelopes. Register writes/reads advance the chip to the caller's time first, then emit
// level deltas to the pair of BandBuffers. All times are 4.19 MHz clocks within the frame.
#include "gb_apu.h"

#include <algorithm>

namespace {

constexpr float kGain = 1.0f / 640;  // per level step times master volume (1..8)
constexpr uint32_t kSequencerPeriod = 8192;  // 512 Hz
// Which of the 8 duty steps are high (bit n = step n): 12.5%, 25%, 50%, 75%.
constexpr uint8_t kDuty[4] = {0x80, 0x81, 0xE1, 0x7E};
// Bits that always read back as 1, for $FF10-$FF2F.
constexpr uint8_t kReadMask[0x20] = {0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F,
                                     0xFF, 0x9F, 0xFF, 0xBF, 0xFF, 0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00,
                                     0x70, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
enum : int { NR10 = 0x00, NR30 = 0x0A, NR32 = 0x0C, NR43 = 0x12, NR50 = 0x14, NR51 = 0x15, NR52 = 0x16, kWave = 0x20 };

}  // namespace

void GbApu::Reset(BandBuffer* left, BandBuffer* right) {
    left_ = left;
    right_ = right;
    std::fill(std::begin(regs_), std::end(regs_), (uint8_t)0);
    for (Channel& c : ch_) c = Channel();
    lfsr_ = 0x7FFF;
    sweepTimer_ = 0;
    sweepShadow_ = 0;
    sweepOn_ = false;
    seqStep_ = 0;
    seqNext_ = kSequencerPeriod;
}

bool GbApu::Dac(int c) const { return c == 2 ? (regs_[NR30] & 0x80) != 0 : (regs_[c * 5 + 2] & 0xF8) != 0; }

int GbApu::Level(int c) const {
    const Channel& ch = ch_[c];
    if (!ch.on || !Dac(c)) return 0;
    switch (c) {
        case 0:
        case 1: return (kDuty[regs_[c * 5 + 1] >> 6] >> ch.phase) & 1 ? ch.volume : 0;
        case 2: {
            static const int kShift[4] = {4, 0, 1, 2};  // mute, 100%, 50%, 25%
            const uint8_t byte = regs_[kWave + ch.phase / 2];
            const int sample = (ch.phase & 1) ? byte & 15 : byte >> 4;
            return sample >> kShift[(regs_[NR32] >> 5) & 3];
        }
        default: return (lfsr_ & 1) ? 0 : ch.volume;
    }
}

void GbApu::Emit(int c, uint32_t t) {
    Channel& ch = ch_[c];
    const int level = Level(c), pan = regs_[NR51], master = regs_[NR50];
    const int l = (pan >> (c + 4)) & 1 ? level * (((master >> 4) & 7) + 1) : 0;
    const int r = (pan >> c) & 1 ? level * ((master & 7) + 1) : 0;
    if (l != ch.lastL) left_->Add(t, (float)(l - ch.lastL) * kGain), ch.lastL = l;
    if (r != ch.lastR) right_->Add(t, (float)(r - ch.lastR) * kGain), ch.lastR = r;
}

void GbApu::EmitAll(uint32_t t) {
    for (int c = 0; c < 4; ++c) Emit(c, t);
}

void GbApu::RunChannel(int c, uint32_t t) {
    Channel& ch = ch_[c];
    uint32_t period;
    if (c == 3) {
        const int shift = regs_[NR43] >> 4, code = regs_[NR43] & 7;
        if (shift >= 14) {  // the noise generator gets no clocks
            ch.next = std::max(ch.next, t);
            return;
        }
        period = (uint32_t)(code ? code * 16 : 8) << shift;
    } else {
        period = (2048u - ch.freq) * (c == 2 ? 2u : 4u);
    }
    const bool audible = ch.on && Dac(c);
    if (!audible) {  // nothing to hear; a trigger resets the phase and the noise register anyway
        if (ch.next < t) ch.next += (t - ch.next + period - 1) / period * period;
        return;
    }
    while (ch.next < t) {
        if (c == 3) {
            const unsigned x = (lfsr_ ^ (lfsr_ >> 1)) & 1;
            lfsr_ = (uint16_t)((lfsr_ >> 1) | (x << 14));
            if (regs_[NR43] & 8) lfsr_ = (uint16_t)((lfsr_ & ~0x40) | (x << 6));
        } else {
            ch.phase = (ch.phase + 1) & (c == 2 ? 31 : 7);
        }
        Emit(c, ch.next);
        ch.next += period;
    }
}

int GbApu::SweepTarget() {
    const int shift = regs_[NR10] & 7;
    const int delta = sweepShadow_ >> shift;
    const int target = (regs_[NR10] & 8) ? sweepShadow_ - delta : sweepShadow_ + delta;
    if (target > 2047) ch_[0].on = false;
    return target;
}

void GbApu::Sequencer() {
    if (!(seqStep_ & 1)) {
        for (Channel& ch : ch_)
            if (ch.lengthEnable && ch.length > 0 && --ch.length == 0) ch.on = false;
    }
    if (seqStep_ == 2 || seqStep_ == 6) {
        const int period = (regs_[NR10] >> 4) & 7;
        if (--sweepTimer_ <= 0) {
            sweepTimer_ = period ? period : 8;
            if (sweepOn_ && period) {
                const int target = SweepTarget();
                if (target <= 2047 && (regs_[NR10] & 7)) {
                    sweepShadow_ = (uint16_t)target;
                    ch_[0].freq = (uint16_t)target;
                    SweepTarget();
                }
            }
        }
    }
    if (seqStep_ == 7) {
        for (int c : {0, 1, 3}) {
            Channel& ch = ch_[c];
            if (!ch.envPeriod || --ch.envTimer > 0) continue;
            ch.envTimer = ch.envPeriod;
            if (ch.envUp && ch.volume < 15) ++ch.volume;
            else if (!ch.envUp && ch.volume > 0) --ch.volume;
        }
    }
    seqStep_ = (seqStep_ + 1) & 7;
}

void GbApu::RunTo(uint32_t t) {
    while (seqNext_ <= t) {
        for (int c = 0; c < 4; ++c) RunChannel(c, seqNext_);
        Sequencer();
        EmitAll(seqNext_);
        seqNext_ += kSequencerPeriod;
    }
    for (int c = 0; c < 4; ++c) RunChannel(c, t);
}

void GbApu::Trigger(int c) {
    Channel& ch = ch_[c];
    const uint8_t* r = regs_ + c * 5;
    ch.on = Dac(c);
    if (ch.length == 0) ch.length = c == 2 ? 256 : 64;
    ch.freq = (uint16_t)(r[3] | (r[4] & 7) << 8);
    ch.phase = 0;
    if (c != 2) {
        ch.volume = r[2] >> 4;
        ch.envUp = (r[2] & 8) != 0;
        ch.envPeriod = ch.envTimer = r[2] & 7;
    }
    if (c == 3) lfsr_ = 0x7FFF;
    if (c == 0) {
        const int period = (regs_[NR10] >> 4) & 7, shift = regs_[NR10] & 7;
        sweepShadow_ = ch.freq;
        sweepTimer_ = period ? period : 8;
        sweepOn_ = period || shift;
        if (shift) SweepTarget();
    }
}

void GbApu::Write(uint32_t t, uint16_t addr, uint8_t v) {
    const int i = addr - 0xFF10;
    if (i < 0 || i >= 0x30) return;
    RunTo(t);
    const bool power = (regs_[NR52] & 0x80) != 0;
    if (i >= kWave) {
        regs_[i] = v;
        return;
    }
    if (i == NR52) {
        regs_[NR52] = v & 0x80;
        if (!(v & 0x80)) {  // power off clears every sound register
            std::fill(regs_, regs_ + NR52, (uint8_t)0);
            for (Channel& ch : ch_) ch.on = false;
        }
        EmitAll(t);
        return;
    }
    if (!power) return;
    regs_[i] = v;
    if (i < 0x14) {
        const int c = i / 5, reg = i % 5;
        Channel& ch = ch_[c];
        switch (reg) {
            case 1: ch.length = c == 2 ? 256 - v : 64 - (v & 63); break;
            case 2: if (!Dac(c)) ch.on = false; break;
            case 3: ch.freq = (uint16_t)((ch.freq & 0x700) | v); break;
            case 4:
                ch.freq = (uint16_t)((ch.freq & 0xFF) | (v & 7) << 8);
                ch.lengthEnable = (v & 0x40) != 0;
                if (v & 0x80) {
                    Trigger(c);
                    ch.next = t;
                }
                break;
            default:
                if (c == 2 && !Dac(2)) ch.on = false;  // NR30
                break;
        }
    }
    EmitAll(t);
}

uint8_t GbApu::Read(uint32_t t, uint16_t addr) {
    const int i = addr - 0xFF10;
    if (i < 0 || i >= 0x30) return 0xFF;
    RunTo(t);
    if (i >= kWave) return regs_[i];
    if (i == NR52) {
        uint8_t v = (uint8_t)(regs_[NR52] | 0x70);
        for (int c = 0; c < 4; ++c)
            if (ch_[c].on) v |= (uint8_t)(1 << c);
        return v;
    }
    return regs_[i] | kReadMask[i];
}

void GbApu::EndFrame(uint32_t t) {
    RunTo(t);
    for (Channel& ch : ch_) ch.next -= std::min(ch.next, t);
    seqNext_ -= t;
}
