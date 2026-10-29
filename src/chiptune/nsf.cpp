// NSF and NSFE: NES music rips. The file holds the game's sound code; the player calls its
// init routine once with the song number, then its play routine at the NES frame rate.
#include "band_buffer.h"
#include "chiptune.h"
#include "cpu6502.h"
#include "nes_apu.h"
#include "nes_expansion.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace chiptune {
namespace {

constexpr double kNtscClock = 1789773, kPalClock = 1662607;
constexpr uint16_t kIdle = 0x4100;  // routines "return" here; open bus, so nothing runs it
// Expansion chips a rip may need, matching the NSF/NSFE "chips" flag bits.
enum Chip : uint8_t { kVrc6 = 0x01, kVrc7 = 0x02, kFds = 0x04, kMmc5 = 0x08, kNamco = 0x10, kSunsoft = 0x20 };

class NsfEmu final : public Emulator, Cpu6502::Bus {
public:
    // Fields the loaders fill in.
    bool nsfe = false;
    std::string data;  // the program, as loaded at `load`
    uint16_t load = 0, init = 0, play = 0;
    uint8_t banks[8] = {};
    bool bankswitched = false, pal = false;
    uint8_t chips = 0;
    int songs = 1;
    uint32_t playMicros = 0;
    std::vector<uint8_t> playlist;
    std::string game, author, copyright, dumper;
    std::vector<std::string> titles;  // by song number
    std::vector<int> times;           // by song number, milliseconds or -1

    NsfEmu() : cpu_(*this) {}

    void Prepare() {
        // The ROM in 4 KB banks. Without bank switching the program sits at its load address;
        // with it, the load address only sets the offset inside the first bank.
        const size_t pad = bankswitched ? (load & 0xFFF) : (size_t)(load - ((chips & kFds) ? 0x6000 : 0x8000));
        rom_.assign(pad, '\0');
        rom_ += data;
        rom_.resize(std::max<size_t>(0x1000, (rom_.size() + 0xFFF) & ~(size_t)0xFFF), '\0');
        bankCount_ = rom_.size() / 0x1000;
        if (!bankswitched)
            for (int i = 0; i < 8; ++i) banks[i] = (uint8_t)i;
        const double clock = pal ? kPalClock : kNtscClock;
        playPeriod_ = playMicros * clock / 1e6;
        // The NES output stage high-passes at about 90 Hz. The gain matches the loudness this
        // player has always had.
        buf_.Setup(clock, kRate, 20000, BandBuffer::Input::Steps, 4096, 90, 2.78f);
    }

    Format Type() const override { return nsfe ? Format::NSFE : Format::NSF; }
    int TrackCount() const override { return playlist.empty() ? songs : (int)playlist.size(); }

    bool Info(int track, TrackInfo& out) const override {
        const int song = SongOf(track);
        if (song < 0) return false;
        out = TrackInfo();
        out.game = game;
        out.author = author;
        out.copyright = copyright;
        out.dumper = dumper;
        if (song < (int)titles.size()) out.song = titles[song];
        if (song < (int)times.size() && times[song] >= 0) out.lengthMs = times[song];
        return true;
    }

    bool Start(int track) override {
        const int song = SongOf(track);
        if (song < 0) return false;
        std::memset(ram_, 0, sizeof ram_);
        std::memset(sram_, 0, sizeof sram_);
        std::memset(exram_, 0, sizeof exram_);
        multiply_[0] = multiply_[1] = 0;
        std::memcpy(bank_, banks, sizeof bank_);
        for (uint8_t& b : bank_) b = (uint8_t)(b % bankCount_);
        buf_.Clear();
        apu_.Reset(pal, &buf_, [this](uint16_t a) { return Read(a); });
        vrc6_.Reset(&buf_);
        namco_.Reset(&buf_);
        sunsoft_.Reset(&buf_);
        fds_.Reset(&buf_);
        mmc5_.Reset(&buf_);
        vrc7_.Reset(pal, &buf_);
        if (chips & kFds) {
            fdsRam_.assign(0xA000, '\0');
            if (bankswitched) {
                for (int i = 0; i < 10; ++i) LoadFdsBank(i, banks[i < 2 ? i + 6 : i - 2]);
            } else {
                const size_t offset = load - 0x6000;
                std::memcpy(fdsRam_.data() + offset, data.data(), std::min(data.size(), fdsRam_.size() - offset));
            }
        }
        cpu_.Reset();
        // Clear every APU register, enable all channels, then use the 4-step frame counter.
        for (uint16_t a = 0x4000; a <= 0x4013; ++a) apu_.Write(0, a, 0);
        apu_.Write(0, 0x4015, 0x0F);
        apu_.Write(0, 0x4017, 0x40);
        // The init routine sees the song number in A and the region flag in X (0 NTSC, 1 PAL).
        cpu_.a = (uint8_t)song;
        cpu_.x = pal ? 1 : 0;
        cpu_.Call(init, kIdle);
        nextPlay_ = playPeriod_;
        return true;
    }

