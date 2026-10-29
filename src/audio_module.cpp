// Tracker modules: ProTracker MOD (and its 15-sample, multichannel and FastTracker variants),
// Scream Tracker 3 S3M, FastTracker 2 XM and Impulse Tracker IT, including IT's compressed samples,
// instruments, envelopes and new-note actions. Each loader translates its format into one song
// model (patterns of cells, samples, instruments) whose effects are named, not lettered; a single
// sequencer and mixer play them. Written from the formats' own documentation (ProTracker's and
// FastTracker's notes, ST3's TECH.DOC, ITTECH.TXT). A song plays once: it ends where it would
// start repeating, and its length is found by running the sequencer silently.
#include "audio_file.h"
#include "library.h"
#include "native_audio.h"
#include "tags.h"

#include <array>
#include <cctype>
#include <cmath>
#include <string_view>
#include <vector>

namespace {

constexpr unsigned kRate = 44100;
constexpr int kChannels = 64, kBackground = 64;
constexpr uint8_t kNoteOff = 255, kNoteCut = 254, kNoteFade = 253;  // cell notes 1..120 are notes 0..119
constexpr int kBase = 60;            // the note at which a sample plays at its own rate
constexpr double kAmigaBase = 1712;  // period (×4 Amiga units) of the base note at 8363 Hz

enum class Kind : uint8_t { MOD, S3M, XM, IT };

// Effects by meaning. Raw IT-style parameters (VolSlideIT, PortaUpIT...) carry fine and extra-fine
// variants in their high nibble, as S3M and IT write them.
enum Fx : uint8_t {
    None, Arpeggio, PortaUp, PortaDown, PortaUpIT, PortaDownIT, FinePortaUp, FinePortaDown, XFinePortaUp,
    XFinePortaDown, TonePorta, TonePortaVol, Vibrato, FineVibrato, VibratoVol, Tremolo, SetPan, Offset,
    VolSlide, VolSlideIT, FineVolUp, FineVolDown, Jump, Break, SetVol, Speed, Tempo, SpeedTempo, PatternLoop,
    PatternDelay, NoteCut, NoteDelay, Retrig, MultiRetrig, Tremor, GlobalVol, GlobalVolSlide, KeyOff, EnvPos,
    PanSlide, PanSlideIT, ChanVol, ChanVolSlide, Panbrello, VibWave, TremWave, PanbrelloWave, FinePatDelay,
    PastNotes, SetNna, HighOffset, SetFinetune, FinePanLeft, FinePanRight,
};
// The volume column, by meaning.
enum Vc : uint8_t { VcNone, VcVol, VcUp, VcDown, VcFineUp, VcFineDown, VcVibSpeed, VcVibDepth, VcPan,
                    VcPanLeft, VcPanRight, VcPorta, VcPortaDown, VcPortaUp };

struct Cell {
    uint8_t note = 0, instr = 0, vc = VcNone, vcParam = 0, fx = None, param = 0;
};

struct Pattern {
    int rows = 64;
    std::vector<Cell> cells;  // rows × song channels
};

struct Sample {
    std::vector<int16_t> data;  // mono, plus one guard sample
    uint32_t length = 0, loopStart = 0, loopEnd = 0, susStart = 0, susEnd = 0;
    uint8_t loop = 0, susLoop = 0;  // 0 none, 1 forward, 2 ping-pong
    int volume = 64, globalVol = 64, pan = -1;  // pan 0..256, -1 none
    double rate = 8363;                          // Hz at the base note
    uint8_t vibType = 0, vibSweep = 0, vibDepth = 0, vibRate = 0;
};

struct Envelope {
    struct Point { int tick, value; };
    std::vector<Point> points;
    bool on = false, loop = false, sus = false;
    int loopStart = 0, loopEnd = 0, susStart = 0, susEnd = 0;  // point indices
};

struct Instrument {
    std::array<uint8_t, 120> sample{}, note{};  // sample number (1-based, 0 none) and played note per note
    Envelope vol, pan, pitch;
    int fadeout = 0, globalVol = 128, defaultPan = -1, nna = 0;  // nna: 0 cut, 1 continue, 2 off, 3 fade
    uint8_t vibType = 0, vibSweep = 0, vibDepth = 0, vibRate = 0;  // XM's per-instrument auto-vibrato
};

struct Song {
    Kind kind = Kind::MOD;
    std::string name, tracker;
    int channels = 4;
    std::vector<uint8_t> orders;  // 0xFE skip, 0xFF end
    std::vector<Pattern> patterns;
    std::vector<Sample> samples;
    std::vector<Instrument> instruments;
    bool instrumentMode = false, linear = false, compatGxx = false;
    int speed = 6, tempo = 125, globalVol = 128;  // global 0..128
    std::array<int, kChannels> pan{}, chanVol{};
    double clock = 14317056;  // ×4 Amiga period to Hz: NTSC for S3M/XM/IT, PAL for MOD
    double gain = 1;
};

// Bounds-checked little-endian reads over the whole file.
struct Bytes {
    std::string_view s;
    size_t size() const { return s.size(); }
    uint8_t u8(size_t at) const { return at < s.size() ? (uint8_t)s[at] : 0; }
    uint32_t u16(size_t at) const { return u8(at) | (uint32_t)u8(at + 1) << 8; }
    uint32_t u32(size_t at) const { return u16(at) | u16(at + 2) << 16; }
    uint32_t b16(size_t at) const { return (uint32_t)u8(at) << 8 | u8(at + 1); }
    bool is(size_t at, std::string_view tag) const { return at <= s.size() && s.substr(at, tag.size()) == tag; }
    // A fixed-size name field, as Latin-1, trimmed, in UTF-8.
    std::string text(size_t at, size_t n) const {
        std::wstring t;
        for (size_t i = 0; i < n && u8(at + i); ++i) t += u8(at + i) < 32 ? L' ' : (wchar_t)u8(at + i);
        while (!t.empty() && t.back() == L' ') t.pop_back();
        return ToUtf8(t);
    }
};

// Sample data into 16-bit mono. `bits` 8 or 16; signed or unsigned; delta-coded (XM) or plain;
// stereo halves (left then right) are averaged.
void LoadPcm(const Bytes& b, size_t at, Sample& s, int bits, bool isSigned, bool delta, bool stereo) {
    const size_t width = bits / 8, n = s.length;
    s.data.assign(n + 1, 0);
    for (int half = 0; half < (stereo ? 2 : 1); ++half) {
        int acc = 0;
        for (size_t i = 0; i < n; ++i) {
            const size_t p = at + (half * n + i) * width;
            if (p + width > b.size()) break;
            int v = bits == 16 ? (int)b.u16(p) : (int)b.u8(p);
            if (delta) v = acc = (acc + v) & (bits == 16 ? 0xFFFF : 0xFF);
            if (bits == 16) v = isSigned ? (int16_t)v : v - 32768;
            else v = (isSigned ? (int8_t)v : v - 128) * 256;
            s.data[i] = (int16_t)(stereo ? (half ? (s.data[i] + v) / 2 : v) : v);
        }
    }
}

// IT 2.14/2.15 compressed samples: blocks of 0x8000 samples, each a bit stream whose code width
// changes in-band; values are deltas (2.15: deltas of deltas).
void LoadItCompressed(const Bytes& b, size_t at, Sample& s, bool sixteen, bool it215, bool stereo) {
    const size_t n = s.length;
    s.data.assign(n + 1, 0);
    for (int half = 0; half < (stereo ? 2 : 1); ++half) {
        for (size_t done = 0; done < n;) {
            const size_t blockBytes = b.u16(at);
            at += 2;
            const size_t blockEnd = std::min(at + blockBytes, b.size());
            size_t bitPos = at * 8;
            auto bits = [&](int count) {
                uint32_t v = 0;
                for (int i = 0; i < count; ++i, ++bitPos)
                    if (bitPos / 8 < blockEnd) v |= (uint32_t)((b.u8(bitPos / 8) >> (bitPos % 8)) & 1) << i;
                return v;
            };
            const size_t count = std::min<size_t>(0x8000, n - done);
            const int top = sixteen ? 17 : 9;
            int width = top, d1 = 0, d2 = 0;
            for (size_t i = 0; i < count;) {
                if (bitPos / 8 >= blockEnd) break;
                uint32_t v = bits(width);
                if (width < 7) {
                    if (v == 1u << (width - 1)) {
                        const int w = (int)bits(sixteen ? 4 : 3) + 1;
                        width = w < width ? w : w + 1;
                        continue;
                    }
                } else if (width < top) {
                    const uint32_t border = ((sixteen ? 0xFFFFu : 0xFFu) >> (top - width)) - (sixteen ? 8 : 4);
                    if (v > border && v <= border + (sixteen ? 16 : 8)) {
                        const int w = (int)(v - border);
                        width = w < width ? w : w + 1;
                        continue;
                    }
                } else if (width == top) {
                    if (v & (1u << (top - 1))) {
                        width = (int)((v + 1) & 0xFF);
                        continue;
                    }
                } else {
                    break;
                }
                const int full = sixteen ? 16 : 8;
                int value = width < full ? (int)(v << (32 - width)) >> (32 - width) : (sixteen ? (int16_t)v : (int8_t)v);
                d1 += value;
                d2 += d1;
                int out = it215 ? d2 : d1;
                out = sixteen ? (int16_t)out : (int8_t)out * 256;
                const size_t k = done + i;
                s.data[k] = (int16_t)(stereo && half ? (s.data[k] + out) / 2 : out);
                ++i;
            }
            done += count;
            at += blockBytes;
        }
    }
}

void FixLoops(Sample& s) {
    s.loopEnd = std::min(s.loopEnd, s.length);
    s.susEnd = std::min(s.susEnd, s.length);
    if (s.loopStart + 1 >= s.loopEnd) s.loop = 0;
    if (s.susStart + 1 >= s.susEnd) s.susLoop = 0;
    if (s.data.size() < s.length + 1) s.data.resize(s.length + 1, 0);
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---------------------------------------------------------------------------
// Loaders

// The ProTracker family's effects, shared by MOD and XM (whose 0-F are the same).
void ProtrackerFx(Cell& c, int fx, int p, bool xm) {
    const int x = p >> 4, y = p & 15;
    c.param = (uint8_t)p;
    switch (fx) {
        case 0: c.fx = p ? Arpeggio : None; break;
        case 1: c.fx = p || xm ? PortaUp : None; break;
        case 2: c.fx = p || xm ? PortaDown : None; break;
        case 3: c.fx = TonePorta; break;
        case 4: c.fx = Vibrato; break;
        case 5: c.fx = TonePortaVol; c.param = (uint8_t)(x ? x << 4 : y); break;
        case 6: c.fx = VibratoVol; c.param = (uint8_t)(x ? x << 4 : y); break;
        case 7: c.fx = Tremolo; break;
        case 8: c.fx = SetPan; break;
        case 9: c.fx = Offset; break;
        case 0xA: c.fx = p || xm ? VolSlide : None; c.param = (uint8_t)(x ? x << 4 : y); break;
        case 0xB: c.fx = Jump; break;
        case 0xC: c.fx = SetVol; break;
        case 0xD: c.fx = Break; c.param = (uint8_t)(x * 10 + y); break;
        case 0xF: c.fx = xm ? (p < 32 ? Speed : Tempo) : SpeedTempo; break;
        case 0xE:
            c.param = (uint8_t)y;
            switch (x) {
                case 1: c.fx = FinePortaUp; break;
                case 2: c.fx = FinePortaDown; break;
                case 4: c.fx = VibWave; break;
                case 5: c.fx = SetFinetune; break;
                case 6: c.fx = PatternLoop; break;
                case 7: c.fx = TremWave; break;
                case 8: c.fx = SetPan; c.param = (uint8_t)(y * 17); break;
                case 9: c.fx = Retrig; break;
                case 0xA: c.fx = FineVolUp; break;
                case 0xB: c.fx = FineVolDown; break;
                case 0xC: c.fx = NoteCut; break;
                case 0xD: c.fx = NoteDelay; break;
                case 0xE: c.fx = PatternDelay; break;
                default: c.fx = None; break;
            }
            break;
        default: c.fx = None; break;
    }
}

bool LoadMod(const Bytes& b, Song& song, bool sure) {
    if (b.size() < 600) return false;
    const std::string magic(b.size() >= 1084 ? b.s.substr(1080, 4) : std::string_view());
    int channels = 0;
    if (magic == "M.K." || magic == "M!K!" || magic == "FLT4" || magic == "4CHN" || magic == "M&K!" || magic == "N.T.") channels = 4;
    else if (magic == "FLT8" || magic == "OKTA" || magic == "CD81") channels = 8;
    else if (magic.size() == 4 && magic[0] >= '1' && magic[0] <= '9' && magic.substr(1) == "CHN") channels = magic[0] - '0';
    else if (magic.size() == 4 && isdigit((unsigned char)magic[0]) && isdigit((unsigned char)magic[1]) &&
             (magic.substr(2) == "CH" || magic.substr(2) == "CN"))
        channels = (magic[0] - '0') * 10 + magic[1] - '0';
    else if (magic.size() == 4 && magic.substr(0, 3) == "TDZ" && magic[3] >= '1' && magic[3] <= '9') channels = magic[3] - '0';
    const bool old = channels == 0;  // Ultimate Soundtracker: 15 samples, no tag
    if (old) {  // nothing marks these but sane headers
        if (!sure || b.u8(470) > 128) return false;
        for (int i = 0; i < 15; ++i)
            if (b.u8(20 + i * 30 + 25) > 64 || (b.u8(20 + i * 30 + 24) & 0xF0)) return false;
        channels = 4;
    }
    if (channels < 1 || channels > 32) return false;
    const int sampleCount = old ? 15 : 31;
    const size_t orderAt = 20 + sampleCount * 30, patternAt = orderAt + 130 + (old ? 0 : 4);
    song.kind = Kind::MOD;
    song.name = b.text(0, 20);
    song.tracker = channels == 4 ? "ProTracker module" : std::to_string(channels) + "-channel MOD";
    song.channels = channels;
    song.clock = 14187580;  // PAL
    const int length = Clamp(b.u8(orderAt), 1, 128);
    int patterns = 0;
    for (int i = 0; i < 128; ++i) patterns = std::max(patterns, b.u8(orderAt + 2 + i) + 1);
    for (int i = 0; i < length; ++i) song.orders.push_back(b.u8(orderAt + 2 + i));
    if (patterns > 128) return false;
    size_t at = patternAt;
    for (int p = 0; p < patterns; ++p) {
        Pattern pat;
        pat.cells.resize(64 * channels);
        for (int i = 0; i < 64 * channels; ++i, at += 4) {
            const uint8_t* q = (const uint8_t*)b.s.data() + std::min(at, b.size());
            if (at + 4 > b.size()) break;
            Cell& c = pat.cells[i];
            const int period = (q[0] & 15) << 8 | q[1];
            c.instr = (uint8_t)((q[0] & 0xF0) | q[2] >> 4);
            if (period) c.note = (uint8_t)(Clamp((int)std::lround(12 * std::log2(kAmigaBase / (period * 4.0))) + kBase, 0, 119) + 1);
            ProtrackerFx(c, q[2] & 15, q[3], false);
        }
        song.patterns.push_back(std::move(pat));
    }
    if (at > b.size() + 64 * 4 * (size_t)channels) return false;  // the patterns aren't all there
    for (int i = 0; i < sampleCount; ++i) {
        const size_t h = 20 + i * 30;
        Sample s;
        s.length = b.b16(h + 22) * 2;
        const int ft = b.u8(h + 24) & 15;
        s.rate = 8363 * std::pow(2.0, (ft < 8 ? ft : ft - 16) / 96.0);
        s.volume = std::min(64, (int)b.u8(h + 25));
        s.loopStart = b.b16(h + 26) * (old ? 1 : 2);
        const uint32_t loopLength = b.b16(h + 28) * 2;
        s.loopEnd = s.loopStart + loopLength;
        s.loop = loopLength > 2 ? 1 : 0;
        LoadPcm(b, at, s, 8, true, false, false);
        at += s.length;
        FixLoops(s);
        song.samples.push_back(std::move(s));
    }
    for (int c = 0; c < channels; ++c) {
        song.pan[c] = (c & 3) == 0 || (c & 3) == 3 ? 64 : 192;  // Amiga LRRL, softened to half separation
        song.chanVol[c] = 64;
    }
    song.gain = 0.5;
    return true;
}

// S3M's and IT's lettered effects (A = 1), IT's being a superset. S3M writes its break row in BCD,
// its global volume out of 64 and its pan out of 128.
void LetterFx(Cell& c, int cmd, int arg, bool it) {
    static const uint8_t kLetters[26] = {
        Speed, Jump, Break, VolSlideIT, PortaDownIT, PortaUpIT, TonePorta, Vibrato, Tremor, Arpeggio, VibratoVol,
        TonePortaVol, ChanVol, ChanVolSlide, Offset, PanSlideIT, MultiRetrig, Tremolo, None, Tempo, FineVibrato,
        GlobalVol, GlobalVolSlide, SetPan, Panbrello, None};
    static const uint8_t kSpecial[16] = {None, None, SetFinetune, VibWave, TremWave, PanbrelloWave, FinePatDelay,
                                         SetNna, SetPan, None, HighOffset, PatternLoop, NoteCut, NoteDelay,
                                         PatternDelay, None};
    const int x = arg >> 4, y = arg & 15;
    c.fx = cmd >= 1 && cmd <= 26 ? kLetters[cmd - 1] : None;
    c.param = (uint8_t)arg;
    if (cmd == 'S' - '@') {
        c.fx = kSpecial[x];
        c.param = (uint8_t)(x == 8 ? y * 17 : y);
        if (x == 7) c.fx = y <= 2 ? PastNotes : y <= 6 ? SetNna : None;
    }
    if (it) return;
    if (c.fx == Break) c.param = (uint8_t)(x * 10 + y);
    else if (c.fx == GlobalVol) c.param = (uint8_t)std::min(128, arg * 2);
    else if (c.fx == SetPan && cmd != 'S' - '@') {
        c.param = (uint8_t)std::min(255, arg * 2);
        if (arg > 0x80) c.fx = None;  // 0xA4 is surround
    }
}

bool LoadS3m(const Bytes& b, Song& song) {
    if (b.size() < 0x60 || !b.is(0x2C, "SCRM")) return false;
    song.kind = Kind::S3M;
    song.name = b.text(0, 28);
    song.tracker = "Scream Tracker 3 module";
    const int orders = b.u16(0x20), instruments = b.u16(0x22), patterns = b.u16(0x24);
    const bool unsignedSamples = b.u16(0x2A) != 1;
    song.globalVol = std::min(64, (int)b.u8(0x30)) * 2;
    song.speed = b.u8(0x31) ? b.u8(0x31) : 6;
    song.tempo = b.u8(0x32) >= 32 ? b.u8(0x32) : 125;
    const bool stereo = (b.u8(0x33) & 0x80) != 0;
    song.gain = std::max(16, b.u8(0x33) & 0x7F) / 96.0;
    // Channels 0-7 are left, 8-15 right; Adlib and unused channels are dropped.
    std::array<int, 32> map{};
    int used = 0;
    for (int c = 0; c < 32; ++c) {
        const int setting = b.u8(0x40 + c);
        map[c] = setting < 16 ? used++ : -1;
        if (map[c] >= 0) song.pan[map[c]] = !stereo ? 128 : setting < 8 ? 0x30 : 0xD0;
    }
    if (!used) return false;
    song.channels = used;
    size_t at = 0x60;
    for (int i = 0; i < orders; ++i) song.orders.push_back(b.u8(at + i));
    at += orders;
    const size_t instrAt = at, patAt = at + instruments * 2;
    if (b.u8(0x35) == 252)
        for (int c = 0; c < 32; ++c) {
            const int p = b.u8(patAt + patterns * 2 + c);
            if (map[c] >= 0 && (p & 0x20)) song.pan[map[c]] = (p & 15) * 256 / 15;
        }
    for (int c = 0; c < used; ++c) song.chanVol[c] = 64;
    for (int i = 0; i < instruments; ++i) {
        const size_t h = (size_t)b.u16(instrAt + i * 2) * 16;
        Sample s;
        if (b.u8(h) == 1 && b.is(h + 0x4C, "SCRS")) {
            const size_t data = ((size_t)b.u8(h + 0x0D) << 16 | b.u16(h + 0x0E)) * 16;
            const int flags = b.u8(h + 0x1F);
            s.length = std::min<uint32_t>(b.u32(h + 0x10), 1u << 24);
            s.loopStart = b.u32(h + 0x14);
            s.loopEnd = b.u32(h + 0x18);
            s.loop = flags & 1;
            s.volume = std::min(64, (int)b.u8(h + 0x1C));
            s.rate = b.u32(h + 0x20) ? b.u32(h + 0x20) : 8363;
            LoadPcm(b, data, s, flags & 4 ? 16 : 8, !unsignedSamples, false, (flags & 2) != 0);
        }
        FixLoops(s);
        song.samples.push_back(std::move(s));
    }
    for (int p = 0; p < patterns; ++p) {
        Pattern pat;
        pat.cells.resize(64 * used);
        size_t q = (size_t)b.u16(patAt + p * 2) * 16;
        if (q) {
            const size_t end = q + b.u16(q);
            q += 2;
            for (int row = 0; row < 64 && q < end && q < b.size();) {
                const int what = b.u8(q++);
                if (!what) {
                    ++row;
                    continue;
                }
                Cell scratch, &c = map[what & 31] >= 0 ? pat.cells[row * used + map[what & 31]] : scratch;
                if (what & 32) {
                    const int note = b.u8(q), instr = b.u8(q + 1);
                    q += 2;
                    if (note == 254) c.note = kNoteCut;
                    else if (note < 0x80) c.note = (uint8_t)(Clamp((note >> 4) * 12 + (note & 15) + 12, 0, 119) + 1);
                    c.instr = (uint8_t)instr;
                }
                if (what & 64) {
                    c.vc = VcVol;
                    c.vcParam = (uint8_t)std::min<int>(64, b.u8(q++));
                }
                if (what & 128) {
                    LetterFx(c, b.u8(q), b.u8(q + 1), false);
                    q += 2;
                }
            }
        }
        song.patterns.push_back(std::move(pat));
    }
    return true;
}

void XmVolumeColumn(Cell& c, int v) {
    const int y = v & 15;
    if (v >= 0x10 && v <= 0x50) { c.vc = VcVol; c.vcParam = (uint8_t)(v - 0x10); return; }
    c.vcParam = (uint8_t)y;
    switch (v >> 4) {
        case 6: c.vc = VcDown; break;
        case 7: c.vc = VcUp; break;
        case 8: c.vc = VcFineDown; break;
        case 9: c.vc = VcFineUp; break;
        case 0xA: c.vc = VcVibSpeed; break;
        case 0xB: c.vc = VcVibDepth; break;
        case 0xC: c.vc = VcPan; c.vcParam = (uint8_t)(y * 17); break;
        case 0xD: c.vc = VcPanLeft; break;
        case 0xE: c.vc = VcPanRight; break;
        case 0xF: c.vc = VcPorta; c.vcParam = (uint8_t)(y * 16); break;
        default: c.vc = VcNone; break;
    }
}

// XM and IT envelope points into one model. XM points are (tick, 0..64) word pairs.
void XmEnvelope(const Bytes& b, size_t at, int count, int sus, int loopStart, int loopEnd, int type, Envelope& e) {
    count = std::min(count, 12);
    for (int i = 0; i < count; ++i) e.points.push_back({(int)b.u16(at + i * 4), std::min(64, (int)b.u16(at + i * 4 + 2))});
    e.on = (type & 1) && count > 0;
    e.sus = (type & 2) != 0;
    e.loop = (type & 4) != 0;
    e.susStart = e.susEnd = std::min(sus, std::max(count - 1, 0));
    e.loopStart = std::min(loopStart, std::max(count - 1, 0));
    e.loopEnd = std::min(loopEnd, std::max(count - 1, 0));
}

bool LoadXm(const Bytes& b, Song& song) {
    if (b.size() < 80 || !b.is(0, "Extended Module: ")) return false;
    song.kind = Kind::XM;
    song.name = b.text(17, 20);
    song.tracker = "FastTracker 2 module";
    const size_t header = 60 + b.u32(60);
    const int length = std::min<int>(b.u16(64), 256), channels = b.u16(68), patterns = b.u16(70), instruments = b.u16(72);
    if (channels < 1 || channels > kChannels || patterns > 256 || instruments > 128) return false;
    song.channels = channels;
    song.linear = b.u16(74) & 1;
    song.speed = b.u16(76) ? std::min<int>(b.u16(76), 31) : 6;
    song.tempo = b.u16(78) >= 32 ? std::min<int>(b.u16(78), 255) : 125;
    for (int i = 0; i < length; ++i) song.orders.push_back(b.u8(80 + i));
    for (int c = 0; c < channels; ++c) {
        song.pan[c] = 128;
        song.chanVol[c] = 64;
    }
    size_t at = header;
    for (int p = 0; p < patterns; ++p) {
        const size_t len = b.u32(at);
        const int rows = Clamp((int)b.u16(at + 5), 1, 256);
        const size_t packed = b.u16(at + 7);
        Pattern pat;
        pat.rows = rows;
        pat.cells.resize(rows * channels);
        size_t q = at + len;
        const size_t end = std::min(q + packed, b.size());
        for (int i = 0; packed && i < rows * channels && q < end; ++i) {
            int flags = b.u8(q);
            if (flags & 0x80) ++q;
            else flags = 0x1F;
            Cell& c = pat.cells[i];
            int note = 0, fx = 0, param = 0;
            if (flags & 1) note = b.u8(q++);
            if (flags & 2) c.instr = b.u8(q++);
            if (flags & 4) XmVolumeColumn(c, b.u8(q++));
            if (flags & 8) fx = b.u8(q++);
            if (flags & 16) param = b.u8(q++);
            if (note == 97) c.note = kNoteOff;
            else if (note >= 1 && note <= 96) c.note = (uint8_t)(note - 1 + 12 + 1);
            const int x = param >> 4, y = param & 15;
            if (fx <= 0xF) {
                ProtrackerFx(c, fx, param, true);
            } else {
                c.param = (uint8_t)param;
                switch (fx) {
                    case 'G' - 55: c.fx = GlobalVol; c.param = (uint8_t)std::min(128, param * 2); break;
                    case 'H' - 55: c.fx = GlobalVolSlide; c.param = (uint8_t)(x ? x << 4 : y); break;
                    case 'K' - 55: c.fx = KeyOff; break;
                    case 'L' - 55: c.fx = EnvPos; break;
                    case 'P' - 55: c.fx = PanSlide; c.param = (uint8_t)(x ? x << 4 : y); break;
                    case 'R' - 55: c.fx = MultiRetrig; break;
                    case 'T' - 55: c.fx = Tremor; break;
                    case 'X' - 55:
                        c.fx = x == 1 ? XFinePortaUp : x == 2 ? XFinePortaDown : None;
                        c.param = (uint8_t)y;
                        break;
                    default: c.fx = None; break;
                }
            }
        }
        song.patterns.push_back(std::move(pat));
        at += len + packed;
    }
    // Instruments, each followed by its sample headers and then their delta-coded data.
    for (int i = 0; i < instruments && at + 29 <= b.size(); ++i) {
        const size_t size = b.u32(at);
        const int count = b.u16(at + 27);
        Instrument ins;
        ins.nna = 0;
        std::vector<Sample> local;
        if (count > 0 && count <= 256) {
            const size_t sampleHeader = b.u32(at + 29);
            for (int n = 0; n < 96; ++n) {
                const int k = b.u8(at + 33 + n);
                ins.sample[n + 12] = (uint8_t)(k < count && song.samples.size() + k < 255 ? song.samples.size() + k + 1 : 0);
                ins.note[n + 12] = (uint8_t)(n + 12);
            }
            XmEnvelope(b, at + 129, b.u8(at + 225), b.u8(at + 227), b.u8(at + 228), b.u8(at + 229), b.u8(at + 233), ins.vol);
            XmEnvelope(b, at + 177, b.u8(at + 226), b.u8(at + 230), b.u8(at + 231), b.u8(at + 232), b.u8(at + 234), ins.pan);
            for (auto& p : ins.pan.points) p.value = p.value * 4 - 128;  // 0..64 around the centre, as -128..128
            ins.vibType = b.u8(at + 235);
            ins.vibSweep = b.u8(at + 236);
            ins.vibDepth = b.u8(at + 237);
            ins.vibRate = b.u8(at + 238);
            ins.fadeout = b.u16(at + 239);
            size_t h = at + size;
            std::vector<int> types;
            for (int n = 0; n < count; ++n, h += sampleHeader) {
                Sample s;
                const int type = b.u8(h + 14);
                const bool sixteen = (type & 16) != 0;
                s.length = b.u32(h) >> (sixteen ? 1 : 0);
                s.loopStart = b.u32(h + 4) >> (sixteen ? 1 : 0);
                s.loopEnd = s.loopStart + (b.u32(h + 8) >> (sixteen ? 1 : 0));
                s.loop = (type & 3) == 1 ? 1 : (type & 3) == 2 ? 2 : 0;
                s.volume = std::min(64, (int)b.u8(h + 12));
                s.pan = b.u8(h + 15);
                s.rate = 8363 * std::pow(2.0, ((int8_t)b.u8(h + 16) * 128 + (int8_t)b.u8(h + 13)) / 1536.0);
                types.push_back(type | (b.u8(h + 17) == 0xAD ? 0x100 : 0));
                local.push_back(std::move(s));
            }
            for (int n = 0; n < count; ++n) {
                Sample& s = local[n];
                const bool sixteen = (types[n] & 16) != 0;
                if (!(types[n] & 0x100)) LoadPcm(b, h, s, sixteen ? 16 : 8, true, true, false);
                h += (size_t)s.length * (sixteen ? 2 : 1);
                FixLoops(s);
                s.vibType = ins.vibType;
                s.vibSweep = ins.vibSweep;
                s.vibDepth = ins.vibDepth;
                s.vibRate = ins.vibRate;
            }
            at = h;
        } else {
            at += size;
        }
        for (auto& s : local) song.samples.push_back(std::move(s));
        song.instruments.push_back(std::move(ins));
    }
    song.instrumentMode = true;
    song.gain = 0.5;
    return true;
}

void ItEnvelope(const Bytes& b, size_t at, Envelope& e, bool pitch) {
    const int flags = b.u8(at), count = std::min<int>(b.u8(at + 1), 25);
    e.on = (flags & 1) && count > 0 && !(pitch && (flags & 0x80));  // a filter envelope isn't a pitch one
    e.loop = (flags & 2) != 0;
    e.sus = (flags & 4) != 0;
    const int last = std::max(count - 1, 0);
    e.loopStart = std::min<int>(b.u8(at + 2), last);
    e.loopEnd = std::min<int>(b.u8(at + 3), last);
    e.susStart = std::min<int>(b.u8(at + 4), last);
    e.susEnd = std::min<int>(b.u8(at + 5), last);
    for (int i = 0; i < count; ++i) e.points.push_back({(int)b.u16(at + 7 + i * 3), (int8_t)b.u8(at + 6 + i * 3)});
}

bool LoadIt(const Bytes& b, Song& song) {
    if (b.size() < 0xC0 || !b.is(0, "IMPM")) return false;
    song.kind = Kind::IT;
    song.name = b.text(4, 26);
    song.tracker = "Impulse Tracker module";
    const int orders = b.u16(0x20), instruments = b.u16(0x22), samples = b.u16(0x24), patterns = b.u16(0x26);
    const int cmwt = b.u16(0x2A), flags = b.u16(0x2C);
    song.instrumentMode = (flags & 4) && cmwt >= 0x200;
    song.linear = (flags & 8) != 0;
    song.compatGxx = (flags & 32) != 0;
    song.globalVol = std::min(128, (int)b.u8(0x30));
    song.gain = std::max(16, (int)b.u8(0x31)) / 96.0;
    song.speed = b.u8(0x32) ? b.u8(0x32) : 6;
    song.tempo = b.u8(0x33) >= 32 ? b.u8(0x33) : 125;
    for (int c = 0; c < 64; ++c) {
        const int p = b.u8(0x40 + c);
        song.pan[c] = !(flags & 1) ? 128 : p == 100 ? 128 : std::min(64, p & 127) * 4;
        song.chanVol[c] = std::min(64, (int)b.u8(0x80 + c));
    }
    size_t at = 0xC0;
    for (int i = 0; i < orders; ++i) song.orders.push_back(b.u8(at + i));
    at += orders;
    const size_t insAt = at, smpAt = insAt + instruments * 4, patAt = smpAt + samples * 4;
    for (int i = 0; i < samples; ++i) {
        const size_t h = b.u32(smpAt + i * 4);
        Sample s;
        if (b.is(h, "IMPS")) {
            const int f = b.u8(h + 0x12), cvt = b.u8(h + 0x2E);
            s.globalVol = std::min(64, (int)b.u8(h + 0x11));
            s.volume = std::min(64, (int)b.u8(h + 0x13));
            s.pan = b.u8(h + 0x2F) & 0x80 ? std::min(64, b.u8(h + 0x2F) & 127) * 4 : -1;
            s.length = std::min<uint32_t>(b.u32(h + 0x30), 1u << 26);
            s.loopStart = b.u32(h + 0x34);
            s.loopEnd = b.u32(h + 0x38);
            s.rate = b.u32(h + 0x3C) ? b.u32(h + 0x3C) : 8363;
            s.susStart = b.u32(h + 0x40);
            s.susEnd = b.u32(h + 0x44);
            s.loop = f & 16 ? (f & 64 ? 2 : 1) : 0;
            s.susLoop = f & 32 ? (f & 128 ? 2 : 1) : 0;
            s.vibRate = b.u8(h + 0x4C);
            s.vibDepth = b.u8(h + 0x4D);
            s.vibSweep = b.u8(h + 0x4E);
            s.vibType = b.u8(h + 0x4F);
            const size_t data = b.u32(h + 0x48);
            if (f & 1) {
                if (f & 8) LoadItCompressed(b, data, s, (f & 2) != 0, (cvt & 4) != 0, (f & 4) != 0);
                else LoadPcm(b, data, s, f & 2 ? 16 : 8, (cvt & 1) != 0, (cvt & 4) != 0, (f & 4) != 0);
            } else {
                s.length = 0;
            }
        }
        FixLoops(s);
        song.samples.push_back(std::move(s));
    }
    if (song.instrumentMode)
        for (int i = 0; i < instruments; ++i) {
            const size_t h = b.u32(insAt + i * 4);
            Instrument ins;
            if (b.is(h, "IMPI")) {
                ins.nna = b.u8(h + 0x11) & 3;
                ins.fadeout = b.u16(h + 0x14);
                ins.globalVol = std::min(128, (int)b.u8(h + 0x18));
                ins.defaultPan = b.u8(h + 0x19) & 0x80 ? -1 : std::min(64, (int)b.u8(h + 0x19)) * 4;
                for (int n = 0; n < 120; ++n) {
                    ins.note[n] = (uint8_t)std::min<int>(119, b.u8(h + 0x40 + n * 2));
                    ins.sample[n] = b.u8(h + 0x41 + n * 2);
                }
                ItEnvelope(b, h + 0x130, ins.vol, false);
                ItEnvelope(b, h + 0x182, ins.pan, false);
                ItEnvelope(b, h + 0x1D4, ins.pitch, true);
                for (auto& p : ins.pan.points) p.value *= 4;  // -32..32 as -128..128
            }
            song.instruments.push_back(std::move(ins));
        }
    song.channels = 0;
    for (int p = 0; p < patterns; ++p) {
        const size_t h = b.u32(patAt + p * 4);
        Pattern pat;
        pat.rows = h ? Clamp((int)b.u16(h + 2), 1, 200) : 64;
        pat.cells.resize(pat.rows * kChannels);
        if (h) {
            size_t q = h + 8;
            const size_t end = std::min(q + b.u16(h), b.size());
            std::array<uint8_t, kChannels> mask{};
            std::array<Cell, kChannels> last{};
            for (int row = 0; row < pat.rows && q < end;) {
                const int cv = b.u8(q++);
                if (!cv) {
                    ++row;
                    continue;
                }
                const int ch = (cv - 1) & 63;
                if (cv & 128) mask[ch] = b.u8(q++);
                const int m = mask[ch];
                Cell& c = pat.cells[row * kChannels + ch];
                Cell& l = last[ch];
                if (m & 1) {
                    const int n = b.u8(q++);
                    l.note = n < 120 ? (uint8_t)(n + 1) : n == 255 ? kNoteOff : n == 254 ? kNoteCut : kNoteFade;
                }
                if (m & 2) l.instr = b.u8(q++);
                if (m & 4) {
                    const int v = b.u8(q++);
                    l.vc = VcNone;
                    if (v <= 64) { l.vc = VcVol; l.vcParam = (uint8_t)v; }
                    else if (v <= 74) { l.vc = VcFineUp; l.vcParam = (uint8_t)(v - 65); }
                    else if (v <= 84) { l.vc = VcFineDown; l.vcParam = (uint8_t)(v - 75); }
                    else if (v <= 94) { l.vc = VcUp; l.vcParam = (uint8_t)(v - 85); }
                    else if (v <= 104) { l.vc = VcDown; l.vcParam = (uint8_t)(v - 95); }
                    else if (v <= 114) { l.vc = VcPortaDown; l.vcParam = (uint8_t)((v - 105) * 4); }
                    else if (v <= 124) { l.vc = VcPortaUp; l.vcParam = (uint8_t)((v - 115) * 4); }
                    else if (v >= 128 && v <= 192) { l.vc = VcPan; l.vcParam = (uint8_t)std::min(255, (v - 128) * 4); }
                    else if (v >= 193 && v <= 202) {
                        static const uint8_t kPorta[10] = {0, 1, 4, 8, 16, 32, 64, 96, 128, 255};
                        l.vc = VcPorta;
                        l.vcParam = kPorta[v - 193];
                    } else if (v >= 203 && v <= 212) { l.vc = VcVibDepth; l.vcParam = (uint8_t)(v - 203); }
                }
                if (m & 8) {
                    LetterFx(l, b.u8(q), b.u8(q + 1), true);
                    q += 2;
                }
                if (m & (1 | 16)) c.note = l.note;
                if (m & (2 | 32)) c.instr = l.instr;
                if (m & (4 | 64)) { c.vc = l.vc; c.vcParam = l.vcParam; }
                if (m & (8 | 128)) { c.fx = l.fx; c.param = l.param; }
                if ((m & (1 | 2 | 4 | 8 | 16 | 32 | 64 | 128)) && ch + 1 > song.channels) song.channels = ch + 1;
            }
        }
        song.patterns.push_back(std::move(pat));
    }
    // Patterns were read 64 channels wide; keep only the channels the song uses.
    song.channels = std::max(song.channels, 1);
    for (auto& pat : song.patterns) {
        std::vector<Cell> cells(pat.rows * song.channels);
        for (int r = 0; r < pat.rows; ++r)
            for (int c = 0; c < song.channels; ++c) cells[r * song.channels + c] = pat.cells[r * kChannels + c];
        pat.cells = std::move(cells);
    }
    return true;
}

bool LoadSong(const std::string& bytes, const std::wstring& ext, Song& song) {
    const Bytes b{bytes};
    if (b.is(0, "IMPM")) return LoadIt(b, song);
    if (b.is(0, "Extended Module: ")) return LoadXm(b, song);
    if (b.is(0x2C, "SCRM")) return LoadS3m(b, song);
    return LoadMod(b, song, ext == L"mod");
}

// ---------------------------------------------------------------------------
// Playback

// Vibrato, tremolo and panbrello waveforms, -255..255 over 64 steps.
int Wave(int type, int pos) {
    pos &= 63;
    switch (type & 3) {
        case 1: return 255 - pos * 8;  // ramp down
        case 2: return pos < 32 ? 255 : -255;
        case 3: return (int)(((unsigned)pos * 1103515245u + 12345u) >> 16 & 511) - 255;  // random, but the same each time
        default: return (int)std::lround(255 * std::sin(pos * 3.14159265358979 / 32));
    }
}

struct Voice {
    const Sample* s = nullptr;
    const Instrument* ins = nullptr;
    bool active = false, back = false, keyOff = false, fading = false;
    uint64_t pos = 0, step = 0;           // 32.32 sample frames
    float left = 0, right = 0;             // gains the ramp is heading for
    float curLeft = 0, curRight = 0;       // gains now
    int volEnv = 0, panEnv = 0, pitchEnv = 0;  // envelope ticks
    double fade = 1;
    int autoVibPos = 0, autoVibSweep = 0;
    // A voice left playing by a new-note action keeps the last level, pan and pitch its channel gave it.
    double base = 0, freq = 0;
    int pan = 128;
};

struct Channel {
    int note = -1, instr = 0, sample = -1;
    double period = 0, target = 0;  // ×4 Amiga period, or 1/64 semitones in linear mode
    int vol = 64, chanVol = 64, pan = 128;
    int volShift = 0, panShift = 0;  // this tick's tremolo and panbrello
    double pitchShift = 0;          // this tick's vibrato, in period or pitch units
    int arp = 0;                    // this tick's arpeggio, semitones
    int nna = 0;
    Cell cell;
    int delayTick = -1, cutTick = -1, keyOffTick = -1;
    // Effect memories.
    uint8_t memPorta = 0, memPortaUp = 0, memPortaDown = 0, memTone = 0, memVol = 0, memFineUp = 0, memFineDown = 0;
    uint8_t memXUp = 0, memXDown = 0, memVib = 0, memTrem = 0, memOffset = 0, memRetrig = 0, memTremor = 0;
    uint8_t memArp = 0, memGlobal = 0, memPan = 0, memChanVol = 0, memPanbrello = 0, memTempo = 0;
    int vibPos = 0, vibSpeed = 0, vibDepth = 0, vibWave = 0;
    int tremPos = 0, tremSpeed = 0, tremDepth = 0, tremWave = 0;
    int panbPos = 0, panbSpeed = 0, panbDepth = 0, panbWave = 0;
    int tremorCount = 0, retrigCount = 0, highOffset = 0;
    bool tremorOff = false;
    int loopRow = 0, loopCount = 0;
};

class ModulePlayer {
public:
    // Plays `song`, which must outlive the player.
    explicit ModulePlayer(const Song& song) : song_(song) { Restart(); }

    const Song& Info() const { return song_; }
    bool Ended() const { return ended_; }

    void Restart() {
        for (auto& c : ch_) c = Channel{};
        for (int c = 0; c < kChannels; ++c) {
            ch_[c].pan = song_.pan[c];
            ch_[c].chanVol = song_.chanVol[c];
        }
        for (auto& v : voices_) v = Voice{};
        speed_ = song_.speed;
        tempo_ = song_.tempo;
        globalVol_ = song_.globalVol;
        order_ = 0;
        row_ = 0;
        tick_ = 0;
        patDelay_ = 0;
        fineDelay_ = 0;
        ended_ = false;
        visited_.assign(song_.orders.size(), std::vector<bool>(256, false));
        jumpOrder_ = breakRow_ = -1;
        loopJump_ = false;
        left_ = 0;
        if (!SkipOrders()) ended_ = true;
    }

    // Mixes `frames` frames into out (stereo, overwritten). Returns frames produced; fewer at the end.
    size_t Render(float* out, size_t frames, bool mix) {
        size_t done = 0;
        while (done < frames && !ended_) {
            if (left_ == 0) {
                Tick();
                if (ended_) break;
                left_ = (size_t)(kRate * 5 / 2 / std::max(tempo_, 1));  // 2.5 / BPM seconds per tick
            }
            const size_t n = std::min(frames - done, left_);
            if (mix) {
                std::fill(out + done * 2, out + (done + n) * 2, 0.0f);
                for (auto& v : voices_)
                    if (v.active) Mix(v, out + done * 2, n);
            } else {
                for (auto& v : voices_)
                    if (v.active) Advance(v, n);
            }
            done += n;
            left_ -= n;
        }
        return done;
    }

    // Runs the sequencer alone to the song's end: its length in frames.
    uint64_t Length() {
        uint64_t total = 0;
        for (int guard = 0; guard < 2000000 && !ended_; ++guard) {
            Tick();
            if (ended_) break;
            total += (uint64_t)(kRate * 5 / 2 / std::max(tempo_, 1));
        }
        return total;
    }

private:
    bool SkipOrders() {
        while (order_ < (int)song_.orders.size() && song_.orders[order_] == 0xFE) ++order_;
        return order_ < (int)song_.orders.size() && song_.orders[order_] != 0xFF &&
               song_.orders[order_] < song_.patterns.size();
    }
    const Pattern& Current() const { return song_.patterns[song_.orders[order_]]; }

    void Tick() {
        if (tick_ == 0 && patDelay_ == 0 && !StartRow()) return;
        for (int c = 0; c < song_.channels; ++c) Effects(c, tick_ == 0 && !inDelay_);
        for (int c = 0; c < song_.channels; ++c) UpdateVoice(c);
        for (int v = kChannels; v < kChannels + kBackground; ++v) UpdateBackground(voices_[v]);
        if (++tick_ >= speed_ + fineDelay_) {
            tick_ = 0;
            if (patDelay_ > 0) {
                --patDelay_;
                inDelay_ = true;
                if (patDelay_ > 0) return;
            }
            inDelay_ = false;
            fineDelay_ = 0;
            NextRow();
        }
    }

    // Reads the row's cells into the channels. False at the song's end.
    bool StartRow() {
        if (ended_) return false;
        if (row_ >= Current().rows) row_ = 0;
        if (visited_[order_][row_] && loopsActive_ == 0) {
            ended_ = true;
            return false;
        }
        visited_[order_][row_] = true;
        const Pattern& pat = Current();
        for (int c = 0; c < song_.channels; ++c) {
            Channel& ch = ch_[c];
            ch.cell = pat.cells[row_ * song_.channels + c];
            ch.delayTick = ch.cutTick = ch.keyOffTick = -1;
            if (ch.cell.fx == NoteDelay && ch.cell.param) ch.delayTick = ch.cell.param;
            else Trigger(c);
        }
        return true;
    }

    void NextRow() {
        if (jumpOrder_ >= 0 || breakRow_ >= 0) {
            const int next = jumpOrder_ >= 0 ? jumpOrder_ : order_ + 1;
            row_ = std::max(breakRow_, 0);
            if (jumpOrder_ >= 0 && jumpOrder_ <= order_) loopsActive_ = 0;
            order_ = next;
            jumpOrder_ = breakRow_ = -1;
        } else if (loopJump_) {
            loopJump_ = false;
            row_ = loopTarget_;
            // Rows a pattern loop replays are not the song repeating.
            for (int r = row_; r < 256; ++r) visited_[order_][r] = false;
            return;
        } else if (++row_ >= Current().rows) {
            row_ = 0;
            ++order_;
        }
        if (!SkipOrders()) ended_ = true;
        else if (row_ >= Current().rows) row_ = 0;
    }

    const Sample* SampleOf(int index) const {
        return index >= 0 && index < (int)song_.samples.size() && song_.samples[index].length ? &song_.samples[index] : nullptr;
    }

    double PeriodOf(int note, const Sample& s) const {
        if (song_.linear) return note * 64.0;
        return kAmigaBase * 8363 / s.rate * std::pow(2.0, (kBase - note) / 12.0);
    }

    // A new note on a channel: the old voice is cut or, by its new-note action, left playing.
    void NewVoice(int c) {
        Voice& v = voices_[c];
        if (v.active && song_.kind == Kind::IT && ch_[c].nna != 0) {
            Voice* slot = nullptr;
            for (int b = kChannels; b < kChannels + kBackground && !slot; ++b)
                if (!voices_[b].active) slot = &voices_[b];
            if (!slot) {  // replace the quietest
                slot = &voices_[kChannels];
                for (int b = kChannels; b < kChannels + kBackground; ++b)
                    if (voices_[b].base * voices_[b].fade < slot->base * slot->fade) slot = &voices_[b];
            }
            *slot = v;
            slot->pan = ch_[c].pan;
            if (ch_[c].nna == 2) slot->keyOff = true;
            if (ch_[c].nna == 3 || (ch_[c].nna == 2 && !(v.ins && v.ins->vol.on))) slot->fading = true;
        }
        v = Voice{};
    }

    void Trigger(int c) {
        Channel& ch = ch_[c];
        const Cell& cell = ch.cell;
        const bool porta = cell.fx == TonePorta || cell.fx == TonePortaVol || cell.vc == VcPorta;
        Voice& v = voices_[c];
        int sampleIndex = ch.sample;
        if (cell.instr) {
            ch.instr = cell.instr;
            if (!song_.instrumentMode) sampleIndex = cell.instr - 1;
        }
        const Instrument* ins = song_.instrumentMode && ch.instr > 0 && ch.instr <= (int)song_.instruments.size()
                                    ? &song_.instruments[ch.instr - 1] : nullptr;
        int note = cell.note >= 1 && cell.note <= 120 ? cell.note - 1 : -1;
        int played = note;
        if (note >= 0 && ins) {
            sampleIndex = ins->sample[note] - 1;
            played = ins->note[note];
        }
        const Sample* s = SampleOf(sampleIndex);
        if (cell.note == kNoteOff) {
            v.keyOff = true;
            if (song_.kind == Kind::XM && !(v.ins && v.ins->vol.on)) ch.vol = 0;
            if (song_.kind == Kind::IT && !(v.ins && v.ins->vol.on)) v.fading = true;
        } else if (cell.note == kNoteCut) {
            v.active = false;
        } else if (cell.note == kNoteFade) {
            v.fading = true;
        }
        // An instrument number alone resets the volume (and in XM the envelopes) without a new note.
        if (cell.instr && (s || (ins && note < 0))) {
            const Sample* ds = s ? s : SampleOf(ch.sample);
            if (ds) {
                ch.vol = ds->volume;
                if (ds->pan >= 0 && song_.kind != Kind::S3M) ch.pan = ds->pan;
            }
            if (ins && ins->defaultPan >= 0) ch.pan = ins->defaultPan;
            if (song_.kind == Kind::XM && v.active) {
                v.volEnv = v.panEnv = 0;
                v.fade = 1;
                v.keyOff = v.fading = false;
            }
        }
        if (note >= 0 && s) {
            if (porta && v.active && ch.sample == sampleIndex) {
                ch.target = PeriodOf(played, *s);
            } else {
                ch.nna = ins ? ins->nna : 0;
                NewVoice(c);
                v.s = s;
                v.ins = ins;
                v.active = true;
                ch.sample = sampleIndex;
                ch.note = played;
                ch.period = ch.target = PeriodOf(played, *s);
                if (cell.fx == Offset || ch.highOffset) {
                    const int p = cell.fx == Offset ? (cell.param ? (ch.memOffset = cell.param) : ch.memOffset) : 0;
                    const uint64_t at = ((uint64_t)ch.highOffset << 16) + (uint64_t)p * 256;
                    if (at >= s->length) v.active = song_.kind == Kind::IT && s->loop;
                    v.pos = (at < s->length ? at : s->loop ? s->loopStart : 0) << 32;
                }
                if (!(ch.vibWave & 4)) ch.vibPos = 0;
                if (!(ch.tremWave & 4)) ch.tremPos = 0;
                ch.tremorCount = 0;
                ch.tremorOff = false;
                ch.retrigCount = 0;
            }
        }
        // The volume column's row-start part.
        if (cell.vc == VcVol) ch.vol = cell.vcParam;
        else if (cell.vc == VcPan) ch.pan = cell.vcParam;
    }

    void Retrigger(int c) {
        Voice& v = voices_[c];
        if (!v.s) return;
        v.active = true;
        v.pos = 0;
        v.back = false;
        v.volEnv = v.panEnv = v.pitchEnv = 0;
        v.keyOff = v.fading = false;
        v.fade = 1;
    }

    // Slides pitch up (positive) or down by `amount` units: ×4 periods, or 1/64 semitones.
    void Slide(Channel& ch, int amount) {
        if (song_.linear) ch.period += amount;
        else ch.period = std::max(1.0, ch.period - amount);
        if (song_.kind == Kind::MOD) ch.period = std::clamp(ch.period, 113.0 * 4, 856.0 * 4);
    }

    // IT/S3M raw volume slide: x0 up, 0y down, xF fine up, Fy fine down.
    static int VolumeDelta(int p, bool first) {
        const int x = p >> 4, y = p & 15;
        if (y == 15 && x) return first ? x : 0;
        if (x == 15 && y) return first ? -y : 0;
        if (first) return 0;
        return x ? x : -y;
    }

    uint8_t Mem(uint8_t param, uint8_t& mem) {
        if (param) mem = param;
        return mem;
    }

    void Effects(int c, bool first) {
        Channel& ch = ch_[c];
        const Cell& cell = ch.cell;
        const int t = tick_;
        ch.pitchShift = 0;
        ch.volShift = 0;
        ch.panShift = 0;
        ch.arp = 0;
        if (t == ch.delayTick && !inDelay_) Trigger(c);
        if (t == ch.cutTick) ch.vol = 0;
        if (t == ch.keyOffTick) {
            voices_[c].keyOff = true;
            if (!(voices_[c].ins && voices_[c].ins->vol.on)) ch.vol = 0;
        }
        // Volume column effects.
        const int vp = cell.vcParam;
        switch (cell.vc) {
            case VcUp: if (!first) ch.vol = std::min(64, ch.vol + vp); break;
            case VcDown: if (!first) ch.vol = std::max(0, ch.vol - vp); break;
            case VcFineUp: if (first) ch.vol = std::min(64, ch.vol + vp); break;
            case VcFineDown: if (first) ch.vol = std::max(0, ch.vol - vp); break;
            case VcVibSpeed: if (vp) ch.vibSpeed = vp * 4; Vibe(ch, first, 4); break;
            case VcVibDepth: if (vp) ch.vibDepth = vp * 4; Vibe(ch, first, 4); break;
            case VcPanLeft: if (!first) ch.pan = std::max(0, ch.pan - vp * 4); break;
            case VcPanRight: if (!first) ch.pan = std::min(256, ch.pan + vp * 4); break;
            case VcPorta: if (vp) ch.memTone = (uint8_t)vp; if (!first) TonePortaStep(ch); break;
            case VcPortaDown: if (!first) Slide(ch, -vp * 4); break;
            case VcPortaUp: if (!first) Slide(ch, vp * 4); break;
            default: break;
        }
        const uint8_t p = cell.param;
        const int x = p >> 4, y = p & 15;
        switch (cell.fx) {
            case Arpeggio: {
                const int a = Mem(p, ch.memArp);
                ch.arp = t % 3 == 1 ? a >> 4 : t % 3 == 2 ? a & 15 : 0;
                break;
            }
            case PortaUp: if (!first) Slide(ch, Mem(p, ch.memPortaUp) * 4); break;
            case PortaDown: if (!first) Slide(ch, -Mem(p, ch.memPortaDown) * 4); break;
            case PortaUpIT:
            case PortaDownIT: {
                uint8_t& mem = song_.kind == Kind::IT ? (cell.fx == PortaUpIT ? ch.memPortaUp : ch.memPortaDown) : ch.memPorta;
                const int q = Mem(p, song_.compatGxx ? ch.memPorta : mem), sign = cell.fx == PortaUpIT ? 1 : -1;
                if (song_.compatGxx) mem = (uint8_t)q;
                if (q >= 0xF0) { if (first) Slide(ch, sign * (q & 15) * 4); }
                else if (q >= 0xE0) { if (first) Slide(ch, sign * (q & 15)); }
                else if (!first) Slide(ch, sign * q * 4);
                break;
            }
            case FinePortaUp: if (first) Slide(ch, Mem(p, ch.memFineUp) * 4); break;
            case FinePortaDown: if (first) Slide(ch, -Mem(p, ch.memFineDown) * 4); break;
            case XFinePortaUp: if (first) Slide(ch, Mem(p, ch.memXUp)); break;
            case XFinePortaDown: if (first) Slide(ch, -Mem(p, ch.memXDown)); break;
            case TonePorta:
                Mem(p, song_.compatGxx ? ch.memPorta : ch.memTone);
                if (song_.compatGxx) ch.memTone = ch.memPorta;
                if (!first) TonePortaStep(ch);
                break;
            case TonePortaVol:
                if (!first) TonePortaStep(ch);
                VolumeSlide(ch, p, first);
                break;
            case Vibrato:
            case FineVibrato:
                if (x) ch.vibSpeed = x * 4;
                if (y) ch.vibDepth = y * 4;
                Vibe(ch, first, cell.fx == FineVibrato ? 1 : 4);
                break;
            case VibratoVol:
                Vibe(ch, first, 4);
                VolumeSlide(ch, p, first);
                break;
            case Tremolo: {
                if (x) ch.tremSpeed = x * 4;
                if (y) ch.tremDepth = y;
                ch.volShift = Wave(ch.tremWave, ch.tremPos >> 2) * ch.tremDepth / 64;
                if (!first || song_.kind == Kind::IT) ch.tremPos += ch.tremSpeed;
                break;
            }
            case SetPan: if (first) ch.pan = p; break;
            case VolSlide: VolumeSlide(ch, p, first); break;
            case VolSlideIT: {
                const int q = Mem(p, ch.memVol);
                ch.vol = Clamp(ch.vol + VolumeDelta(q, first), 0, 64);
                break;
            }
            case FineVolUp: if (first) ch.vol = std::min(64, ch.vol + Mem(p, ch.memFineUp)); break;
            case FineVolDown: if (first) ch.vol = std::max(0, ch.vol - Mem(p, ch.memFineDown)); break;
            case Jump: if (first) { jumpOrder_ = p; if (breakRow_ < 0) breakRow_ = 0; } break;
            case Break: if (first) breakRow_ = p; break;
            case SetVol: if (first) ch.vol = std::min<int>(64, p); break;
            case Speed: if (first && p) speed_ = p; break;
            case Tempo:
                if (song_.kind == Kind::IT && p < 0x20) {  // T0x / T1x slide the tempo every tick after the first
                    const int q = Mem(p, ch.memTempo);
                    if (!first) tempo_ = Clamp(tempo_ + ((q >> 4) ? (q & 15) : -(q & 15)), 32, 255);
                } else if (first && p >= 32) tempo_ = p;
                break;
            case SpeedTempo: if (first && p) { if (p < 32) speed_ = p; else tempo_ = p; } break;
            case PatternLoop:
                if (first) {
                    if (y == 0) ch.loopRow = row_;
                    else if (ch.loopCount == 0) {
                        ch.loopCount = y;
                        Loop(ch);
                    } else if (--ch.loopCount > 0) Loop(ch);
                    else {
                        --loopsActive_;
                        if (song_.kind == Kind::IT) ch.loopRow = row_ + 1;
                    }
                }
                break;
            case PatternDelay: if (first && patDelay_ == 0 && !inDelay_) patDelay_ = y + 1; break;
            case FinePatDelay: if (first) fineDelay_ += y; break;
            case NoteCut:
                if (first) {
                    ch.cutTick = y ? y : song_.kind == Kind::IT ? 1 : 0;
                    if (ch.cutTick == 0) ch.vol = 0;
                }
                break;
            case Retrig:
                if (y && t > 0 && t % y == 0) Retrigger(c);
                break;
            case MultiRetrig: {
                const int q = Mem(p, ch.memRetrig), every = q & 15;
                if (!every) break;
                if (++ch.retrigCount >= every && !(first && song_.kind != Kind::IT)) {
                    ch.retrigCount = 0;
                    static const int kAdd[16] = {0, -1, -2, -4, -8, -16, 0, 0, 0, 1, 2, 4, 8, 16, 0, 0};
                    const int mode = q >> 4;
                    if (mode == 6) ch.vol = ch.vol * 2 / 3;
                    else if (mode == 7) ch.vol /= 2;
                    else if (mode == 14) ch.vol = ch.vol * 3 / 2;
                    else if (mode == 15) ch.vol *= 2;
                    else ch.vol += kAdd[mode];
                    ch.vol = Clamp(ch.vol, 0, 64);
                    Retrigger(c);
                }
                break;
            }
            case Tremor: {
                const int q = Mem(p, ch.memTremor), on = std::max(1, q >> 4), off = std::max(1, q & 15);
                if (ch.tremorCount <= 0) {
                    ch.tremorOff = !ch.tremorOff;
                    ch.tremorCount = ch.tremorOff ? off : on;
                }
                --ch.tremorCount;
                if (ch.tremorOff) ch.volShift = -64;
                break;
            }
            case GlobalVol: if (first) globalVol_ = std::min<int>(128, p); break;
            case GlobalVolSlide: {
                const int q = Mem(p, ch.memGlobal);
                // IT slides the 0..128 global volume itself; XM slides its 0..64 one.
                const int d = song_.kind == Kind::IT ? VolumeDelta(q, first) : (first ? 0 : (q >> 4 ? q >> 4 : -(q & 15)) * 2);
                globalVol_ = Clamp(globalVol_ + d, 0, 128);
                break;
            }
            case KeyOff: if (first) ch.keyOffTick = p; break;
            case EnvPos: if (first) voices_[c].volEnv = voices_[c].panEnv = p; break;
            case PanSlide:
                if (!first) {
                    const int q = Mem(p, ch.memPan);
                    ch.pan = Clamp(ch.pan + ((q >> 4) ? (q >> 4) : -(q & 15)), 0, 256);
                }
                break;
            case PanSlideIT: {
                const int q = Mem(p, ch.memPan);
                ch.pan = Clamp(ch.pan - VolumeDelta(q, first) * 4, 0, 256);
                break;
            }
            case ChanVol: if (first) ch.chanVol = std::min<int>(64, p); break;
            case ChanVolSlide: {
                const int q = Mem(p, ch.memChanVol);
                ch.chanVol = Clamp(ch.chanVol + VolumeDelta(q, first), 0, 64);
                break;
            }
            case Panbrello:
                if (x) ch.panbSpeed = x;
                if (y) ch.panbDepth = y;
                ch.panShift = Wave(ch.panbWave, ch.panbPos) * ch.panbDepth / 32;
                ch.panbPos += ch.panbSpeed;
                break;
            case VibWave: if (first) ch.vibWave = y; break;
            case TremWave: if (first) ch.tremWave = y; break;
            case PanbrelloWave: if (first) ch.panbWave = y; break;
            case PastNotes:
                if (first)
                    for (int b = kChannels; b < kChannels + kBackground; ++b) {
                        Voice& v = voices_[b];
                        if (!v.active || v.s == nullptr) continue;
                        if (y == 0) v.active = false;
                        else if (y == 1) v.keyOff = true;
                        else v.fading = true;
                    }
                break;
            case SetNna: if (first) ch.nna = y - 3; break;
            case HighOffset: if (first) ch.highOffset = y; break;
            case SetFinetune:
                if (first && ch.sample >= 0 && voices_[c].s) {
                    const int ft = y < 8 ? y : y - 16;
                    const double rate = 8363 * std::pow(2.0, ft / 96.0);
                    ch.period = song_.linear ? ch.period : kAmigaBase * 8363 / rate * std::pow(2.0, (kBase - ch.note) / 12.0);
                }
                break;
            default: break;
        }
    }

    void Loop(Channel& ch) {
        loopJump_ = true;
        loopTarget_ = ch.loopRow;
        if (ch.loopCount == ch.cell.param) ++loopsActive_;
    }

    void VolumeSlide(Channel& ch, uint8_t p, bool first) {
        if (song_.kind == Kind::IT || song_.kind == Kind::S3M) {
            ch.vol = Clamp(ch.vol + VolumeDelta(Mem(p, ch.memVol), first), 0, 64);
            return;
        }
        const int q = song_.kind == Kind::XM ? Mem(p, ch.memVol) : p;
        if (!first) ch.vol = Clamp(ch.vol + ((q >> 4) ? (q >> 4) : -(q & 15)), 0, 64);
    }

    void TonePortaStep(Channel& ch) {
        const int speed = ch.memTone * 4;
        if (ch.period < ch.target) ch.period = std::min(ch.target, ch.period + speed);
        else if (ch.period > ch.target) ch.period = std::max(ch.target, ch.period - speed);
    }

    void Vibe(Channel& ch, bool first, int scale) {
        ch.pitchShift = (double)Wave(ch.vibWave, ch.vibPos >> 2) * ch.vibDepth * scale / 512;
        if (!first || song_.kind == Kind::IT) ch.vibPos += ch.vibSpeed;
    }

    // An envelope's value at its tick, then the tick moved on. Sustain holds while the key is down.
    static int EnvelopeStep(const Envelope& e, int& tick, bool keyOff, bool xm, bool& finished) {
        const auto& pts = e.points;
        finished = false;
        if (pts.empty()) return 64;
        int i = 0;
        while (i + 1 < (int)pts.size() && tick >= pts[i + 1].tick) ++i;
        int value;
        if (i + 1 >= (int)pts.size() || pts[i + 1].tick == pts[i].tick) value = pts[i].value;
        else value = pts[i].value + (pts[i + 1].value - pts[i].value) * (tick - pts[i].tick) / (pts[i + 1].tick - pts[i].tick);
        const bool held = e.sus && !keyOff;
        const int susStart = pts[e.susStart].tick, susEnd = pts[e.susEnd].tick;
        if (held && xm && tick == susStart) return value;  // XM: hold on the sustain point
        ++tick;
        if (held && !xm && tick > susEnd) tick = susStart;
        else if (e.loop && tick > pts[e.loopEnd].tick) tick = pts[e.loopStart].tick;
        if (tick > pts.back().tick) {
            tick = pts.back().tick;
            finished = true;
        }
        return value;
    }

    // Envelopes, fadeout and auto-vibrato for one tick; returns the envelope volume 0..1 and
    // sets the pan offset (-128..128) and pitch offset (1/64 semitones).
    double Envelopes(Voice& v, int& panOffset, double& pitchOffset) {
        panOffset = 0;
        pitchOffset = 0;
        double env = 1;
        const bool xm = song_.kind == Kind::XM;
        if (v.ins) {
            bool done = false;
            if (v.ins->vol.on) {
                env = EnvelopeStep(v.ins->vol, v.volEnv, v.keyOff, xm, done) / 64.0;
                // IT: an envelope that has run out starts the fadeout, and one that ends at zero ends
                // the note; a looping envelope fades from the note-off. XM holds the last value.
                if (song_.kind == Kind::IT) {
                    if (done && env <= 0) v.active = false;
                    if ((done && !v.ins->vol.loop) || (v.keyOff && v.ins->vol.loop)) v.fading = true;
                }
            }
            if (v.ins->pan.on) panOffset = EnvelopeStep(v.ins->pan, v.panEnv, v.keyOff, xm, done);
            if (v.ins->pitch.on) pitchOffset = EnvelopeStep(v.ins->pitch, v.pitchEnv, v.keyOff, xm, done) * 32.0;
            if (xm && v.keyOff) v.fading = true;
            if (v.fading) {
                const double stepFade = xm ? v.ins->fadeout / 32768.0 : v.ins->fadeout / 1024.0;
                v.fade = std::max(0.0, v.fade - stepFade);
                if (v.fade <= 0) v.active = false;
            }
        } else if (v.fading) {
            v.active = false;
        }
        if (v.s && v.s->vibDepth && v.s->vibRate) {
            const Sample& s = *v.s;
            const int sweep = s.vibSweep ? std::min(256, v.autoVibSweep * 256 / s.vibSweep) : 256;
            if (v.autoVibSweep < s.vibSweep) ++v.autoVibSweep;
            static const int kXm[4] = {0, 2, 1, 1};  // XM: sine, square, ramp down, ramp up (as down)
            const int type = song_.kind == Kind::XM ? kXm[s.vibType & 3] : s.vibType & 3;
            pitchOffset -= (double)Wave(type, v.autoVibPos >> 2) * s.vibDepth * sweep / (256.0 * 64);
            v.autoVibPos += s.vibRate;
        }
        return env;
    }

    // Equal-power panning; more channels share the headroom, so a 4-channel MOD is louder per
    // channel than a 32-channel IT.
    void SetGains(Voice& v, double level, int pan) {
        pan = Clamp(pan, 0, 256);
        const double gain = level * song_.gain * 2 / std::sqrt(std::max(song_.channels, 4));
        v.left = (float)(gain * std::sqrt((256 - pan) / 256.0));
        v.right = (float)(gain * std::sqrt(pan / 256.0));
    }

    void SetStep(Voice& v, double freq) {
        v.freq = freq;
        v.step = freq > 0 && freq < 1e7 ? (uint64_t)(freq / kRate * 4294967296.0) : 0;
    }

    void UpdateVoice(int c) {
        Channel& ch = ch_[c];
        Voice& v = voices_[c];
        if (!v.active || !v.s) return;
        int panOffset;
        double pitchOffset;
        const double env = Envelopes(v, panOffset, pitchOffset);
        if (!v.active) return;
        const Sample& s = *v.s;
        const int vol = Clamp(ch.vol + ch.volShift, 0, 64);
        const double insGlobal = v.ins ? v.ins->globalVol / 128.0 : 1;
        v.base = vol / 64.0 * ch.chanVol / 64.0 * s.globalVol / 64.0 * insGlobal * globalVol_ / 128.0;
        int pan = ch.pan + ch.panShift;
        if (panOffset) pan += panOffset * (128 - std::abs(pan - 128)) / 128;
        v.pan = pan;
        SetGains(v, v.base * env * v.fade, pan);
        double freq;
        if (song_.linear) {
            const double pitch = ch.period + ch.pitchShift + ch.arp * 64 + pitchOffset;
            freq = s.rate * std::pow(2.0, (pitch - kBase * 64) / 768.0);
        } else {
            const double period = std::max(1.0, ch.period + ch.pitchShift);
            freq = song_.clock / period * std::pow(2.0, (ch.arp * 64 + pitchOffset) / 768.0);
        }
        SetStep(v, freq);
    }

    void UpdateBackground(Voice& v) {
        if (!v.active || !v.s) return;
        int panOffset;
        double pitchOffset;
        const double env = Envelopes(v, panOffset, pitchOffset);
        SetGains(v, v.base * env * v.fade, v.pan + panOffset);
        if (v.base * env * v.fade < 1e-4 && (v.fading || v.keyOff)) v.active = false;
    }

    // Moves a voice through its sample, following loops; false once it runs off the end.
    static bool Step(Voice& v) {
        const Sample& s = *v.s;
        const bool sus = s.susLoop && !v.keyOff;
        const uint32_t start = sus ? s.susStart : s.loopStart, end = sus ? s.susEnd : s.loopEnd;
        const uint8_t mode = sus ? s.susLoop : s.loop;
        if (v.back) {
            v.pos -= v.step;
            if ((int64_t)v.pos < (int64_t)((uint64_t)start << 32)) {
                v.pos = ((uint64_t)start << 32) * 2 - v.pos;
                v.back = false;
            }
            return true;
        }
        v.pos += v.step;
        const uint64_t limit = (uint64_t)(mode ? end : s.length) << 32;
        if (v.pos < limit) return true;
        if (!mode) return v.active = false;
        if (mode == 1) {
            const uint64_t span = (uint64_t)(end - start) << 32;
            v.pos = ((uint64_t)start << 32) + (v.pos - limit) % span;
        } else {
            v.pos = limit * 2 - v.pos - (1ull << 32);
            v.back = true;
        }
        return true;
    }

    // Step() n times, in one jump when the voice runs forward through a forward or no loop.
    static void Advance(Voice& v, size_t n) {
        const Sample& s = *v.s;
        const bool sus = s.susLoop && !v.keyOff;
        const uint8_t mode = sus ? s.susLoop : s.loop;
        if (v.back || mode == 2 || n < 2) {
            for (size_t i = 0; i < n && v.active; ++i) Step(v);
            return;
        }
        const uint64_t start = (uint64_t)(sus ? s.susStart : s.loopStart) << 32;
        const uint64_t limit = (uint64_t)(mode ? (sus ? s.susEnd : s.loopEnd) : s.length) << 32;
        const double ahead = (double)v.pos + (double)v.step * n;
        if (!mode) {
            if (ahead >= (double)limit) v.active = false;
            else v.pos += v.step * n;
            return;
        }
        if (ahead < (double)limit) v.pos += v.step * n;
        else v.pos = start + (uint64_t)std::fmod(ahead - (double)start, (double)(limit - start));
    }

    void Mix(Voice& v, float* out, size_t n) {
        const Sample& s = *v.s;
        const int16_t* d = s.data.data();
        constexpr size_t kRamp = 64;
        const size_t ramp = std::min(n, kRamp);
        const float dl = (v.left - v.curLeft) / ramp, dr = (v.right - v.curRight) / ramp;
        for (size_t i = 0; i < n && v.active; ++i) {
            const uint32_t idx = std::min((uint32_t)(v.pos >> 32), s.length - 1);
            const float frac = (float)(uint32_t)v.pos * (1.0f / 4294967296.0f);
            const bool loopEdge = s.loop == 1 && idx + 1 == s.loopEnd && !(s.susLoop && !v.keyOff);
            const float a = d[idx], b = loopEdge ? d[s.loopStart] : d[idx + 1];
            const float x = (a + (b - a) * frac) * (1.0f / 32768.0f);
            if (i < ramp) {
                v.curLeft += dl;
                v.curRight += dr;
            }
            out[i * 2] += x * v.curLeft;
            out[i * 2 + 1] += x * v.curRight;
            Step(v);
        }
        v.curLeft = v.left;
        v.curRight = v.right;
    }

    const Song& song_;
    std::array<Channel, kChannels> ch_;
    std::array<Voice, kChannels + kBackground> voices_;
    int speed_ = 6, tempo_ = 125, globalVol_ = 128;
    int order_ = 0, row_ = 0, tick_ = 0, patDelay_ = 0, fineDelay_ = 0;
    bool inDelay_ = false, ended_ = false, loopJump_ = false;
    int loopTarget_ = 0, loopsActive_ = 0;
    int jumpOrder_ = -1, breakRow_ = -1;
    std::vector<std::vector<bool>> visited_;
    size_t left_ = 0;  // frames left in this tick
};

bool LoadFile(const std::wstring& path, Song& song) {
    nativeaudio::File file;
    if (!file.Open(path) || file.Size() > (64u << 20)) return false;
    const uint8_t* p = file.View(0, (size_t)file.Size());
    if (!p) return false;
    const std::string bytes((const char*)p, (size_t)file.Size());
    return LoadSong(bytes, ExtOf(path), song) && !song.orders.empty() && !song.patterns.empty();
}

class ModuleDecoder final : public Decoder {
public:
    ModuleDecoder(Song song, uint64_t length) : song_(std::move(song)), player_(song_), length_(length) {}

    size_t Read(float* out, size_t frames) override {
        const size_t n = player_.Render(out, frames, true);
        for (size_t i = 0; i < n * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
        pos_ += n;
        return n;
    }
    bool Seek(double seconds) override {
        if (!std::isfinite(seconds)) return false;
        const uint64_t target = std::min<uint64_t>(length_, (uint64_t)(std::max(0.0, seconds) * kRate));
        if (target < pos_) {  // a sequencer only runs forward
            player_.Restart();
            pos_ = 0;
        }
        while (pos_ < target) {
            const size_t n = player_.Render(nullptr, (size_t)std::min<uint64_t>(target - pos_, 1 << 16), false);
            if (!n) break;
            pos_ += n;
        }
        return true;
    }
    double Duration() const override { return (double)length_ / kRate; }
    unsigned Rate() const override { return kRate; }

private:
    Song song_;
    ModulePlayer player_;
    uint64_t length_;
    uint64_t pos_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> OpenModuleDecoder(const std::wstring& path) {
    Song song;
    if (!LoadFile(path, song)) return nullptr;
    const uint64_t length = ModulePlayer(song).Length();
    if (!length) return nullptr;
    return std::make_unique<ModuleDecoder>(std::move(song), length);
}

bool ReadModuleTags(const std::wstring& path, TagData& out) {
    Song song;
    if (!LoadFile(path, song)) return false;
    out.title = song.name.empty() ? ToUtf8(StemOf(path)) : song.name;
    out.comment = song.tracker + ", " + std::to_string(song.channels) + (song.channels == 1 ? " channel" : " channels");
    out.duration = (double)ModulePlayer(song).Length() / kRate;
    out.sampleRate = kRate;
    out.channels = 2;
    out.codec = Codec::Module;
    return out.duration > 0;
}
