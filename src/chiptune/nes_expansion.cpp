// FDS, MMC5 and VRC7 expansion-audio cores for NSF playback. Each mirrors one cartridge chip's
// register file and mixes band-limited output on the NES CPU's cycle timeline; the register
// behavior implemented here is described in docs/nes-expansion.md.
#include "nes_expansion.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
// Standard NES APU length-counter values and pulse duty patterns (the MMC5 pulses reuse both).
constexpr uint8_t kLength[32] = {10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
                                12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
constexpr uint8_t kDuty[4] = {0x02, 0x06, 0x1E, 0xF9};
constexpr uint32_t kMmc5Frame = 7458;  // independent ~240 Hz envelope AND length clock
constexpr float kTau = 6.283185307179586f;

// VRC7 instrument ROM, dumped from hardware (see the NESdev VRC7 audio reference).
constexpr uint8_t kVrc7Patch[16][8] = {
    {},
    {0x03,0x21,0x05,0x06,0xE8,0x81,0x42,0x27}, {0x13,0x41,0x14,0x0D,0xD8,0xF6,0x23,0x12},
    {0x11,0x11,0x08,0x08,0xFA,0xB2,0x20,0x12}, {0x31,0x61,0x0C,0x07,0xA8,0x64,0x61,0x27},
    {0x32,0x21,0x1E,0x06,0xE1,0x76,0x01,0x28}, {0x02,0x01,0x06,0x00,0xA3,0xE2,0xF4,0xF4},
    {0x21,0x61,0x1D,0x07,0x82,0x81,0x11,0x07}, {0x23,0x21,0x22,0x17,0xA2,0x72,0x01,0x17},
    {0x35,0x11,0x25,0x00,0x40,0x73,0x72,0x01}, {0xB5,0x01,0x0F,0x0F,0xA8,0xA5,0x51,0x02},
    {0x17,0xC1,0x24,0x07,0xF8,0xF8,0x22,0x12}, {0x71,0x23,0x11,0x06,0x65,0x74,0x18,0x16},
    {0x01,0x02,0xD3,0x05,0xC9,0x95,0x03,0x02}, {0x61,0x63,0x0C,0x00,0x94,0xC0,0x33,0xF6},
    {0x21,0x72,0x0D,0x00,0xC1,0xD5,0x56,0x06}
};
constexpr int kMultiplier[16] = {1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30}; // twice multiplier
constexpr int kKeyLevel[16] = {0,24,32,37,40,43,45,47,48,50,51,52,53,54,55,56}; // .375 dB
constexpr uint8_t kRatePattern[4][8] = {{0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1},
                                      {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1}};
}  // namespace

void FdsAudio::Reset(BandBuffer* out) {
    out_ = out;
    std::memset(wave_, 0, sizeof wave_);
    std::memset(mod_, 0, sizeof mod_);
    std::memset(regs_, 0, sizeof regs_);
    regs_[3] = regs_[7] = 0x80;
    regs_[10] = 0xE8;  // NSF playback supplies the BIOS envelope-speed default
    for (Envelope& e : env_) { e = Envelope(); e.next = EnvelopePeriod(e); }
    next_ = 16;
    wavePhase_ = modPhase_ = 0;
    modCounter_ = waveVolume_ = waveDac_ = 0;
    enabled_ = true;
    last_ = 0;
}

uint32_t FdsAudio::EnvelopePeriod(const Envelope& e) const {
    return 8u * ((e.control & 63) + 1u) * (regs_[10] + 1u);
}

void FdsAudio::ClockEnvelope(Envelope& e) {
    if (e.control & 0x40) {
        if (e.gain < 32) ++e.gain;
    } else if (e.gain) --e.gain;
}

// The wave's frequency is modulated: the modulator output times its envelope scales the pitch.
unsigned FdsAudio::WavePitch() const {
    const int pitch = regs_[2] | (regs_[3] & 15) << 8;
    int factor = modCounter_ * env_[1].gain;
    if ((factor & 15) && !(factor & 0x800)) factor += 32;
    factor = ((factor + 0x400) >> 4) & 255;
    return (unsigned)(pitch * factor) & 0xFFFFF;
}

