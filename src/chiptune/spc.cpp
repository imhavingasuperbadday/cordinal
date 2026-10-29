// SPC: a snapshot of the SNES sound system (64 KB RAM, DSP registers, CPU registers) taken
// while a song plays. Playback resumes the CPU from the snapshot; the DSP runs at 32 kHz.
#include "band_buffer.h"
#include "chiptune.h"
#include "spc700.h"
#include "spc_dsp.h"

#include <algorithm>
#include <cstring>

namespace chiptune {
namespace {

constexpr int kDspRate = 32000;
constexpr int kCyclesPerSample = 32;  // the CPU runs at 1.024 MHz
constexpr size_t kRamAt = 0x100, kRegsAt = 0x10100, kExtraAt = 0x101C0, kMinSize = 0x10180;

// The 64-byte boot ROM mapped at $FFC0 while bit 7 of $F1 is set.
constexpr uint8_t kIplRom[64] = {
    0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0, 0xFC, 0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5, 0x78,
    0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4, 0xD0, 0xFC, 0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5,
    0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3, 0xAB, 0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA,
    0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4, 0xDD, 0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF};

// Holds a song's whole SNES sound state and resumes it. The CPU is stepped until it has run 32
// cycles per 32 kHz DSP sample, so the two stay locked regardless of the host frame size.
class SpcEmu final : public Emulator, Spc700::Bus {
public:
    std::string file;
    TrackInfo info;

    SpcEmu() : cpu_(*this) {
        left_.Setup(kDspRate, kRate, 15000, BandBuffer::Input::Samples, 2048, 20);
        right_.Setup(kDspRate, kRate, 15000, BandBuffer::Input::Samples, 2048, 20);
    }

    Format Type() const override { return Format::SPC; }
    int TrackCount() const override { return 1; }
    bool Info(int track, TrackInfo& out) const override {
        if (track != 0) return false;
        out = info;
        return true;
    }

    bool Start(int track) override {
        if (track != 0) return false;
        const auto* p = (const unsigned char*)file.data();
        std::memcpy(ram_, p + kRamAt, 0x10000);
        control_ = ram_[0xF1];
        // With the boot ROM mapped, the dump shows the ROM there; the RAM beneath is kept apart.
        if (file.size() >= kExtraAt + 64 && (control_ & 0x80)) std::memcpy(ram_ + 0xFFC0, p + kExtraAt, 64);
        dspAddr_ = ram_[0xF2];
        std::memcpy(inPorts_, ram_ + 0xF4, 4);
        for (int i = 0; i < 3; ++i) {
            Timer& t = timers_[i];
            t.period = i == 2 ? 16 : 128;  // 64 kHz for timer 2, 8 kHz for the others
            t.target = ram_[0xFA + i];
            t.counter = ram_[0xFD + i] & 15;
            t.stage = 0;
            t.on = (control_ >> i) & 1;
            t.next = t.period;
        }
        cycles_ = 0;
        cpuTime_ = 0;

        const uint8_t* regs = p + kRegsAt;
        // Clear the echo buffer if echo writes are on: a dump's buffer holds stale sound
        // that would otherwise play back at the start.
        if (!(regs[0x6C] & 0x20)) {
            const int size = std::max((regs[0x7D] & 15) * 0x800, 4);
            for (int i = 0; i < size; ++i) ram_[(uint16_t)((regs[0x6D] << 8) + i)] = 0;
        }
        dsp_.Reset(ram_, regs);

        cpu_.pc = (uint16_t)Le16(p + 0x25);
        cpu_.a = p[0x27];
        cpu_.x = p[0x28];
        cpu_.y = p[0x29];
        cpu_.psw = p[0x2A];
        cpu_.sp = p[0x2B];
        cpu_.halted = false;
        left_.Clear();
        right_.Clear();
        return true;
    }

    void Render(float* out, size_t frames) override {
        std::fill(out, out + frames * 2, 0.0f);
        for (size_t done = 0; done < frames;) {
            if (Available() == 0) RunFrame(frames - done);
            const size_t n = std::min(frames - done, Available());
            left_.Read(out + done * 2, n, 2);
            right_.Read(out + done * 2 + 1, n, 2);
            done += n;
        }
    }

    void Skip(size_t frames) override {
        while (frames > 0) {
            if (Available() == 0) RunFrame(frames);
            const size_t n = std::min(frames, Available());
            left_.Skip(n);
            right_.Skip(n);
            frames -= n;
        }
    }

    uint8_t Read(uint16_t addr) override {
        // $F0-$FF are the CPU's on-chip registers: DSP data/address, timers, ports and control.
        if ((addr & 0xFFF0) == 0x00F0) {
            switch (addr & 15) {
                case 0x2: return dspAddr_;
                case 0x3: return dsp_.Read(dspAddr_);
                case 0x4: case 0x5: case 0x6: case 0x7: return inPorts_[addr - 0xF4];
                case 0x8: case 0x9: return ram_[addr];
                case 0xD: case 0xE: case 0xF: {  // timer outputs clear when read
                    Timer& t = timers_[addr - 0xFD];
                    CatchUp(t);
                    const uint8_t v = t.counter;
                    t.counter = 0;
                    return v;
                }
                default: return 0;
            }
        }
        if (addr >= 0xFFC0 && (control_ & 0x80)) return kIplRom[addr - 0xFFC0];
        return ram_[addr];
    }

