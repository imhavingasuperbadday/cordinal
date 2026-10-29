// Implementation of the SNES S-DSP. One 32 kHz stereo sample per call: every voice is stepped
// (BRR decode, Gaussian interpolation, envelope, mixing), then the echo FIR, in hardware order.
#include "spc_dsp.h"

#include <algorithm>

namespace {

// Global (non-voice) register offsets in the 128-byte DSP register file.
enum : uint8_t {
    MVOLL = 0x0C, MVOLR = 0x1C, EVOLL = 0x2C, EVOLR = 0x3C, KON = 0x4C, KOFF = 0x5C, FLG = 0x6C, ENDX = 0x7C,
    EFB = 0x0D, PMON = 0x2D, NON = 0x3D, EON = 0x4D, DIR = 0x5D, ESA = 0x6D, EDL = 0x7D,
};
// Per-voice register offsets; each of the eight voices owns a 16-byte block.
enum : int { VOLL = 0, VOLR = 1, PITCHL = 2, PITCHH = 3, SRCN = 4, ADSR1 = 5, ADSR2 = 6, GAIN = 7, ENVX = 8, OUTX = 9 };

// Interpolation weights from the DSP's ROM (as listed in the fullsnes hardware reference).
// Entries i, 255-i, 256+i and 511-i weigh the four samples and sum to about 0x800.
constexpr int16_t kGauss[512] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
    2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5,
    6, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 10,
    11, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 15, 16, 16, 17, 17,
    18, 19, 19, 20, 20, 21, 21, 22, 23, 23, 24, 24, 25, 26, 27, 27,
    28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 36, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56,
    58, 59, 60, 61, 62, 64, 65, 66, 67, 69, 70, 71, 73, 74, 76, 77,
    78, 80, 81, 83, 84, 86, 87, 89, 90, 92, 94, 95, 97, 99, 100, 102,
    104, 106, 107, 109, 111, 113, 115, 117, 118, 120, 122, 124, 126, 128, 130, 132,
    134, 137, 139, 141, 143, 145, 147, 150, 152, 154, 156, 159, 161, 163, 166, 168,
    171, 173, 175, 178, 180, 183, 186, 188, 191, 193, 196, 199, 201, 204, 207, 210,
    212, 215, 218, 221, 224, 227, 230, 233, 236, 239, 242, 245, 248, 251, 254, 257,
    260, 263, 267, 270, 273, 276, 280, 283, 286, 290, 293, 297, 300, 304, 307, 311,
    314, 318, 321, 325, 328, 332, 336, 339, 343, 347, 351, 354, 358, 362, 366, 370,
    374, 378, 381, 385, 389, 393, 397, 401, 405, 410, 414, 418, 422, 426, 430, 434,
    439, 443, 447, 451, 456, 460, 464, 469, 473, 477, 482, 486, 491, 495, 499, 504,
    508, 513, 517, 522, 527, 531, 536, 540, 545, 550, 554, 559, 563, 568, 573, 577,
    582, 587, 592, 596, 601, 606, 611, 615, 620, 625, 630, 635, 640, 644, 649, 654,
    659, 664, 669, 674, 678, 683, 688, 693, 698, 703, 708, 713, 718, 723, 728, 732,
    737, 742, 747, 752, 757, 762, 767, 772, 777, 782, 787, 792, 797, 802, 806, 811,
    816, 821, 826, 831, 836, 841, 846, 851, 855, 860, 865, 870, 875, 880, 884, 889,
    894, 899, 904, 908, 913, 918, 923, 927, 932, 937, 941, 946, 951, 955, 960, 965,
    969, 974, 978, 983, 988, 992, 997, 1001, 1005, 1010, 1014, 1019, 1023, 1027, 1032, 1036,
    1040, 1045, 1049, 1053, 1057, 1061, 1066, 1070, 1074, 1078, 1082, 1086, 1090, 1094, 1098, 1102,
    1106, 1109, 1113, 1117, 1121, 1125, 1128, 1132, 1136, 1139, 1143, 1146, 1150, 1153, 1157, 1160,
    1164, 1167, 1170, 1174, 1177, 1180, 1183, 1186, 1190, 1193, 1196, 1199, 1202, 1205, 1207, 1210,
    1213, 1216, 1219, 1221, 1224, 1227, 1229, 1232, 1234, 1237, 1239, 1241, 1244, 1246, 1248, 1251,
    1253, 1255, 1257, 1259, 1261, 1263, 1265, 1267, 1269, 1270, 1272, 1274, 1275, 1277, 1279, 1280,
    1282, 1283, 1284, 1286, 1287, 1288, 1290, 1291, 1292, 1293, 1294, 1295, 1296, 1297, 1297, 1298,
    1299, 1300, 1300, 1301, 1302, 1302, 1303, 1303, 1303, 1304, 1304, 1304, 1304, 1304, 1305, 1305,
};

