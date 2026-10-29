// MIDI: Standard MIDI Files (.mid, .kar, and .rmi's RIFF wrapper) played by a sample synthesizer
// through a SoundFont 2 or DLS bank. The bank is the first of: a .sf2 or .dls with the song's name
// beside it, any .sf2 in its folder, one in <data>\soundfonts (SetMidiSoundFontFolder), and
// Windows' own General MIDI set (drivers\gm.dls). A bank becomes one model of instruments and
// key/velocity regions; SoundFont preset and instrument generators are combined at load, DLS
// articulations are read as the same settings. Each voice has a DAHDSR volume envelope, a
// low-pass filter, vibrato and constant-power pan; channels follow the General MIDI controllers
// (volume, expression, pan, sustain, modulation, pitch bend and its range).
#include "audio_file.h"
#include "library.h"
#include "native_audio.h"
#include "tags.h"

#include <array>
#include <cmath>
#include <mutex>
#include <vector>

namespace {
using namespace nativeaudio;

constexpr unsigned kRate = 44100;
constexpr size_t kBlock = 32;     // samples between envelope and pitch updates
constexpr int kMaxVoices = 128;
constexpr double kTail = 1.5;     // seconds after the last event, for releases to finish
constexpr double kSilentDb = 100;

std::wstring g_soundFontFolder;
std::mutex g_bankMx;

double TimeCents(double tc) { return tc <= -12000 ? 0 : std::pow(2.0, tc / 1200); }

// ---------------------------------------------------------------------------
// The bank model

struct Region {
    uint8_t keyLo = 0, keyHi = 127, velLo = 0, velHi = 127;
    const int16_t* data = nullptr;
    uint32_t start = 0, end = 0, loopStart = 0, loopEnd = 0;  // into data; ends exclusive
    uint8_t loopMode = 0;                                       // 0 none, 1 always, 3 until release
    int root = 60, scale = 100, exclusive = 0;
    double tune = 0, rate = 44100;          // cents; the sample's rate
    double attenuation = 0, pan = 0;        // dB; -1..1
    double delay = 0, attack = 0, hold = 0, decay = 0, release = 0, sustain = 0;  // seconds; dB down
    double holdKey = 0, decayKey = 0;       // timecents per key from 60
    double filterHz = 20000, filterQ = 0;   // dB of resonance
    double vibHz = 8.176, vibDelay = 0, vibCents = 0;
};

struct Instrument {
    int bank = 0, program = 0;
    bool drums = false;
    std::vector<Region> regions;
};

struct Bank {
    std::wstring path;
    uint64_t stamp = 0;
    File file;                 // keeps SoundFont samples mapped
    std::vector<int16_t> pcm;  // DLS samples, converted
    std::vector<Instrument> instruments;

