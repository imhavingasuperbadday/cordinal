// Unit tests for the NES expansion-audio chips: Sunsoft 5B, FDS, MMC5 and VRC7, plus the NSF/NSFE
// bus that drives them. Called from selftest.cpp's "NES expansion audio" group. Small hand-assembled
// 6502 programs are played through the real NSF player so register routing is exercised end to end,
// not just the chip APIs.
#include "chiptune/chiptune.h"
#include "chiptune/nes_apu.h"
#include "chiptune/nes_expansion.h"
#include "testlog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {
// NTSC CPU clock (Hz), the timebase the expansion chips are clocked with.
constexpr double kClock = 1789773;
char Byte(unsigned value) { return (char)(value & 255); }

// A band buffer configured exactly as the player runs it, so rendered audio matches production.
void Setup(BandBuffer& b, double clock = kClock) {
    b.Setup(clock, 44100, 18000, BandBuffer::Input::Steps, 4096, 0);
}

template<class Chip>
// Clocks `chip` and its band buffer together for `cycles` CPU cycles in blocks, returning the
// mixed output; chunking is parameterised so callers can prove output is independent of it.
std::vector<float> Render(Chip& chip, BandBuffer& b, uint32_t cycles, uint32_t chunk = 8191) {
    std::vector<float> result;
    while (cycles) {
        const uint32_t n = std::min(cycles, chunk);
        chip.EndFrame(n);
        b.EndFrame(n);
        const size_t from = result.size(), count = b.Available();
        result.resize(from + count, 0);
        b.Read(result.data() + from, count, 1);
        cycles -= n;
    }
    return result;
}

// Root-mean-square of v[from..]; used to distinguish silence from a signal.
double Rms(const std::vector<float>& v, size_t from = 0) {
    double sum = 0;
    for (size_t i = from; i < v.size(); ++i) sum += v[i] * v[i];
    return v.size() > from ? std::sqrt(sum / (v.size() - from)) : 0;
}

// Standard deviation of the tail (the first 100 samples skipped): ~0 for a constant DC level and
// clearly positive for a tone or noise, which is how most "is this channel making sound?" checks work.
double Spread(const std::vector<float>& v) {
    if (v.size() < 100) return 0;
    double sum = 0, square = 0;
    for (size_t i = 100; i < v.size(); ++i) { sum += v[i]; square += v[i] * v[i]; }
    const size_t n = v.size() - 100;
    return std::sqrt(std::max(0.0, square / n - sum * sum / n / n));
}

// Non-empty with every sample finite and within the mixer's allowed range.
bool Finite(const std::vector<float>& v) {
    return !v.empty() && std::all_of(v.begin(), v.end(), [](float f) { return std::isfinite(f) && std::abs(f) < 4; });
}

// Frequency estimated from positive-going crossings of `mean` over the tail of v (skipping the
// initial 500 samples so start-up transients don't skew it).
double Frequency(const std::vector<float>& v, double mean = 0) {
    int crossings = 0;
    const size_t from = std::min<size_t>(500, v.size());
    for (size_t i = from + 1; i < v.size(); ++i) if (v[i-1] < mean && v[i] >= mean) ++crossings;
    return v.size() > from ? crossings * 44100.0 / (v.size() - from) : 0;
}

// Sunsoft 5B has a single register port: latch the address, then write the data.
void SunWrite(Sunsoft5b& chip, uint8_t reg, uint8_t value) {
    chip.WriteAddr(reg);
    chip.WriteData(0, value);
}