// Samples between envelope (and noise) steps for each 5-bit rate, and when in the global
// counter's 30720-sample cycle each rate steps.
constexpr uint16_t kRatePeriod[32] = {0,   2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256,
                                      192, 160,  128,  96,   80,   64,  48,  40,  32,  24,  20,
                                      16,  12,   10,   8,    6,    5,   4,   3,   2,   1};
constexpr uint16_t kRateOffset[32] = {0,   0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
                                      0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0};

int Clamp16(int v) { return std::clamp(v, -32768, 32767); }

}  // namespace

void SpcDsp::Reset(uint8_t* ram, const uint8_t regs[128]) {
    ram_ = ram;
    std::copy(regs, regs + 128, regs_);
    for (Voice& v : voices_) v = Voice();
    konLatch_ = konNow_ = koffNow_ = 0;
    poll_ = false;
    counter_ = 0;
    noise_ = 0x4000;
    for (auto& f : fir_) f[0] = f[1] = 0;
    firPos_ = 0;
    echoOffset_ = 0;
    echoLength_ = 0;
}

void SpcDsp::Write(uint8_t addr, uint8_t v) {
    addr &= 0x7F;
    regs_[addr] = v;
    if (addr == KON) konLatch_ = v;
    if (addr == ENDX) regs_[ENDX] = 0;  // any write clears every flag
}

bool SpcDsp::RateFires(int rate) const {
    if (rate == 0) return false;
    return (counter_ + kRateOffset[rate]) % kRatePeriod[rate] == 0;
}

uint16_t SpcDsp::DirEntry(int v, int which) const {
    const uint16_t entry = (uint16_t)((regs_[DIR] << 8) + regs_[v * 16 + SRCN] * 4 + which * 2);
    return (uint16_t)(ram_[entry] | ram_[(uint16_t)(entry + 1)] << 8);
}

// Decodes the next four samples of the current block into the ring.
void SpcDsp::DecodeGroup(Voice& vc, uint8_t header) {
    const int shift = header >> 4, filter = (header >> 2) & 3;
    int nibbles = ram_[(uint16_t)(vc.brrAddr + vc.brrOffset)] << 8 | ram_[(uint16_t)(vc.brrAddr + vc.brrOffset + 1)];
    for (int n = 0; n < 4; ++n, nibbles <<= 4) {
        int s = (int16_t)nibbles >> 12;
        if (shift <= 12) s = (s << shift) >> 1;
        else s &= ~0x7FF;  // shifts 13-15 give 0 or -2048
        // The previous samples, stored doubled: p1 at twice its value, p2 back to normal.
        const int p1 = vc.ring[(vc.ringPos + 11) % 12], p2 = vc.ring[(vc.ringPos + 10) % 12] >> 1;
        switch (filter) {
            case 1: s += (p1 >> 1) + ((-p1) >> 5); break;
            case 2: s += p1 - p2 + (p2 >> 4) + ((p1 * -3) >> 6); break;
            case 3: s += p1 - p2 + ((p1 * -13) >> 7) + ((p2 * 3) >> 4); break;
            default: break;
        }
        vc.ring[vc.ringPos] = (int16_t)(Clamp16(s) << 1);  // 15 bits kept, as the hardware does
        vc.ringPos = (vc.ringPos + 1) % 12;
    }
}