    const Instrument* Find(int bank, int program, bool drums) const {
        const Instrument* fallback = nullptr;
        for (const auto& i : instruments) {
            if (i.drums != drums) continue;
            if (i.program == program && i.bank == bank) return &i;
            if (i.program == program && (!fallback || i.bank == 0)) fallback = &i;
        }
        if (fallback) return fallback;
        if (drums)  // any drum kit rather than silence
            for (const auto& i : instruments)
                if (i.drums) return &i;
        return nullptr;
    }
};

// Little-endian reads with bounds checks over one mapped range.
struct Span {
    const uint8_t* p = nullptr;
    size_t n = 0;
    uint32_t u8(size_t at) const { return at < n ? p[at] : 0; }
    uint32_t u16(size_t at) const { return u8(at) | u8(at + 1) << 8; }
    uint32_t u32(size_t at) const { return u16(at) | u16(at + 2) << 16; }
    int16_t s16(size_t at) const { return (int16_t)u16(at); }
    bool is(size_t at, const char* id) const { return at + 4 <= n && std::memcmp(p + at, id, 4) == 0; }
};

// Calls f(id, bodyOffset, bodySize) for each chunk in [at, end), descending into nothing.
template <class F> void Chunks(const Span& s, size_t at, size_t end, F&& f) {
    while (at + 8 <= end && at + 8 <= s.n) {
        const size_t size = s.u32(at + 4), body = at + 8;
        if (size > s.n - body) break;
        f(at, body, size);
        at = body + size + (size & 1);
    }
}

// --- SoundFont 2 ---

enum Gen {
    StartOffset = 0, EndOffset = 1, LoopStartOffset = 2, LoopEndOffset = 3, StartCoarse = 4, ModLfoPitch = 5,
    VibLfoPitch = 6, FilterFc = 8, FilterQ = 9, EndCoarse = 12, Pan = 17, VibDelay = 23, VibFreq = 24,
    VolDelay = 33, VolAttack = 34, VolHold = 35, VolDecay = 36, VolSustain = 37, VolRelease = 38, KeyToHold = 39,
    KeyToDecay = 40, InstrumentGen = 41, KeyRange = 43, VelRange = 44, LoopStartCoarse = 45, KeyNum = 46,
    Velocity = 47, Attenuation = 48, LoopEndCoarse = 50, CoarseTune = 51, FineTune = 52, SampleId = 53,
    SampleModes = 54, ScaleTuning = 56, Exclusive = 57, RootKey = 58, GenCount = 61,
};

using Gens = std::array<int, GenCount>;

Gens DefaultGens() {
    Gens g{};
    g[FilterFc] = 13500;
    for (int i : {VolDelay, VolAttack, VolHold, VolDecay, VolRelease, VibDelay}) g[i] = -12000;
    g[KeyRange] = 127 << 8;
    g[VelRange] = 127 << 8;
    g[KeyNum] = g[Velocity] = g[RootKey] = -1;
    g[ScaleTuning] = 100;
    return g;
}

bool LoadSf2(Bank& bank) {
    const uint8_t* all = bank.file.View(0, (size_t)bank.file.Size());
    if (!all) return false;
    const Span f{all, (size_t)bank.file.Size()};
    if (!f.is(0, "RIFF") || !f.is(8, "sfbk")) return false;
    Span smpl, phdr, pbag, pgen, inst, ibag, igen, shdr;
    Chunks(f, 12, f.n, [&](size_t at, size_t body, size_t size) {
        if (!f.is(at, "LIST")) return;
        Chunks(f, body + 4, body + size, [&](size_t sub, size_t b, size_t n) {
            const Span s{f.p + b, n};
            if (f.is(sub, "smpl")) smpl = s;
            else if (f.is(sub, "phdr")) phdr = s;
            else if (f.is(sub, "pbag")) pbag = s;
            else if (f.is(sub, "pgen")) pgen = s;
            else if (f.is(sub, "inst")) inst = s;
            else if (f.is(sub, "ibag")) ibag = s;
            else if (f.is(sub, "igen")) igen = s;
            else if (f.is(sub, "shdr")) shdr = s;
        });
    });
    if (!smpl.p || phdr.n < 76 || inst.n < 44 || shdr.n < 92) return false;
    const auto* pcm = (const int16_t*)smpl.p;
    const uint32_t pcmFrames = (uint32_t)(smpl.n / 2);
    const size_t presets = phdr.n / 38 - 1, instruments = inst.n / 22 - 1, samples = shdr.n / 46;
    // A zone's generators, applied over `g`; the instrument or sample it ends with, or -1.
    auto zone = [](const Span& bags, const Span& gens, size_t bag, Gens& g, int terminal) {
        const size_t first = bags.u16(bag * 4), last = bags.u16(bag * 4 + 4);
        int found = -1;
        for (size_t i = first; i < last && i * 4 + 4 <= gens.n; ++i) {
            const int op = (int)gens.u16(i * 4), amount = gens.s16(i * 4 + 2);
            if (op >= GenCount) continue;
            g[op] = op == KeyRange || op == VelRange ? (int)gens.u16(i * 4 + 2) : amount;
            if (op == terminal) found = (int)gens.u16(i * 4 + 2);
        }
        return found;
    };
    auto rangeOk = [](int a, int b) { return (a & 0xFF) <= (b >> 8) && (b & 0xFF) <= (a >> 8); };
    // Preset zones add to these instrument generators; sample addresses, ranges and the like are
    // the instrument's alone.
    auto additive = [](int i) {
        return i > LoopEndOffset && i != StartCoarse && i != EndCoarse && i != LoopStartCoarse && i != LoopEndCoarse &&
               i != KeyRange && i != VelRange && i != InstrumentGen && i != SampleId && i != SampleModes &&
               i != Exclusive && i != RootKey && i != KeyNum && i != Velocity;
    };
    auto intersect = [](int a, int b) {
        return std::max(a & 0xFF, b & 0xFF) | std::min(a >> 8, b >> 8) << 8;
    };
    for (size_t p = 0; p < presets; ++p) {
        const size_t h = p * 38;
        Instrument out;
        out.program = (int)phdr.u16(h + 20);
        const int bankNumber = (int)phdr.u16(h + 22);
        out.drums = bankNumber == 128;
        out.bank = out.drums ? 0 : bankNumber;
        const size_t bagFirst = phdr.u16(h + 24), bagLast = phdr.u16(h + 38 + 24);
        Gens presetGlobal{};
        for (size_t pb = bagFirst; pb < bagLast; ++pb) {
            Gens pz = presetGlobal;
            pz[KeyRange] = presetGlobal[KeyRange] ? presetGlobal[KeyRange] : 127 << 8;
            pz[VelRange] = presetGlobal[VelRange] ? presetGlobal[VelRange] : 127 << 8;
            const int instrument = zone(pbag, pgen, pb, pz, InstrumentGen);
            if (instrument < 0) {
                if (pb == bagFirst) presetGlobal = pz;
                continue;
            }
            if ((size_t)instrument >= instruments) continue;
            const size_t ibFirst = inst.u16(instrument * 22 + 20), ibLast = inst.u16(instrument * 22 + 22 + 20);
            Gens instGlobal = DefaultGens();
            for (size_t ib = ibFirst; ib < ibLast; ++ib) {
                Gens g = instGlobal;
                const int sample = zone(ibag, igen, ib, g, SampleId);
                if (sample < 0) {
                    if (ib == ibFirst) instGlobal = g;
                    continue;
                }
                if ((size_t)sample >= samples || !rangeOk(g[KeyRange], pz[KeyRange]) || !rangeOk(g[VelRange], pz[VelRange])) continue;
                // Preset generators add to the instrument's, except the ranges, which narrow.
                for (int i = 0; i < GenCount; ++i)
                    if (additive(i)) g[i] += pz[i];
                const int keys = intersect(g[KeyRange], pz[KeyRange]), vels = intersect(g[VelRange], pz[VelRange]);
                const size_t s = sample * 46;
                Region r;
                r.keyLo = (uint8_t)(keys & 0xFF);
                r.keyHi = (uint8_t)(keys >> 8);
                r.velLo = (uint8_t)(vels & 0xFF);
                r.velHi = (uint8_t)(vels >> 8);
                r.data = pcm;
                const int64_t start = (int64_t)shdr.u32(s + 20) + g[StartOffset] + 32768LL * g[StartCoarse];
                const int64_t end = (int64_t)shdr.u32(s + 24) + g[EndOffset] + 32768LL * g[EndCoarse];
                const int64_t loopStart = (int64_t)shdr.u32(s + 28) + g[LoopStartOffset] + 32768LL * g[LoopStartCoarse];
                const int64_t loopEnd = (int64_t)shdr.u32(s + 32) + g[LoopEndOffset] + 32768LL * g[LoopEndCoarse];
                if (start < 0 || end <= start || end > pcmFrames) continue;
                r.start = (uint32_t)start;
                r.end = (uint32_t)end;
                r.loopStart = (uint32_t)std::clamp<int64_t>(loopStart, start, end);
                r.loopEnd = (uint32_t)std::clamp<int64_t>(loopEnd, start, end);
                r.loopMode = (uint8_t)(g[SampleModes] & 3);
                if (r.loopMode == 2 || r.loopEnd <= r.loopStart + 1) r.loopMode = 0;
                r.rate = std::max<uint32_t>(shdr.u32(s + 36), 1);
                r.root = g[RootKey] >= 0 ? g[RootKey] : (int)shdr.u8(s + 40);
                if (r.root > 127) r.root = 60;
                r.tune = g[CoarseTune] * 100.0 + g[FineTune] + (int8_t)shdr.u8(s + 41);
                r.scale = g[ScaleTuning];
                r.exclusive = g[Exclusive];
                // EMU hardware applied 40% of the attenuation; banks are voiced for that.
                r.attenuation = std::max(0, g[Attenuation]) * 0.04;
                r.pan = std::clamp(g[Pan] / 500.0, -1.0, 1.0);
                r.delay = TimeCents(g[VolDelay]);
                r.attack = TimeCents(g[VolAttack]);
                r.hold = TimeCents(g[VolHold]);
                r.decay = TimeCents(g[VolDecay]);
                r.release = TimeCents(g[VolRelease]);
                r.sustain = std::clamp(g[VolSustain], 0, 1440) / 10.0;
                r.holdKey = g[KeyToHold];
                r.decayKey = g[KeyToDecay];
                r.filterHz = 8.176 * std::pow(2.0, std::clamp(g[FilterFc], 1500, 13500) / 1200.0);
                r.filterQ = std::clamp(g[FilterQ], 0, 960) / 10.0;
                r.vibHz = 8.176 * std::pow(2.0, std::clamp(g[VibFreq], -16000, 4500) / 1200.0);
                r.vibDelay = TimeCents(g[VibDelay]);
                r.vibCents = g[VibLfoPitch] ? g[VibLfoPitch] : g[ModLfoPitch];
                out.regions.push_back(r);
            }
        }
        if (!out.regions.empty()) bank.instruments.push_back(std::move(out));
    }
    return !bank.instruments.empty();
}

// --- DLS (level 1 and 2) ---

struct DlsArt {
    double delay = 0, attack = 0, hold = 0, decay = 0, release = 0, sustain = 0;  // sustain: dB down
    double pan = 0, decayKey = 0, vibHz = 5, vibDelay = 0, vibCents = 0, gain = 0;
    bool any = false;
};

void ReadArt(const Span& f, size_t body, size_t size, DlsArt& a) {
    const size_t header = f.u32(body), count = f.u32(body + 4);
    for (size_t i = 0; i < count; ++i) {
        const size_t c = body + header + i * 12;
        if (c + 12 > body + size) break;
        const uint32_t source = f.u16(c), control = f.u16(c + 2), dest = f.u16(c + 4);
        const double scale = (int32_t)f.u32(c + 8) / 65536.0;
        const bool time = dest == 0x206 || dest == 0x207 || dest == 0x209 || dest == 0x20B || dest == 0x20C;
        const double seconds = (int32_t)f.u32(c + 8) == INT32_MIN ? 0 : std::pow(2.0, scale / 1200);
        a.any = true;
        if (source == 0 && control == 0) {
            if (time) {
                (dest == 0x206 ? a.attack : dest == 0x207 ? a.decay : dest == 0x209 ? a.release : dest == 0x20B ? a.delay : a.hold) = seconds;
            } else if (dest == 0x20A) {
                const double level = std::clamp(scale / 1000.0, 0.0, 1.0);
                a.sustain = level <= 0.00001 ? kSilentDb : -20 * std::log10(level);
            } else if (dest == 0x004) a.pan = std::clamp(scale / 500.0, -1.0, 1.0);
            else if (dest == 0x104) a.vibHz = 440 * std::pow(2.0, (scale - 6900) / 1200);
            else if (dest == 0x105) a.vibDelay = seconds;
            else if (dest == 0x001) a.gain = scale / 10.0;
        } else if (source == 3 && dest == 0x207) {
            a.decayKey = -scale / 128;  // over the keyboard; Region counts down from key 60
        } else if (source == 1 && dest == 0x003 && (control == 0 || control == 0x81)) {
            a.vibCents = std::max(a.vibCents, std::abs(scale));
        }
    }
}

bool LoadDls(Bank& bank) {
    const uint8_t* all = bank.file.View(0, (size_t)bank.file.Size());
    if (!all) return false;
    const Span f{all, (size_t)bank.file.Size()};
    if (!f.is(0, "RIFF") || !f.is(8, "DLS ")) return false;
    size_t lins = 0, linsSize = 0, ptbl = 0, wvpl = 0, wvplSize = 0;
    Chunks(f, 12, f.n, [&](size_t at, size_t body, size_t size) {
        if (f.is(at, "LIST") && f.is(body, "lins")) { lins = body + 4; linsSize = size - 4; }
        else if (f.is(at, "LIST") && f.is(body, "wvpl")) { wvpl = body + 4; wvplSize = size - 4; }
        else if (f.is(at, "ptbl")) ptbl = body;
    });
    if (!lins || !ptbl || !wvpl) return false;
    // Every wave into one 16-bit pool; `waves` remembers where each starts and how it loops.
    struct Wave { size_t at = 0; uint32_t frames = 0, rate = 22050; int unity = 60; double tune = 0, gain = 0;
                  bool loop = false; uint32_t loopStart = 0, loopLength = 0; };
    std::vector<Wave> waves;
    const size_t cues = f.u32(ptbl + 4);
    auto readWsmp = [&](size_t body, int& unity, double& tune, double& gain, bool& loop, uint32_t& ls, uint32_t& ll) {
        const size_t header = f.u32(body);
        unity = (int)f.u16(body + 4);
        tune = (int16_t)f.u16(body + 6);
        gain = (int32_t)f.u32(body + 8) / 655360.0;
        if (f.u32(body + 16) > 0) {
            const size_t l = body + header;
            loop = true;
            ls = f.u32(l + 8);
            ll = f.u32(l + 12);
        }
    };
    for (size_t i = 0; i < cues; ++i) {
        const size_t at = wvpl + f.u32(ptbl + f.u32(ptbl) + i * 4);
        Wave w;
        if (at + 12 <= wvpl + wvplSize && f.is(at, "LIST") && f.is(at + 8, "wave")) {
            int bits = 16, channels = 1;
            size_t data = 0, dataSize = 0;
            Chunks(f, at + 12, at + 8 + f.u32(at + 4), [&](size_t c, size_t body, size_t size) {
                if (f.is(c, "fmt ")) {
                    channels = std::max<int>(1, (int)f.u16(body + 2));
                    w.rate = std::max<uint32_t>(1, f.u32(body + 4));
                    bits = (int)f.u16(body + 14);
                } else if (f.is(c, "data")) { data = body; dataSize = size; }
                else if (f.is(c, "wsmp")) readWsmp(body, w.unity, w.tune, w.gain, w.loop, w.loopStart, w.loopLength);
            });
            if (data && (bits == 8 || bits == 16)) {
                w.at = bank.pcm.size();
                const size_t step = (size_t)channels * bits / 8;
                w.frames = (uint32_t)(dataSize / step);
                for (uint32_t k = 0; k < w.frames; ++k)
                    bank.pcm.push_back(bits == 16 ? f.s16(data + k * step) : (int16_t)(((int)f.u8(data + k * step) - 128) * 256));
            }
        }
        waves.push_back(w);
    }
    bank.pcm.push_back(0);  // a guard after the last wave
    Chunks(f, lins, lins + linsSize, [&](size_t at, size_t body, size_t size) {
        if (!f.is(at, "LIST") || !f.is(body, "ins ")) return;
        Instrument ins;
        DlsArt global;
        std::vector<std::pair<size_t, size_t>> regions;
        Chunks(f, body + 4, body + size, [&](size_t c, size_t b, size_t n) {
            if (f.is(c, "insh")) {
                const uint32_t locale = f.u32(b + 4);
                ins.drums = (locale & 0x80000000u) != 0;
                ins.bank = (int)(locale >> 8 & 0x7F);
                ins.program = (int)(f.u32(b + 8) & 0x7F);
            } else if (f.is(c, "LIST") && f.is(b, "lrgn")) {
                Chunks(f, b + 4, b + n, [&](size_t r, size_t rb, size_t rn) {
                    if (f.is(r, "LIST") && (f.is(rb, "rgn ") || f.is(rb, "rgn2"))) regions.push_back({rb + 4, rn - 4});
                });
            } else if (f.is(c, "LIST") && (f.is(b, "lart") || f.is(b, "lar2"))) {
                Chunks(f, b + 4, b + n, [&](size_t a, size_t ab, size_t an) {
                    if (f.is(a, "art1") || f.is(a, "art2")) ReadArt(f, ab, an, global);
                });
            }
        });
        for (auto [rb, rn] : regions) {
            Region r;
            DlsArt art = global;
            size_t wave = SIZE_MAX;
            bool haveWsmp = false;
            int unity = 60;
            double tune = 0, gain = 0;
            bool loop = false;
            uint32_t ls = 0, ll = 0;
            Chunks(f, rb, rb + rn, [&](size_t c, size_t b, size_t n) {
                if (f.is(c, "rgnh")) {
                    r.keyLo = (uint8_t)std::min<uint32_t>(127, f.u16(b));
                    r.keyHi = (uint8_t)std::min<uint32_t>(127, f.u16(b + 2));
                    // DLS level 1 ignores the velocity range, and banks leave junk in it.
                    r.exclusive = (int)f.u16(b + 10);
                } else if (f.is(c, "wsmp")) {
                    haveWsmp = true;
                    readWsmp(b, unity, tune, gain, loop, ls, ll);
                } else if (f.is(c, "wlnk")) {
                    wave = f.u32(b + 8);
                } else if (f.is(c, "LIST") && (f.is(b, "lart") || f.is(b, "lar2"))) {
                    DlsArt own;
                    Chunks(f, b + 4, b + n, [&](size_t a, size_t ab, size_t an) {
                        if (f.is(a, "art1") || f.is(a, "art2")) ReadArt(f, ab, an, own);
                    });
                    if (own.any) art = own;
                }
            });
            if (wave >= waves.size() || !waves[wave].frames) continue;
            const Wave& w = waves[wave];
            if (!haveWsmp) {
                unity = w.unity;
                tune = w.tune;
                gain = w.gain;
                loop = w.loop;
                ls = w.loopStart;
                ll = w.loopLength;
            }
            r.data = bank.pcm.data();
            r.start = (uint32_t)w.at;
            r.end = (uint32_t)(w.at + w.frames);
            if (loop && ll > 1 && ls < w.frames) {
                r.loopMode = 1;
                r.loopStart = r.start + ls;
                r.loopEnd = r.start + std::min(w.frames, ls + ll);
            }
            r.rate = w.rate;
            r.root = std::min(unity, 127);
            r.tune = tune;
            r.attenuation = std::max(0.0, -(gain + art.gain));
            r.pan = art.pan;
            r.delay = art.delay;
            r.attack = art.attack;
            r.hold = art.hold;
            r.decay = art.decay;
            r.release = art.release;
            r.sustain = art.sustain;
            r.decayKey = art.decayKey;
            r.vibHz = art.vibHz;
            r.vibDelay = art.vibDelay;
            r.vibCents = art.vibCents;
            ins.regions.push_back(r);
        }
        if (!ins.regions.empty()) bank.instruments.push_back(std::move(ins));
    });
    return !bank.instruments.empty();
}

uint64_t Stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 ^ a.ftLastWriteTime.dwLowDateTime ^ (uint64_t)a.nFileSizeLow << 20;
}