// Sunsoft 5B: the shape-based envelope generator and its boundaries.
void TestSunsoft() {
    using testlog::Check;
    constexpr int endLevel[16] = {0,0,0,0,0,0,0,0,31,0,0,31,0,31,31,0};
    bool boundaries = true, retriggers = true;
    for (int shape = 0; shape < 16; ++shape) {
        BandBuffer b; Setup(b);
        Sunsoft5b chip; chip.Reset(&b);
        SunWrite(chip, 7, 0x3F);
        SunWrite(chip, 8, 0x10); // envelope must work with fixed volume bits all zero
        SunWrite(chip, 11, 0); SunWrite(chip, 12, 1);
        SunWrite(chip, 13, (uint8_t)shape);
        auto audio = Render(chip, b, 32 * 4096 + 2000);
        const float expected = endLevel[shape] == 31 ? 0.28f : 0;
        boundaries &= Finite(audio) && std::abs(audio.back() - expected) < 0.0001;
        SunWrite(chip, 13, 9);
        audio = Render(chip, b, 2000);
        retriggers &= std::abs(audio.back() - 0.28f) < 0.0001;
    }
    Check(boundaries, L"5B envelope shapes 0-F each reach their documented boundary");
    Check(retriggers, L"5B envelope shapes 0-F each retrigger from shape 9");
    BandBuffer b; Setup(b);
    Sunsoft5b chip; chip.Reset(&b);
    SunWrite(chip, 6, 15); SunWrite(chip, 7, 0x37); SunWrite(chip, 8, 15);
    auto noise = Render(chip, b, 200000);
    Check(Finite(noise) && Spread(noise) > 0.05, L"5B noise-only channel produces varying sound");
    SunWrite(chip, 7, 0x3F);
    auto dc = Render(chip, b, 20000);
    Check(Spread(dc) < 0.0001 && std::abs(dc.back() - 0.28f) < 0.0001, L"5B disabled tone/noise gates pass constant volume");
    SunWrite(chip, 0x18, 0); // invalid upper latch bits disable writes, rather than aliasing register 8
    auto ignored = Render(chip, b, 20000);
    Check(std::abs(ignored.back() - 0.28f) < 0.0001, L"5B invalid register latch ignores data");
    b.Clear(); chip.Reset(&b);
    SunWrite(chip, 7, 0x3F); SunWrite(chip, 8, 16);
    SunWrite(chip, 11, 0); SunWrite(chip, 12, 1); SunWrite(chip, 13, 0x0C);
    auto ramp = Render(chip, b, 16 * 4096 + 2000);
    const double middle = 0.28 * std::pow(10.0, (16 - 31) * 1.5 / 20);
    Check(std::abs(ramp.back() - middle) < 0.00001, L"5B envelope uses 32 levels at 1.5 dB spacing");
}

// Enables the wave table and starts a sustained tone at 1024 (437 Hz) for the FDS checks.
void LoadFdsWave(FdsAudio& chip) {
    chip.Write(0, 0x4089, 0x80);
    for (int i = 0; i < 64; ++i) chip.Write(0, (uint16_t)(0x4040 + i), (uint8_t)i);
    chip.Write(0, 0x4089, 0);
    chip.Write(0, 0x4080, 0xA0);
    chip.Write(0, 0x4082, 0x00);
    chip.Write(0, 0x4083, 0x04); // 1024 -> 437 Hz
}

