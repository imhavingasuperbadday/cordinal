// The Linux sound streams (src/linux/sound_stream.h) against a real server, for core_test and
// tools/audio-check.sh:
//   sound-pattern SECONDS [RATE [TARGET [STALL_MS]]]  play a known pattern and report the timing
//   sound-compare FILE [RATE [SECONDS]]               check that a capture of it holds the pattern
//                                                     bit for bit, for at least SECONDS
//   sound-fuzz [ITERATIONS [SEED]]                    the protocol parsers on mutated messages
//   sound-pod                                         the POD builder and reader on known values
#pragma once
#include "linux/pulse_native.h"
#include "linux/pw_native.h"
#include "linux/sound_stream.h"
#include "linux/spa_pod.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace sound_checks {

// Frame `i` of the pattern: 24-bit values, exact in float, different on each channel and never two
// frames alike within a minute, so a capture can be aligned and compared sample for sample.
inline float PatternSample(uint64_t i, int channel) {
    uint64_t x = (i + 1) * 0x9E3779B97F4A7C15ull + (uint64_t)channel * 0xBF58476D1CE4E5B9ull;
    x ^= x >> 31;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 29;
    int32_t v = (int32_t)((x >> 40) & 0xffffff) - 0x800000;  // [-2^23, 2^23)
    return (float)v / (float)(1 << 24);                        // [-0.5, 0.5)
}

// Plays the pattern for `seconds` and prints how the reported position kept up with the clock.
// `stallMs` makes one render call (one second in) take that long: an underrun the stream must ride out.
inline int PlayPattern(double seconds, unsigned rate, const char* target, int stallMs) {
    std::atomic<uint64_t> rendered{0}, played{0};
    std::atomic<unsigned> serverRate{0};
    std::atomic<bool> stalled{false};
    lx::sound::Params p;
    p.name = "cordinal-pattern";
    p.rate = rate;
    p.latencyFrames = rate / 50;
    p.exclusive = getenv("CORDINAL_EXCLUSIVE") != nullptr;
    if (target) p.target = target;
    lx::sound::Callbacks cb;
    cb.render = [&](float* out, uint32_t frames) {
        uint64_t at = rendered;
        for (uint32_t f = 0; f < frames; ++f) {
            out[f * 2] = PatternSample(at + f, 0);
            out[f * 2 + 1] = PatternSample(at + f, 1);
        }
        rendered = at + frames;
        if (stallMs > 0 && !stalled && at > rate) {
            stalled = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(stallMs));
        }
    };
    cb.timing = [&](uint64_t p2, unsigned r) {
        played = p2;
        serverRate = r;
    };
    std::atomic<bool> failed{false};
    std::string error;
    auto stream = lx::sound::Open(p, cb, [&] { failed = true; }, error);
    if (!stream) {
        fprintf(stderr, "no stream: %s\n", error.c_str());
        return 1;
    }
    printf("backend %s, %u Hz%s%s%s\n", stream->Backend(), rate, p.exclusive ? ", exclusive" : "", target ? ", target " : "",
           target ? target : "");
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    std::vector<std::pair<double, uint64_t>> samples;  // (seconds since start, played)
    while (clock::now() - start < std::chrono::duration<double>(seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        double t = std::chrono::duration<double>(clock::now() - start).count();
        samples.push_back({t, played.load()});
        if (failed) break;
    }
    stream.reset();
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    check(!failed, "the stream stayed up");
    // The played position over the last part of the run must move at the stream's rate: the first
    // second is start-up (buffers filling, the stall).
    size_t from = 0;
    while (from < samples.size() && samples[from].first < std::min(1.5 + stallMs / 1000.0, seconds / 2)) ++from;
    if (from + 2 < samples.size()) {
        double dt = samples.back().first - samples[from].first;
        double df = (double)(samples.back().second - samples[from].second);
        double speed = df / (dt * rate);
        printf("     played %.0f frames in %.3f s: %.4f x real time\n", df, dt, speed);
        check(std::fabs(speed - 1) < 0.02, "the position moves at the stream's rate");
    } else {
        check(false, "the run was long enough to measure");
    }
    uint64_t r = rendered, pl = played;
    double latency = r > pl ? (double)(r - pl) * 1000.0 / rate : 0;
    printf("     rendered %llu, played %llu: %.1f ms ahead of the device; server at %u Hz\n", (unsigned long long)r,
           (unsigned long long)pl, latency, serverRate.load());
    check(pl > 0 && pl <= r, "played never runs ahead of rendered");
    check(latency < 250, "the stream stays within a quarter second of the device");
    return failures;
}