// The bank at `path`, shared: the last one loaded stays cached while it is unchanged.
std::shared_ptr<const Bank> LoadBank(const std::wstring& path) {
    static std::shared_ptr<const Bank> cached;
    const uint64_t stamp = Stamp(path);
    std::lock_guard<std::mutex> lock(g_bankMx);
    if (cached && cached->path == path && cached->stamp == stamp) return cached;
    auto bank = std::make_shared<Bank>();
    bank->path = path;
    bank->stamp = stamp;
    if (!bank->file.Open(path)) return nullptr;
    const std::wstring ext = ExtOf(path);
    if (!(ext == L"dls" ? LoadDls(*bank) : LoadSf2(*bank))) return nullptr;
    cached = bank;
    return cached;
}

bool Exists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The first .sf2 (or .dls) in a folder, by name.
std::wstring FirstBankIn(const std::wstring& dir) {
    std::wstring best;
    for (const wchar_t* pattern : {L"*.sf2", L"*.dls"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + kPathSep + pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                (best.empty() || CompareStringOrdinal(fd.cFileName, -1, best.c_str(), -1, TRUE) == CSTR_LESS_THAN))
                best = fd.cFileName;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (!best.empty()) return dir + kPathSep + best;
    }
    return {};
}

std::wstring BankFor(const std::wstring& midi) {
    const std::wstring stem = midi.substr(0, midi.size() - ExtOf(midi).size());
    for (const wchar_t* ext : {L"sf2", L"dls"}) {
        File probe;  // also finds one beside a song inside a ZIP
        if (probe.Open(stem + ext)) return stem + ext;
    }
    if (std::wstring b = FirstBankIn(DirectoryOf(midi)); !b.empty()) return b;
    if (!g_soundFontFolder.empty())
        if (std::wstring b = FirstBankIn(g_soundFontFolder); !b.empty()) return b;
#ifdef _WIN32
    wchar_t windows[MAX_PATH];
    if (GetWindowsDirectoryW(windows, MAX_PATH)) {
        const std::wstring gm = std::wstring(windows) + L"\\System32\\drivers\\gm.dls";
        if (Exists(gm)) return gm;
    }
#else
    for (const wchar_t* p : {L"/usr/share/sounds/sf2/FluidR3_GM.sf2", L"/usr/share/soundfonts/default.sf2",
                             L"/usr/share/soundfonts/FluidR3_GM.sf2"})
        if (Exists(p)) return p;
#endif
    return {};
}