// FDS: wave RAM rules, modulation (pitch and table), volume, and the two envelopes.
void TestFds() {
    using testlog::Check;
    BandBuffer b; Setup(b);
    FdsAudio chip; chip.Reset(&b);
    chip.Write(0, 0x4089, 0x80);
    chip.Write(0, 0x4045, 0xFF);
    Check(chip.Read(0, 0x4045) == 0x7F, L"FDS wave RAM is six bits with open-bus bit 6");
    chip.Write(0, 0x4089, 0);
    chip.Write(0, 0x4040, 63);
    Check(chip.Read(0, 0x4045) == 0x40, L"FDS protected wave writes are ignored and reads follow playback position");
    LoadFdsWave(chip);
    auto plain = Render(chip, b, 400000);
    const double hz = Frequency(plain, 0.135);
    Check(Finite(plain) && Spread(plain) > 0.05 && std::abs(hz - kClock / 4096) < 10,
          L"FDS wavetable has expected pitch (%.2f Hz)", hz);
    chip.Write(0, 0x4089, 0x80);
    auto held = Render(chip, b, 20000);
    Check(Spread(held) < 0.0001 && Rms(held, 100) > 0.01, L"FDS wave-write mode holds the current DAC level");
    chip.Write(0, 0x4089, 0);
    chip.Write(0, 0x4084, 0xA0);
    chip.Write(0, 0x4085, 8);
    auto modulated = Render(chip, b, 400000);
    Check(Frequency(modulated, 0.135) > hz * 1.2, L"FDS modulation gain and counter change wave pitch even with stopped table");
    chip.Write(0, 0x4080, 0x80);
    auto muted = Render(chip, b, 20000);
    Check(Rms(muted, 100) < 0.0001, L"FDS zero direct volume mutes immediately");

    b.Clear(); chip.Reset(&b); LoadFdsWave(chip);
    for (int i = 0; i < 32; ++i) chip.Write(0, 0x4088, 1);
    chip.Write(0, 0x4084, 0xA0); chip.Write(0, 0x4086, 0); chip.Write(0, 0x4087, 4);
    auto table = Render(chip, b, 400000);
    Check(Finite(table) && table != plain && Spread(table) > 0.01, L"FDS modulation table advances and changes sound");

    b.Clear(); chip.Reset(&b);
    chip.Write(0, 0x408A, 1);
    chip.Write(0, 0x4083, 0);
    chip.Write(0, 0x4080, 0x40);
    chip.Write(0, 0x4084, 0x40);
    Render(chip, b, 80);
    Check(chip.Read(0, 0x4090) == 0x44 && chip.Read(0, 0x4092) == 0x44,
          L"FDS both envelopes tick every 8*(speed+1)*(master+1) cycles");
    chip.Write(0, 0x4083, 0x40);
    Render(chip, b, 20000);
    Check(chip.Read(0, 0x4090) == 0x44, L"FDS envelope halt freezes gain across frames");
    chip.Write(0, 0x4023, 0);
    chip.Write(0, 0x4080, 0x9F);
    chip.Write(0, 0x4023, 2);
    Check(chip.Read(0, 0x4090) == 0x44, L"FDS master enable gates register access");
}

// MMC5: two pulse channels, the length counter, and the eight-bit PCM DAC (write and read mode).
void TestMmc5() {
    using testlog::Check;
    BandBuffer b; Setup(b);
    Mmc5Audio chip; chip.Reset(&b);
    chip.Write(0, 0x5015, 3);
    chip.Write(0, 0x5000, 0xBF); // constant 50% pulse, length halt
    chip.Write(0, 0x5002, 253);
    chip.Write(0, 0x5003, 0xF8);
    auto pulse = Render(chip, b, 400000);
    const double hz = Frequency(pulse, -0.00752 * 15 / 2);
    Check(Finite(pulse) && Spread(pulse) > 0.04 && std::abs(hz - kClock / (16 * 254)) < 10,
          L"MMC5 pulse has expected pitch and inverted polarity (%.2f Hz)", hz);
    chip.Write(0, 0x5015, 0);
    auto silence = Render(chip, b, 20000);
    Check(Rms(silence, 100) < 0.0001 && chip.Read(0, 0x5015) == 0, L"MMC5 channel disable clears length and mutes");
    chip.Write(0, 0x5015, 1);
    chip.Write(0, 0x5000, 0x1F);
    chip.Write(0, 0x5003, 0x18); // length 2, expires at second envelope clock
    Render(chip, b, 16000);
    Check(chip.Read(0, 0x5015) == 0, L"MMC5 length counter clocks at 240 Hz");
    chip.Write(0, 0x5015, 2);
    chip.Write(0, 0x5004, 0xA0); // second pulse, looping envelope
    chip.Write(0, 0x5006, 253); chip.Write(0, 0x5007, 0xF8);
    auto envelope = Render(chip, b, 400000);
    Check(chip.Read(0, 0x5015) == 2 && Spread(envelope) > 0.01,
          L"MMC5 second pulse and looping volume envelope produce sound");
    chip.Write(0, 0x5015, 0);

    chip.Write(0, 0x5011, 128);
    auto dac = Render(chip, b, 20000);
    Check(std::abs(dac.back() + 128 * 0.00335f) < 0.0001, L"MMC5 full eight-bit PCM DAC is inverted");
    chip.Write(0, 0x5010, 0x80);
    chip.Write(0, 0x5011, 0);
    auto hold = Render(chip, b, 20000);
    Check(std::abs(hold.back() - dac.back()) < 0.0001 && chip.Read(0, 0x5010) == 0x80 && chip.Read(0, 0x5010) == 0,
          L"MMC5 PCM zero holds DAC and IRQ status acknowledges on read");
    chip.Write(0, 0x5010, 1);
    chip.Write(0, 0x5011, 64); // ignored in read mode
    chip.PcmRead(0, 0xC000, 64); // out of PCM read range
    hold = Render(chip, b, 20000);
    Check(std::abs(hold.back() - dac.back()) < 0.0001, L"MMC5 read-mode ignores direct writes and out-of-range reads");
    chip.PcmRead(0, 0x8000, 64);
    auto readDac = Render(chip, b, 20000);
    Check(std::abs(readDac.back() + 64 * 0.00335f) < 0.0001, L"MMC5 ROM reads update read-mode PCM");
}

