// Additional NSF sound hardware: the Famicom Disk System, MMC5 and VRC7 (FM) audio. Register
// behavior is described in docs/nes-expansion.md; these cores share the NES CPU's cycle timeline.
#pragma once
#include "band_buffer.h"

#include <cstdint>

// Famicom Disk System: a wavetable channel whose frequency is modulated by a second channel.
class FdsAudio {
public:
    void Reset(BandBuffer* out);
    void Write(uint32_t t, uint16_t addr, uint8_t v);
    uint8_t Read(uint32_t t, uint16_t addr);
    void EndFrame(uint32_t t);

private:
    struct Envelope {
        uint8_t control = 0x80, gain = 0;
        uint32_t next = 0;
    };
    uint32_t EnvelopePeriod(const Envelope& e) const;
    void RunTo(uint32_t t);
    void Output(uint32_t t);
    void ClockEnvelope(Envelope& e);
    unsigned WavePitch() const;

    BandBuffer* out_ = nullptr;
    uint8_t wave_[64] = {}, mod_[32] = {}, regs_[11] = {};
    Envelope env_[2];
    uint32_t wavePhase_ = 0, modPhase_ = 0, next_ = 16;
    int modCounter_ = 0, waveVolume_ = 0, waveDac_ = 0;
    bool enabled_ = true;
    float last_ = 0;
};

// MMC5: two NES-style pulse channels plus a 1-bit PCM channel.
class Mmc5Audio {
public:
    void Reset(BandBuffer* out);
    void Write(uint32_t t, uint16_t addr, uint8_t v);
    uint8_t Read(uint32_t t, uint16_t addr);
    void PcmRead(uint32_t t, uint16_t addr, uint8_t v);
    void EndFrame(uint32_t t);

private:
    struct Pulse {
        uint8_t control = 0, length = 0, phase = 0, decay = 0, divider = 0;
        uint16_t timer = 0;
        uint32_t next = 0;
        bool enabled = false, start = false;
        int Level() const;
    };
    void RunTo(uint32_t t);
    void RunPulse(Pulse& p, uint32_t t);
    void Output(uint32_t t);
    void Pcm(uint8_t v);

    BandBuffer* out_ = nullptr;
    Pulse pulse_[2];
    uint32_t frameNext_ = 7458;
    uint8_t pcm_ = 0, pcmControl_ = 0;
    bool pcmIrq_ = false;
    float last_ = 0;
};

// Six two-operator FM channels. The fixed instrument bytes are hardware ROM data.
class Vrc7Audio {
public:
    void Reset(bool pal, BandBuffer* out);
    void WriteAddr(uint8_t v) { addr_ = v & 0x3F; }
    void WriteData(uint32_t t, uint8_t v);
    void EndFrame(uint32_t t);

private:
    enum class Stage { Off, Damp, Attack, Decay, Sustain, Release };
    struct Operator {
        uint32_t phase = 0;
        int attenuation = 127;
        Stage stage = Stage::Off;
        float previous = 0, feedback = 0;
    };
    struct Channel { Operator op[2]; };
    void RunTo(uint32_t t);
    float Sample();
    void Envelope(Operator& op, const uint8_t* patch, int slot, int block, int freq, bool sustain);
    float OperatorSample(Operator& op, const uint8_t* patch, int slot, int block, int freq,
                         int volume, float modulation);

    BandBuffer* out_ = nullptr;
    Channel channels_[6];
    uint8_t regs_[0x40] = {}, addr_ = 0;
    uint64_t next_ = 0, step_ = 0;
    uint32_t samples_ = 0, amPhase_ = 0, pmPhase_ = 0;
    float sine_[1024] = {}, amplitude_[256] = {}, last_ = 0;
};