// ---------------------------------------------------------------------------
// The song

struct Event {
    uint64_t time;  // samples
    uint8_t status, a, b;
};

struct Song {
    std::vector<Event> events;
    uint64_t length = 0;  // samples, with the release tail
    std::string title, copyright;
};

uint32_t VarLen(const Span& s, size_t& at, size_t end) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && at < end; ++i) {
        const uint8_t c = (uint8_t)s.u8(at++);
        v = v << 7 | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return v;
}

bool ParseSmf(const uint8_t* p, size_t n, Song& song) {
    Span f{p, n};
    size_t at = 0;
    if (f.is(0, "RIFF") && f.is(8, "RMID")) {  // .rmi: the file is a RIFF "data" chunk
        size_t found = 0, size = 0;
        Chunks(f, 12, n, [&](size_t c, size_t body, size_t sz) { if (f.is(c, "data") && !found) { found = body; size = sz; } });
        if (!found) return false;
        f = Span{p + found, size};
    }
    if (!f.is(0, "MThd")) return false;
    auto be16 = [&](size_t i) { return f.u8(i) << 8 | f.u8(i + 1); };
    auto be32 = [&](size_t i) { return (uint32_t)be16(i) << 16 | be16(i + 2); };
    const uint32_t headerSize = be32(4);
    const int tracks = be16(10), division = be16(12);
    if (!division) return false;
    struct Raw { uint64_t tick; uint32_t order; uint8_t status, a, b; uint32_t tempo; };
    std::vector<Raw> raw;
    at = 8 + headerSize;
    uint32_t order = 0;
    for (int t = 0; t < tracks && at + 8 <= f.n; ++t) {
        const size_t size = be32(at + 4), body = at + 8, end = std::min(body + size, f.n);
        const bool track = f.is(at, "MTrk");
        at = body + size;
        if (!track) {
            --t;
            continue;
        }
        uint64_t tick = 0;
        uint8_t running = 0;
        for (size_t q = body; q < end;) {
            tick += VarLen(f, q, end);
            uint8_t status = (uint8_t)f.u8(q);
            if (status & 0x80) ++q;
            else status = running;
            if (status == 0xFF) {
                const uint8_t type = (uint8_t)f.u8(q++);
                const uint32_t len = VarLen(f, q, end);
                if (type == 0x51 && len >= 3)
                    raw.push_back({tick, order++, 0xFF, 0, 0, f.u8(q) << 16 | f.u8(q + 1) << 8 | f.u8(q + 2)});
                else if ((type == 0x03 && t == 0 && song.title.empty()) || (type == 0x02 && song.copyright.empty())) {
                    std::wstring text;
                    for (uint32_t i = 0; i < len && q + i < end; ++i) text += (wchar_t)f.u8(q + i);  // Latin-1
                    (type == 0x02 ? song.copyright : song.title) = ToUtf8(text);
                }
                q += len;
                if (type == 0x2F) break;
            } else if (status == 0xF0 || status == 0xF7) {
                q += VarLen(f, q, end);
            } else if (status >= 0x80) {
                running = status;
                const int kind = status >> 4;
                const uint8_t a = (uint8_t)(f.u8(q++) & 0x7F);
                const uint8_t b = kind == 0xC || kind == 0xD ? 0 : (uint8_t)(f.u8(q++) & 0x7F);
                raw.push_back({tick, order++, status, a, b, 0});
            } else {
                break;  // data with no status: the track is damaged from here
            }
        }
    }
    std::stable_sort(raw.begin(), raw.end(), [](const Raw& x, const Raw& y) { return x.tick < y.tick; });
    // Ticks to samples through the tempo map (or the SMPTE rate).
    const bool smpte = division & 0x8000;
    const double ticksPerSecond = smpte ? (double)(256 - (division >> 8)) * (division & 0xFF) : 0;
    double tempo = 500000, seconds = 0;
    uint64_t lastTick = 0;
    for (const Raw& r : raw) {
        seconds += smpte ? (r.tick - lastTick) / ticksPerSecond : (double)(r.tick - lastTick) * tempo / 1e6 / division;
        lastTick = r.tick;
        if (r.status == 0xFF) {
            if (r.tempo) tempo = r.tempo;
            continue;
        }
        song.events.push_back({(uint64_t)(seconds * kRate), r.status, r.a, r.b});
    }
    if (song.events.empty()) return false;
    song.length = song.events.back().time + (uint64_t)(kTail * kRate);
    return true;
}