// VRC7 has an address port and a data port, like the 5B.
void VrcWrite(Vrc7Audio& chip, uint8_t reg, uint8_t value) {
    chip.WriteAddr(reg); chip.WriteData(0, value);
}

// Sets a patch whose carrier dominates, then keys on a single FM channel at fnum 288, block 4.
void PureVrc7(Vrc7Audio& chip, int channel = 0) {
    // Modulator is heavily attenuated; sustained carrier is a simple sine.
    constexpr uint8_t patch[8] = {0x21,0x21,0x3F,0,0xF0,0xF0,0,0x0F};
    for (int r = 0; r < 8; ++r) VrcWrite(chip, (uint8_t)r, patch[r]);
    VrcWrite(chip, (uint8_t)(0x30 + channel), 0);
    VrcWrite(chip, (uint8_t)(0x10 + channel), 0x20);
    VrcWrite(chip, (uint8_t)(0x20 + channel), 0x19); // fnum 288, block 4, key on
}

// VRC7: the built-in sine patch in NTSC and PAL, key-off release/retrigger, all instrument ROM
// patches, timbre changes, and stability of the fractional clock across frame sizes.
void TestVrc7() {
    using testlog::Check;
    bool vrc7Pitch = true, vrc7Release = true, vrc7Retrigger = true;
    for (bool pal : {false, true}) {
        BandBuffer b; Setup(b, pal ? 1662607 : kClock);
        Vrc7Audio chip; chip.Reset(pal, &b);
        PureVrc7(chip);
        const uint32_t cycles = pal ? 831303 : 894886;
        auto audio = Render(chip, b, cycles);
        const double hz = Frequency(audio);
        vrc7Pitch &= Finite(audio) && Spread(audio) > 0.04 && std::abs(hz - 436.96) < 5;
        VrcWrite(chip, 0x20, 0x09);
        auto release = Render(chip, b, cycles);
        vrc7Release &= Rms(release, release.size() / 2) < 0.0001;
        VrcWrite(chip, 0x20, 0x19);
        auto retrigger = Render(chip, b, 100000);
        vrc7Retrigger &= Spread(retrigger) > 0.04;
    }
    Check(vrc7Pitch, L"VRC7 custom sine keeps 436.96 Hz in both NTSC and PAL");
    Check(vrc7Release, L"VRC7 key-off release silences the carrier in both regions");
    Check(vrc7Retrigger, L"VRC7 key-on retriggers after release in both regions");
    std::vector<float> first;
    bool instruments = true, timbre = true;
    for (int instrument = 1; instrument <= 15; ++instrument) {
        BandBuffer b; Setup(b);
        Vrc7Audio chip; chip.Reset(false, &b);
        const int channel = (instrument - 1) % 6;
        VrcWrite(chip, (uint8_t)(0x30 + channel), (uint8_t)(instrument << 4));
        VrcWrite(chip, (uint8_t)(0x10 + channel), 0x20);
        VrcWrite(chip, (uint8_t)(0x20 + channel), 0x19);
        auto audio = Render(chip, b, 400000);
        instruments &= Finite(audio) && Rms(audio) > 0.00001;
        if (instrument == 1) first = audio;
        if (instrument == 2) timbre = audio != first;
    }
    Check(instruments, L"VRC7 instruments 1-F each produce sound");
    Check(timbre, L"VRC7 instrument ROM changes timbre");
    auto render = [](uint32_t chunk) {
        BandBuffer b; Setup(b);
        Vrc7Audio chip; chip.Reset(false, &b); PureVrc7(chip, 5);
        return Render(chip, b, 500000, chunk);
    };
    auto a = render(8191), b = render(20003);
    double difference = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) difference = std::max(difference, (double)std::abs(a[i] - b[i]));
    Check(a.size() == b.size() && difference < 0.00001, L"VRC7 fractional clock is stable across different frame sizes");
}

