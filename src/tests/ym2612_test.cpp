// Unit test for the native YM2612 (Mega Drive) FM synth core: Reset/Write/Sample/Status.
// Run from selftest.cpp's "YM2612 audio" group. The checks pin pitch, sine shape, stereo routing,
// total level, key-on/off, the channel-6 DAC, the timers, SSG envelope shapes, all eight FM
// algorithms, and robustness to streams of malformed register writes.
#include "chiptune/ym2612.h"
#include "testlog.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
using chiptune::Ym2612;
// Programs channel 0 as a single carrier: full operator level, fast attack/release, key-on with a
// fixed fnum/block, so the output is a plain sine at a known frequency.
void Tone(Ym2612& chip,unsigned level=0,unsigned attack=31,unsigned release=15) {
    chip.Reset();
    chip.Write(0,0x30,1); chip.Write(0,0x40,(uint8_t)level); chip.Write(0,0x50,(uint8_t)attack);
    chip.Write(0,0x80,(uint8_t)release); chip.Write(0,0xB0,7); chip.Write(0,0xB4,0xC0);
    chip.Write(0,0xA4,0x24); chip.Write(0,0xA0,0); chip.Write(0,0x28,0x10);
}
// Mean stereo power over `count` samples, used to compare loudness across register settings.
double Energy(Ym2612& chip,unsigned count) {
    double e=0;
    for (unsigned i=0;i<count;++i) { float l,r; chip.Sample(l,r); e+=double(l)*l+double(r)*r; }
    return e/count;
}
}
// One check per chip behaviour: the sine carrier is compared sample-for-sample against an ideal
// sine at the same phase, so both the oscillator and the 16-bit scaling are pinned.
void TestYm2612() {
    Ym2612 chip;
    Tone(chip);
    unsigned crossings=0; float previous=0; bool finite=true,stereo=true; double error=0;
    constexpr unsigned n=12800;
    for (unsigned i=0;i<n;++i) {
        float l,r; chip.Sample(l,r); finite&=std::isfinite(l)&&std::isfinite(r); stereo&=l==r;
        if (i && previous<0 && l>=0) ++crossings;
        previous=l;
        double expected=std::sin((i*8+0.5)*2*3.14159265358979323846/1024)*(8191.0/65536);
        error=std::max(error,std::abs(l-expected));
    }
    testlog::Check(finite && stereo && crossings==99 && error<1.0/65536,L"YM2612 pure carrier: exact pitch, sine and stereo (error %.8f)",error);
    Tone(chip,8); double half=Energy(chip,4096);
    Tone(chip); double full=Energy(chip,4096);
    testlog::Check(std::abs(half/full-0.25)<0.001,L"YM2612 total level: 8 steps halve amplitude");
    Tone(chip,0,0);
    testlog::Check(Energy(chip,8192)==0,L"YM2612 zero attack holds a new voice silent");
    Tone(chip); chip.Write(0,0xB4,0x80); bool pan=true;
    for (unsigned i=0;i<256;++i) { float l,r;chip.Sample(l,r);pan&=r==0; }
    testlog::Check(pan && Energy(chip,1024)>0.001,L"YM2612 left pan has sound only on the selected output");
    chip.Write(0,0x28,0); Energy(chip,4096);
    testlog::Check(Energy(chip,4096)==0,L"YM2612 key-off releases to silence");
    chip.Reset(); chip.Write(1,0xB6,0x40); chip.Write(0,0x2A,255); chip.Write(0,0x2B,128);
    float left,right; chip.Sample(left,right);
    testlog::Check(left==0 && right==8128.0f/65536,L"YM2612 channel 6 DAC replaces FM and follows stereo routing");
    chip.Write(0,0x2A,128); chip.Sample(left,right);
    testlog::Check(left==0 && right==0,L"YM2612 unsigned DAC midpoint is silent");
    chip.Reset(); chip.Write(0,0x24,255); chip.Write(0,0x25,3); chip.Write(0,0x27,5); chip.Sample(left,right);
    bool timerA=chip.Status()==1; chip.Write(0,0x27,16);
    testlog::Check(timerA && chip.Status()==0,L"YM2612 timer A overflow, enable and reset");
    chip.Reset(); chip.Write(0,0x26,255); chip.Write(0,0x27,10); Energy(chip,15);
    bool before=chip.Status()==0; chip.Sample(left,right);
    testlog::Check(before && chip.Status()==2,L"YM2612 timer B uses sixteen sample ticks");
    bool ssgShapes=true;
    for (unsigned shape:{9u,11u,13u,15u}) {
        Tone(chip); chip.Write(0,0x60,31); chip.Write(0,0x70,31); chip.Write(0,0x90,(uint8_t)shape);
        Energy(chip,8192); double e=Energy(chip,4096); bool loud=shape==11 || shape==13;
        ssgShapes&=loud?e>0.01:e==0;
    }
    testlog::Check(ssgShapes,L"YM2612 SSG hold shapes 9/11/13/15 hold or drop their volume as documented");
    std::vector<std::vector<float>> algorithms;
    bool algorithmsOk=true;
    for (unsigned algorithm=0;algorithm<8;++algorithm) {
        Tone(chip);
        for (unsigned reg:{0x34u,0x38u,0x3Cu}) {
            chip.Write(0,(uint8_t)reg,(uint8_t)(reg==0x34?3:reg==0x38?2:4));
            chip.Write(0,(uint8_t)(reg+0x10),16); chip.Write(0,(uint8_t)(reg+0x20),31);
        }
        chip.Write(0,0xB0,(uint8_t)algorithm); chip.Write(0,0x28,0xF0);
        std::vector<float> samples(2048); double power=0; bool valid=true;
        for (float& v:samples) { float r;chip.Sample(v,r);valid&=std::isfinite(v)&&v==r;power+=double(v)*v; }
        algorithmsOk&=valid && power>0.01;
        algorithms.push_back(samples);
    }
    bool distinct=true;
    for (unsigned a=0;a<8;++a) for (unsigned b=0;b<a;++b) distinct&=algorithms[a]!=algorithms[b];
    testlog::Check(algorithmsOk && distinct,L"YM2612 eight algorithms produce finite, distinct modulation topologies");
    // Register fuzz also checks unused channel addresses and timer/SSG transitions.
    chip.Reset(); uint32_t random=0x734819;
    bool robust=true;
    for (unsigned i=0;i<100000;++i) {
        random=random*1664525+1013904223;
        if (!(i&7)) chip.Write(random>>31,(uint8_t)(random>>8),(uint8_t)(random>>16));
        chip.Sample(left,right); robust&=std::isfinite(left)&&std::isfinite(right)&&std::abs(left)<=0.75&&std::abs(right)<=0.75;
    }
    testlog::Check(robust,L"YM2612 malformed register logs remain bounded and finite");
}
