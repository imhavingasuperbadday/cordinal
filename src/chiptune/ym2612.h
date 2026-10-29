// Cordinal's register-level OPN2 synthesizer. One sample per 144 master clocks.
#pragma once
#include <array>
#include <cstdint>

namespace chiptune {
class Ym2612 {
public:
    void Reset();
    // Writes one register; bank 0 has the global registers and channels 1-3, bank 1 channels 4-6.
    void Write(unsigned bank,uint8_t address,uint8_t value);
    void Sample(float& left,float& right);
    uint8_t Status() const { return status_; }
private:
    enum class Stage : uint8_t { Attack,Decay,Sustain,Release };
    struct Operator {
        uint32_t phase=0,increment=0;
        unsigned envelope=1023;
        uint8_t multiple=0,detune=0,level=0,scale=0,attack=0,decay=0,sustain=0,release=0,sustainLevel=0,ssg=0,keyCode=0;
        Stage stage=Stage::Release;
        bool am=false,key=false,invert=false,held=false;
    };
    struct Channel {
        Operator op[4];
        uint16_t frequency=0;
        uint8_t algorithm=0,feedback=0,pan=3,pms=0,ams=0,keys=0;
        int history[2]{};
    };
    void Pitch(unsigned channel);
    void Keys(unsigned channel);
    void Envelope(Operator& op);
    int Oscillator(Operator& op,int modulation,unsigned amplitude,float pitch);
    Channel channel_[6];
    uint16_t special_[3]{};
    uint8_t frequencyLatch_[2]{},specialLatch_=0,lfo_=0,mode_=0,status_=0,dac_=128;
    bool dacEnabled_=false,csmKey_=false;
    unsigned envelopeClock_=0,envelopeDivider_=0,lfoClock_=0,lfoPhase_=0,timerA_=0,timerB_=0;
    uint16_t timerAValue_=0;
    uint8_t timerBValue_=0;
};
} // namespace chiptune
