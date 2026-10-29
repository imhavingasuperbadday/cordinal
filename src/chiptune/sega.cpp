// VGM/VGZ and GYM: logs of register writes to the SN76489 PSG and the YM2612 FM chip, with
// waits in between. VGM counts time in 44100 Hz samples, GYM in 60 Hz frames. Both the PSG
// and the YM2612 are Cordinal implementations.
#include "band_buffer.h"
#include "chiptune.h"
#include "ym2612.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace chiptune {
namespace {

constexpr uint32_t kNtscPsgClock = 3579545, kNtscFmClock = 7670453;

// SN76489: three square waves and a noise channel, each with a 4-bit attenuation in 2 dB
// steps. Its counters run at clock/16; times here are in those ticks.
class Psg {
public:
    void Reset(unsigned feedback, int width, float gain) {
        feedback_ = feedback ? feedback : 0x0009;
        width_ = width >= 2 && width <= 16 ? width : 16;
        for (int v = 0; v < 15; ++v) volume_[v] = gain * (float)std::pow(10.0, -0.1 * v);
        volume_[15] = 0;
        std::fill(std::begin(period_), std::end(period_), uint16_t(0));
        for (int c = 0; c < 4; ++c) {
            next_[c] = 0;
            atten_[c] = 15;
            high_[c] = false;
            lastL_[c] = lastR_[c] = 0;
        }
        noiseMode_ = 0;
        noiseToggle_ = false;
        lfsr_ = 1u << (width_ - 1);
        latch_ = 0;
        pan_ = 0xFF;
        time_ = 0;
    }

    void Write(uint32_t t, uint8_t v) {
        Run(t);
        if (v & 0x80) latch_ = (v >> 4) & 7;
        const int c = latch_ >> 1;
        if (latch_ & 1) {
            atten_[c] = v & 15;
        } else if (c < 3) {
            period_[c] = (v & 0x80) ? (uint16_t)((period_[c] & 0x3F0) | (v & 15)) : (uint16_t)((period_[c] & 15) | (v & 0x3F) << 4);
        } else {
            noiseMode_ = v & 7;
            lfsr_ = 1u << (width_ - 1);
        }
        Output(c, t);
    }

    // Game Gear stereo: bits 4-7 put channels 0-3 on the left, bits 0-3 on the right.
    void Stereo(uint32_t t, uint8_t v) {
        Run(t);
        pan_ = v;
        for (int c = 0; c < 4; ++c) Output(c, t);
    }

    void EndFrame(uint32_t t) {
        Run(t);
        for (int c = 0; c < 4; ++c) next_[c] -= std::min(next_[c], t);
        time_ = 0;
    }

    BandBuffer *left = nullptr, *right = nullptr;

private:
    uint16_t NoisePeriod() const {
        return (noiseMode_ & 3) == 3 ? std::max<uint16_t>(period_[2], 1) : (uint16_t)(0x10 << (noiseMode_ & 3));
    }

    void Run(uint32_t t) {
        for (int c = 0; c < 3; ++c) {
            // A period of 0 or 1 holds the output high: games use that to play samples
            // through the volume register.
            if (period_[c] <= 1) {
                if (!high_[c]) {
                    high_[c] = true;
                    Output(c, time_);
                }
                next_[c] = t;
                continue;
            }
            while (next_[c] < t) {
                high_[c] = !high_[c];
                Output(c, next_[c]);
                next_[c] += period_[c];
            }
        }
        // The shift register steps on every second edge of its own square wave.
        const uint16_t np = NoisePeriod();
        while (next_[3] < t) {
            noiseToggle_ = !noiseToggle_;
            if (noiseToggle_) {
                const unsigned in = (noiseMode_ & 4) ? Parity(lfsr_ & feedback_) : (lfsr_ & 1);
                lfsr_ = (lfsr_ >> 1) | (in << (width_ - 1));
                high_[3] = lfsr_ & 1;
                Output(3, next_[3]);
            }
            next_[3] += np;
        }
        time_ = t;
    }

    static unsigned Parity(unsigned v) {
        v ^= v >> 8;
        v ^= v >> 4;
        v ^= v >> 2;
        v ^= v >> 1;
        return v & 1;
    }