// A float32 stereo WAV capture must contain the pattern: found once, then frame after frame without a
// changed sample. Silence before it (and after it) is allowed. With `gaps`, silence may also come
// in the middle (an underrun, a move to another device, a server restart) and the pattern may
// resume further on, but every sound frame must still be a pattern frame, in order, unchanged.
inline int ComparePattern(const char* file, unsigned rate, double minSeconds, bool gaps = false) {
    FILE* f = fopen(file, "rb");
    if (!f) {
        fprintf(stderr, "can't open %s\n", file);
        return 1;
    }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    // RIFF chunks: fmt gives the format, data the frames.
    uint16_t format = 0, channels = 0, bits = 0;
    const uint8_t* frames = nullptr;
    size_t bytes = 0;
    for (size_t at = 12; at + 8 <= data.size();) {
        uint32_t size;
        memcpy(&size, &data[at + 4], 4);
        if (!memcmp(&data[at], "fmt ", 4) && at + 24 <= data.size()) {
            memcpy(&format, &data[at + 8], 2);
            memcpy(&channels, &data[at + 10], 2);
            memcpy(&bits, &data[at + 22], 2);
            if (format == 0xFFFE && at + 34 <= data.size()) memcpy(&format, &data[at + 32], 2);  // extensible
        } else if (!memcmp(&data[at], "data", 4)) {
            frames = &data[at + 8];
            bytes = std::min<size_t>(size, data.size() - at - 8);
            break;
        }
        at += 8 + size + (size & 1);
    }
    if (!frames || channels != 2 || format != 3 || bits != 32) {
        fprintf(stderr, "%s: want a stereo float32 WAV (format %u, %u channels, %u bits)\n", file, format, channels, bits);
        return 1;
    }
    size_t count = bytes / 8;
    auto sample = [&](size_t i, int c) {
        float v;
        memcpy(&v, frames + i * 8 + c * 4, 4);
        return v;
    };
    // Where the pattern's first frames are (the stream may have started before the capture).
    size_t start = count, offset = 0;
    for (size_t i = 0; i < count && start == count; ++i) {
        if (sample(i, 0) == 0 && sample(i, 1) == 0) continue;
        for (uint64_t k = 0; k < 48000 * 30; ++k) {
            if (PatternSample(k, 0) == sample(i, 0) && PatternSample(k, 1) == sample(i, 1)) {
                start = i;
                offset = (size_t)k;
                break;
            }
        }
        if (start == count) {
            printf("FAIL frame %zu (%g, %g) is not in the pattern\n", i, sample(i, 0), sample(i, 1));
            return 1;
        }
    }
    if (start == count) {
        printf("FAIL the capture is silent\n");
        return 1;
    }
    size_t same = 0, mismatches = 0, firstBad = count, gapCount = 0;
    size_t i = start;
    for (; i < count; ++i) {
        uint64_t k = offset + (i - start);
        float l = sample(i, 0), r = sample(i, 1);
        if (l == PatternSample(k, 0) && r == PatternSample(k, 1)) {
            ++same;
            continue;
        }
        if (l == 0 && r == 0) {  // the stream ended (or the capture outlived it): only silence may follow
            size_t j = i;
            while (j < count && sample(j, 0) == 0 && sample(j, 1) == 0) ++j;
            if (j == count) break;
            if (gaps) {  // a gap: the pattern must pick up again at or after where it stopped
                bool found = false;
                for (uint64_t k2 = k; k2 < k + (uint64_t)rate * 30 && !found; ++k2) {
                    if (PatternSample(k2, 0) == sample(j, 0) && PatternSample(k2, 1) == sample(j, 1)) {
                        found = true;
                        printf("     gap: %zu silent frames at capture frame %zu, then the pattern %lld frames on\n", j - i,
                               i, (long long)(k2 - k));
                        start = j;
                        offset = (size_t)k2;
                    }
                }
                if (found) {
                    ++gapCount;
                    i = j - 1;
                    continue;
                }
            }
        }
        if (firstBad == count) firstBad = i;
        ++mismatches;
    }
    if (gaps) printf("     %zu gaps\n", gapCount);
    printf("     pattern frame %zu at capture frame %zu; %zu frames equal, %zu differ\n", offset, start, same, mismatches);
    if (firstBad != count) {
        uint64_t k = offset + (firstBad - start);
        printf("     first difference at capture frame %zu: (%.9g, %.9g), want (%.9g, %.9g)\n", firstBad, sample(firstBad, 0),
               sample(firstBad, 1), PatternSample(k, 0), PatternSample(k, 1));
    }
    bool ok = mismatches == 0 && same >= (size_t)(minSeconds * rate);
    printf("%s the capture holds the pattern bit for bit (%.2f s)\n", ok ? "ok  " : "FAIL", same / (double)rate);
    return ok ? 0 : 1;
}

