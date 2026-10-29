// The SNES sound DSP (S-DSP): eight voices playing BRR-compressed samples from the shared
// 64 KB RAM, with ADSR/gain envelopes, Gaussian interpolation, noise, pitch modulation and
// an echo with an 8-tap FIR filter. Emulated a whole 32 kHz sample at a time, in the order
// the hardware does each step within a sample.
#pragma once
#include <cstdint>

class SpcDsp {
public:
    // Starts from a register snapshot, with every voice silent.
    void Reset(uint8_t* ram, const uint8_t regs[128]);
    uint8_t Read(uint8_t addr) const { return regs_[addr & 0x7F]; }
    void Write(uint8_t addr, uint8_t v);
    // Produces one stereo sample.
    void Sample(int& left, int& right);

private:
    enum class Mode { Release, Attack, Decay, Sustain };
    struct Voice {
        int16_t ring[12] = {};  // decoded samples at twice their 15-bit value, three groups of four
        int ringPos = 0;        // where the next group goes; also the oldest sample
        uint16_t brrAddr = 0;
        int brrOffset = 1;      // next byte pair in the 9-byte block
        int counter = 0;        // pitch counter: bits 12-14 pick the sample, 4-11 the Gauss weights
        int env = 0;
        int lastEnv = 0;        // the last computed envelope before clipping (bent-line gain uses it)
        Mode mode = Mode::Release;
        int konDelay = 0;
        bool justKeyed = false;  // the sample in which key-on loads the start address
    };

    uint8_t& VoiceReg(int v, int r) { return regs_[v * 16 + r]; }
    bool RateFires(int rate) const;
    void DecodeGroup(Voice& vc, uint8_t header);
    int Interpolate(const Voice& vc) const;
    void Envelope(int v);
    uint16_t DirEntry(int v, int which) const;

    uint8_t* ram_ = nullptr;  // the shared 64 KB, borrowed from the SPC700
    uint8_t regs_[128] = {};
    Voice voices_[8];
    uint8_t konLatch_ = 0, konNow_ = 0, koffNow_ = 0;
    bool poll_ = false;  // KON and KOFF are acted on every other sample
    int counter_ = 0;    // global rate counter
    int noise_ = 0x4000;
    int fir_[8][2] = {};  // echo history, 15-bit
    int firPos_ = 0;
    int echoOffset_ = 0, echoLength_ = 0;
};
