// GBS: Game Boy music rips. Like NSF, the file holds the game's sound code: an init routine
// called with the song number, and a play routine called at the vertical blank rate (about
// 59.7 Hz) or at the rate of the timer the header sets up.
#include "band_buffer.h"
#include "chiptune.h"
#include "cpu_gb.h"
#include "gb_apu.h"

#include <algorithm>
#include <cstring>

namespace chiptune {
namespace {

constexpr double kClock = 4194304;
constexpr uint32_t kFrameClocks = 70224;  // one LCD frame
constexpr uint16_t kIdle = 0xFEB0;        // in the unusable gap after sprite RAM: never executed

class GbsEmu final : public Emulator, CpuGb::Bus {
public:
    std::string rom;  // the program at its load address, padded to whole 16 KB banks
    uint16_t load = 0, init = 0, play = 0, stack = 0;
    uint8_t tma = 0, tac = 0;
    int songs = 1;
    std::string game, author, copyright;

    GbsEmu() : cpu_(*this) {}

    void Prepare() {
        // The output capacitor high-passes at about 30 Hz.
        left_.Setup(kClock, kRate, 20000, BandBuffer::Input::Steps, 4096, 30, 1.85f);
        right_.Setup(kClock, kRate, 20000, BandBuffer::Input::Steps, 4096, 30, 1.85f);
    }

    Format Type() const override { return Format::GBS; }
    int TrackCount() const override { return songs; }

    bool Info(int track, TrackInfo& out) const override {
        if (track < 0 || track >= songs) return false;
        out = TrackInfo();
        out.game = game;
        out.author = author;
        out.copyright = copyright;
        return true;
    }

    bool Start(int track) override {
        if (track < 0 || track >= songs) return false;
        std::memset(ram_, 0, sizeof ram_);
        bank_ = 1;
        left_.Clear();
        right_.Clear();
        apu_.Reset(&left_, &right_);
        apu_.Write(0, 0xFF26, 0x80);  // sound on, both outputs at full volume, every channel panned centre
        apu_.Write(0, 0xFF24, 0x77);
        apu_.Write(0, 0xFF25, 0xFF);
        // The header's TMA/TAC program the timer that drives the play routine when it uses one.
        ram_[0xFF06 - 0x8000] = tma;
        ram_[0xFF07 - 0x8000] = tac;
        UpdateRate();
        cpu_.Reset();
        cpu_.rstBase = load;
        cpu_.sp = stack;
        cpu_.a = (uint8_t)track;
        cpu_.Call(init, kIdle);
        nextPlay_ = playPeriod_;
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
        if (addr < 0x4000) return addr < rom.size() ? (uint8_t)rom[addr] : 0xFF;
        if (addr < 0x8000) {
            const size_t at = (size_t)bank_ * 0x4000 + (addr - 0x4000);
            return at < rom.size() ? (uint8_t)rom[at] : 0xFF;
        }
        if (addr >= 0xE000 && addr < 0xFE00) addr -= 0x2000;  // echo of work RAM
        if (addr >= 0xFF10 && addr < 0xFF40) return apu_.Read(cpu_.time, addr);
        return ram_[addr - 0x8000];
    }

    void Write(uint16_t addr, uint8_t v, uint32_t t) override {
        if (addr < 0x8000) {
            if (addr >= 0x2000 && addr < 0x4000) {  // ROM bank select; bank 0 selects 1
                const size_t banks = rom.size() / 0x4000;
                bank_ = v ? v % banks : 1 % banks;
            }
            return;
        }
        if (addr >= 0xE000 && addr < 0xFE00) addr -= 0x2000;
        if (addr >= 0xFF10 && addr < 0xFF40) {
            apu_.Write(t, addr, v);
            return;
        }
        ram_[addr - 0x8000] = v;
        if (addr == 0xFF06 || addr == 0xFF07) UpdateRate();
    }

private:
    size_t Available() const { return std::min(left_.Available(), right_.Available()); }

    // The play routine runs from the timer interrupt if the timer is on, else from vblank.
    void UpdateRate() {
        const uint8_t control = ram_[0xFF07 - 0x8000];
        if (control & 4) {
            static const int kDivider[4] = {1024, 16, 64, 256};
            playPeriod_ = (256 - ram_[0xFF06 - 0x8000]) * kDivider[control & 3];
            if (control & 0x80) playPeriod_ /= 2;  // Game Boy Color double speed
        } else {
            playPeriod_ = kFrameClocks;
        }
        cpu_.doubleSpeed = (tac & 0x80) != 0;
    }

    void RunFrame() {
        const uint32_t end = std::clamp(nextPlay_, 1u, left_.MaxFrame());
        cpu_.Run(end, kIdle);
        apu_.EndFrame(end);
        left_.EndFrame(end);
        right_.EndFrame(end);
        cpu_.time -= std::min(cpu_.time, end);
        nextPlay_ -= end;
        if (nextPlay_ == 0) {
            nextPlay_ = playPeriod_;
            cpu_.halted = false;  // the interrupt wakes a halted CPU
            if (cpu_.pc == kIdle) cpu_.Call(play, kIdle);
        }
    }

    CpuGb cpu_;
    GbApu apu_;
    BandBuffer left_, right_;
    uint8_t ram_[0x8000] = {};  // $8000-$FFFF
    size_t bank_ = 1;
    uint32_t playPeriod_ = kFrameClocks, nextPlay_ = 0;
};

}  // namespace

std::unique_ptr<Emulator> OpenGbs(const std::string& bytes, std::string& error) {
    const auto* p = (const unsigned char*)bytes.data();
    constexpr size_t kHeader = 0x70;
    // Only GBS version 1 is defined; a later number would change the header layout.
    if (bytes.size() < kHeader || p[3] != 1) {
        error = bytes.size() < kHeader ? "the GBS header is cut short" : "the GBS version isn't supported";
        return nullptr;
    }
    auto emu = std::make_unique<GbsEmu>();
    emu->songs = p[4];
    emu->load = (uint16_t)Le16(p + 6);
    emu->init = (uint16_t)Le16(p + 8);
    emu->play = (uint16_t)Le16(p + 10);
    emu->stack = (uint16_t)Le16(p + 12);
    emu->tma = p[14];
    emu->tac = p[15];
    emu->game = FieldText(p + 0x10, 32);
    emu->author = FieldText(p + 0x30, 32);
    emu->copyright = FieldText(p + 0x50, 32);
    if (emu->songs < 1) {
        error = "the GBS has no songs";
        return nullptr;
    }
    if (emu->load < 0x400 || emu->load >= 0x8000 || bytes.size() == kHeader) {
        error = "the GBS program is missing or in the wrong place";
        return nullptr;
    }
    std::string& rom = emu->rom;
    rom.assign(emu->load, '\0');
    rom.append(bytes, kHeader, std::min<size_t>(bytes.size() - kHeader, 4u << 20));
    rom.resize(std::max<size_t>(0x8000, (rom.size() + 0x3FFF) & ~(size_t)0x3FFF), '\0');
    emu->Prepare();
    return emu;
}

}  // namespace chiptune