void FdsAudio::Output(uint32_t t) {
    constexpr float volume[4] = {1, 2.0f/3, 0.5f, 0.4f};
    // Wave-write mode holds the DAC while the accumulators continue to run.
    if (!(regs_[9] & 0x80)) waveDac_ = wave_[wavePhase_ >> 18] * waveVolume_;
    const float level = enabled_ ? waveDac_ * (0.27f / (63 * 32)) * volume[regs_[9] & 3] : 0;
    if (level != last_) { out_->Add(t, level - last_); last_ = level; }
}

void FdsAudio::RunTo(uint32_t t) {
    const bool envelopes = enabled_ && !(regs_[3] & 0xC0) && regs_[10];
    for (;;) {
        uint32_t at = next_;
        if (envelopes)
            for (const Envelope& e : env_) if (!(e.control & 0x80)) at = std::min(at, e.next);
        if (at >= t) break;
        if (envelopes) {
            for (Envelope& e : env_) if (!(e.control & 0x80) && e.next == at) {
                ClockEnvelope(e);
                e.next += EnvelopePeriod(e);
            }
        }
        if (at == next_) {
            if (enabled_) {
                if (!(regs_[7] & 0x80) && !(regs_[3] & 0x80)) {
                    const unsigned freq = regs_[6] | (regs_[7] & 15u) << 8;
                    const unsigned old = modPhase_;
                    modPhase_ = (modPhase_ + freq) & 0x3FFFF;
                    if ((old >> 12) != (modPhase_ >> 12)) {
                        const unsigned entry = mod_[(old >> 13) & 31];
                        constexpr int delta[8] = {0,1,2,4,0,-4,-2,-1};
                        modCounter_ = entry == 4 ? 0 : ((modCounter_ + delta[entry] + 64) & 127) - 64;
                    }
                }
                if (!(regs_[3] & 0x80)) wavePhase_ = (wavePhase_ + WavePitch()) & 0xFFFFFF;
                if ((wavePhase_ >> 18) == 0) waveVolume_ = std::min<int>(env_[0].gain, 32);
            }
            next_ += 16;
        }
        Output(at);
    }
    // Paused envelope timers restart when enabled; they must never remain in the past.
    for (Envelope& e : env_)
        if (!envelopes || (e.control & 0x80)) e.next = t + EnvelopePeriod(e);
}

void FdsAudio::Write(uint32_t t, uint16_t addr, uint8_t v) {
    RunTo(t);
    if (addr == 0x4023) { enabled_ = (v & 2) != 0; Output(t); return; }
    if (!enabled_) return;
    if (addr >= 0x4040 && addr <= 0x407F) {
        if (regs_[9] & 0x80) wave_[addr & 63] = v & 63;
    } else if (addr >= 0x4080 && addr <= 0x408A) {
        const int r = addr - 0x4080;
        regs_[r] = v;
        if (r == 0 || r == 4) {
            Envelope& e = env_[r >> 2];
            e.control = v;
            if (v & 0x80) e.gain = v & 63;
            e.next = t + EnvelopePeriod(e);
            if (r == 0 && !e.gain) waveVolume_ = 0;
        }
        if (r == 3) {
            if (v & 0x80) wavePhase_ = 0;
            if (v & 0x40) for (Envelope& e : env_) e.next = t + EnvelopePeriod(e);
        }
        if (r == 5) modCounter_ = ((v + 64) & 127) - 64;
        if (r == 7 && (v & 0x80)) modPhase_ &= 0x3E000;
        if (r == 8 && (regs_[7] & 0x80)) {
            mod_[(modPhase_ >> 13) & 31] = v & 7;
            modPhase_ = (modPhase_ + 0x2000) & 0x3FFFF;
        }
        if (r == 10) for (Envelope& e : env_) e.next = t + EnvelopePeriod(e);
    }
    if ((wavePhase_ >> 18) == 0) waveVolume_ = std::min<int>(env_[0].gain, 32);
    Output(t);
}