bool LoadSong(const std::wstring& path, Song& song) {
    File file;
    if (!file.Open(path) || file.Size() > (32u << 20)) return false;
    const uint8_t* p = file.View(0, (size_t)file.Size());
    return p && ParseSmf(p, (size_t)file.Size(), song);
}

// ---------------------------------------------------------------------------
// The synthesizer

struct Voice {
    const Region* r = nullptr;
    bool active = false, released = false, sustained = false;
    int channel = 0, key = 0;
    uint64_t pos = 0, step = 0;  // 32.32 frames into r->data
    double baseCents = 0;        // pitch before bends and vibrato
    double velocityDb = 0;
    uint64_t age = 0;
    // Envelope: stage 0 delay, 1 attack, 2 hold, 3 decay, 4 sustain, 5 release.
    int stage = 0;
    double stageTime = 0, levelDb = kSilentDb, attackGain = 0, releaseFromDb = 0;
    double hold = 0, decay = 0;
    double vibTime = 0;
    float gain = 0;  // at the end of the last block
    // Biquad low-pass state and coefficients.
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    bool filtered = false;
};

struct Channel {
    int program = 0, bankMsb = 0, bankLsb = 0;
    double volume = 100 / 127.0, expression = 1, pan = 0, bend = 0, bendRange = 2, mod = 0;
    bool sustain = false;
    int rpn = 0x3FFF;
};