    void Output(int c, uint32_t t) {
        const float level = high_[c] ? volume_[atten_[c]] : 0.0f;
        const float l = (pan_ >> (c + 4)) & 1 ? level : 0.0f, r = (pan_ >> c) & 1 ? level : 0.0f;
        if (l != lastL_[c]) left->Add(t, l - lastL_[c]), lastL_[c] = l;
        if (r != lastR_[c]) right->Add(t, r - lastR_[c]), lastR_[c] = r;
    }

    unsigned feedback_ = 9;
    int width_ = 16;
    float volume_[16] = {};
    uint16_t period_[3] = {};
    uint32_t next_[4] = {};
    uint8_t atten_[4] = {};
    bool high_[4] = {};
    float lastL_[4] = {}, lastR_[4] = {};
    int noiseMode_ = 0;
    bool noiseToggle_ = false;
    unsigned lfsr_ = 0x8000;
    int latch_ = 0;
    uint8_t pan_ = 0xFF;
    uint32_t time_ = 0;
};

// Converts a count of 44100 Hz samples into another clock, carrying the fraction between
// frames so that long songs don't drift.
struct ClockRatio {
    uint64_t step = 0, frac = 0;
    void Setup(double rate) { step = (uint64_t)std::llround(rate / kRate * 4294967296.0); }
    uint32_t At(uint32_t samples) const { return (uint32_t)((frac + samples * step) >> 32); }
    uint32_t End(uint32_t samples) {
        const uint64_t total = frac + samples * step;
        frac = total & 0xFFFFFFFFu;
        return (uint32_t)(total >> 32);
    }
};

// GD3 tag text is little-endian UTF-16; the metadata this player keeps is UTF-8.
std::string Utf16ToUtf8(const unsigned char* p, size_t units) {
    std::string out;
    for (size_t i = 0; i < units; ++i) {
        uint32_t c = Le16(p + i * 2);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) {
            const uint32_t lo = Le16(p + (i + 1) * 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80) {
            out += (char)c;
        } else if (c < 0x800) {
            out += (char)(0xC0 | c >> 6);
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | c >> 12);
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | c >> 18);
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Plays one VGM or GYM by interpreting its command stream; the PSG and FM buffers are mixed at
// the output rate. Commands are position-indexed so the loop point can be jumped to.
class SegaEmu final : public Emulator {
public:
    // Fields the loaders fill in.
    bool gym = false;
    std::string data;  // the whole file
    size_t start = 0, end = 0, loop = 0;  // command positions; loop 0 = no loop
    uint32_t psgClock = 0, fmClock = 0;
    unsigned psgFeedback = 9;
    int psgWidth = 16;
    TrackInfo info;

    Format Type() const override { return gym ? Format::GYM : Format::VGM; }
    int TrackCount() const override { return 1; }
    bool Info(int track, TrackInfo& out) const override {
        if (track != 0) return false;
        out = info;
        return true;
    }

    bool Start(int track) override {
        if (track != 0) return false;
        if (!setUp_) SetUp();
        for (BandBuffer* b : {&psgL_, &psgR_, &fmL_, &fmR_}) b->Clear();
        psg_.Reset(psgFeedback, psgWidth, 0.168f);
        if (fm_) {
            fm_->Reset();
        }
        psgTime_ = {psgTime_.step, 0};
        fmTime_ = {fmTime_.step, 0};
        fmDone_ = 0;
        pos_ = start;
        wait_ = 0;
        pcm_.clear();
        pcmPos_ = 0;
        ended_ = false;
        waitedSinceLoop_ = true;
        return true;
    }

    void Render(float* out, size_t frames) override {
        std::fill(out, out + frames * 2, 0.0f);
        while (frames > 0) {
            if (Available() == 0) RunFrame((uint32_t)std::min<size_t>(frames, 1024));
            const size_t n = std::min(frames, Available());
            if (psgClock) psgL_.Read(out, n, 2), psgR_.Read(out + 1, n, 2);
            if (fm_) fmL_.Read(out, n, 2), fmR_.Read(out + 1, n, 2);
            out += n * 2;
            frames -= n;
        }
    }

    void Skip(size_t frames) override {
        while (frames > 0) {
            if (Available() == 0) RunFrame((uint32_t)std::min<size_t>(frames, 1024));
            const size_t n = std::min(frames, Available());
            for (BandBuffer* b : {&psgL_, &psgR_, &fmL_, &fmR_}) b->Skip(n);
            frames -= n;
        }
    }

    bool Ended() const override { return ended_; }

private:
    // The PSG's counters tick at clock/16 and the YM2612 produces one sample per 144 clocks.
    void SetUp() {
        setUp_ = true;
        const double fmRate = fmClock / 144.0;
        psgTime_.Setup(psgClock / 16.0);
        fmTime_.Setup(fmRate);
        // Frames are at most 1024 samples, so the buffers need a little more than that.
        psgL_.Setup(psgClock / 16.0, kRate, 20000, BandBuffer::Input::Steps, 1100);
        psgR_.Setup(psgClock / 16.0, kRate, 20000, BandBuffer::Input::Steps, 1100);
        const double cut = 0.9 * std::min(fmRate, (double)kRate) / 2;
        fmL_.Setup(fmRate, kRate, cut, BandBuffer::Input::Samples, 1100);
        fmR_.Setup(fmRate, kRate, cut, BandBuffer::Input::Samples, 1100);
        psg_.left = &psgL_;
        psg_.right = &psgR_;
        if (fmClock) fm_ = std::make_unique<Ym2612>();
    }

    size_t Available() const {
        size_t n = SIZE_MAX;
        if (psgClock) n = std::min({n, psgL_.Available(), psgR_.Available()});
        if (fm_) n = std::min({n, fmL_.Available(), fmR_.Available()});
        return n == SIZE_MAX ? 0 : n;
    }

    void FmWrite(int port, uint8_t reg, uint8_t value) {
        fm_->Write((unsigned)port,reg,value);
    }

    // Generates FM samples up to `samples` into the frame.
    void FmRun(uint32_t samples) {
        const uint32_t until = fmTime_.At(samples);
        for (; fmDone_ < until; ++fmDone_) {
            float left,right;
            fm_->Sample(left,right);
            fmL_.Add(fmDone_,left);
            fmR_.Add(fmDone_,right);
        }
    }

    void FmAt(uint32_t s, int port, uint8_t reg, uint8_t value) {
        if (!fm_) return;
        FmRun(s);
        FmWrite(port, reg, value);
    }

    void PsgWrite(uint32_t s, uint8_t v, bool stereo) {
        if (!psgClock) return;
        if (stereo) psg_.Stereo(psgTime_.At(s), v);
        else psg_.Write(psgTime_.At(s), v);
    }

    void RunFrame(uint32_t samples) {
        uint32_t s = 0;
        int budget = 1 << 20;  // commands without a wait; a damaged file could loop forever
        while (s < samples) {
            if (wait_) {
                const uint32_t n = std::min(wait_, samples - s);
                s += n;
                wait_ -= n;
                waitedSinceLoop_ = true;
                continue;
            }
            if (ended_ || --budget < 0) {
                ended_ = true;
                break;
            }
            if (gym) GymCommand(s);
            else VgmCommand(s);
        }
        if (psgClock) {
            const uint32_t ticks = psgTime_.End(samples);
            psg_.EndFrame(ticks);
            psgL_.EndFrame(ticks);
            psgR_.EndFrame(ticks);
        }
        if (fm_) {
            FmRun(samples);
            const uint32_t n = fmTime_.End(samples);
            fmL_.EndFrame(n);
            fmR_.EndFrame(n);
            fmDone_ = 0;
        }
    }

    // The end of the data: go back to the loop point, or stop.
    void AtEnd() {
        if (loop && waitedSinceLoop_) {
            pos_ = loop;
            waitedSinceLoop_ = false;
        } else {
            ended_ = true;
        }
    }

    bool Have(size_t n) const { return pos_ + n <= end; }
    uint8_t At(size_t i) const { return (uint8_t)data[pos_ + i]; }

    void VgmCommand(uint32_t s) {
        if (!Have(1)) return AtEnd();
        const uint8_t op = At(0);
        const unsigned char* p = (const unsigned char*)data.data() + pos_;
        size_t len;
        switch (op) {
            case 0x4F: len = 2; if (Have(len)) PsgWrite(s, p[1], true); break;
            case 0x50: len = 2; if (Have(len)) PsgWrite(s, p[1], false); break;
            case 0x52:
            case 0x53: len = 3; if (Have(len)) FmAt(s, op & 1, p[1], p[2]); break;
            case 0x61: len = 3; if (Have(len)) wait_ = Le16(p + 1); break;
            case 0x62: len = 1; wait_ = 735; break;
            case 0x63: len = 1; wait_ = 882; break;
            case 0x66: return AtEnd();
            case 0x67: {  // data block: 0x67 0x66 type size32 bytes
                if (!Have(7) || p[1] != 0x66) return AtEnd();
                const uint32_t size = Le32(p + 3) & 0x7FFFFFFF;
                if (end - pos_ - 7 < size) return AtEnd();
                if (p[2] == 0x00) pcm_.append((const char*)p + 7, size);  // YM2612 PCM
                len = 7 + (size_t)size;
                break;
            }
            case 0xE0: len = 5; if (Have(len)) pcmPos_ = Le32(p + 1); break;
            default:
                if (op >= 0x70 && op <= 0x7F) {
                    len = 1;
                    wait_ = (op & 15) + 1;
                } else if (op >= 0x80 && op <= 0x8F) {  // a DAC byte from the PCM bank, then a wait
                    len = 1;
                    if (pcmPos_ < pcm_.size()) FmAt(s, 0, 0x2A, (uint8_t)pcm_[pcmPos_++]);
                    wait_ = op & 15;
                } else {
                    len = OtherLength(op);  // other chips' commands are skipped
                    if (len == 0) return AtEnd();
                }
        }
        if (!Have(len)) return AtEnd();
        pos_ += len;
    }

    static size_t OtherLength(uint8_t op) {
        if (op >= 0x30 && op <= 0x3F) return 2;
        if (op >= 0x40 && op <= 0x4E) return 3;
        if (op >= 0x51 && op <= 0x5F) return 3;
        if (op == 0x68) return 12;
        static const uint8_t kStream[6] = {5, 5, 6, 11, 2, 5};  // 0x90-0x95: DAC stream control
        if (op >= 0x90 && op <= 0x95) return kStream[op - 0x90];
        if (op >= 0xA0 && op <= 0xBF) return 3;
        if (op >= 0xC0 && op <= 0xDF) return 4;
        if (op >= 0xE1) return 5;
        return 0;
    }

    // Runs one GYM command. The stream is a fixed 60 Hz frame log: opcode 0 advances one frame.
    void GymCommand(uint32_t s) {
        if (!Have(1)) return AtEnd();
        const uint8_t op = At(0);
        switch (op) {
            case 0: wait_ = kRate / 60; pos_ += 1; return;
            case 1:
            case 2:
                if (!Have(3)) return AtEnd();
                FmAt(s, op - 1, At(1), At(2));
                pos_ += 3;
                return;
            case 3:
                if (!Have(2)) return AtEnd();
                PsgWrite(s, At(1), false);
                pos_ += 2;
                return;
            default: return AtEnd();
        }
    }

    bool setUp_ = false;
    Psg psg_;
    std::unique_ptr<Ym2612> fm_;
    BandBuffer psgL_, psgR_, fmL_, fmR_;
    ClockRatio psgTime_, fmTime_;
    uint32_t fmDone_ = 0;
    size_t pos_ = 0;
    uint32_t wait_ = 0;
    std::string pcm_;
    size_t pcmPos_ = 0;
    bool ended_ = false, waitedSinceLoop_ = true;
};

}  // namespace

std::unique_ptr<Emulator> OpenVgm(const std::string& bytes, std::string& error) {
    const auto* p = (const unsigned char*)bytes.data();
    const size_t size = bytes.size();
    if (size < 0x40) {
        error = "the VGM header is cut short";
        return nullptr;
    }
    auto emu = std::make_unique<SegaEmu>();
    const uint32_t version = Le32(p + 8);
    emu->end = std::min<size_t>(size, (size_t)Le32(p + 4) + 4);
    emu->psgClock = Le32(p + 0x0C) & 0x3FFFFFFF;
    emu->fmClock = (version >= 0x110 ? Le32(p + 0x2C) : Le32(p + 0x10)) & 0x3FFFFFFF;
    if (emu->psgClock && (emu->psgClock < 100000 || emu->psgClock > 20000000)) emu->psgClock = 0;
    if (emu->fmClock && (emu->fmClock < 1000000 || emu->fmClock > 20000000)) emu->fmClock = 0;
    if (!emu->psgClock && !emu->fmClock) {
        error = "it uses no sound chip this player emulates";
        return nullptr;
    }
    if (version >= 0x110) {
        emu->psgFeedback = Le16(p + 0x28);
        emu->psgWidth = p[0x2A];
    }
    const uint32_t dataOffset = version >= 0x150 ? Le32(p + 0x34) : 0;
    emu->start = dataOffset ? 0x34 + (size_t)dataOffset : 0x40;
    if (emu->start >= emu->end) {
        error = "the VGM has no data";
        return nullptr;
    }
    const uint32_t loopOffset = Le32(p + 0x1C);
    if (loopOffset && 0x1C + (size_t)loopOffset >= emu->start && 0x1C + (size_t)loopOffset < emu->end)
        emu->loop = 0x1C + (size_t)loopOffset;

    TrackInfo& info = emu->info;
    const uint32_t total = Le32(p + 0x18), loopSamples = Le32(p + 0x20);
    if (emu->loop && loopSamples > 0 && loopSamples <= total) {
        info.introMs = (int)((uint64_t)(total - loopSamples) * 10 / 441);
        info.loopMs = (int)((uint64_t)loopSamples * 10 / 441);
    } else if (total > 0) {
        info.lengthMs = (int)((uint64_t)total * 10 / 441);
    }

    // GD3 tags: "Gd3 ", version, size, then eleven zero-terminated UTF-16 strings.
    const uint32_t gd3 = Le32(p + 0x14);
    if (gd3 && 0x14 + (size_t)gd3 + 12 <= size && std::memcmp(p + 0x14 + gd3, "Gd3 ", 4) == 0) {
        const size_t at = 0x14 + (size_t)gd3 + 12;
        const size_t len = std::min<size_t>(Le32(p + at - 4), size - at) / 2;
        std::vector<std::string> fields;
        size_t from = 0;
        for (size_t i = 0; i < len && fields.size() < 11; ++i) {
            if (Le16(p + at + i * 2) == 0) {
                fields.push_back(Utf16ToUtf8(p + at + from * 2, i - from));
                from = i + 1;
            }
        }
        fields.resize(11);
        auto pick = [&](int en, int jp) { return fields[en].empty() ? fields[jp] : fields[en]; };
        info.song = pick(0, 1);
        info.game = pick(2, 3);
        info.author = pick(6, 7);
        info.copyright = fields[8];
        info.dumper = fields[9];
    }
    emu->data = bytes;
    return emu;
}

std::unique_ptr<Emulator> OpenGym(const std::string& bytes, std::string& error) {
    const auto* p = (const unsigned char*)bytes.data();
    constexpr size_t kHeader = 428;
    if (bytes.size() < kHeader) {
        error = "the GYM header is cut short";
        return nullptr;
    }
    if (Le32(p + 424) != 0) {
        error = "packed GYM files aren't supported";
        return nullptr;
    }
    auto emu = std::make_unique<SegaEmu>();
    emu->gym = true;
    emu->psgClock = kNtscPsgClock;
    emu->fmClock = kNtscFmClock;
    emu->start = kHeader;
    emu->end = bytes.size();

    // Count the frames, and find where the loop starts: after `loopFrame` frames.
    const uint32_t loopFrame = Le32(p + 420);
    size_t frames = 0;
    for (size_t i = kHeader; i < bytes.size();) {
        const uint8_t op = p[i];
        if (op == 0) {
            ++frames;
            ++i;
            if (loopFrame && frames == loopFrame) emu->loop = i;
        } else if (op == 1 || op == 2) {
            i += 3;
        } else if (op == 3) {
            i += 2;
        } else {
            emu->end = i;
            break;
        }
    }
    TrackInfo& info = emu->info;
    info.song = FieldText(p + 4, 32);
    info.game = FieldText(p + 36, 32);
    info.copyright = FieldText(p + 68, 32);
    info.dumper = FieldText(p + 132, 32);
    const int lengthMs = (int)(frames * 50 / 3);
    if (emu->loop) {
        info.introMs = (int)(loopFrame * 50 / 3);
        info.loopMs = lengthMs - info.introMs;
    } else {
        info.lengthMs = lengthMs;
    }
    emu->data = bytes;
    return emu;
}

}  // namespace chiptune