uint8_t FdsAudio::Read(uint32_t t, uint16_t addr) {
    RunTo(t);
    if (!enabled_) return 0x40;
    if (addr >= 0x4040 && addr <= 0x407F)
        return 0x40 | wave_[(regs_[9] & 0x80) ? addr & 63 : wavePhase_ >> 18];
    if (addr == 0x4090 || addr == 0x4092) return 0x40 | env_[(addr >> 1) & 1].gain;
    if (addr == 0x4091) return (uint8_t)(wavePhase_ >> 12);
    if (addr == 0x4093) return (uint8_t)(modPhase_ >> 5);
    return 0x40;
}

void FdsAudio::EndFrame(uint32_t t) {
    RunTo(t);
    next_ -= t;
    for (Envelope& e : env_) e.next -= t;
}

void Mmc5Audio::Reset(BandBuffer* out) {
    out_ = out;
    for (Pulse& p : pulse_) p = Pulse();
    frameNext_ = kMmc5Frame;
    pcm_ = pcmControl_ = 0;
    pcmIrq_ = false;
    last_ = 0;
}

int Mmc5Audio::Pulse::Level() const {
    if (!length || !((kDuty[control >> 6] >> phase) & 1)) return 0;
    return control & 16 ? control & 15 : decay;
}

void Mmc5Audio::Output(uint32_t t) {
    const float level = -(pulse_[0].Level() + pulse_[1].Level()) * 0.00752f - pcm_ * 0.00335f;
    if (level != last_) { out_->Add(t, level - last_); last_ = level; }
}

void Mmc5Audio::RunPulse(Pulse& p, uint32_t t) {
    const uint32_t period = 2u * (p.timer + 1u);
    if (!p.length && p.next < t) {
        const uint32_t steps = (t - p.next + period - 1) / period;
        p.phase = (uint8_t)((p.phase + steps) & 7);
        p.next += steps * period;
        return;
    }
    // Only this pulse changes during this loop; the other pulse's contribution cancels.
    while (p.next < t) {
        const int old = p.Level();
        p.phase = (p.phase + 1) & 7;
        const float delta = (old - p.Level()) * 0.00752f;
        if (delta) { out_->Add(p.next, delta); last_ += delta; }
        p.next += period;
    }
}

void Mmc5Audio::RunTo(uint32_t t) {
    while (frameNext_ < t) {
        for (Pulse& p : pulse_) RunPulse(p, frameNext_);
        for (Pulse& p : pulse_) {
            if (p.start) { p.start = false; p.decay = 15; p.divider = p.control & 15; }
            else if (p.divider) --p.divider;
            else {
                p.divider = p.control & 15;
                if (p.decay) --p.decay;
                else if (p.control & 32) p.decay = 15;
            }
            if (!(p.control & 32) && p.length) --p.length;
        }
        Output(frameNext_);
        frameNext_ += kMmc5Frame;
    }
    for (Pulse& p : pulse_) RunPulse(p, t);
}

void Mmc5Audio::Pcm(uint8_t v) {
    if (v) pcm_ = v;
    else if (pcmControl_ & 0x80) pcmIrq_ = true;
}

void Mmc5Audio::Write(uint32_t t, uint16_t addr, uint8_t v) {
    RunTo(t);
    if (addr >= 0x5000 && addr <= 0x5007) {
        Pulse& p = pulse_[(addr >> 2) & 1];
        switch (addr & 3) {
            case 0: p.control = v; break;
            case 2: p.timer = (uint16_t)((p.timer & 0x700) | v); break;
            case 3:
                p.timer = (uint16_t)((p.timer & 255) | (v & 7) << 8);
                if (p.enabled) p.length = kLength[v >> 3];
                p.phase = 0;
                p.start = true;
                break;
        }
    } else if (addr == 0x5010) {
        pcmControl_ = v;
        if (!(v & 0x80)) pcmIrq_ = false;
    } else if (addr == 0x5011 && !(pcmControl_ & 1)) Pcm(v);
    else if (addr == 0x5015) {
        for (int c = 0; c < 2; ++c) {
            pulse_[c].enabled = (v & (1 << c)) != 0;
            if (!pulse_[c].enabled) pulse_[c].length = 0;
        }
    }
    Output(t);
}