class Synth {
public:
    Synth(std::shared_ptr<const Bank> bank, const Song& song) : bank_(std::move(bank)), song_(song) { Reset(); }

    void Reset() {
        for (auto& c : ch_) c = Channel{};
        for (auto& v : voices_) v = Voice{};
        next_ = 0;
        now_ = 0;
    }

    uint64_t Now() const { return now_; }

    // Renders up to `frames` frames into out (overwritten), or only runs time forward when out is null.
    size_t Render(float* out, size_t frames) {
        frames = (size_t)std::min<uint64_t>(frames, song_.length - std::min(now_, song_.length));
        if (out) std::fill(out, out + frames * 2, 0.0f);
        size_t done = 0;
        while (done < frames) {
            while (next_ < song_.events.size() && song_.events[next_].time <= now_) Handle(song_.events[next_++]);
            uint64_t until = now_ + std::min<uint64_t>(frames - done, kBlock);
            if (next_ < song_.events.size()) until = std::min(until, std::max(song_.events[next_].time, now_ + 1));
            const size_t n = (size_t)(until - now_);
            for (auto& v : voices_)
                if (v.active) Run(v, out ? out + done * 2 : nullptr, n);
            done += n;
            now_ += n;
        }
        return done;
    }

private:
    void Handle(const Event& e) {
        const int c = e.status & 15;
        Channel& ch = ch_[c];
        switch (e.status >> 4) {
            case 0x8: NoteOff(c, e.a); break;
            case 0x9: e.b ? NoteOn(c, e.a, e.b) : NoteOff(c, e.a); break;
            case 0xB:
                switch (e.a) {
                    case 0: ch.bankMsb = e.b; break;
                    case 32: ch.bankLsb = e.b; break;
                    case 1: ch.mod = e.b / 127.0; break;
                    case 7: ch.volume = e.b / 127.0; break;
                    case 10: ch.pan = (e.b - 64) / 64.0; break;
                    case 11: ch.expression = e.b / 127.0; break;
                    case 64:
                        ch.sustain = e.b >= 64;
                        if (!ch.sustain)
                            for (auto& v : voices_)
                                if (v.active && v.channel == c && v.sustained) Release(v);
                        break;
                    case 100: ch.rpn = (ch.rpn & 0x3F80) | e.b; break;
                    case 101: ch.rpn = (ch.rpn & 0x7F) | e.b << 7; break;
                    case 6: if (ch.rpn == 0) ch.bendRange = e.b; break;
                    case 38: if (ch.rpn == 0) ch.bendRange = (int)ch.bendRange + e.b / 100.0; break;
                    case 120: case 123:
                        for (auto& v : voices_)
                            if (v.active && v.channel == c) {
                                if (e.a == 120) v.active = false;
                                else Release(v);
                            }
                        break;
                    case 121:
                        ch.mod = ch.bend = 0;
                        ch.expression = 1;
                        ch.sustain = false;
                        ch.rpn = 0x3FFF;
                        break;
                    default: break;
                }
                break;
            case 0xC: ch.program = e.a; break;
            case 0xE: ch.bend = (((e.b << 7) | e.a) - 8192) / 8192.0; break;
            default: break;
        }
    }