// Valid messages of every kind the clients parse, then each mutated many times (bytes flipped,
// cut short, sizes and counts set to extremes) and random bytes. Nothing may crash or read
// outside its input: run under -fsanitize=address,undefined.
inline int Fuzz(long iterations, unsigned seed) {
    using lx::spa::Builder;
    std::vector<std::vector<uint8_t>> seeds;
    {
        Builder b;  // a registry global
        b.BeginStruct();
        b.Int(31);
        b.Int(0x1ff);
        b.Str("PipeWire:Interface:Node");
        b.Int(3);
        b.Dict({{"node.name", "test-sink-a"}, {"media.class", "Audio/Sink"}, {"object.serial", "77"}});
        b.End();
        seeds.push_back(b.bytes);
    }
    {
        Builder b;  // port_use_buffers with metas and both data kinds
        b.BeginStruct();
        for (int v : {1, 0, -1, 0, 2}) b.Int(v);
        for (int i = 0; i < 2; ++i) {
            b.Int(5);
            b.Int(0);
            b.Int(4096);
            b.Int(2);
            b.IdV(1);
            b.Int(16);
            b.IdV(2);
            b.Int(24);
            b.Int(1);
            b.IdV(i ? 1 : 4);
            b.Int(i ? 64 : 6);
            b.Int(0);
            b.Int(0);
            b.Int(32768);
        }
        b.End();
        seeds.push_back(b.bytes);
    }
    {
        Builder b;  // port_set_param with a DSP format
        b.BeginStruct();
        b.Int(1);
        b.Int(0);
        b.IdV(4);
        b.Int(0);
        b.BeginObject(0x40003, 4);
        b.Prop(1);
        b.IdV(1);
        b.Prop(2);
        b.IdV(2);
        b.Prop(0x10001);
        b.ChoiceId(lx::spa::ChoiceEnum, {518, 518, 5});
        b.End();
        b.End();
        seeds.push_back(b.bytes);
    }
    {
        Builder b;  // metadata property
        b.BeginStruct();
        b.Int(0);
        b.Str("default.audio.sink");
        b.Str("Spa:String:JSON");
        b.Str("{\"name\":\"test-sink-b\", \"x\": [1,2], \"esc\":\"a\\\"b\"}");
        b.End();
        seeds.push_back(b.bytes);
    }
    {
        Builder b;  // transport / set_activation / mix info
        b.BeginStruct();
        b.FdV(0);
        b.FdV(1);
        b.Int(3);
        b.Int(0);
        b.Int(2312);
        b.End();
        seeds.push_back(b.bytes);
        Builder m;
        m.BeginStruct();
        for (int v : {1, 0, 0, 33}) m.Int(v);
        m.Dict({{"k", "v"}});
        m.End();
        seeds.push_back(m.bytes);
    }
    // PulseAudio tagstructs: a sink info list entry and a create-stream reply.
    seeds.push_back(lx::pulse::FuzzSeed(0));
    seeds.push_back(lx::pulse::FuzzSeed(1));
    seeds.push_back(lx::pulse::FuzzSeed(2));
    std::mt19937 rng(seed);
    long parsed = 0;
    std::vector<uint8_t> m;
    for (long it = 0; it < iterations; ++it) {
        const auto& s = seeds[(size_t)it % seeds.size()];
        m = s;
        int edits = 1 + (int)(rng() % 8);
        for (int e = 0; e < edits && !m.empty(); ++e) {
            size_t at = rng() % m.size();
            switch (rng() % 6) {
                case 0: m[at] ^= (uint8_t)(1u << (rng() % 8)); break;
                case 1: m[at] = (uint8_t)rng(); break;
                case 2: m.resize(at); break;  // cut short
                case 3: {  // a 32-bit word to an extreme (sizes, counts, types)
                    static const uint32_t extremes[] = {0, 1, 7, 8, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffff8, 64, 65};
                    uint32_t v = extremes[rng() % 10];
                    size_t w = at & ~(size_t)3;
                    if (w + 4 <= m.size()) memcpy(&m[w], &v, 4);
                    break;
                }
                case 4: m.insert(m.begin() + (std::ptrdiff_t)at, (uint8_t)rng()); break;
                default: {  // copy a stretch over another
                    size_t from = rng() % m.size(), len = std::min<size_t>(rng() % 32, m.size() - std::max(from, at));
                    memmove(&m[at], &m[from], len);
                }
            }
        }
        if (it % 7 == 0) {  // pure noise now and then
            m.resize(rng() % 512);
            for (auto& byte : m) byte = (uint8_t)rng();
        }
        // An exactly sized copy, so the sanitizer sees any read past the end.
        std::unique_ptr<uint8_t[]> exact(new uint8_t[m.size() ? m.size() : 1]);
        if (!m.empty()) memcpy(exact.get(), m.data(), m.size());
        parsed += lx::pw::FuzzParse(exact.get(), m.size());
        parsed += lx::pulse::FuzzParse(exact.get(), m.size());
    }
    printf("ok   %ld mutated messages parsed without a fault (%ld parses succeeded)\n", iterations, parsed);
    return 0;
}