uint8_t Mmc5Audio::Read(uint32_t t, uint16_t addr) {
    RunTo(t);
    if (addr == 0x5015) return (pulse_[0].length ? 1 : 0) | (pulse_[1].length ? 2 : 0);
    const uint8_t status = pcmIrq_ ? 0x80 : 0;
    pcmIrq_ = false;
    return status;
}

void Mmc5Audio::PcmRead(uint32_t t, uint16_t addr, uint8_t v) {
    if (!(pcmControl_ & 1) || addr < 0x8000 || addr >= 0xC000) return;
    RunTo(t);
    Pcm(v);
    Output(t);
}

void Mmc5Audio::EndFrame(uint32_t t) {
    RunTo(t);
    for (Pulse& p : pulse_) p.next -= t;
    frameNext_ -= t;
}

void Vrc7Audio::Reset(bool pal, BandBuffer* out) {
    out_ = out;
    std::memset(regs_, 0, sizeof regs_);
    for (Channel& c : channels_) c = Channel();
    addr_ = 0;
    next_ = 0;
    // The ceramic oscillator is independent of the NES region's CPU clock.
    step_ = (uint64_t)std::llround((pal ? 1662607.0 : 1789773.0) * 72 / 3579545.0 * 4294967296.0);
    samples_ = amPhase_ = pmPhase_ = 0;
    last_ = 0;
    for (int i = 0; i < 1024; ++i) sine_[i] = std::sin(kTau * (i + 0.5f) / 1024);
    for (int i = 0; i < 256; ++i) amplitude_[i] = (float)std::pow(10.0, -i * 0.375 / 20);
}

// One envelope step for one VRC7 operator. The stage picks a rate from the patch, key scaling
// adds the note, and the rate's low bits decide which global samples advance the attenuation.
void Vrc7Audio::Envelope(Operator& op, const uint8_t* patch, int slot, int block, int freq, bool sustain) {
    const int control = patch[slot], adsr = patch[4 + slot], sr = patch[6 + slot];
    if (op.stage == Stage::Off) return;
    int rate = 0;
    switch (op.stage) {
        case Stage::Damp: rate = 12; break;
        case Stage::Attack: rate = adsr >> 4; break;
        case Stage::Decay: rate = adsr & 15; break;
        case Stage::Sustain: rate = (control & 32) ? 0 : sr & 15; break;
        case Stage::Release: rate = sustain ? 5 : (control & 32) ? sr & 15 : 7; break;
        default: break;
    }
    if (!rate) return;
    const int key = (block * 2 + (freq >> 8)) >> ((control & 16) ? 0 : 2);
    const int effective = std::min(63, rate * 4 + key);
    const int hi = effective >> 2, shift = std::max(0, 13 - hi);
    if (samples_ & ((1u << shift) - 1)) return;
    const unsigned index = (samples_ >> shift) & 7;
    int increment = kRatePattern[effective & 3][index];
    if (hi >= 13) increment <<= hi - 13;
    if (op.stage == Stage::Attack) {
        if (hi == 15) op.attenuation = 0;
        else op.attenuation = std::max(0, op.attenuation - ((op.attenuation + 1) * increment + 7) / 8);
        if (op.attenuation == 0) op.stage = Stage::Decay;
    } else {
        op.attenuation = std::min(127, op.attenuation + increment);
        if (op.stage == Stage::Damp && op.attenuation >= 123) {
            op.attenuation = 127;
            op.phase = 0;
            op.previous = op.feedback = 0;
            op.stage = Stage::Attack;
        } else if (op.stage == Stage::Decay && op.attenuation >= (sr >> 4) * 8) {
            op.stage = Stage::Sustain;
        } else if (op.attenuation == 127 && op.stage != Stage::Decay) op.stage = Stage::Off;
    }
}