    void Render(float* out, size_t frames) override {
        std::fill(out, out + frames * 2, 0.0f);
        for (size_t done = 0; done < frames;) {
            if (buf_.Available() == 0) RunFrame();
            const size_t n = std::min(frames - done, buf_.Available());
            buf_.Read(out + done * 2, n, 2);
            for (size_t i = done; i < done + n; ++i) out[i * 2 + 1] = out[i * 2];
            done += n;
        }
    }

    void Skip(size_t frames) override {
        while (frames > 0) {
            if (buf_.Available() == 0) RunFrame();
            const size_t n = std::min(frames, buf_.Available());
            buf_.Skip(n);
            frames -= n;
        }
    }

    uint8_t Read(uint16_t addr) override {
        if (addr < 0x2000) return ram_[addr & 0x7FF];
        if (addr >= 0x8000) {
            const uint8_t v = (chips & kFds) ? (uint8_t)fdsRam_[addr - 0x6000]
                : (uint8_t)rom_[bank_[(addr - 0x8000) >> 12] * (size_t)0x1000 + (addr & 0xFFF)];
            if (chips & kMmc5) mmc5_.PcmRead(cpu_.time, addr, v);
            return v;
        }
        if ((chips & kFds) && addr >= 0x6000) return (uint8_t)fdsRam_[addr - 0x6000];
        if (addr >= 0x6000) return sram_[addr - 0x6000];
        if (addr == 0x4015) return apu_.ReadStatus(cpu_.time);
        if ((chips & kFds) && addr >= 0x4040 && addr <= 0x4097) return fds_.Read(cpu_.time, addr);
        if (chips & kMmc5) {
            if (addr == 0x5010 || addr == 0x5015) return mmc5_.Read(cpu_.time, addr);
            if (addr == 0x5205 || addr == 0x5206) return (uint8_t)((multiply_[0] * multiply_[1]) >> ((addr & 1) ? 0 : 8));
            if (addr >= 0x5C00 && addr <= 0x5FF5) return exram_[addr - 0x5C00];
        }
        if ((chips & kNamco) && addr >= 0x4800 && addr < 0x5000) return namco_.ReadData();
        return (uint8_t)(addr >> 8);  // open bus
    }

    void Write(uint16_t addr, uint8_t v, uint32_t t) override {
        if (addr < 0x2000) {
            ram_[addr & 0x7FF] = v;
        } else if (addr >= 0x4000 && addr <= 0x4017) {
            apu_.Write(t, addr, v);
        } else if ((chips & kFds) && addr >= 0x4023 && addr <= 0x408A) {
            fds_.Write(t, addr, v);
        } else if ((chips & kMmc5) && addr >= 0x5000 && addr <= 0x5015) {
            mmc5_.Write(t, addr, v);
        } else if ((chips & kMmc5) && (addr == 0x5205 || addr == 0x5206)) {
            multiply_[addr - 0x5205] = v;
        } else if ((chips & kMmc5) && addr >= 0x5C00 && addr <= 0x5FF5) {
            exram_[addr - 0x5C00] = v;
        } else if (addr >= 0x5FF6 && addr <= 0x5FFF) {
            if (bankswitched) {
                if (chips & kFds) LoadFdsBank(addr - 0x5FF6, v);
                else if (addr >= 0x5FF8) bank_[addr - 0x5FF8] = (uint8_t)(v % bankCount_);
            }
        } else if (addr >= 0x6000 && addr < 0x8000) {
            if (chips & kFds) fdsRam_[addr - 0x6000] = (char)v;
            else sram_[addr - 0x6000] = v;
        } else {
            if ((chips & kFds) && addr >= 0x8000 && addr < 0xE000) fdsRam_[addr - 0x6000] = (char)v;
            if ((chips & kNamco) && addr >= 0x4800 && addr < 0x5000) namco_.WriteData(t, v);
            if ((chips & kNamco) && addr >= 0xF800) namco_.WriteAddr(v);
            if ((chips & kVrc6) && addr >= 0x9000 && addr < 0xC000) vrc6_.Write(t, addr & 0xF003, v);
            if ((chips & kVrc7) && addr == 0x9010) vrc7_.WriteAddr(v);
            if ((chips & kVrc7) && addr == 0x9030) vrc7_.WriteData(t, v);
            if ((chips & kSunsoft) && addr >= 0xC000 && addr < 0xE000) sunsoft_.WriteAddr(v);
            if ((chips & kSunsoft) && addr >= 0xE000) sunsoft_.WriteData(t, v);
        }
    }

private:
    void LoadFdsBank(int slot, uint8_t bank) {
        // FDS NSF banking copies the selected 4 KB page into writable disk RAM.
        const size_t offset = (size_t)bank * 0x1000;
        char* destination = fdsRam_.data() + slot * 0x1000;
        if (offset < rom_.size()) std::memcpy(destination, rom_.data() + offset, 0x1000);
        else std::memset(destination, 0, 0x1000);
    }