// Four-tap Gaussian interpolation between the last four decoded samples (the DSP's ROM table).
int SpcDsp::Interpolate(const Voice& vc) const {
    const int i = (vc.counter >> 4) & 0xFF;
    const int base = vc.ringPos + (vc.counter >> 12);
    int out = (kGauss[255 - i] * vc.ring[base % 12]) >> 11;
    out += (kGauss[511 - i] * vc.ring[(base + 1) % 12]) >> 11;
    out += (kGauss[256 + i] * vc.ring[(base + 2) % 12]) >> 11;
    out = (int16_t)out;  // these additions wrap
    out += (kGauss[i] * vc.ring[(base + 3) % 12]) >> 11;
    return Clamp16(out) & ~1;  // and this one saturates
}

// Works out the next envelope level. Phase changes are judged on the new level every sample;
// the level itself only changes when the phase's rate fires.
void SpcDsp::Envelope(int v) {
    Voice& vc = voices_[v];
    int env = vc.env;
    if (vc.mode == Mode::Release) {
        vc.env = std::max(0, env - 8);
        return;
    }
    const uint8_t adsr1 = VoiceReg(v, ADSR1);
    int data = VoiceReg(v, ADSR2), rate;
    if (adsr1 & 0x80) {
        if (vc.mode == Mode::Attack) {
            rate = (adsr1 & 15) * 2 + 1;
            env += rate < 31 ? 0x20 : 0x400;
        } else {
            --env;
            env -= env >> 8;
            rate = vc.mode == Mode::Decay ? ((adsr1 >> 4) & 7) * 2 + 16 : data & 31;
        }
    } else {
        data = VoiceReg(v, GAIN);
        const int mode = data >> 5;
        if (mode < 4) {  // direct
            env = data << 4;
            rate = 31;
        } else {
            rate = data & 31;
            if (mode == 4) {
                env -= 0x20;
            } else if (mode == 5) {
                --env;
                env -= env >> 8;
            } else {
                env += 0x20;
                if (mode == 7 && (unsigned)vc.lastEnv >= 0x600) env += 0x8 - 0x20;  // bent line
            }
        }
    }
    // Sustain begins at the level in ADSR2's top bits (in gain modes, GAIN's top bits).
    if ((env >> 8) == (data >> 5) && vc.mode == Mode::Decay) vc.mode = Mode::Sustain;
    vc.lastEnv = env;
    if ((unsigned)env > 0x7FF) {
        env = env < 0 ? 0 : 0x7FF;
        if (vc.mode == Mode::Attack) vc.mode = Mode::Decay;
    }
    if (RateFires(rate)) vc.env = env;
}