    void NoteOn(int c, int key, int velocity) {
        const Channel& ch = ch_[c];
        const bool drums = c == 9;
        const Instrument* ins = bank_->Find(drums ? 0 : ch.bankMsb, ch.program, drums);
        if (!ins) return;
        for (auto& v : voices_)  // the same key again: let the old note go
            if (v.active && v.channel == c && v.key == key && !v.released) Release(v);
        for (const Region& r : ins->regions) {
            if (key < r.keyLo || key > r.keyHi || velocity < r.velLo || velocity > r.velHi) continue;
            if (r.exclusive)
                for (auto& v : voices_)
                    if (v.active && v.channel == c && v.r->exclusive == r.exclusive) v.active = false;
            Voice& v = Allocate();
            v = Voice{};
            v.r = &r;
            v.active = true;
            v.channel = c;
            v.key = key;
            v.age = ++age_;
            v.pos = (uint64_t)r.start << 32;
            v.baseCents = (key - r.root) * r.scale + r.tune + 1200 * std::log2(r.rate / kRate);
            // The SoundFont default: velocity attenuates on a concave curve, 96 dB at its floor.
            v.velocityDb = -40 * std::log10(std::max(velocity, 1) / 127.0);
            v.hold = r.hold * std::pow(2.0, r.holdKey * (60 - key) / 1200);
            v.decay = r.decay * std::pow(2.0, r.decayKey * (60 - key) / 1200);
            v.stage = r.delay > 0 ? 0 : 1;
            if (r.filterHz < 18000) {
                const double w = 2 * 3.14159265358979 * std::min(r.filterHz, kRate * 0.45) / kRate;
                const double q = std::max(0.7071, std::pow(10.0, r.filterQ / 20)), alpha = std::sin(w) / (2 * q);
                const double cw = std::cos(w), a0 = 1 + alpha;
                v.b0 = v.b2 = (float)((1 - cw) / 2 / a0);
                v.b1 = (float)((1 - cw) / a0);
                v.a1 = (float)(-2 * cw / a0);
                v.a2 = (float)((1 - alpha) / a0);
                v.filtered = true;
            }
        }
    }

    void NoteOff(int c, int key) {
        for (auto& v : voices_)
            if (v.active && v.channel == c && v.key == key && !v.released) {
                if (ch_[c].sustain) v.sustained = true;
                else Release(v);
            }
    }

    static void Release(Voice& v) {
        if (v.released) return;
        v.released = true;
        v.sustained = false;
        v.releaseFromDb = v.stage == 1 ? -20 * std::log10(std::max(v.attackGain, 1e-5)) : v.levelDb;
        if (v.stage == 0) v.releaseFromDb = kSilentDb;
        v.stage = 5;
        v.stageTime = 0;
    }

    // A free voice, else the oldest released one, else the oldest.
    Voice& Allocate() {
        Voice* best = nullptr;
        for (auto& v : voices_) {
            if (!v.active) return v;
            if (!best || (v.released && !best->released) || (v.released == best->released && v.age < best->age)) best = &v;
        }
        return *best;
    }

    // The envelope's gain after dt more seconds.
    static double Envelope(Voice& v, double dt) {
        const Region& r = *v.r;
        v.stageTime += dt;
        for (;;) {
            switch (v.stage) {
                case 0:
                    if (v.stageTime < r.delay) return 0;
                    v.stageTime -= r.delay;
                    v.stage = 1;
                    continue;
                case 1:
                    if (v.stageTime < r.attack) {
                        v.attackGain = v.stageTime / r.attack;
                        return v.attackGain;
                    }
                    v.stageTime -= r.attack;
                    v.attackGain = 1;
                    v.stage = 2;
                    continue;
                case 2:
                    if (v.stageTime < v.hold) return 1;
                    v.stageTime -= v.hold;
                    v.stage = 3;
                    continue;
                case 3: {
                    // Decay falls in dB at the rate that would reach silence in the decay time.
                    const double db = v.decay > 0 ? kSilentDb * v.stageTime / v.decay : kSilentDb;
                    if (db < r.sustain) {
                        v.levelDb = db;
                        break;
                    }
                    v.levelDb = r.sustain;
                    v.stage = 4;
                    continue;
                }
                case 4: v.levelDb = r.sustain; break;
                default: {
                    const double db = v.releaseFromDb + (r.release > 0 ? kSilentDb * v.stageTime / r.release : kSilentDb);
                    v.levelDb = db;
                    if (db >= kSilentDb) v.active = false;
                    break;
                }
            }
            return v.levelDb >= kSilentDb ? 0 : std::pow(10.0, -v.levelDb / 20);
        }
    }

