// Original register-level FM synthesis from Yamaha OPN documentation. Fixed phase,
// logarithmic envelopes, generated waveform tables; no vendor implementation or ROM dump.
#include "ym2612.h"
#include <algorithm>
#include <cmath>

namespace chiptune {
namespace {
constexpr double pi=3.14159265358979323846;
// Generated tables: a full sine for the operators, an exponential attenuation curve (log level),
// and the LFO pitch-modulation multipliers (the deepest detune in cents per depth step).
struct Tables {
    std::array<int16_t,1024> sine;
    std::array<float,2048> attenuation;
    float pitch[8][128];
    Tables() {
        for (unsigned i=0;i<sine.size();++i) sine[i]=(int16_t)std::lround(std::sin((i+0.5)*2*pi/1024)*8191);
        for (unsigned i=0;i<attenuation.size();++i) attenuation[i]=(float)std::exp2(-double(i)/64);
        static constexpr double cents[]={0,3.4,6.7,10,14,20,40,80};
        for (unsigned d=0;d<8;++d) for (unsigned i=0;i<128;++i) {
            double triangle=i<32?i/32.0:i<96?(64-int(i))/32.0:(int(i)-128)/32.0;
            pitch[d][i]=(float)std::exp2(cents[d]*triangle/1200);
        }
    }
};
const Tables& Lookup() { static const Tables tables; return tables; }
// Detune units from Yamaha's frequency/key-code table, one phase-accumulator unit.
constexpr uint8_t detune[3][32]={
    {0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,8,8},
    {1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16},
    {2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22}};
// The note portion of the key code, from the frequency's top bits; key scaling uses it.
unsigned Note(unsigned f) { return ((f>>9)&2) | unsigned(((f>>10)&1)?((f&0x380)!=0):((f&0x380)==0x380)); }
// OPN envelope rate generator: rate 0 is off; rates below 48 step a sparse pattern and higher
// rates a scaled one, both sampled from the shared envelope clock.
unsigned Increment(unsigned rate,unsigned clock) {
    if (!rate) return 0;
    if (rate<48) {
        unsigned shift=11-(rate>>2); if (clock&((1u<<shift)-1)) return 0;
        unsigned phase=(clock>>shift)&7, fraction=rate&3;
        return (phase&1) || (fraction==1 && phase==4) || (fraction==2 && (phase==2 || phase==6)) ||
            (fraction==3 && phase!=0);
    }
    unsigned scale=1u<<std::min(3u,(rate-48)/4), fraction=rate&3, phase=clock&3;
    return scale*(1+((fraction==1 && phase==3) || (fraction==2 && (phase&1)) || (fraction==3 && phase!=0)));
}
}

void Ym2612::Reset() {
    for (auto& c:channel_) c=Channel{};
    std::fill_n(special_,3,uint16_t(0)); std::fill_n(frequencyLatch_,2,uint8_t(0));
    specialLatch_=lfo_=mode_=status_=0; dac_=128; dacEnabled_=csmKey_=false;
    envelopeClock_=envelopeDivider_=lfoClock_=lfoPhase_=timerA_=timerB_=0;
    timerAValue_=0; timerBValue_=0;
    (void)Lookup();
}
// Recomputes each operator's phase increment from the channel frequency, block, multiple and
// detune; channel 3 may drive its first three operators from the special-mode frequencies.
void Ym2612::Pitch(unsigned ch) {
    auto& c=channel_[ch];
    static constexpr unsigned specialOrder[]={1,2,0};
    for (unsigned i=0;i<4;++i) {
        auto& op=c.op[i];
        unsigned data=ch==2 && (mode_&0xC0) && i<3?special_[specialOrder[i]]:c.frequency;
        unsigned f=data&2047,block=(data>>11)&7,k=block*4+Note(f);
        op.keyCode=(uint8_t)k;
        int delta=(op.detune&3)?detune[(op.detune&3)-1][k]:0;
        if (op.detune&4) delta=-delta;
        unsigned base=(unsigned)((int)((f<<block)>>1)+delta)&0x1FFFF;
        op.increment=op.multiple?base*op.multiple:base/2;
    }
}
// Key on/off. A key-on resets phase and starts attack; the fastest attack jumps straight to
// decay/sustain. The CSM timer keys channel 3 by setting csmKey_ before calling this.
void Ym2612::Keys(unsigned ch) {
    auto& c=channel_[ch];
    for (unsigned i=0;i<4;++i) {
        auto& op=c.op[i]; bool on=(c.keys&(1u<<i)) || (ch==2 && csmKey_);
        if (on && !op.key) {
            op.phase=0; op.stage=Stage::Attack; op.invert=(op.ssg&4)!=0; op.held=false;
            unsigned rate=op.attack?std::min<unsigned>(63,op.attack*2+(op.keyCode>>(3-op.scale))):0;
            if (rate>=62) { op.envelope=0; op.stage=op.sustainLevel?Stage::Decay:Stage::Sustain; }
        } else if (!on && op.key) {
            if ((op.ssg&8) && op.invert) op.envelope=(512-std::min(op.envelope,512u))&1023;
            op.stage=Stage::Release; op.invert=false; op.held=false;
        }
        op.key=on;
    }
}
// Register write. Bank 0 holds the global registers (timers, LFO, key, mode) and channels 1-3;
// bank 1 holds channels 4-6.
void Ym2612::Write(unsigned bank,uint8_t reg,uint8_t value) {
    if (bank>1) return;
    if (!bank && reg<0x30) {
        switch (reg) {
            case 0x22: lfo_=value&15; if (!(lfo_&8)) lfoClock_=lfoPhase_=0; return;
            case 0x24: timerAValue_=(uint16_t)((timerAValue_&3)|(value<<2)); return;
            case 0x25: timerAValue_=(uint16_t)((timerAValue_&1020)|(value&3)); return;
            case 0x26: timerBValue_=value; return;
            case 0x27:
                if (value&16) status_&=~1;
                if (value&32) status_&=~2;
                if (!(mode_&1) && (value&1)) timerA_=0;
                if (!(mode_&2) && (value&2)) timerB_=0;
                mode_=value&0xCF; if (!(mode_&0x80)) csmKey_=false;
                Pitch(2); Keys(2); return;
            case 0x28: {
                unsigned ch=(value&3)+((value&4)?3:0); if ((value&3)==3) return;
                channel_[ch].keys=value>>4; Keys(ch); return;
            }
            case 0x2A: dac_=value; return;
            case 0x2B: dacEnabled_=(value&128)!=0; return;
            default: return;
        }
    }
    if (reg>=0x30 && reg<0xA0 && (reg&3)!=3) {
        unsigned ch=bank*3+(reg&3);
        static constexpr unsigned slots[]={0,2,1,3};
        auto& op=channel_[ch].op[slots[(reg>>2)&3]];
        switch (reg&0xF0) {
            case 0x30: op.multiple=value&15; op.detune=(value>>4)&7; Pitch(ch); break;
            case 0x40: op.level=value&127; break;
            case 0x50: op.scale=value>>6; op.attack=value&31; break;
            case 0x60: op.am=(value&128)!=0; op.decay=value&31; break;
            case 0x70: op.sustain=value&31; break;
            case 0x80: op.sustainLevel=value>>4; op.release=value&15; break;
            case 0x90: op.invert^=((op.ssg^value)&4)!=0; op.ssg=value&15; break;
        }
    } else if ((reg&0xFC)==0xA4 && (reg&3)!=3) frequencyLatch_[bank]=value&63;
    else if ((reg&0xFC)==0xA0 && (reg&3)!=3) {
        unsigned ch=bank*3+(reg&3); channel_[ch].frequency=(uint16_t)((frequencyLatch_[bank]<<8)|value); Pitch(ch);
    } else if (!bank && (reg&0xFC)==0xAC && (reg&3)!=3) specialLatch_=value&63;
    else if (!bank && (reg&0xFC)==0xA8 && (reg&3)!=3) { special_[reg&3]=(uint16_t)((specialLatch_<<8)|value); Pitch(2); }
    else if ((reg&0xFC)==0xB0 && (reg&3)!=3) {
        auto& c=channel_[bank*3+(reg&3)]; c.algorithm=value&7; c.feedback=(value>>3)&7;
    } else if ((reg&0xFC)==0xB4 && (reg&3)!=3) {
        auto& c=channel_[bank*3+(reg&3)]; c.pan=value>>6; c.pms=value&7; c.ams=(value>>4)&3;
    }
}
// One OPN envelope step. Attack ramps down from full attenuation; every other stage ramps up,
// key-scaled by the key code. SSG-EG modes run the envelope backwards or hold it.
void Ym2612::Envelope(Operator& op) {
    if (op.held || (op.stage==Stage::Release && op.envelope==1023)) return;
    unsigned base=op.stage==Stage::Attack?op.attack:op.stage==Stage::Decay?op.decay:op.stage==Stage::Sustain?op.sustain:op.release*2+1;
    unsigned rate=base?std::min(63u,base*2+(op.keyCode>>(3-op.scale))):0;
    unsigned step=Increment(rate,envelopeClock_);
    if (op.stage==Stage::Attack) {
        unsigned drop=((op.envelope+1)*step+15)/16;
        op.envelope-=std::min(op.envelope,drop);
        if (!op.envelope) op.stage=op.sustainLevel?Stage::Decay:Stage::Sustain;
        return;
    }
    bool ssg=(op.ssg&8) && op.stage!=Stage::Release;
    op.envelope=std::min(1023u,op.envelope+step*(ssg?4:1));
    if (op.stage==Stage::Decay && op.envelope>=(op.sustainLevel==15?992u:op.sustainLevel*32u)) op.stage=Stage::Sustain;
    if (ssg && op.envelope>=512) {
        if (op.ssg&2) op.invert=!op.invert;
        if (op.ssg&1) { op.held=true; op.envelope=512; }
        else { op.envelope=0; op.stage=Stage::Decay; if (!(op.ssg&2)) op.phase=0; }
    }
}
// One FM operator: a phase-modulated sine scaled by the logarithmic attenuation (level,
// envelope and AM), i.e. a table index plus a sine lookup.
int Ym2612::Oscillator(Operator& op,int modulation,unsigned am,float pitch) {
    unsigned envelope=op.envelope;
    if ((op.ssg&8) && op.stage!=Stage::Release) {
        if (op.held) envelope=op.invert?0:1023;
        else if (op.invert) envelope=(512-std::min(envelope,512u))&1023;
    }
    unsigned att=envelope+op.level*8+(op.am?am:0);
    unsigned index=((op.phase>>10)+modulation)&1023;
    op.phase=(op.phase+(uint32_t)(op.increment*pitch))&0xFFFFF;
    return att>=1023?0:(int)(Lookup().sine[index]*Lookup().attenuation[att]);
}
// Produces one stereo sample (one per 144 master clocks). It also ticks the two timers, the LFO
// and the envelope divider, since they all run off this clock.
void Ym2612::Sample(float& left,float& right) {
    if (csmKey_) { csmKey_=false; Keys(2); }
    if ((mode_&1) && ++timerA_>=1024u-timerAValue_) {
        timerA_=0; if (mode_&4) status_|=1;
        if (mode_&0x80) { csmKey_=true; Keys(2); }
    }
    if ((mode_&2) && ++timerB_>=16u*(256u-timerBValue_)) { timerB_=0; if (mode_&8) status_|=2; }
    if (++envelopeDivider_==3) { envelopeDivider_=0; ++envelopeClock_; for (auto& c:channel_) for (auto& op:c.op) Envelope(op); }
    static constexpr unsigned lfoPeriod[]={108,77,71,67,62,44,8,5};
    if ((lfo_&8) && ++lfoClock_>=lfoPeriod[lfo_&7]) { lfoClock_=0; lfoPhase_=(lfoPhase_+1)&127; }
    unsigned triangle=lfoPhase_<64?126-lfoPhase_*2:(lfoPhase_-64)*2;
    static constexpr unsigned amShift[]={8,3,1,0};
    left=right=0;
    for (unsigned ch=0;ch<6;++ch) {
        auto& c=channel_[ch]; unsigned am=(lfo_&8)?triangle>>amShift[c.ams]:0;
        float pitch=(lfo_&8)?Lookup().pitch[c.pms][lfoPhase_]:1;
        int feedback=c.feedback?(c.history[0]+c.history[1])>>(10-c.feedback):0;
        int a=Oscillator(c.op[0],feedback,am,pitch),b=0,d=0,value=0;
        c.history[1]=c.history[0]; c.history[0]=a;
        switch (c.algorithm) {
            // Algorithms 0-7: the modulation routing and which of the four operators sum to the output.
            case 0: b=Oscillator(c.op[1],a>>5,am,pitch); d=Oscillator(c.op[2],b>>5,am,pitch); value=Oscillator(c.op[3],d>>5,am,pitch); break;
            case 1: b=Oscillator(c.op[1],0,am,pitch); d=Oscillator(c.op[2],(a+b)>>5,am,pitch); value=Oscillator(c.op[3],d>>5,am,pitch); break;
            case 2: b=Oscillator(c.op[1],0,am,pitch); d=Oscillator(c.op[2],b>>5,am,pitch); value=Oscillator(c.op[3],(a+d)>>5,am,pitch); break;
            case 3: b=Oscillator(c.op[1],a>>5,am,pitch); d=Oscillator(c.op[2],0,am,pitch); value=Oscillator(c.op[3],(b+d)>>5,am,pitch); break;
            case 4: b=Oscillator(c.op[1],a>>5,am,pitch); d=Oscillator(c.op[2],0,am,pitch); value=b+Oscillator(c.op[3],d>>5,am,pitch); break;
            case 5: b=Oscillator(c.op[1],a>>5,am,pitch); d=Oscillator(c.op[2],a>>5,am,pitch); value=b+d+Oscillator(c.op[3],a>>5,am,pitch); break;
            case 6: b=Oscillator(c.op[1],a>>5,am,pitch); d=Oscillator(c.op[2],0,am,pitch); value=b+d+Oscillator(c.op[3],0,am,pitch); break;
            case 7: b=Oscillator(c.op[1],0,am,pitch); d=Oscillator(c.op[2],0,am,pitch); value=a+b+d+Oscillator(c.op[3],0,am,pitch); break;
        }
        // Channel 6 becomes a DAC when the $2B DAC-enable bit is set (register $2A holds the byte).
        if (ch==5 && dacEnabled_) value=(int(dac_)-128)*64;
        value=std::clamp(value,-8192,8191);
        if (c.pan&2) left+=value*(1.0f/65536);
        if (c.pan&1) right+=value*(1.0f/65536);
    }
}
} // namespace chiptune
