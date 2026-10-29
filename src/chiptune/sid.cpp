// SID: Commodore 64 music in the PSID and RSID formats. The file holds the tune's 6502 code, loaded
// into a C64's memory with a stub KERNAL (its interrupt entry and exit, nothing else). A PSID with
// a play routine is driven the way sidplay's PSID driver does it: init once, then play at the
// vertical blank or CIA timer rate. An RSID, or a PSID that installs its own interrupt handler, runs
// as a real program: CIA timers and the VIC raster raise IRQs (CIA 2 raises NMIs) and the CPU takes
// them. Up to three SID chips (6581 or 8580) are emulated with oscillators, ADSR envelopes and the
// state-variable filter, clocked every 8 cycles; their output goes through a BandBuffer. Files
// give no song lengths, so songs play for the default time.
#include "band_buffer.h"
#include "chiptune.h"
#include "cpu6502.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace chiptune {
namespace {

constexpr int kStep = 8;           // SID cycles per output sample
constexpr uint16_t kIdle = 0x0000;  // the 6510's port register: never executed
constexpr double kPalClock = 985248, kNtscClock = 1022727;

uint16_t Be16(const unsigned char* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t Be32(const unsigned char* p) { return (uint32_t)Be16(p) << 16 | Be16(p + 2); }

// Cycles per envelope step for each rate nibble.
constexpr uint16_t kRates[16] = {9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251};

class SidChip {
public:
    void Reset(bool is8580, double clock) {
        *this = SidChip{};
        model8580_ = is8580;
        clock_ = clock;
        for (auto& v : voice_) v.noise = 0x7FFFF8;
    }

    void Write(int reg, uint8_t v) {
        regs_[reg] = v;
        if (reg < 21) {
            Voice& o = voice_[reg / 7];
            switch (reg % 7) {
                case 0: o.freq = (o.freq & 0xFF00) | v; break;
                case 1: o.freq = (uint16_t)((o.freq & 0xFF) | v << 8); break;
                case 2: o.pw = (uint16_t)((o.pw & 0xF00) | v); break;
                case 3: o.pw = (uint16_t)((o.pw & 0xFF) | (v & 15) << 8); break;
                case 4: {
                    const bool gate = v & 1;
                    if (gate && !(o.control & 1)) o.state = Attack;
                    else if (!gate && (o.control & 1)) o.state = Release;
                    if (v & 8) {
                        o.acc = 0;
                        o.noise = 0x7FFFF8;
                    }
                    o.control = v;
                    break;
                }
                case 5: o.ad = v; break;
                case 6: o.sr = v; break;
            }
        }
    }

    uint8_t Read(int reg) const {
        if (reg == 0x1B) return (uint8_t)(Wave(voice_[2], voice_[1]) >> 4);
        if (reg == 0x1C) return voice_[2].env;
        if (reg == 0x19 || reg == 0x1A) return 0xFF;
        return 0;  // write-only registers read as the bus floats; zero is what tunes expect
    }

    // One output sample, after advancing everything by kStep cycles.
    float Clock() {
        bool msbRose[3];
        for (int i = 0; i < 3; ++i) {
            Voice& o = voice_[i];
            const uint32_t before = o.acc;
            if (!(o.control & 8)) o.acc = (o.acc + (uint32_t)o.freq * kStep) & 0xFFFFFF;
            msbRose[i] = !(before & 0x800000) && (o.acc & 0x800000);
            if (!(before & 0x080000) && (o.acc & 0x080000)) {
                const uint32_t bit = ((o.noise >> 22) ^ (o.noise >> 17)) & 1;
                o.noise = ((o.noise << 1) | bit) & 0x7FFFFF;
            }
            Envelope(o);
        }
        for (int i = 0; i < 3; ++i)  // hard sync from the previous voice
            if ((voice_[i].control & 2) && msbRose[(i + 2) % 3]) voice_[i].acc = 0;
        const int routing = regs_[0x17], mode = regs_[0x18];
        double direct = 0, filtered = 0;
        for (int i = 0; i < 3; ++i) {
            const Voice& o = voice_[i];
            const double out = (Wave(o, voice_[(i + 2) % 3]) - 0x800) * (o.env / 255.0);
            if (routing & (1 << i)) filtered += out;
            else if (!(i == 2 && (mode & 0x80))) direct += out;
        }
        // Chamberlin state-variable filter at the step rate.
        const int fc = (regs_[0x16] << 3) | (regs_[0x15] & 7);
        const double hz = model8580_ ? 12500.0 * fc / 2047 + 30 : 220 + 17800 * std::pow(fc / 2047.0, 1.9);
        const double w = std::min(0.9, 2 * 3.14159265358979 * hz * kStep / clock_);
        const double damping = 1.0 / (0.707 + (regs_[0x17] >> 4) / 15.0 * 1.6);
        lp_ += w * bp_;
        const double hp = filtered - lp_ - damping * bp_;
        bp_ += w * hp;
        double out = direct;
        if (mode & 0x10) out += lp_;
        if (mode & 0x20) out += bp_;
        if (mode & 0x40) out += hp;
        // The mixer's DC offset makes volume-register writes audible: how 4-bit samples were played.
        out += model8580_ ? 300 : 1500;
        return (float)(out * (mode & 15) / 15.0 / 6144.0);
    }

private:
    enum State : uint8_t { Attack, Decay, Release };
    struct Voice {
        uint32_t acc = 0, noise = 0x7FFFF8;
        uint16_t freq = 0, pw = 0;
        uint8_t control = 0, ad = 0, sr = 0, env = 0;
        State state = Release;
        int rate = 0, exp = 0;
    };

    // The 12-bit waveform; several selected at once AND together.
    static unsigned Wave(const Voice& o, const Voice& source) {
        const unsigned sel = o.control >> 4;
        if (!sel) return 0x800;
        unsigned v = 0xFFF;
        if (sel & 1) {  // triangle, ring-modulated by the source's top bit
            const uint32_t msb = (o.control & 4) ? (o.acc ^ source.acc) & 0x800000 : o.acc & 0x800000;
            v &= ((msb ? ~o.acc : o.acc) >> 11) & 0xFFF;
        }
        if (sel & 2) v &= o.acc >> 12;
        if (sel & 4) v &= ((o.control & 8) || (o.acc >> 12) >= o.pw) ? 0xFFF : 0;
        if (sel & 8) {
            const uint32_t n = o.noise;
            v &= (((n >> 20) & 1) << 11 | ((n >> 18) & 1) << 10 | ((n >> 14) & 1) << 9 | ((n >> 11) & 1) << 8 |
                  ((n >> 9) & 1) << 7 | ((n >> 5) & 1) << 6 | ((n >> 2) & 1) << 5 | (n & 1) << 4);
        }
        return v;
    }

    static void Envelope(Voice& o) {
        const int nibble = o.state == Attack ? o.ad >> 4 : o.state == Decay ? o.ad & 15 : o.sr & 15;
        o.rate += kStep;
        while (o.rate >= kRates[nibble]) {
            o.rate -= kRates[nibble];
            if (o.state == Attack) {
                if (++o.env == 0xFF) o.state = Decay;
                continue;
            }
            // Decay and release slow down as the level falls, in steps.
            const int period = o.env >= 0x5D ? 1 : o.env >= 0x36 ? 2 : o.env >= 0x1A ? 4 : o.env >= 0x0E ? 8 : o.env >= 0x06 ? 16 : o.env ? 30 : 1;
            if (++o.exp < period) continue;
            o.exp = 0;
            if (o.state == Decay && o.env <= (o.sr >> 4) * 0x11) continue;
            if (o.env) --o.env;
        }
    }

    Voice voice_[3];
    uint8_t regs_[32] = {};
    double lp_ = 0, bp_ = 0, clock_ = kPalClock;
    bool model8580_ = false;
};

// A 6526 CIA's two timers and interrupt control. Times are absolute CPU cycles.
struct Cia {
    uint16_t latch[2] = {0xFFFF, 0xFFFF}, stopped[2] = {0xFFFF, 0xFFFF};
    uint64_t zero[2] = {};  // when a running timer next underflows
    uint8_t control[2] = {}, mask = 0, flags = 0;

    bool Running(int t) const { return control[t] & 1; }
    void Update(uint64_t now) {
        for (int t = 0; t < 2; ++t)
            while (Running(t) && now >= zero[t]) {
                flags |= (uint8_t)(1 << t);
                if (control[t] & 8) {  // one-shot
                    control[t] &= (uint8_t)~1;
                    stopped[t] = latch[t];
                } else {
                    zero[t] += (uint64_t)latch[t] + 1;
                }
            }
    }
    bool Irq() const { return (flags & mask & 0x1F) != 0; }
    uint64_t Next() const {
        uint64_t n = UINT64_MAX;
        for (int t = 0; t < 2; ++t)
            if (Running(t) && (mask & (1 << t))) n = std::min(n, zero[t]);
        return n;
    }
    uint16_t Value(int t, uint64_t now) const { return Running(t) ? (uint16_t)(zero[t] - now) : stopped[t]; }

    uint8_t Read(int reg, uint64_t now) {
        Update(now);
        switch (reg & 15) {
            case 4: return (uint8_t)Value(0, now);
            case 5: return (uint8_t)(Value(0, now) >> 8);
            case 6: return (uint8_t)Value(1, now);
            case 7: return (uint8_t)(Value(1, now) >> 8);
            case 0xD: {
                const uint8_t v = (uint8_t)(flags | (Irq() ? 0x80 : 0));
                flags = 0;
                return v;
            }
            case 0xE: return control[0];
            case 0xF: return control[1];
            default: return 0xFF;
        }
    }
    void Write(int reg, uint8_t v, uint64_t now) {
        Update(now);
        const int t = (reg & 15) >= 6 && (reg & 15) <= 7 ? 1 : (reg & 15) == 0xF ? 1 : 0;
        switch (reg & 15) {
            case 4: case 6: latch[t] = (uint16_t)((latch[t] & 0xFF00) | v); break;
            case 5: case 7:
                latch[t] = (uint16_t)((latch[t] & 0xFF) | v << 8);
                if (!Running(t)) stopped[t] = latch[t];
                break;
            case 0xD:
                if (v & 0x80) mask |= v & 0x1F;
                else mask &= (uint8_t)~v;
                break;
            case 0xE: case 0xF: {
                uint16_t value = Value(t, now);
                if (v & 0x10) value = latch[t];  // force load
                if ((v & 1) && !Running(t)) zero[t] = now + value + 1;
                else if ((v & 1) && (v & 0x10)) zero[t] = now + value + 1;
                else if (!(v & 1)) stopped[t] = value;
                control[t] = (uint8_t)(v & ~0x10);
                break;
            }
            default: break;
        }
    }
};

class SidEmu final : public Emulator, Cpu6502::Bus {
public:
    std::string data;
    uint16_t load = 0, init = 0, play = 0;
    int songs = 1;
    uint32_t speed = 0;
    bool rsid = false, ntsc = false;
    bool model8580[3] = {};
    uint16_t sidAddress[3] = {0xD400, 0, 0};
    std::string name, author, released;

    SidEmu() : cpu_(*this) { cpu_.decimal = true; }

    void Prepare() {
        clock_ = ntsc ? kNtscClock : kPalClock;
        frameCycles_ = ntsc ? 263 * 65 : 312 * 63;
        lineCycles_ = ntsc ? 65 : 63;
        lines_ = ntsc ? 263 : 312;
        chips_ = sidAddress[2] ? 3 : sidAddress[1] ? 2 : 1;
        for (auto* b : {&left_, &right_}) b->Setup(clock_ / kStep, kRate, 18000, BandBuffer::Input::Samples, 4096, 16, 1.0f);
        // The KERNAL as far as interrupts need it: entry at $FF48 through ($0314), exit at $EA81.
        std::memset(kernal_, 0x60, sizeof kernal_);  // anything else is an RTS
        static const uint8_t kIrq[] = {0x48, 0x8A, 0x48, 0x98, 0x48, 0xBA, 0xBD, 0x04, 0x01, 0x29, 0x10,
                                       0xF0, 0x03, 0x6C, 0x16, 0x03, 0x6C, 0x14, 0x03};
        std::memcpy(kernal_ + 0x1F48, kIrq, sizeof kIrq);
        static const uint8_t kExit[] = {0xAD, 0x0D, 0xDC, 0x68, 0xA8, 0x68, 0xAA, 0x68, 0x40};
        std::memcpy(kernal_ + 0x0A7E, kExit, sizeof kExit);
        static const uint8_t kChain[] = {0x4C, 0x7E, 0xEA};  // $EA31: JMP $EA7E
        std::memcpy(kernal_ + 0x0A31, kChain, sizeof kChain);
        static const uint8_t kNmi[] = {0x78, 0x6C, 0x18, 0x03};  // $FE43: SEI; JMP ($0318)
        std::memcpy(kernal_ + 0x1E43, kNmi, sizeof kNmi);
        kernal_[0x1E72] = 0x40;  // the default NMI handler: RTI
        const uint16_t vectors[3] = {0xFE43, 0xFCE2, 0xFF48};
        for (int i = 0; i < 3; ++i) {
            kernal_[0x1FFA + i * 2] = (uint8_t)vectors[i];
            kernal_[0x1FFB + i * 2] = (uint8_t)(vectors[i] >> 8);
        }
    }

    Format Type() const override { return Format::SID; }
    int TrackCount() const override { return songs; }

    bool Info(int track, TrackInfo& out) const override {
        if (track < 0 || track >= songs) return false;
        out = TrackInfo();
        out.game = name;
        out.song = songs > 1 ? name + " #" + std::to_string(track + 1) : name;
        out.author = author;
        out.copyright = released;
        return true;
    }

    bool Start(int track) override {
        if (track < 0 || track >= songs) return false;
        std::memset(ram_, 0, sizeof ram_);
        std::memcpy(ram_ + load, data.data(), std::min<size_t>(data.size(), 0x10000 - load));
        for (int i = 0; i < 3; ++i) chip_[i].Reset(model8580[i], clock_);
        left_.Clear();
        right_.Clear();
        cia_[0] = Cia{};
        cia_[1] = Cia{};
        vic_ = Vic{};
        now_ = 0;
        sample_ = 0;
        pending_ = 0;
        nmiLine_ = false;
        // RAM vectors as the KERNAL leaves them, and its 60 Hz CIA 1 timer.
        const uint16_t ramVectors[3] = {0xEA31, 0xEA81, 0xFE72};
        for (int i = 0; i < 3; ++i) {
            ram_[0x314 + i * 2] = (uint8_t)ramVectors[i];
            ram_[0x315 + i * 2] = (uint8_t)(ramVectors[i] >> 8);
        }
        const uint16_t kernalTimer = ntsc ? 0x4295 : 0x4025;
        cia_[0].latch[0] = kernalTimer;
        const bool cia = track < 32 ? (speed >> track) & 1 : (speed >> 31) & 1;
        interrupts_ = rsid || play == 0;
        if (interrupts_) {
            cia_[0].Write(0xD, 0x81, 0);
            cia_[0].Write(0xE, 0x11, 0);
        }
        playPeriod_ = cia ? 0 : frameCycles_;  // 0: the CIA 1 timer's rate, read when play is due
        ram_[0] = 0x2F;
        ram_[1] = rsid ? 0x37 : Bank(init);
        cpu_.Reset();
        cpu_.s = 0xFF;
        cpu_.a = (uint8_t)track;
        cpu_.Call(init, kIdle);
        cpu_.MaskInterrupts(true);
        nextPlay_ = 0;
        return true;
    }

    void Render(float* out, size_t frames) override {
        std::fill(out, out + frames * 2, 0.0f);
        for (size_t done = 0; done < frames;) {
            if (Available() == 0) RunFrame();
            const size_t n = std::min(frames - done, Available());
            left_.Read(out + done * 2, n, 2);
            right_.Read(out + done * 2 + 1, n, 2);
            done += n;
        }
    }

    void Skip(size_t frames) override {
        while (frames > 0) {
            if (Available() == 0) RunFrame();
            const size_t n = std::min(frames, Available());
            left_.Skip(n);
            right_.Skip(n);
            frames -= n;
        }
    }

    uint8_t Read(uint16_t addr) override {
        if (addr >= 0xD000 && addr < 0xE000 && IoVisible()) return ReadIo(addr);
        if (addr >= 0xE000 && (ram_[1] & 2)) return kernal_[addr - 0xE000];
        return ram_[addr];
    }

    void Write(uint16_t addr, uint8_t v, uint32_t) override {
        if (addr >= 0xD000 && addr < 0xE000 && IoVisible()) {
            WriteIo(addr, v);
            return;
        }
        ram_[addr] = v;
    }

private:
    struct Vic {
        uint8_t control = 0x1B, compare = 0, flags = 0, enable = 0;
        uint64_t next = UINT64_MAX;  // the next raster compare match
    };

    // The memory configuration sidplay's PSID driver picks for a routine at `addr`.
    static uint8_t Bank(uint16_t addr) { return addr < 0xA000 ? 0x37 : addr < 0xD000 ? 0x36 : addr >= 0xE000 ? 0x35 : 0x34; }
    bool IoVisible() const { return (ram_[1] & 3) && (ram_[1] & 4); }
    uint64_t Now() const { return now_ + cpu_.time; }
    size_t Available() const { return std::min(left_.Available(), right_.Available()); }

    int RasterLine(uint64_t t) const { return (int)(t / lineCycles_ % lines_); }
    void ScheduleRaster(uint64_t from) {
        const int target = vic_.compare | (vic_.control & 0x80) << 1;
        if (target >= lines_) {
            vic_.next = UINT64_MAX;
            return;
        }
        const uint64_t frame = from / frameCycles_ * frameCycles_;
        uint64_t t = frame + (uint64_t)target * lineCycles_;
        if (t <= from) t += frameCycles_;
        vic_.next = t;
    }
    void UpdateVic(uint64_t now) {
        while (now >= vic_.next) {
            vic_.flags |= 1;
            vic_.next += frameCycles_;
        }
    }

    uint8_t ReadIo(uint16_t addr) {
        const uint64_t now = Now();
        if (addr >= 0xD400 && addr < 0xD800) {
            const int chip = ChipAt(addr);
            RunSid(cpu_.time);
            return chip_[chip].Read(addr & 0x1F);
        }
        if (addr < 0xD400) {
            UpdateVic(now);
            switch (addr & 0x3F) {
                case 0x11: return (uint8_t)((vic_.control & 0x7F) | (RasterLine(now) & 0x100) >> 1);
                case 0x12: return (uint8_t)RasterLine(now);
                case 0x19: return (uint8_t)(vic_.flags | 0x70 | ((vic_.flags & vic_.enable) ? 0x80 : 0));
                case 0x1A: return (uint8_t)(vic_.enable | 0xF0);
                default: return 0;
            }
        }
        if (addr >= 0xDC00 && addr < 0xDD00) return cia_[0].Read(addr, now);
        if (addr >= 0xDD00 && addr < 0xDE00) return cia_[1].Read(addr, now);
        return 0;
    }

    void WriteIo(uint16_t addr, uint8_t v) {
        const uint64_t now = Now();
        if (addr >= 0xD400 && addr < 0xD800) {
            RunSid(cpu_.time);
            chip_[ChipAt(addr)].Write(addr & 0x1F, v);
        } else if (addr < 0xD400) {
            UpdateVic(now);
            switch (addr & 0x3F) {
                case 0x11: vic_.control = v; ScheduleRaster(now); break;
                case 0x12: vic_.compare = v; ScheduleRaster(now); break;
                case 0x19: vic_.flags &= (uint8_t)~v; break;
                case 0x1A: vic_.enable = v & 15; break;
                default: break;
            }
        } else if (addr >= 0xDC00 && addr < 0xDD00) {
            cia_[0].Write(addr, v, now);
        } else if (addr >= 0xDD00 && addr < 0xDE00) {
            cia_[1].Write(addr, v, now);
        }
    }

    int ChipAt(uint16_t addr) const {
        for (int i = 1; i < chips_; ++i)
            if ((addr & 0xFFE0) == sidAddress[i]) return i;
        return 0;
    }

    // Clocks the chips up to `time` cycles into this frame, writing their samples.
    void RunSid(uint32_t time) {
        while (pending_ + kStep <= (int64_t)time) {
            pending_ += kStep;
            float l = 0, r = 0;
            for (int i = 0; i < chips_; ++i) {
                const float v = chip_[i].Clock();
                // One chip is centred; a second goes right of the first; a third sits between.
                const float pan = chips_ == 1 ? 0.5f : i == 0 ? 0.25f : i == 1 ? 0.75f : 0.5f;
                l += v * (1 - pan) * 2 / chips_;
                r += v * pan * 2 / chips_;
            }
            left_.Add(sample_, l);
            right_.Add(sample_, r);
            ++sample_;
        }
    }

    // When the CPU next needs attention: a due play call, a timer or raster interrupt, or `end`.
    uint64_t NextEvent(uint64_t end) const {
        uint64_t n = end;
        if (interrupts_) {
            n = std::min({n, cia_[0].Next(), cia_[1].Next()});
            if (vic_.enable & 1) n = std::min(n, vic_.next);
        }
        return n;
    }

    void RunFrame() {
        const uint32_t end = std::min<uint32_t>(frameCycles_, left_.MaxFrame() * kStep - kStep);
        const uint64_t stop = now_ + end;
        while (cpu_.time < end) {
            const uint64_t now = Now();
            if (interrupts_) {
                cia_[0].Update(now);
                cia_[1].Update(now);
                UpdateVic(now);
                const bool nmi = cia_[1].Irq();
                if (nmi && !nmiLine_) cpu_.Interrupt(0xFFFA);
                nmiLine_ = nmi;
                if ((cia_[0].Irq() || (vic_.flags & vic_.enable & 15)) && !cpu_.InterruptsMasked()) cpu_.Interrupt(0xFFFE);
            } else if (cpu_.pc == kIdle && Now() >= nextPlay_ && play) {
                // The PSID driver: call play at the frame or CIA rate, with its memory configuration.
                ram_[1] = Bank(play);
                cpu_.Call(play, kIdle);
                const uint32_t period = playPeriod_ ? playPeriod_ : (uint32_t)cia_[0].latch[0] + 1;
                nextPlay_ = Now() + std::max<uint32_t>(period, 1000);
            }
            if (cpu_.pc == kIdle || cpu_.jammed) {
                if (interrupts_) cpu_.MaskInterrupts(false);  // the driver's idle loop runs with interrupts on
                uint64_t next = NextEvent(stop);
                if (!interrupts_ && play) next = std::min(next, std::max(nextPlay_, Now() + 1));
                cpu_.time = (uint32_t)(std::clamp<uint64_t>(next, Now() + 1, stop) - now_);
                continue;
            }
            cpu_.Step();
        }
        RunSid(end);
        left_.EndFrame(sample_);
        right_.EndFrame(sample_);
        sample_ = 0;
        pending_ -= end;
        now_ += end;
        cpu_.time -= end;
    }

    Cpu6502 cpu_;
    SidChip chip_[3];
    Cia cia_[2];
    Vic vic_;
    BandBuffer left_, right_;
    uint8_t ram_[0x10000] = {};
    uint8_t kernal_[0x2000] = {};
    double clock_ = kPalClock;
    uint32_t frameCycles_ = 312 * 63, lineCycles_ = 63, playPeriod_ = 0;
    int lines_ = 312, chips_ = 1;
    uint64_t now_ = 0, nextPlay_ = 0;
    uint32_t sample_ = 0;  // samples written this frame
    int64_t pending_ = 0;  // cycle of this frame the chips have been clocked to
    bool interrupts_ = false, nmiLine_ = false;
};

}  // namespace

std::unique_ptr<Emulator> OpenSid(const std::string& bytes, std::string& error) {
    const auto* p = (const unsigned char*)bytes.data();
    if (bytes.size() < 0x76) {
        error = "the SID header is cut short";
        return nullptr;
    }
    const unsigned version = Be16(p + 4), offset = Be16(p + 6);
    if (version < 1 || version > 4 || offset < 0x76 || offset >= bytes.size()) {
        error = "the SID header isn't one this player knows";
        return nullptr;
    }
    auto emu = std::make_unique<SidEmu>();
    emu->rsid = p[0] == 'R';
    emu->load = Be16(p + 8);
    emu->init = Be16(p + 10);
    emu->play = Be16(p + 12);
    emu->songs = std::clamp<int>(Be16(p + 14), 1, 256);
    emu->speed = Be32(p + 18);
    emu->name = FieldText(p + 0x16, 32);
    emu->author = FieldText(p + 0x36, 32);
    emu->released = FieldText(p + 0x56, 32);
    size_t data = offset;
    if (emu->load == 0) {  // the C64 file's own two-byte load address
        if (bytes.size() < data + 2) {
            error = "the SID program is missing";
            return nullptr;
        }
        emu->load = (uint16_t)(p[data] | p[data + 1] << 8);
        data += 2;
    }
    if (emu->init == 0) emu->init = emu->load;
    if (version >= 2 && offset >= 0x7C) {
        const unsigned flags = Be16(p + 0x76);
        emu->ntsc = ((flags >> 2) & 3) == 2;
        emu->model8580[0] = ((flags >> 4) & 3) == 2;
        emu->model8580[1] = (((flags >> 6) & 3) ? ((flags >> 6) & 3) : ((flags >> 4) & 3)) == 2;
        emu->model8580[2] = (((flags >> 8) & 3) ? ((flags >> 8) & 3) : ((flags >> 4) & 3)) == 2;
        // Extra chips sit at $Dxx0 for an even xx from $D420 up, given as the middle byte.
        auto extra = [](unsigned b) { return (uint16_t)(b >= 0x42 && b <= 0xFE && !(b & 1) && (b < 0x80 || b >= 0xE0) ? 0xD000 | b << 4 : 0); };
        if (version >= 3) emu->sidAddress[1] = extra(p[0x7A]);
        if (version >= 4 && emu->sidAddress[1]) emu->sidAddress[2] = extra(p[0x7B]);
    }
    emu->data = bytes.substr(data);
    if (emu->data.empty() || emu->load + emu->data.size() > 0x10000 + 2) {
        error = "the SID program doesn't fit in memory";
        return nullptr;
    }
    emu->Prepare();
    return emu;
}

}  // namespace chiptune
