// Game Boy sound: two pulse channels (the first with a frequency sweep), a 32-step wavetable
// channel and a noise channel, panned left/right and mixed with a master volume. Registers
// $FF10-$FF26 plus wave RAM $FF30-$FF3F; times are in 4.19 MHz clocks.
#pragma once
#include "band_buffer.h"

#include <cstdint>

class GbApu {
public:
    void Reset(BandBuffer* left, BandBuffer* right);
    void Write(uint32_t t, uint16_t addr, uint8_t v);
    uint8_t Read(uint32_t t, uint16_t addr);
    void EndFrame(uint32_t t);

private:
    struct Channel {
        bool on = false;          // playing (cleared by the length counter, sweep overflow or DAC)
        bool lengthEnable = false;
        int length = 0;           // steps left
        int volume = 0, envPeriod = 0, envTimer = 0;
        bool envUp = false;
        uint16_t freq = 0;
        uint32_t next = 0;
        int phase = 0;
        int lastL = 0, lastR = 0;
    };

    bool Dac(int c) const;
    int Level(int c) const;
    void Emit(int c, uint32_t t);
    void EmitAll(uint32_t t);
    void RunTo(uint32_t t);
    void RunChannel(int c, uint32_t t);
    // Frame sequencer, clocked at 512 Hz: length every other step, sweep on steps 2 and 6,
    // envelopes on step 7.
    void Sequencer();
    void Trigger(int c);
    int SweepTarget();

    BandBuffer *left_ = nullptr, *right_ = nullptr;
    uint8_t regs_[0x30] = {};  // $FF10-$FF3F
    Channel ch_[4];
    uint16_t lfsr_ = 0x7FFF;
    int sweepTimer_ = 0;
    uint16_t sweepShadow_ = 0;
    bool sweepOn_ = false;
    int seqStep_ = 0;
    uint32_t seqNext_ = 0;
};