    void Write(uint16_t addr, uint8_t v) override {
        ram_[addr] = v;
        if ((addr & 0xFFF0) != 0x00F0) return;
        switch (addr & 15) {
            case 0x1:
                for (int i = 0; i < 3; ++i) {
                    Timer& t = timers_[i];
                    CatchUp(t);
                    const bool on = (v >> i) & 1;
                    if (on && !t.on) t.stage = t.counter = 0;
                    t.on = on;
                }
                if (v & 0x10) inPorts_[0] = inPorts_[1] = 0;
                if (v & 0x20) inPorts_[2] = inPorts_[3] = 0;
                control_ = v;
                break;
            case 0x2: dspAddr_ = v; break;
            case 0x3: if (dspAddr_ < 0x80) dsp_.Write(dspAddr_, v); break;
            case 0xA: case 0xB: case 0xC: CatchUp(timers_[addr - 0xFA]); timers_[addr - 0xFA].target = v; break;
            default: break;  // $F4-$F7 go to the main CPU, which isn't there
        }
    }

private:
    struct Timer {
        uint32_t period = 128;
        uint64_t next = 128;
        uint8_t target = 0, stage = 0, counter = 0;
        bool on = false;
    };

    void CatchUp(Timer& t) {
        while (t.next <= cycles_) {
            t.next += t.period;
            if (t.on && ++t.stage == t.target) {  // a target of 0 counts 256
                t.stage = 0;
                t.counter = (t.counter + 1) & 15;
            }
        }
    }

    size_t Available() const { return std::min(left_.Available(), right_.Available()); }

    // Runs the CPU and the DSP together: 32 CPU cycles per 32 kHz DSP sample, in lockstep.
    void RunFrame(size_t wanted) {
        const uint32_t samples = (uint32_t)std::clamp<size_t>(wanted * kDspRate / kRate + 2, 1, 1024);
        for (uint32_t i = 0; i < samples; ++i) {
            while (cpuTime_ < kCyclesPerSample) {
                const int c = cpu_.Step();
                cpuTime_ += c;
                cycles_ += c;
            }
            cpuTime_ -= kCyclesPerSample;
            for (Timer& t : timers_) CatchUp(t);
            int l, r;
            dsp_.Sample(l, r);
            left_.Add(i, (float)l / 32768);
            right_.Add(i, (float)r / 32768);
        }
        left_.EndFrame(samples);
        right_.EndFrame(samples);
    }

    Spc700 cpu_;
    SpcDsp dsp_;
    BandBuffer left_, right_;
    uint8_t ram_[0x10000] = {};
    Timer timers_[3];
    uint8_t control_ = 0, dspAddr_ = 0, inPorts_[4] = {};
    uint64_t cycles_ = 0;
    int cpuTime_ = 0;
};

// ID666 text fields are digits; the binary layout stores numbers. Old dumpers disagree on
// where the artist field starts (0xB0 or 0xB1) in the text layout.
int TextNumber(const unsigned char* p, size_t n) {
    int v = 0;
    for (size_t i = 0; i < n && p[i] >= '0' && p[i] <= '9'; ++i) v = v * 10 + (p[i] - '0');
    return v;
}

void ReadId666(const unsigned char* p, TrackInfo& info) {
    info.song = FieldText(p + 0x2E, 32);
    info.game = FieldText(p + 0x4E, 32);
    info.dumper = FieldText(p + 0x6E, 16);
    bool text = true;
    for (int i = 0xA9; i < 0xB0; ++i)
        if (p[i] && (p[i] < '0' || p[i] > '9')) text = false;
    int seconds;
    size_t artistAt = 0xB0;
    if (text) {
        seconds = TextNumber(p + 0xA9, 3);
        if (p[0xB0] == 0 || (p[0xB0] >= '0' && p[0xB0] <= '9')) artistAt = 0xB1;
    } else {
        seconds = p[0xA9] | p[0xAA] << 8 | p[0xAB] << 16;
    }
    info.author = FieldText(p + artistAt, 32);
    if (seconds > 0) info.lengthMs = seconds * 1000;
}

// Extended ID666 ("xid6") after the snapshot: id, type, then a 16-bit value or a length and
// that many bytes padded to 4.
void ReadXid6(const std::string& file, TrackInfo& info) {
    constexpr size_t kAt = 0x10200;
    if (file.size() < kAt + 8 || file.compare(kAt, 4, "xid6") != 0) return;
    const auto* p = (const unsigned char*)file.data();
    const size_t end = std::min(file.size(), kAt + 8 + Le32(p + kAt + 4));
    std::string year, publisher;
    for (size_t at = kAt + 8; at + 4 <= end;) {
        const uint8_t id = p[at], type = p[at + 1];
        const unsigned value = Le16(p + at + 2);
        at += 4;
        if (type == 0) {
            if (id == 0x14) year = std::to_string(value);
            continue;
        }
        if (at + value > end) break;
        if (type == 1) {
            const std::string s = FieldText(p + at, value);
            switch (id) {
                case 0x01: info.song = s; break;
                case 0x02: info.game = s; break;
                case 0x03: info.author = s; break;
                case 0x04: info.dumper = s; break;
                case 0x13: publisher = s; break;
                default: break;
            }
        }
        at += (value + 3) & ~3u;
    }
    if (!year.empty() || !publisher.empty()) info.copyright = year + (year.empty() || publisher.empty() ? "" : " ") + publisher;
}

}  // namespace

std::unique_ptr<Emulator> OpenSpc(const std::string& bytes, std::string& error) {
    if (bytes.size() < kMinSize) {
        error = "the SPC file is cut short";
        return nullptr;
    }
    auto emu = std::make_unique<SpcEmu>();
    const auto* p = (const unsigned char*)bytes.data();
    if (p[0x23] == 26) ReadId666(p, emu->info);
    ReadXid6(bytes, emu->info);
    emu->file = bytes.substr(0, std::min<size_t>(bytes.size(), 0x10200));
    return emu;
}

}  // namespace chiptune
