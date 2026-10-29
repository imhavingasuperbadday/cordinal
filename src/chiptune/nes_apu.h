// NES sound: the 2A03's two pulse channels, triangle, noise and delta-modulation channel, and
// the cartridge chips NSF rips use most: Konami VRC6, Namco 163 and Sunsoft 5B. Each channel
// is event-driven: it runs from one timer step to the next and reports level changes to a
// BandBuffer with their CPU cycle times.
#pragma once
#include "band_buffer.h"

#include <cstdint>
#include <functional>

class NesApu {
public:
    // dmcRead fetches a sample byte for the delta-modulation channel.
    void Reset(bool pal, BandBuffer* out, std::function<uint8_t(uint16_t)> dmcRead);
    void Write(uint32_t t, uint16_t addr, uint8_t v);  // $4000-$4017
    uint8_t ReadStatus(uint32_t t);                    // $4015
    void EndFrame(uint32_t t);

private:
    struct Envelope {
        uint8_t period = 0, divider = 0, decay = 0;
        bool start = false, loop = false, constant = false;
        int Volume() const { return constant ? period : decay; }
        void Clock();
    };
    struct Pulse {
        Envelope env;
        uint8_t duty = 0, phase = 0, length = 0;
        uint16_t timer = 0;
        uint8_t sweepPeriod = 0, sweepShift = 0, sweepDivider = 0;
        bool sweepOn = false, sweepNegate = false, sweepReload = false, halt = false, enabled = false;
        bool second = false;  // pulse 2 negates in two's complement
        uint32_t next = 0;
        int last = 0;
        int Target() const;
        bool Muted() const { return timer < 8 || (!sweepNegate && Target() > 0x7FF); }
        int Level() const;
    };
    struct Triangle {
        uint8_t step = 0, length = 0, linear = 0, linearReload = 0;
        uint16_t timer = 0;
        bool control = false, reloadFlag = false, enabled = false;
        uint32_t next = 0;
        int last = 0;
        bool Running() const { return length && linear && timer >= 2; }
        int Level() const { return step < 16 ? 15 - step : step - 16; }
    };
    struct Noise {
        Envelope env;
        uint8_t length = 0, periodIndex = 0;
        uint16_t lfsr = 1;
        bool mode = false, halt = false, enabled = false;
        uint32_t next = 0;
        int last = 0;
        int Level() const { return (length && !(lfsr & 1)) ? env.Volume() : 0; }
    };
    struct Dmc {
        uint8_t rateIndex = 0, level = 0, shift = 0, bits = 8, buffer = 0;
        uint16_t start = 0, addr = 0, lengthReg = 0, remaining = 0;
        bool loop = false, silent = true, full = false;
        uint32_t next = 0;
        int last = 0;
    };

    void RunTo(uint32_t t);
    void RunPulse(Pulse& p, uint32_t t);
    void RunTriangle(uint32_t t);
    void RunNoise(uint32_t t);
    void RunDmc(uint32_t t);
    void FrameEvent(uint32_t t);
    void Quarter();
    void Half();
    void Emit(int& last, int level, float gain, uint32_t t);
    void EmitAll(uint32_t t);
    void DmcFetch();

    BandBuffer* out_ = nullptr;
    std::function<uint8_t(uint16_t)> dmcRead_;
    bool pal_ = false;
    Pulse pulse_[2];
    Triangle tri_;
    Noise noise_;
    Dmc dmc_;
    // Frame sequencer state: 4- or 5-step mode, the current step, and the cycle time of the next.
    bool fiveStep_ = false;
    int frameStep_ = 0;
    uint32_t frameNext_ = 0;
    uint32_t time_ = 0;
};

// Konami VRC6: two pulse channels with 16-step duty and a sawtooth.
class Vrc6 {
public:
    void Reset(BandBuffer* out);
    void Write(uint32_t t, uint16_t addr, uint8_t v);  // $9000-$B002
    void EndFrame(uint32_t t);

private:
    struct Channel {
        uint8_t regs[3] = {};
        uint8_t phase = 0, acc = 0;
        uint32_t next = 0;
        int last = 0;
        uint16_t Period() const { return (uint16_t)((regs[1] | (regs[2] & 15) << 8) + 1); }
        bool On() const { return (regs[2] & 0x80) != 0; }
    };
    int PulseLevel(const Channel& c) const;
    void RunTo(uint32_t t);
    void Emit(Channel& c, int level, uint32_t t);

    BandBuffer* out_ = nullptr;
    Channel ch_[3];
    uint32_t time_ = 0;
};

// Namco 163: up to eight wavetable channels sharing 128 bytes of RAM. The chip plays one
// channel at a time, 15 CPU cycles each; this mixes them as their average.
class Namco163 {
public:
    void Reset(BandBuffer* out);
    void WriteAddr(uint8_t v) { addr_ = v; }
    void WriteData(uint32_t t, uint8_t v);
    uint8_t ReadData();
    void EndFrame(uint32_t t);

private:
    int Channels() const { return ((ram_[0x7F] >> 4) & 7) + 1; }
    int Level() const;
    void RunTo(uint32_t t);

    BandBuffer* out_ = nullptr;
    uint8_t ram_[128] = {};
    uint8_t addr_ = 0;
    int levels_[8] = {};
    int active_ = 0;  // channel updated next, counting down from 7
    uint32_t next_ = 0, time_ = 0;
    float last_ = 0;
};

// Sunsoft 5B (YM2149): three tones, shared 17-bit noise and a 32-step envelope.
class Sunsoft5b {
public:
    void Reset(BandBuffer* out);
    void WriteAddr(uint8_t v) { addr_ = v; }
    void WriteData(uint32_t t, uint8_t v);
    void EndFrame(uint32_t t);

private:
    float Level(int c) const;
    void RunTo(uint32_t t);
    void Output(uint32_t t);
    void ClockEnvelope();

    BandBuffer* out_ = nullptr;
    uint8_t regs_[16] = {};
    uint8_t addr_ = 0;
    bool high_[3] = {};
    uint16_t toneCount_[3] = {};
    uint32_t noiseCount_ = 0, envCount_ = 0, noise_ = 1;
    int envStep_ = 31, envAttack_ = 0;
    bool envHold_ = true, noiseHalf_ = false;
    float volume_[32] = {};
    float last_[3] = {};
    uint32_t next_ = 16;
};