void SpcDsp::Sample(int& left, int& right) {
    poll_ = !poll_;
    if (poll_) {
        konLatch_ &= (uint8_t)~konNow_;  // a key-on is taken once, even if written again meanwhile
        konNow_ = konLatch_;
        koffNow_ = regs_[KOFF];
    }
    counter_ = counter_ == 0 ? 0x77FF : counter_ - 1;
    if (RateFires(regs_[FLG] & 31)) noise_ = ((noise_ << 13 ^ noise_ << 14) & 0x4000) | noise_ >> 1;

    int mainL = 0, mainR = 0, echoL = 0, echoR = 0, prevOut = 0;
    for (int v = 0; v < 8; ++v) {
        Voice& vc = voices_[v];
        int pitch = (VoiceReg(v, PITCHL) | VoiceReg(v, PITCHH) << 8) & 0x3FFF;
        if (v > 0 && (regs_[PMON] >> v & 1)) pitch += ((prevOut >> 5) * pitch) >> 10;
        uint8_t header = ram_[vc.brrAddr];
        if (vc.konDelay) {
            if (vc.konDelay == 5) {  // load the start address; the old block's header no longer counts
                vc.brrAddr = DirEntry(v, 0);
                vc.brrOffset = 1;
                vc.ringPos = 0;
                header = 0;
            }
            vc.env = 0;
            vc.lastEnv = 0;
            // Three groups are decoded during the delay, so playback starts on the first sample.
            --vc.konDelay;
            vc.counter = (vc.konDelay & 3) ? 0x4000 : 0;
            pitch = 0;
        }

        int sample = (regs_[NON] >> v & 1) ? (int16_t)(noise_ << 1) : Interpolate(vc);
        const int out = ((sample * vc.env) >> 11) & ~1;
        VoiceReg(v, ENVX) = (uint8_t)(vc.env >> 4);

        // A block ending without a loop silences the voice as soon as it's reached.
        if ((regs_[FLG] & 0x80) || (header & 3) == 1) {
            vc.mode = Mode::Release;
            vc.env = 0;
        }
        if (poll_) {
            if (koffNow_ >> v & 1) vc.mode = Mode::Release;
            if (konNow_ >> v & 1) {
                vc.konDelay = 5;
                vc.mode = Mode::Attack;
            }
        }
        if (!vc.konDelay) Envelope(v);

        // BRR decoding: at most one group of four samples per output sample.
        bool looped = false;
        if (vc.counter >= 0x4000) {
            DecodeGroup(vc, header);
            vc.brrOffset += 2;
            if (vc.brrOffset >= 9) {
                vc.brrAddr = (uint16_t)(vc.brrAddr + 9);
                if (header & 1) {
                    vc.brrAddr = DirEntry(v, 1);
                    looped = true;
                }
                vc.brrOffset = 1;
            }
        }
        vc.counter = std::min((vc.counter & 0x3FFF) + pitch, 0x7FFF);

        const int l = (out * (int8_t)VoiceReg(v, VOLL)) >> 7, r = (out * (int8_t)VoiceReg(v, VOLR)) >> 7;
        mainL = Clamp16(mainL + l);
        mainR = Clamp16(mainR + r);
        if (regs_[EON] >> v & 1) {
            echoL = Clamp16(echoL + l);
            echoR = Clamp16(echoR + r);
        }
        if (looped) regs_[ENDX] |= (uint8_t)(1 << v);
        if (vc.konDelay == 5) regs_[ENDX] &= (uint8_t)~(1 << v);
        VoiceReg(v, OUTX) = (uint8_t)(out >> 8);
        prevOut = out;
    }

    // Echo: read the oldest entry of the ring buffer in RAM, filter it, write the new one.
    const uint16_t addr = (uint16_t)((regs_[ESA] << 8) + echoOffset_);
    firPos_ = (firPos_ + 1) & 7;
    int firOut[2];
    for (int ch = 0; ch < 2; ++ch) {
        const uint16_t at = (uint16_t)(addr + ch * 2);
        fir_[firPos_][ch] = (int16_t)(ram_[at] | ram_[(uint16_t)(at + 1)] << 8) >> 1;
        int sum = 0;
        for (int k = 0; k < 7; ++k) sum += (fir_[(firPos_ + k + 1) & 7][ch] * (int8_t)regs_[0x0F + k * 16]) >> 6;
        sum = (int16_t)sum;
        sum += (int16_t)((fir_[firPos_][ch] * (int8_t)regs_[0x7F]) >> 6);
        firOut[ch] = Clamp16(sum) & ~1;
    }
    left = Clamp16((int16_t)((mainL * (int8_t)regs_[MVOLL]) >> 7) + (int16_t)((firOut[0] * (int8_t)regs_[EVOLL]) >> 7));
    right = Clamp16((int16_t)((mainR * (int8_t)regs_[MVOLR]) >> 7) + (int16_t)((firOut[1] * (int8_t)regs_[EVOLR]) >> 7));
    if (regs_[FLG] & 0x40) left = right = 0;  // mute
    if (!echoOffset_) echoLength_ = (regs_[EDL] & 15) << 11;
    echoOffset_ += 4;
    if (echoOffset_ >= echoLength_) echoOffset_ = 0;
    if (!(regs_[FLG] & 0x20)) {
        const int in[2] = {echoL, echoR};
        for (int ch = 0; ch < 2; ++ch) {
            const int e = Clamp16(in[ch] + (int16_t)((firOut[ch] * (int8_t)regs_[EFB]) >> 7)) & ~1;
            const uint16_t at = (uint16_t)(addr + ch * 2);
            ram_[at] = (uint8_t)e;
            ram_[(uint16_t)(at + 1)] = (uint8_t)(e >> 8);
        }
    }
}