// Tiny original 6502 programs drive the actual NSF bus, rather than only the chip APIs.
// Program assembles them by hand: LDA imm / STA abs, LDA abs / STA abs, and a CMP+BEQ that returns
// early unless a register equals an expected value (so a failed check makes no sound).
struct Program {
    std::string bytes;
    void Write(uint16_t addr, uint8_t v) {
        bytes += "\xA9"; bytes += (char)v;
        bytes += "\x8D"; bytes += (char)addr; bytes += (char)(addr >> 8);
    }
    void Copy(uint16_t from, uint16_t to) {
        bytes += "\xAD"; bytes += (char)from; bytes += (char)(from >> 8);
        bytes += "\x8D"; bytes += (char)to; bytes += (char)(to >> 8);
    }
    void Require(uint16_t addr, uint8_t value) {
        bytes += "\xAD"; bytes += (char)addr; bytes += (char)(addr >> 8);
        bytes += "\xC9"; bytes += (char)value;
        bytes += "\xF0\x01\x60"; // BEQ past RTS; a failed condition returns before making sound
    }
};

// Builds a minimal NSF v1 image: a 128-byte header plus the program at `load`, with init and play
// both pointing at a trailing RTS. `chips` is the expansion-chip enable mask byte ($7B).
std::string Nsf(Program program, uint8_t chips, uint16_t load = 0x8000) {
    std::string file(128, '\0');
    std::memcpy(file.data(), "NESM\x1A", 5);
    file[5] = 1; file[6] = 1; file[7] = 1;
    auto put = [&](int at, unsigned v) { file[at] = (char)v; file[at+1] = (char)(v >> 8); };
    put(8, load); put(10, load); put(12, load + (unsigned)program.bytes.size());
    file[0x7B] = (char)chips;
    file += program.bytes;
    file += (char)0x60; // init and play both return here
    return file;
}

// Plays a file for a fixed 0.5 s of stereo output (22050 frames), so two paths can be compared
// exactly; an empty result means the file failed to open or start.
std::vector<float> Play(const std::string& file, bool nsfe = false) {
    std::string error;
    auto emu = nsfe ? chiptune::OpenNsfe(file, error) : chiptune::OpenNsf(file, error);
    if (!emu || !emu->Start(0)) return {};
    std::vector<float> out(22050 * 2);
    emu->Render(out.data(), out.size() / 2);
    return out;
}

// Wraps the same header fields, banks and program data as an NSFE file, so the two loaders can be
// checked to produce identical output.
std::string Nsfe(const std::string& nsf) {
    std::string file = "NSFE";
    auto chunk = [&](const char* id, const std::string& data) {
        const uint32_t size = (uint32_t)data.size();
        for (int i = 0; i < 4; ++i) file += (char)(size >> (i * 8));
        file += id; file += data;
    };
    std::string info = nsf.substr(8, 6);
    info += '\0'; info += nsf[0x7B]; info += (char)1;
    chunk("INFO", info); chunk("BANK", nsf.substr(0x70, 8));
    chunk("DATA", nsf.substr(128)); chunk("NEND", "");
    return file;
}