    void Run(Voice& v, float* out, size_t n) {
        const Region& r = *v.r;
        const Channel& ch = ch_[v.channel];
        const double dt = (double)n / kRate;
        // Vibrato: a triangle LFO after its delay, plus the modulation wheel's 50 cents.
        v.vibTime += dt;
        double vib = 0;
        if (v.vibTime > r.vibDelay) {
            const double phase = std::fmod((v.vibTime - r.vibDelay) * r.vibHz, 1.0);
            vib = (phase < 0.5 ? 4 * phase - 1 : 3 - 4 * phase) * (r.vibCents + ch.mod * 50);
        }
        const double cents = v.baseCents + ch.bend * ch.bendRange * 100 + vib;
        v.step = (uint64_t)(std::pow(2.0, cents / 1200) * 4294967296.0);
        const double env = Envelope(v, dt);
        const double db = r.attenuation + v.velocityDb;
        const float target = (float)(env * std::pow(10.0, -db / 20) * ch.volume * ch.volume * ch.expression * ch.expression * 0.3);
        const double pan = std::clamp(r.pan + ch.pan, -1.0, 1.0);
        const float left = (float)std::sqrt((1 - pan) / 2), right = (float)std::sqrt((1 + pan) / 2);
        const float start = v.gain, slope = (target - start) / n;
        v.gain = target;
        const int16_t* d = r.data;
        const bool looping = r.loopMode == 1 || (r.loopMode == 3 && !v.released);
        const uint64_t loopEnd = (uint64_t)r.loopEnd << 32, span = (uint64_t)(r.loopEnd - r.loopStart) << 32;
        if (!out) {  // seeking: only the position moves
            v.pos += v.step * n;
            if (looping && v.pos >= loopEnd) v.pos = loopEnd - span + (v.pos - loopEnd) % span;
            else if (!looping && (v.pos >> 32) + 1 >= r.end) v.active = false;
            return;
        }
        for (size_t i = 0; i < n; ++i) {
            if (looping)
                while (v.pos >= loopEnd) v.pos -= span;
            const uint32_t idx = (uint32_t)(v.pos >> 32);
            if (!looping && idx + 1 >= r.end) {
                v.active = false;
                break;
            }
            {
                const float a = d[idx], b = looping && idx + 1 == r.loopEnd ? d[r.loopStart] : d[idx + 1];
                float x = (a + (b - a) * ((float)(uint32_t)v.pos * (1.0f / 4294967296.0f))) * (1.0f / 32768.0f);
                if (v.filtered) {
                    const float y = v.b0 * x + v.b1 * v.x1 + v.b2 * v.x2 - v.a1 * v.y1 - v.a2 * v.y2;
                    v.x2 = v.x1;
                    v.x1 = x;
                    v.y2 = v.y1;
                    v.y1 = y;
                    x = y;
                }
                const float g = start + slope * (float)i;
                out[i * 2] += x * g * left;
                out[i * 2 + 1] += x * g * right;
            }
            v.pos += v.step;
        }
    }

    std::shared_ptr<const Bank> bank_;
    const Song& song_;
    std::array<Channel, 16> ch_;
    std::array<Voice, kMaxVoices> voices_;
    size_t next_ = 0;
    uint64_t now_ = 0, age_ = 0;
};

class MidiDecoder final : public Decoder {
public:
    MidiDecoder(Song song, std::shared_ptr<const Bank> bank) : song_(std::move(song)), synth_(std::move(bank), song_) {}

    size_t Read(float* out, size_t frames) override {
        const size_t n = synth_.Render(out, frames);
        for (size_t i = 0; i < n * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
        return n;
    }
    bool Seek(double seconds) override {
        if (!std::isfinite(seconds)) return false;
        const uint64_t target = std::min<uint64_t>(song_.length, (uint64_t)(std::max(0.0, seconds) * kRate));
        if (target < synth_.Now()) synth_.Reset();
        while (synth_.Now() < target)
            if (!synth_.Render(nullptr, (size_t)std::min<uint64_t>(target - synth_.Now(), 1 << 16))) break;
        return true;
    }
    double Duration() const override { return (double)song_.length / kRate; }
    unsigned Rate() const override { return kRate; }

private:
    Song song_;
    Synth synth_;
};

}  // namespace

void SetMidiSoundFontFolder(const std::wstring& folder) { g_soundFontFolder = folder; }

std::unique_ptr<Decoder> OpenMidiDecoder(const std::wstring& path) {
    Song song;
    if (!LoadSong(path, song)) return nullptr;
    const std::wstring bankPath = BankFor(path);
    auto bank = bankPath.empty() ? nullptr : LoadBank(bankPath);
    if (!bank) return nullptr;
    return std::make_unique<MidiDecoder>(std::move(song), std::move(bank));
}

bool ReadMidiTags(const std::wstring& path, TagData& out) {
    Song song;
    if (!LoadSong(path, song)) return false;
    out.title = song.title.empty() ? ToUtf8(StemOf(path)) : song.title;
    out.comment = song.copyright;
    out.duration = (double)song.length / kRate;
    out.sampleRate = kRate;
    out.channels = 2;
    out.codec = Codec::MIDI;
    return true;
}