    int SongOf(int track) const {
        if (track < 0 || track >= TrackCount()) return -1;
        return playlist.empty() ? track : playlist[track];
    }

    void RunFrame() {
        const uint32_t end = (uint32_t)std::clamp(std::ceil(nextPlay_), 1.0, (double)buf_.MaxFrame());
        cpu_.Run(end, kIdle);
        apu_.EndFrame(end);
        if (chips & kVrc6) vrc6_.EndFrame(end);
        if (chips & kNamco) namco_.EndFrame(end);
        if (chips & kSunsoft) sunsoft_.EndFrame(end);
        if (chips & kFds) fds_.EndFrame(end);
        if (chips & kMmc5) mmc5_.EndFrame(end);
        if (chips & kVrc7) vrc7_.EndFrame(end);
        buf_.EndFrame(end);
        cpu_.time -= end;
        nextPlay_ -= end;
        if (nextPlay_ <= 0) {
            nextPlay_ += playPeriod_;
            // A routine still running (init, or a slow play) misses this call.
            if (cpu_.pc == kIdle) {
                cpu_.s = 0xFD;
                cpu_.Call(play, kIdle);
            }
        }
    }

    Cpu6502 cpu_;
    NesApu apu_;
    Vrc6 vrc6_;
    Namco163 namco_;
    Sunsoft5b sunsoft_;
    FdsAudio fds_;
    Mmc5Audio mmc5_;
    Vrc7Audio vrc7_;
    BandBuffer buf_;
    std::string rom_;
    std::string fdsRam_;
    size_t bankCount_ = 1;
    uint8_t bank_[8] = {};
    uint8_t ram_[0x800] = {};
    uint8_t sram_[0x2000] = {};
    uint8_t exram_[0x400] = {}, multiply_[2] = {};
    double playPeriod_ = 29780, nextPlay_ = 0;
};

// Header text, with the conventional "<?>" for unknown left empty.
std::string NsfText(const unsigned char* p) {
    std::string s = FieldText(p, 32);
    return s == "<?>" ? std::string() : s;
}

bool Finish(NsfEmu& emu, std::string& error) {
    if (emu.data.empty()) {
        error = "the NSF has no program";
        return false;
    }
    if (emu.load < ((emu.chips & kFds) ? 0x6000 : 0x8000)) {
        error = "the NSF loads below its program memory";
        return false;
    }
    if (emu.songs < 1) {
        error = "the NSF has no songs";
        return false;
    }
    if (emu.data.size() > (1u << 20)) emu.data.resize(1u << 20);  // 256 banks is the most it can address
    for (uint8_t b : emu.banks) emu.bankswitched |= b != 0;
    // Default play period: one NTSC frame (~16639 us) or one PAL frame (~19997 us).
    if (!emu.playMicros) emu.playMicros = emu.pal ? 19997 : 16639;
    emu.Prepare();
    return true;
}

}  // namespace