float Vrc7Audio::OperatorSample(Operator& op, const uint8_t* patch, int slot, int block, int freq,
                               int volume, float modulation) {
    const int control = patch[slot];
    uint32_t increment = (uint32_t)((freq << block) * kMultiplier[control & 15]);
    if (control & 0x40) {
        constexpr int vibrato[8] = {0,1,2,1,0,-1,-2,-1};
        increment = (uint32_t)((int)increment + (int)increment * vibrato[(pmPhase_ >> 10) & 7] / 256);
    }
    op.phase = (op.phase + increment) & 0xFFFFF;
    if (regs_[15] & 4) op.phase = 0;
    const int ksl = (patch[2 + slot] >> 6);
    int attenuation = (regs_[15] & 1) ? 0 : op.attenuation;
    if (ksl) {
        const int keyLevel = std::max(0, kKeyLevel[freq >> 5] - (7 - block) * 16);
        attenuation += keyLevel >> (ksl == 3 ? 0 : ksl == 1 ? 1 : 2);
    }
    attenuation += slot ? volume * 8 : (patch[2] & 63) * 2;
    if (control & 0x80) {
        const int am = (int)(amPhase_ >> 6) % 210;
        attenuation += (am < 105 ? am : 209 - am) / 13;
    }
    if ((op.stage == Stage::Off || op.attenuation == 127) && !(regs_[15] & 1)) return 0;
    const int phase = ((int)(op.phase >> 10) + (int)(modulation * 1024)) & 1023;
    if ((patch[3] & (slot ? 16 : 8)) && (phase & 512)) return 0;
    return sine_[phase] * amplitude_[std::min(255, attenuation)];
}

// Produces one VRC7 sample: each channel runs a modulator operator into a carrier, summed dry.
float Vrc7Audio::Sample() {
    if (regs_[15] & 2) amPhase_ = pmPhase_ = 0;
    else { amPhase_ += (regs_[15] & 8) ? 64 : 1; pmPhase_ += (regs_[15] & 8) ? 1024 : 1; }
    float sum = 0;
    for (int c = 0; c < 6; ++c) {
        const int high = regs_[0x20 + c], block = (high >> 1) & 7;
        const int freq = regs_[0x10 + c] | (high & 1) << 8;
        const int instrument = regs_[0x30 + c] >> 4;
        const uint8_t* patch = instrument ? kVrc7Patch[instrument] : regs_;
        Operator& mod = channels_[c].op[0];
        Operator& car = channels_[c].op[1];
        Envelope(mod, patch, 0, block, freq, (high & 32) != 0);
        Envelope(car, patch, 1, block, freq, (high & 32) != 0);
        const int feedback = patch[3] & 7;
        const float fm = feedback ? (mod.previous + mod.feedback) * (float)(1 << feedback) / 128 : 0;
        const float m = OperatorSample(mod, patch, 0, block, freq, 0, fm);
        mod.feedback = mod.previous;
        mod.previous = m;
        sum += OperatorSample(car, patch, 1, block, freq, regs_[0x30 + c] & 15, m * 2);
    }
    ++samples_;
    return sum * 0.12f;
}

void Vrc7Audio::RunTo(uint32_t t) {
    const uint64_t end = (uint64_t)t << 32;
    while (next_ < end) {
        const float level = Sample();
        if (level != last_) { out_->Add((uint32_t)(next_ >> 32), level - last_); last_ = level; }
        next_ += step_;
    }
}

void Vrc7Audio::WriteData(uint32_t t, uint8_t v) {
    RunTo(t);
    const uint8_t old = regs_[addr_];
    regs_[addr_] = v;
    if (addr_ >= 0x20 && addr_ <= 0x25 && ((old ^ v) & 16)) {
        Channel& c = channels_[addr_ - 0x20];
        if (v & 16) {
            for (Operator& op : c.op) op.stage = Stage::Damp;
        } else {
            // On key-off the modulator keeps its envelope; the carrier releases.
            c.op[1].stage = Stage::Release;
        }
    }
}

void Vrc7Audio::EndFrame(uint32_t t) {
    RunTo(t);
    next_ -= (uint64_t)t << 32;
}