// End-to-end NSF/NSFE playback: the CPU writes reach each expansion chip, low/expansion RAM is
// writable and bankable, ROM-read PCM works, and restart/seek reproduce rendered samples exactly.
void TestNsfBus() {
    using testlog::Check;
    Program fds;
    fds.Write(0x4089, 0x80);
    for (int i = 0; i < 64; ++i) fds.Write((uint16_t)(0x4040 + i), (uint8_t)i);
    fds.Write(0x4089, 0); fds.Write(0x4080, 0xA0); fds.Write(0x4082, 0); fds.Write(0x4083, 4);
    const auto fdsFile = Nsf(fds, 4, 0x6000);
    auto audio = Play(fdsFile);
    Check(Finite(audio) && Rms(audio, 1000) > 0.01, L"FDS NSF loads and executes at $6000 with expansion sound");
    Check(Play(Nsfe(fdsFile), true) == audio, L"FDS NSFE follows the same chip and low-memory path");
    auto bad = fdsFile; bad[0x7B] = 0;
    Check(Play(bad).empty(), L"non-FDS files cannot load below $8000");
    bool memoryWritable = true;
    for (uint16_t addr : std::initializer_list<uint16_t>{0x6000,0x8000,0xDFFF}) {
        Program p;
        p.Write(addr, 0x5A); p.Copy(addr, 0x5011);
        // Put code at $E000 to leave the tested writable memory independent of the program.
        auto writable = Play(Nsf(p, 0x0C, 0xE000));
        Program reference; reference.Write(0x5011, 0x5A);
        auto expected = Play(Nsf(reference, 0x0C, 0xE000));
        // DC decays through the player's high-pass, but a successful write/read produces a transient.
        memoryWritable &= Rms(writable) > 0.001 && Rms(writable) > Rms(expected) * 0.9;
    }
    Check(memoryWritable, L"FDS program memory is writable at $6000, $8000 and $DFFF");
    Program mapper;
    mapper.Write(0x5205, 19); mapper.Write(0x5206, 13); // 247
    mapper.Copy(0x5205, 0x5C00); mapper.Copy(0x5C00, 0x5011);
    auto mapped = Play(Nsf(mapper, 8));
    Program reference; reference.Write(0x5011, 247);
    auto expected = Play(Nsf(reference, 8));
    Check(Finite(mapped) && Rms(mapped) > Rms(expected) * 0.95, L"MMC5 multiply and expansion RAM reach the PCM DAC");
    Program arithmetic;
    arithmetic.Write(0x5205, 255); arithmetic.Write(0x5206, 255);
    arithmetic.Require(0x5205, 1); arithmetic.Require(0x5206, 254);
    arithmetic.Write(0x5FF5, 87); arithmetic.Require(0x5FF5, 87);
    arithmetic.Write(0x5011, 128);
    Check(Rms(Play(Nsf(arithmetic, 8))) > 0.001, L"MMC5 product exposes both bytes and the last expansion-RAM address");

    bool bankedOk = true, bankedMatch = true;
    for (uint16_t bankRegister : std::initializer_list<uint16_t>{0x5FF6,0x5FF7,0x5FF8}) {
        Program banking;
        banking.Require(0x6000, 0x41); banking.Require(0x7000, 0x52);
        banking.Write(bankRegister, 1);
        const uint16_t address = (uint16_t)(0x6000 + (bankRegister - 0x5FF6) * 0x1000);
        banking.Require(address, 0x63);
        banking.Write(address, 0x74); banking.Require(address, 0x74);
        banking.Write(bankRegister, 1); banking.Require(address, 0x63); // a bank copy restores its original bytes
        banking.Write(0x5011, 128);
        auto file = Nsf(banking, 0x0C, 0xE000);
        const std::string code = file.substr(128);
        file.resize(128 + 0x7000, '\0');
        file[128 + 0x0000] = 0x41; file[128 + 0x1000] = 0x63;
        file[128 + 0x2000] = 0x52;
        file.replace(128 + 0x6000, code.size(), code);
        file[0x70 + 6] = 0; file[0x70 + 7] = 2; // initial $6000/$7000 mappings
        file[0x70 + 0] = 3; file[0x70 + 1] = 3; file[0x70 + 2] = 3;
        file[0x70 + 3] = 3; file[0x70 + 4] = 3; file[0x70 + 5] = 3;
        // Header slot 6 also initializes $E000; put code in bank 0 after its sentinel byte.
        file[10] = 1; file[11] = Byte(0xE0);
        const uint16_t play = (uint16_t)(0xE001 + banking.bytes.size());
        file[12] = (char)play; file[13] = (char)(play >> 8);
        file.replace(128 + 1, code.size(), code);
        const auto banked = Play(file);
        bankedOk &= Finite(banked) && Rms(banked) > 0.001;
        bankedMatch &= Play(Nsfe(file), true) == banked;
    }
    Check(bankedOk, L"FDS $5FF6/$5FF7/$5FF8 banking copies writable program pages");
    Check(bankedMatch, L"FDS NSFE bank mapping matches NSF for all three registers");

    Program pcmRead;
    pcmRead.Write(0x5010, 1);
    pcmRead.Copy(0xB000, 0x6000); // write PCM by reading a byte inside $8000-$BFFF
    auto pcmFile = Nsf(pcmRead, 8, 0xC000); // execute outside the read-PCM range
    // The unbanked NSF image begins at $8000 even when its LOAD is above that.
    // Put the read byte at $B000 by loading a contiguous program image from $8000.
    const std::string pcmCode = pcmFile.substr(128);
    pcmFile.resize(128 + 0x4000 + pcmCode.size(), '\0');
    pcmFile[8] = 0; pcmFile[9] = Byte(0x80);
    pcmFile[128 + 0x3000] = Byte(128);
    pcmFile.replace(128 + 0x4000, pcmCode.size(), pcmCode);
    Check(Rms(Play(pcmFile)) > 0.001, L"MMC5 NSF bus forwards ROM reads to the PCM channel");

    Program fm;
    auto reg = [&](uint8_t r, uint8_t v) { fm.Write(0x9010, r); fm.Write(0x9030, v); };
    reg(0x30, 0x40); reg(0x10, 0x20); reg(0x20, 0x19);
    const auto fmFile = Nsf(fm, 2);
    audio = Play(fmFile);
    Check(Finite(audio) && Rms(audio, 1000) > 0.001, L"VRC7 NSF routes the address/data ports");
    Program sunsoft;
    auto sun = [&](uint8_t r, uint8_t v) { sunsoft.Write(0xC000, r); sunsoft.Write(0xE000, v); };
    sun(6, 15); sun(7, 0x37); sun(8, 16); sun(11, 240); sun(12, 0); sun(13, 10);
    const auto sunFile = Nsf(sunsoft, 0x20);
    Check(Finite(Play(sunFile)) && Rms(Play(sunFile), 1000) > 0.01, L"5B NSF routes noise with envelope volume");
    const auto mmcFile = Nsf(mapper, 8);
    testlog::Cases seeks, nsfe;
    for (const auto& fixture : {std::pair{fdsFile, L"FDS"}, std::pair{fmFile, L"VRC7"},
                                std::pair{mmcFile, L"MMC5"}, std::pair{sunFile, L"5B"}}) {
        std::string error;
        auto emu = chiptune::OpenNsf(fixture.first, error);
        bool exact = false;
        if (emu && emu->Start(0)) {
            std::vector<float> whole(16000 * 2), after(5000 * 2), restart(16000 * 2);
            emu->Render(whole.data(), 16000);
            emu->Start(0); emu->Skip(11000); emu->Render(after.data(), 5000);
            emu->Start(0); emu->Render(restart.data(), 16000);
            exact = std::equal(after.begin(), after.end(), whole.begin() + 22000) && restart == whole;
        }
        seeks.Add(exact, fixture.second);
        nsfe.Add(Play(Nsfe(fixture.first), true) == Play(fixture.first), fixture.second);
    }
    Check(seeks.Passed(), L"FDS, VRC7, MMC5 and 5B NSF restart and seek reproduce rendered samples exactly%ls",
          seeks.failed.c_str());
    Check(nsfe.Passed(), L"FDS, VRC7, MMC5 and 5B NSFE match NSF playback%ls", nsfe.failed.c_str());
}
}  // namespace

void TestNesExpansion() {
    TestSunsoft();
    TestFds();
    TestMmc5();
    TestVrc7();
    TestNsfBus();
}