std::unique_ptr<Emulator> OpenNsf(const std::string& bytes, std::string& error) {
    const auto* p = (const unsigned char*)bytes.data();
    if (bytes.size() < 0x80) {
        error = "the NSF header is cut short";
        return nullptr;
    }
    auto emu = std::make_unique<NsfEmu>();
    emu->songs = p[6];
    emu->load = (uint16_t)Le16(p + 8);
    emu->init = (uint16_t)Le16(p + 0x0A);
    emu->play = (uint16_t)Le16(p + 0x0C);
    emu->game = NsfText(p + 0x0E);
    emu->author = NsfText(p + 0x2E);
    emu->copyright = NsfText(p + 0x4E);
    std::memcpy(emu->banks, p + 0x70, 8);
    // PAL only if the file is PAL-only; dual-region files play at NTSC speed.
    emu->pal = (p[0x7A] & 3) == 1;
    emu->playMicros = Le16(p + (emu->pal ? 0x78 : 0x6E));
    emu->chips = p[0x7B];
    emu->data = bytes.substr(0x80);
    // NSF2 may give the program length, with metadata after it.
    const uint32_t programLength = p[0x7D] | p[0x7E] << 8 | p[0x7F] << 16;
    if (p[5] >= 2 && programLength && programLength < emu->data.size()) emu->data.resize(programLength);
    if (!Finish(*emu, error)) return nullptr;
    return emu;
}

std::unique_ptr<Emulator> OpenNsfe(const std::string& bytes, std::string& error) {
    const auto* p = (const unsigned char*)bytes.data();
    auto emu = std::make_unique<NsfEmu>();
    emu->nsfe = true;
    bool info = false, end = false;
    auto strings = [](const unsigned char* d, size_t n) {
        std::vector<std::string> out;
        size_t from = 0;
        for (size_t i = 0; i < n; ++i)
            if (d[i] == 0) {
                out.emplace_back((const char*)d + from, i - from);
                from = i + 1;
            }
        if (from < n) out.emplace_back((const char*)d + from, n - from);
        return out;
    };
    // Chunks: a 32-bit size, a four-letter id, then the data. Ids starting with a capital
    // letter must be understood; others may be skipped.
    for (size_t at = 4; at + 8 <= bytes.size() && !end;) {
        const uint32_t size = Le32(p + at);
        const std::string id = bytes.substr(at + 4, 4);
        at += 8;
        if (size > bytes.size() - at) {
            error = "an NSFE chunk is cut short";
            return nullptr;
        }
        const unsigned char* d = p + at;
        if (id == "INFO") {
            if (size < 9) {
                error = "the NSFE INFO chunk is too short";
                return nullptr;
            }
            emu->load = (uint16_t)Le16(d);
            emu->init = (uint16_t)Le16(d + 2);
            emu->play = (uint16_t)Le16(d + 4);
            emu->pal = (d[6] & 3) == 1;
            emu->chips = d[7];
            emu->songs = d[8];
            info = true;
        } else if (id == "DATA") {
            emu->data.assign((const char*)d, size);
        } else if (id == "BANK") {
            std::memcpy(emu->banks, d, std::min<size_t>(size, 8));
        } else if (id == "RATE") {
            if (size >= 2 && !emu->pal) emu->playMicros = Le16(d);
            if (size >= 4 && emu->pal) emu->playMicros = Le16(d + 2);
        } else if (id == "plst") {
            emu->playlist.assign(d, d + size);
        } else if (id == "time") {
            for (size_t i = 0; i + 4 <= size; i += 4) emu->times.push_back((int32_t)Le32(d + i));
        } else if (id == "tlbl") {
            emu->titles = strings(d, size);
        } else if (id == "auth") {
            std::vector<std::string> a = strings(d, size);
            a.resize(4);
            emu->game = a[0] == "<?>" ? "" : a[0];
            emu->author = a[1] == "<?>" ? "" : a[1];
            emu->copyright = a[2] == "<?>" ? "" : a[2];
            emu->dumper = a[3] == "<?>" ? "" : a[3];
        } else if (id == "NEND") {
            end = true;
        } else if (id[0] >= 'A' && id[0] <= 'Z') {
            error = "the NSFE uses a feature this player doesn't support (" + id + ")";
            return nullptr;
        }
        at += size;
    }
    if (!info) {
        error = "the NSFE has no INFO chunk";
        return nullptr;
    }
    // Playlist entries must name real songs.
    emu->playlist.erase(std::remove_if(emu->playlist.begin(), emu->playlist.end(), [&](uint8_t s) { return s >= emu->songs; }),
                        emu->playlist.end());
    if (!Finish(*emu, error)) return nullptr;
    return emu;
}

}  // namespace chiptune