// Known encodings: the builder's bytes for a few values, and the reader taking them back.
inline int CheckPod() {
    using lx::spa::Builder;
    using lx::spa::Reader;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    Builder b;
    b.BeginStruct();
    b.Int(-5);
    b.Str("abc");
    b.LongV(1ll << 40);
    b.BoolV(true);
    b.NoneV();
    b.Dict({{"a", "b"}});
    b.End();
    // Struct{Int -5, String "abc", Long 2^40, Bool true, None, Struct{Int 1, String a, String b}}.
    static const uint8_t want[] = {
        0x80, 0, 0, 0, 14, 0, 0, 0,                          // Struct, 128 bytes (libspa's builder writes the same)
        4, 0, 0, 0, 4, 0, 0, 0, 0xfb, 0xff, 0xff, 0xff, 0, 0, 0, 0,  // Int -5, padded
        4, 0, 0, 0, 8, 0, 0, 0, 'a', 'b', 'c', 0, 0, 0, 0, 0,  // String "abc"
        8, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0,        // Long
        4, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,        // Bool
        0, 0, 0, 0, 1, 0, 0, 0,                                // None
        0x30, 0, 0, 0, 14, 0, 0, 0,                            // Struct, 48 bytes
        4, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
        2, 0, 0, 0, 8, 0, 0, 0, 'a', 0, 0, 0, 0, 0, 0, 0,
        2, 0, 0, 0, 8, 0, 0, 0, 'b', 0, 0, 0, 0, 0, 0, 0,
    };
    check(b.bytes.size() == sizeof(want) && !memcmp(b.bytes.data(), want, sizeof(want)), "the builder writes SPA's layout");
    Reader r(b.bytes.data(), b.bytes.size());
    Reader s = r.Struct();
    int32_t i = s.Int();
    std::string str = s.Str();
    int64_t l = s.LongV();
    bool t = s.BoolV();
    std::string none;
    bool hadString = s.StrOrNull(none);
    std::vector<std::pair<std::string, std::string>> d;
    s.Dict(d);
    check(s.Ok() && i == -5 && str == "abc" && l == (1ll << 40) && t && !hadString && d.size() == 1 && d[0].first == "a" &&
              d[0].second == "b" && s.AtEnd(),
          "the reader takes every value back");
    Reader wrong(b.bytes.data(), b.bytes.size());
    Reader ws = wrong.Struct();
    ws.Str();  // an Int is next
    ws.Int();
    check(!ws.Ok(), "a value of the wrong type fails the reader");
    Reader cut(b.bytes.data(), b.bytes.size() - 9);
    cut.Struct();
    check(!cut.Ok(), "a struct cut short fails");
    Builder o;
    o.BeginObject(0x40003, 3);
    o.Prop(1);
    o.IdV(1);
    o.Prop(0x10001);
    o.ChoiceId(lx::spa::ChoiceEnum, {518, 518, 5});
    o.End();
    Reader orr(o.bytes.data(), o.bytes.size());
    lx::spa::ObjectView ov = lx::spa::ParseObject(orr.ObjectV());
    uint32_t mt = 0, fmt = 0;
    check(ov.ok && ov.type == 0x40003 && ov.id == 3 && lx::spa::PropId(ov, 1, mt) && mt == 1 &&
              lx::spa::PropId(ov, 0x10001, fmt) && fmt == 518 && !lx::spa::PropId(ov, 2, mt),
          "object properties, a choice's default and a missing key");
    return failures;
}

}  // namespace sound_checks
