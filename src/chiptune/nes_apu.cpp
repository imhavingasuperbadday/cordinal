// NES APU (2A03) plus the Konami VRC6, Namco 163 and Sunsoft 5B cartridge chips. Each channel is
// event-driven: it advances from one timer step to the next, emitting level changes to a
// BandBuffer with their exact CPU-cycle time. The 2A03's 4/5-step frame sequencer clocks the
// length, sweep and envelope units; the cartridge chips run off their own counters.
#include "nes_apu.h"

#include <algorithm>
#include <cmath>

namespace {

// Output per volume step, a linear fit of the 2A03's mixer (which is close to linear at the
// levels music uses). All channels at full level reach about 0.85.
constexpr float kPulseGain = 0.00752f, kTriangleGain = 0.00851f, kNoiseGain = 0.00494f, kDmcGain = 0.00335f;
// The cartridge chips relative to those, as measured against Game_Music_Emu's long-standing balance.
constexpr float kVrc6Gain = 0.00484f;

constexpr uint8_t kLength[32] = {10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                 12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
constexpr uint16_t kNoisePeriod[2][16] = {
    {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068},
    {4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778}};
constexpr uint16_t kDmcPeriod[2][16] = {
    {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54},
    {398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50}};
// Which of the 8 sequencer steps are high, per duty setting (bit n = step n).
constexpr uint8_t kDuty[4] = {0x02, 0x06, 0x1E, 0xF9};
// Frame sequencer step times in CPU cycles, and the length of a whole sequence.
constexpr uint32_t kFrameSteps[2][5] = {{7457, 14913, 22371, 29829, 37281}, {8313, 16627, 24939, 33253, 41565}};
constexpr uint32_t kFrameCycle[2][2] = {{29830, 37282}, {33254, 41566}};

// Steps a timer of `period` from `next` past `t` without output, returning how many steps.
uint32_t SkipTo(uint32_t& next, uint32_t period, uint32_t t) {
    if (next >= t) return 0;
    const uint32_t n = (t - next + period - 1) / period;
    next += n * period;
    return n;
}

}  // namespace

void NesApu::Envelope::Clock() {
    if (start) {
        start = false;
        decay = 15;
        divider = period;
    } else if (divider) {
        --divider;
    } else {
        divider = period;
        if (decay) --decay;
        else if (loop) decay = 15;
    }
}

int NesApu::Pulse::Target() const {
    const int change = timer >> sweepShift;
    if (!sweepNegate) return timer + change;
    return timer - change - (second ? 0 : 1);
}

int NesApu::Pulse::Level() const {
    if (!length || Muted()) return 0;
    return (kDuty[duty] >> phase) & 1 ? env.Volume() : 0;
}

void NesApu::Reset(bool pal, BandBuffer* out, std::function<uint8_t(uint16_t)> dmcRead) {
    pal_ = pal;
    out_ = out;
    dmcRead_ = std::move(dmcRead);
    pulse_[0] = Pulse();
    pulse_[1] = Pulse();
    pulse_[1].second = true;
    tri_ = Triangle();
    noise_ = Noise();
    dmc_ = Dmc();
    fiveStep_ = false;
    frameStep_ = 0;
    frameNext_ = kFrameSteps[pal_][0];
    time_ = 0;
}

void NesApu::Emit(int& last, int level, float gain, uint32_t t) {
    if (level == last) return;
    out_->Add(t, (float)(level - last) * gain);
    last = level;
}

void NesApu::EmitAll(uint32_t t) {
    for (Pulse& p : pulse_) Emit(p.last, p.Level(), kPulseGain, t);
    Emit(tri_.last, tri_.Level(), kTriangleGain, t);
    Emit(noise_.last, noise_.Level(), kNoiseGain, t);
    Emit(dmc_.last, dmc_.level, kDmcGain, t);
}

void NesApu::RunPulse(Pulse& p, uint32_t t) {
    const uint32_t period = (p.timer + 1u) * 2;  // the pulse timer runs at half the CPU clock
    if (!p.length || p.Muted() || p.env.Volume() == 0) {
        p.phase = (uint8_t)((p.phase + SkipTo(p.next, period, t)) & 7);
        return;
    }
    while (p.next < t) {
        p.phase = (p.phase + 1) & 7;
        Emit(p.last, p.Level(), kPulseGain, p.next);
        p.next += period;
    }
}

void NesApu::RunTriangle(uint32_t t) {
    const uint32_t period = tri_.timer + 1u;
    if (!tri_.Running()) {  // the sequencer stops and the output holds its level
        SkipTo(tri_.next, period, t);
        return;
    }
    while (tri_.next < t) {
        tri_.step = (tri_.step + 1) & 31;
        Emit(tri_.last, tri_.Level(), kTriangleGain, tri_.next);
        tri_.next += period;
    }
}

void NesApu::RunNoise(uint32_t t) {
    const uint32_t period = kNoisePeriod[pal_][noise_.periodIndex];
    const bool audible = noise_.length && noise_.env.Volume();
    while (noise_.next < t) {
        const unsigned tap = noise_.mode ? 6 : 1;
        const unsigned feedback = (noise_.lfsr ^ (noise_.lfsr >> tap)) & 1;
        noise_.lfsr = (uint16_t)((noise_.lfsr >> 1) | (feedback << 14));
        if (audible) Emit(noise_.last, noise_.Level(), kNoiseGain, noise_.next);
        noise_.next += period;
    }
}

void NesApu::DmcFetch() {
    if (dmc_.full || !dmc_.remaining) return;
    dmc_.buffer = dmcRead_ ? dmcRead_(dmc_.addr) : 0;
    dmc_.full = true;
    dmc_.addr = dmc_.addr == 0xFFFF ? 0x8000 : (uint16_t)(dmc_.addr + 1);
    if (--dmc_.remaining == 0 && dmc_.loop) {
        dmc_.addr = dmc_.start;
        dmc_.remaining = dmc_.lengthReg;
    }
}

void NesApu::RunDmc(uint32_t t) {
    const uint32_t period = kDmcPeriod[pal_][dmc_.rateIndex];
    if (dmc_.silent && !dmc_.full && !dmc_.remaining) {
        SkipTo(dmc_.next, period, t);
        return;
    }
    while (dmc_.next < t) {
        if (!dmc_.silent) {
            if (dmc_.shift & 1) {
                if (dmc_.level <= 125) dmc_.level += 2;
            } else if (dmc_.level >= 2) {
                dmc_.level -= 2;
            }
            Emit(dmc_.last, dmc_.level, kDmcGain, dmc_.next);
        }
        dmc_.shift >>= 1;
        if (--dmc_.bits == 0) {
            dmc_.bits = 8;
            dmc_.silent = !dmc_.full;
            if (dmc_.full) {
                dmc_.shift = dmc_.buffer;
                dmc_.full = false;
                DmcFetch();
            }
        }
        dmc_.next += period;
    }
}

void NesApu::Quarter() {
    pulse_[0].env.Clock();
    pulse_[1].env.Clock();
    noise_.env.Clock();
    if (tri_.reloadFlag) tri_.linear = tri_.linearReload;
    else if (tri_.linear) --tri_.linear;
    if (!tri_.control) tri_.reloadFlag = false;
}

void NesApu::Half() {
    for (Pulse& p : pulse_) {
        if (!p.halt && p.length) --p.length;
        if (p.sweepDivider == 0 && p.sweepOn && p.sweepShift && !p.Muted()) p.timer = (uint16_t)p.Target();
        if (p.sweepDivider == 0 || p.sweepReload) {
            p.sweepDivider = p.sweepPeriod;
            p.sweepReload = false;
        } else {
            --p.sweepDivider;
        }
    }
    if (!tri_.control && tri_.length) --tri_.length;
    if (!noise_.halt && noise_.length) --noise_.length;
}

// One frame-sequencer tick: quarter-frame (envelope, triangle linear counter) and half-frame
// (length, sweep) clocks, then emits any level changes. The 5-step mode skips step 3's quarter
// clock. Schedules the next step from the PAL/NTSC tables, wrapping into the next sequence.
void NesApu::FrameEvent(uint32_t t) {
    const int count = fiveStep_ ? 5 : 4;
    const int step = frameStep_;
    if (!(fiveStep_ && step == 3)) Quarter();
    if (step == 1 || step == count - 1) Half();
    EmitAll(t);
    const uint32_t* steps = kFrameSteps[pal_];
    if (step + 1 < count) {
        frameNext_ += steps[step + 1] - steps[step];
        frameStep_ = step + 1;
    } else {
        frameNext_ += kFrameCycle[pal_][fiveStep_] - steps[step] + steps[0];
        frameStep_ = 0;
    }
}

void NesApu::RunTo(uint32_t t) {
    while (frameNext_ <= t) {
        const uint32_t e = frameNext_;
        RunPulse(pulse_[0], e);
        RunPulse(pulse_[1], e);
        RunTriangle(e);
        RunNoise(e);
        RunDmc(e);
        FrameEvent(e);
    }
    RunPulse(pulse_[0], t);
    RunPulse(pulse_[1], t);
    RunTriangle(t);
    RunNoise(t);
    RunDmc(t);
    time_ = t;
}

void NesApu::Write(uint32_t t, uint16_t addr, uint8_t v) {
    RunTo(t);
    if (addr < 0x4008) {
        Pulse& p = pulse_[(addr >> 2) & 1];
        switch (addr & 3) {
            case 0:
                p.duty = v >> 6;
                p.halt = p.env.loop = (v & 0x20) != 0;
                p.env.constant = (v & 0x10) != 0;
                p.env.period = v & 15;
                break;
            case 1:
                p.sweepOn = (v & 0x80) != 0;
                p.sweepPeriod = (v >> 4) & 7;
                p.sweepNegate = (v & 8) != 0;
                p.sweepShift = v & 7;
                p.sweepReload = true;
                break;
            case 2: p.timer = (uint16_t)((p.timer & 0x700) | v); break;
            case 3:
                p.timer = (uint16_t)((p.timer & 0xFF) | (v & 7) << 8);
                if (p.enabled) p.length = kLength[v >> 3];
                p.phase = 0;
                p.env.start = true;
                break;
        }
    } else {
        switch (addr) {
            case 0x4008:
                tri_.control = (v & 0x80) != 0;
                tri_.linearReload = v & 0x7F;
                break;
            case 0x400A: tri_.timer = (uint16_t)((tri_.timer & 0x700) | v); break;
            case 0x400B:
                tri_.timer = (uint16_t)((tri_.timer & 0xFF) | (v & 7) << 8);
                if (tri_.enabled) tri_.length = kLength[v >> 3];
                tri_.reloadFlag = true;
                break;
            case 0x400C:
                noise_.halt = noise_.env.loop = (v & 0x20) != 0;
                noise_.env.constant = (v & 0x10) != 0;
                noise_.env.period = v & 15;
                break;
            case 0x400E:
                noise_.mode = (v & 0x80) != 0;
                noise_.periodIndex = v & 15;
                break;
            case 0x400F:
                if (noise_.enabled) noise_.length = kLength[v >> 3];
                noise_.env.start = true;
                break;
            case 0x4010:
                dmc_.loop = (v & 0x40) != 0;
                dmc_.rateIndex = v & 15;
                break;
            case 0x4011: dmc_.level = v & 0x7F; break;
            case 0x4012: dmc_.start = (uint16_t)(0xC000 + v * 64); break;
            case 0x4013: dmc_.lengthReg = (uint16_t)(v * 16 + 1); break;
            case 0x4015:
                pulse_[0].enabled = v & 1;
                pulse_[1].enabled = v & 2;
                tri_.enabled = v & 4;
                noise_.enabled = v & 8;
                if (!pulse_[0].enabled) pulse_[0].length = 0;
                if (!pulse_[1].enabled) pulse_[1].length = 0;
                if (!tri_.enabled) tri_.length = 0;
                if (!noise_.enabled) noise_.length = 0;
                if (!(v & 16)) {
                    dmc_.remaining = 0;
                } else if (!dmc_.remaining) {
                    dmc_.addr = dmc_.start;
                    dmc_.remaining = dmc_.lengthReg;
                    DmcFetch();
                }
                break;
            case 0x4017:
                fiveStep_ = (v & 0x80) != 0;
                frameStep_ = 0;
                frameNext_ = t + kFrameSteps[pal_][0];
                if (fiveStep_) {
                    Quarter();
                    Half();
                }
                break;
            default: break;
        }
    }
    EmitAll(t);
}

uint8_t NesApu::ReadStatus(uint32_t t) {
    RunTo(t);
    return (uint8_t)((pulse_[0].length ? 1 : 0) | (pulse_[1].length ? 2 : 0) | (tri_.length ? 4 : 0) |
                     (noise_.length ? 8 : 0) | (dmc_.remaining ? 16 : 0));
}

void NesApu::EndFrame(uint32_t t) {
    RunTo(t);
    for (Pulse& p : pulse_) p.next -= std::min(p.next, t);
    tri_.next -= std::min(tri_.next, t);
    noise_.next -= std::min(noise_.next, t);
    dmc_.next -= std::min(dmc_.next, t);
    frameNext_ -= t;
    time_ = 0;
}

// --- VRC6 ---

void Vrc6::Reset(BandBuffer* out) {
    out_ = out;
    for (Channel& c : ch_) c = Channel();
    time_ = 0;
}

int Vrc6::PulseLevel(const Channel& c) const {
    if (!c.On()) return 0;
    const int volume = c.regs[0] & 15;
    if (c.regs[0] & 0x80) return volume;  // "digitized" mode: always high
    return c.phase <= ((c.regs[0] >> 4) & 7) ? volume : 0;
}

void Vrc6::Emit(Channel& c, int level, uint32_t t) {
    if (level == c.last) return;
    out_->Add(t, (float)(level - c.last) * kVrc6Gain);
    c.last = level;
}

void Vrc6::RunTo(uint32_t t) {
    for (int i = 0; i < 2; ++i) {
        Channel& c = ch_[i];
        const uint32_t period = c.Period();
        if (!c.On() || (c.regs[0] & 0x80)) {
            SkipTo(c.next, period, t);
            continue;
        }
        while (c.next < t) {
            c.phase = (c.phase + 1) & 15;
            Emit(c, PulseLevel(c), c.next);
            c.next += period;
        }
    }
    // Sawtooth: the accumulator adds the rate on every second clock and clears after 7 adds.
    Channel& s = ch_[2];
    if (!s.On()) {
        SkipTo(s.next, s.Period(), t);
    } else {
        while (s.next < t) {
            if (++s.phase == 14) {
                s.phase = 0;
                s.acc = 0;
            } else if (!(s.phase & 1)) {
                s.acc = (uint8_t)(s.acc + (s.regs[0] & 0x3F));
            }
            Emit(s, s.acc >> 3, s.next);
            s.next += s.Period();
        }
    }
    time_ = t;
}

void Vrc6::Write(uint32_t t, uint16_t addr, uint8_t v) {
    const int c = (addr >> 12) - 9, reg = addr & 3;
    if (c < 0 || c > 2 || reg == 3) return;
    RunTo(t);
    ch_[c].regs[reg] = v;
    if (c == 2 && !ch_[2].On()) {
        ch_[2].acc = 0;
        ch_[2].phase = 0;
    }
    Emit(ch_[0], PulseLevel(ch_[0]), t);
    Emit(ch_[1], PulseLevel(ch_[1]), t);
    Emit(ch_[2], ch_[2].On() ? ch_[2].acc >> 3 : 0, t);
}

void Vrc6::EndFrame(uint32_t t) {
    RunTo(t);
    for (Channel& c : ch_) c.next -= std::min(c.next, t);
    time_ = 0;
}

// --- Namco 163 ---

namespace {
constexpr float kNamcoGain = 0.000635f;  // per (sample - 8) * volume step, before averaging
constexpr uint32_t kNamcoCycles = 15;  // CPU cycles per channel update
}  // namespace

void Namco163::Reset(BandBuffer* out) {
    out_ = out;
    std::fill(std::begin(ram_), std::end(ram_), (uint8_t)0);
    std::fill(std::begin(levels_), std::end(levels_), 0);
    addr_ = 0;
    active_ = 7;
    next_ = time_ = 0;
    last_ = 0;
}

int Namco163::Level() const {
    int sum = 0;
    for (int c = 8 - Channels(); c < 8; ++c) sum += levels_[c];
    return sum;
}

void Namco163::RunTo(uint32_t t) {
    while (next_ < t) {
        const int c = active_;
        uint8_t* r = ram_ + 0x40 + c * 8;
        const uint32_t freq = r[0] | r[2] << 8 | (r[4] & 3u) << 16;
        uint32_t phase = r[1] | r[3] << 8 | (uint32_t)r[5] << 16;
        const uint32_t length = 256 - (r[4] & 0xFCu);
        phase = (phase + freq) % (length << 16);
        r[1] = (uint8_t)phase;
        r[3] = (uint8_t)(phase >> 8);
        r[5] = (uint8_t)(phase >> 16);
        const unsigned s = (r[6] + (phase >> 16)) & 0xFF;
        const int sample = (ram_[(s >> 1) & 0x7F] >> ((s & 1) * 4)) & 15;
        levels_[c] = (sample - 8) * (r[7] & 15);
        const int n = Channels();
        const float level = (float)Level() * kNamcoGain / (float)n;
        if (level != last_) {
            out_->Add(next_, level - last_);
            last_ = level;
        }
        active_ = active_ <= 8 - n ? 7 : active_ - 1;
        next_ += kNamcoCycles;
    }
    time_ = t;
}

void Namco163::WriteData(uint32_t t, uint8_t v) {
    RunTo(t);
    ram_[addr_ & 0x7F] = v;
    if (addr_ & 0x80) addr_ = (uint8_t)(0x80 | ((addr_ + 1) & 0x7F));
}

uint8_t Namco163::ReadData() {
    const uint8_t v = ram_[addr_ & 0x7F];
    if (addr_ & 0x80) addr_ = (uint8_t)(0x80 | ((addr_ + 1) & 0x7F));
    return v;
}

void Namco163::EndFrame(uint32_t t) {
    RunTo(t);
    next_ -= std::min(next_, t);
    time_ = 0;
}

// --- Sunsoft 5B ---

void Sunsoft5b::Reset(BandBuffer* out) {
    out_ = out;
    std::fill(std::begin(regs_), std::end(regs_), (uint8_t)0);
    regs_[7] = 0xFF;
    for (int c = 0; c < 3; ++c) {
        high_[c] = false;
        toneCount_[c] = 0;
        last_[c] = 0;
    }
    addr_ = 0;
    next_ = 16;
    noiseCount_ = envCount_ = 0;
    noise_ = 1;
    noiseHalf_ = false;
    envStep_ = 31;
    envAttack_ = 0;
    envHold_ = true;
    for (int v = 0; v < 32; ++v)
        volume_[v] = v < 2 ? 0 : 0.28f * (float)std::pow(10.0, (v - 31) * 1.5 / 20);
}

float Sunsoft5b::Level(int c) const {
    const int r = regs_[8 + c];
    const int volume = (r & 16) ? (envStep_ ^ envAttack_) : ((r & 15) * 2 + 1);
    const bool toneOff = (regs_[7] >> c) & 1;  // a disabled tone holds the output high
    if (!toneOff && !high_[c]) return 0;
    if (!(regs_[7] & (8 << c)) && !(noise_ & 1)) return 0;
    return volume_[volume];
}

void Sunsoft5b::Output(uint32_t t) {
    for (int c = 0; c < 3; ++c) {
        const float level = Level(c);
        if (level != last_[c]) {
            out_->Add(t, level - last_[c]);
            last_[c] = level;
        }
    }
}

void Sunsoft5b::ClockEnvelope() {
    if (envHold_ || --envStep_ >= 0) return;
    const int shape = regs_[13];
    if (!(shape & 8)) {
        envHold_ = true;
        envAttack_ = 0;
        envStep_ = 0;
    } else if (shape & 1) {
        if (shape & 2) envAttack_ ^= 31;
        envStep_ = 0;
        envHold_ = true;
    } else {
        if (shape & 2) envAttack_ ^= 31;
        envStep_ = 31;
    }
}

void Sunsoft5b::RunTo(uint32_t t) {
    while (next_ < t) {
        for (int c = 0; c < 3; ++c) {
            const int period = std::max(1, regs_[c * 2] | (regs_[c * 2 + 1] & 15) << 8);
            if (++toneCount_[c] >= period) {
                toneCount_[c] = 0;
                high_[c] = !high_[c];
            }
        }
        noiseHalf_ = !noiseHalf_;
        if (!noiseHalf_ && ++noiseCount_ >= (uint32_t)std::max(1, regs_[6] & 31)) {
            noiseCount_ = 0;
            noise_ = (noise_ >> 1) | (((noise_ ^ (noise_ >> 3)) & 1) << 16);
        }
        if (++envCount_ >= (uint32_t)std::max(1, regs_[11] | regs_[12] << 8)) {
            envCount_ = 0;
            ClockEnvelope();
        }
        Output(next_);
        next_ += 16;
    }
}

void Sunsoft5b::WriteData(uint32_t t, uint8_t v) {
    RunTo(t);
    if (addr_ >= 16) return;  // upper address bits disable the register port
    regs_[addr_] = v;
    if (addr_ == 13) {
        envStep_ = 31;
        envAttack_ = (v & 4) ? 31 : 0;
        envHold_ = false;
        envCount_ = 0;
    }
    Output(t);
}

void Sunsoft5b::EndFrame(uint32_t t) {
    RunTo(t);
    next_ -= t;
}
