// Audio decoding checks of the self test (see selftest.cpp): the per-format decoders, gapless
// joins, source ranges, game-music loops, the resampler, sound effects and CUE sheets, with the
// fixture builders they need. RunSelfTest calls each one inside its own testlog group.
#include "selftest_common.h"
#include "resource.h"
#include "audio_kernels.h"

namespace {

// Little-endian RIFF/WAVE bytes for 16-bit PCM, with `extra` chunks before the data.
std::string LeBytes(uint32_t v, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += (char)((v >> (8 * i)) & 0xFF);
    return s;
}
std::string RiffChunk(const char* id, const std::string& body) {
    return std::string(id, 4) + LeBytes((uint32_t)body.size(), 4) + body + (body.size() & 1 ? std::string(1, '\0') : std::string());
}
std::string WavFile(unsigned rate, unsigned channels, const std::string& pcm, const std::string& extra) {
    std::string fmt = LeBytes(1, 2) + LeBytes(channels, 2) + LeBytes(rate, 4) + LeBytes(rate * channels * 2, 4) +
                      LeBytes(channels * 2, 2) + LeBytes(16, 2);
    std::string body = "WAVE" + RiffChunk("fmt ", fmt) + extra + RiffChunk("data", pcm);
    return "RIFF" + LeBytes((uint32_t)body.size(), 4) + body;
}

// Every frame is different, so a sample from the wrong place can't pass for the right one.
std::string StereoRamp16(size_t frames) {
    std::string pcm;
    for (size_t i = 0; i < frames; ++i) {
        int l = (int)((i * 37) % 65536) - 32768, r = (int)((i * 91 + 5) % 65536) - 32768;
        pcm += LeBytes((uint32_t)(uint16_t)(int16_t)l, 2) + LeBytes((uint32_t)(uint16_t)(int16_t)r, 2);
    }
    return pcm;
}

std::vector<float> ReadAllFrom(Decoder& d) {
    std::vector<float> all;
    float buf[1000 * 2];
    while (size_t n = d.Read(buf, 1000)) all.insert(all.end(), buf, buf + n * 2);
    return all;
}
}  // namespace

// Gapless playback of a.flac+b.flac against a hand-authored reference: the joined decode must line
// up with the reference (lag 0, ~exact length) and its seam must be as clean as a track's interior.
// Lossy codecs are compared against FFmpeg's own decode of the same two files, since their encoders
// smear the file edges.
void TestGapless(const std::wstring& dir) {
    if (!FindFFmpeg()) testlog::Skip(L"lossy gapless seam comparison against FFmpeg: oracle unavailable");
    unsigned refRate = 0;
    auto ref = DecodeAll(dir + L"\\ref.wav", refRate);
    testlog::Cases joins;
    for (const wchar_t* ext : {L"flac", L"mp3", L"m4a", L"ogg"}) {
        unsigned ra = 0, rb = 0;
        auto a = DecodeAll(dir + L"\\a." + ext, ra);
        auto b = DecodeAll(dir + L"\\b." + ext, rb);
        std::vector<float> joined(a);
        joined.insert(joined.end(), b.begin(), b.end());
        long long lenDiff = (long long)joined.size() / 2 - (long long)ref.size() / 2;
        // Best alignment against the reference, and error power at the seam vs. inside a track.
        auto err = [&](long long lag, size_t from, size_t to) {
            double e = 0, s = 0;
            for (size_t i = from; i < to; ++i) {
                long long j = (long long)i + lag;
                if (j < 0 || (size_t)j * 2 + 1 >= joined.size() || i * 2 + 1 >= ref.size()) continue;
                double d = joined[(size_t)j * 2] - ref[i * 2];
                e += d * d;
                s += (double)ref[i * 2] * ref[i * 2];
            }
            return 10 * std::log10(s / std::max(e, 1e-20));
        };
        long long bestLag = 0;
        double best = -1e9;
        for (long long lag = -64; lag <= 64; ++lag) {
            double snr = err(lag, 20000, 60000);
            if (snr > best) { best = snr; bestLag = lag; }
        }
        size_t seam = a.size() / 2;
        double seamSnr = err(bestLag, seam - 2048, seam + 2048), bodySnr = err(bestLag, 20000, 60000);
        // Lossy codecs smear their own file edges, so compare the seam with FFmpeg's decode of
        // the same two files: matching it means the trimming is right.
        double vsReference = 999;
        if (FindFFmpeg() && std::wstring(ext) != L"flac") {
            std::vector<float> ff;
            for (const wchar_t* part : {L"a.", L"b."}) {
                auto d = OpenFFmpegDecoder(dir + L"\\" + part + ext, ra);
                float buf[4096 * 2];
                while (d && true) {
                    size_t n = d->Read(buf, 4096);
                    if (!n) break;
                    ff.insert(ff.end(), buf, buf + n * 2);
                }
            }
            auto agree = [&](size_t from, size_t to) {
                double e = 0, s = 0;
                for (size_t i = from; i < to && i * 2 < ff.size() && i * 2 < joined.size(); ++i) {
                    double dd = joined[i * 2] - ff[i * 2];
                    e += dd * dd;
                    s += (double)ff[i * 2] * ff[i * 2];
                }
                return 10 * std::log10(s / std::max(e, 1e-20));
            };
            // Different decoders (Media Foundation vs FFmpeg AAC) differ slightly everywhere;
            // the seam must agree as well as the inside of a track does.
            vsReference = agree(seam - 2048, seam + 2048) - std::min(agree(20000, 60000), 60.0) + 60;
        }
        bool lossless = std::wstring(ext) == L"flac";
        // Media Foundation's AAC decoder rebuilds part of the first block after the priming
        // frame differently from FFmpeg's (window-shape start-up), ~35 dB agreement over that
        // block; the trimming itself is sample exact (lag 0, exact length).
        double threshold = std::wstring(ext) == L"m4a" ? 30 : 40;
        bool ok = bestLag == 0 && std::llabs(lenDiff) <= 1 && (lossless ? seamSnr > 90 : vsReference > threshold);
        wchar_t detail[160];
        swprintf_s(detail, L"%ls lag=%lld length_diff=%lld snr_body=%.1f dB snr_seam=%.1f dB seam_vs_ffmpeg=%.1f dB", ext,
                   bestLag, lenDiff, bodySnr, seamSnr, vsReference);
        joins.Add(ok, detail);
    }
    Check(joins.Passed(), L"GAPLESS FLAC, MP3, M4A and Ogg joins line up with the reference and keep a clean seam%ls",
          joins.failed.c_str());
}

// Resampler: chunked pushes must match one whole push exactly, the length and a resampled 1 kHz tone
// must be right, and every rate pair (up, down, equal, extreme) must preserve a stereo DC level.
void TestResampler() {
    if (nativeaudio::FastAudioKernels()) {
        // Reproduce the original lane arithmetic with scalar FMA. Synthetic rows
        // expose a misplaced coefficient even when a DC or tone check would pass.
        bool exact = true;
        std::vector<float> table(257 * 32), history(256 * 2);
        for (size_t i = 0; i < table.size(); ++i) table[i] = (float)((int)(i * 37 % 211) - 105) / 211;
        for (size_t i = 0; i < history.size(); ++i) history[i] = (float)((int)(i * 73 % 509) - 254) / 127;
        for (unsigned denominator : {1u, 6u, 147u, 160u, 256u, 257u}) {
            auto prepared = table;
            if (denominator <= 256) nativeaudio::AudioResamplePrepareAvx2(prepared.data(), denominator);
            constexpr unsigned frames = 37;
            float actual[frames * 2 + 2], expected[frames * 2];
            std::fill_n(actual, frames * 2 + 2, 123.0f);
            size_t index = 15, referenceIndex = index;
            uint64_t numerator = denominator - 1, referenceNum = numerator;
            const unsigned remainder = denominator / 2;
            nativeaudio::AudioResampleAvx2(history.data(), prepared.data(), frames, 1, remainder, denominator,
                                         index, numerator, actual + 1);
            for (unsigned frame = 0; frame < frames; ++frame) {
                const double phase = (unsigned)referenceNum * (256.0 / denominator);
                const unsigned p = (unsigned)phase;
                const float blend = (float)(phase - p);
                float a[8]{}, b[8]{};
                for (unsigned k = 0; k < 32; k += 8) for (unsigned lane = 0; lane < 8; ++lane) {
                    const unsigned tap = k + lane / 2;
                    const float lo = std::fma(table[(p + 1) * 32 + tap] - table[p * 32 + tap], blend, table[p * 32 + tap]);
                    const float hi = std::fma(table[(p + 1) * 32 + tap + 4] - table[p * 32 + tap + 4], blend, table[p * 32 + tap + 4]);
                    const size_t at = (referenceIndex - 15) * 2 + k * 2 + lane;
                    a[lane] = std::fma(history[at], lo, a[lane]);
                    b[lane] = std::fma(history[at + 8], hi, b[lane]);
                }
                for (unsigned c = 0; c < 2; ++c)
                    expected[frame * 2 + c] = ((a[c] + b[c]) + (a[c + 4] + b[c + 4])) +
                                              ((a[c + 2] + b[c + 2]) + (a[c + 6] + b[c + 6]));
                referenceNum += remainder; ++referenceIndex;
                if (referenceNum >= denominator) { referenceNum -= denominator; ++referenceIndex; }
            }
            exact &= memcmp(actual + 1, expected, sizeof expected) == 0 && index == referenceIndex &&
                     numerator == referenceNum && actual[0] == 123 && actual[frames * 2 + 1] == 123;
        }
        Check(exact, L"RESAMPLE prepared and interpolated AVX2 phases preserve exact FMA results, cursor and output guards");
    } else testlog::Skip(L"RESAMPLE exact AVX2 phase comparison requires AVX2/FMA");
    const unsigned in = 44100, out = 48000;
    std::vector<float> src(in * 2 * 2);
    for (size_t i = 0; i < src.size() / 2; ++i) src[i * 2] = src[i * 2 + 1] = (float)(0.5 * std::sin(2 * 3.14159265358979 * 1000 * i / in));
    Resampler one, chunked;
    one.Init(in, out);
    chunked.Init(in, out);
    std::vector<float> a, b;
    one.Push(src.data(), src.size() / 2, a);
    one.Flush(a);
    for (size_t pos = 0, step = 1; pos < src.size() / 2; pos += step, step = step * 7 % 1013 + 1)
        chunked.Push(src.data() + pos * 2, std::min(step, src.size() / 2 - pos), b);
    chunked.Flush(b);
    double e = 0, s = 0;
    for (size_t n = 200; n + 200 < a.size() / 2; ++n) {
        double ideal = 0.5 * std::sin(2 * 3.14159265358979 * 1000 * n / out);
        e += (a[n * 2] - ideal) * (a[n * 2] - ideal);
        s += ideal * ideal;
    }
    double snr = 10 * std::log10(s / e);
    Check(a == b, L"RESAMPLE chunked pushes produce identical output (%zu frames)", a.size() / 2);
    Check(std::fabs((double)a.size() / 2 - src.size() / 2.0 * out / in) <= 2 && snr > 80,
          L"RESAMPLE 44100->48000 frames=%zu snr=%.1f dB", a.size() / 2, snr);
    testlog::Cases reuse, gain;
    for (auto rates : {std::pair{8000u,48000u},std::pair{48000u,8000u},std::pair{44100u,48000u},
                       std::pair{48000u,44100u},std::pair{44100u,96000u},std::pair{96000u,48000u},
                       std::pair{48000u,48000u},std::pair{192000u,8000u},
                       std::pair{65792u,65536u},std::pair{65536u,65792u}}) {
        const std::wstring pair=std::to_wstring(rates.first)+L"->"+std::to_wstring(rates.second);
        std::vector<float> dc(8191*2);
        for (size_t i=0;i<dc.size()/2;++i) { dc[i*2]=0.25f;dc[i*2+1]=-0.125f; }
        Resampler whole,reused;whole.Init(rates.first,rates.second);reused.Init(rates.first,rates.second);
        std::vector<float> expected,actual,block(777,123.0f);
        whole.Push(dc.data(),dc.size()/2,expected);whole.Flush(expected);
        size_t position=0,chunk=1;
        while (position<dc.size()/2) {
            size_t n=std::min(chunk,dc.size()/2-position);
            reused.Process(dc.data()+position*2,n,block);
            actual.insert(actual.end(),block.begin(),block.end());position+=n;chunk=chunk*13%2053+1;
        }
        reused.Flush(actual);
        bool same=actual==expected;
        reused.Reset();reused.Process(dc.data(),dc.size()/2,block);reused.Flush(block);
        reuse.Add(same && block==expected,pair);
        bool dcGain=true;
        size_t guard=(size_t)std::max(32.0,std::ceil(32.0*rates.second/rates.first));
        for (size_t i=guard;i+guard<expected.size()/2;++i)
            dcGain&=std::abs(expected[i*2]-0.25f)<1e-6f && std::abs(expected[i*2+1]+0.125f)<1e-6f;
        gain.Add(dcGain && std::abs(expected.size()/2.0-8191.0*rates.second/rates.first)<=2,
                 pair+L" "+std::to_wstring(expected.size()/2)+L" frames"+(dcGain?L"":L", DC off"));
    }
    Check(reuse.Passed(),L"RESAMPLE reused output, chunking and reset agree at every rate pair (up, down, equal, extreme)%ls",
          reuse.failed.c_str());
    Check(gain.Passed(),L"RESAMPLE duration and stereo DC gain hold at every rate pair%ls",gain.failed.c_str());
}

// Range decoding on a synthetic ramp WAV: exact, fractional and open-ended [start, end) windows
// slice the file precisely, seeking is relative to the range start, and the "file.wav|2" sub-track
// path opens the whole file. A FLAC range is checked the same way.
void TestRanges(const std::wstring& root, const std::wstring& scratch) {
    const unsigned rate = 44100;
    const size_t frames = 2 * rate;
    std::wstring wav = scratch + L"\\ranges.wav", error;
    Ready(WriteTestBytes(wav, WavFile(rate, 2, StereoRamp16(frames), "")), L"RANGE fixture written");
    unsigned r = 0;
    auto full = DecodeAll(wav, r);
    // Start and end in frames at 44.1 kHz, exact and fractional, then open-ended.
    struct Case {
        double start, end;
    };
    bool rangesOk = true;
    for (Case c : {Case{0, 0.5}, Case{0.5, 1.0}, Case{1.0, 0}, Case{0.123456, 0.654321}, Case{1.999, 0}}) {
        auto d = OpenRange(OpenDecoder(wav, 48000, error), c.start, c.end);
        size_t from = (size_t)std::llround(c.start * rate), to = c.end > 0 ? (size_t)std::llround(c.end * rate) : frames;
        std::vector<float> got = d ? ReadAllFrom(*d) : std::vector<float>();
        bool exact = d && got.size() == (to - from) * 2 && std::equal(got.begin(), got.end(), full.begin() + from * 2);
        bool duration = d && std::fabs(d->Duration() - (double)(to - from) / rate) < 1e-9;
        rangesOk &= exact && duration;
    }
    Check(rangesOk, L"RANGE wav exact, fractional and open-ended ranges slice the file and report their duration");
    // Seeking is relative to the range start, and a seek past the end ends the track.
    auto d = OpenRange(OpenDecoder(wav, 48000, error), 0.5, 1.0);
    bool seekOk = d && d->Seek(0.25);
    float buf[100 * 2] = {};
    size_t n = d ? d->Read(buf, 100) : 0;
    size_t at = (size_t)std::llround(0.75 * rate);
    bool seekExact = n == 100 && std::equal(buf, buf + 200, full.begin() + at * 2);
    seekOk = seekOk && d->Seek(10.0) && d->Read(buf, 100) == 0;
    Check(seekExact && seekOk, L"RANGE seek is relative to the start and exact (frame %zu), past the end ends it", at);
    // "file.wav|2" opens the real file; the whole decode is unchanged.
    auto sub = DecodeAll(wav + L"|2", r);
    Check(sub == full && r == rate, L"RANGE sub-track path opens the file (%zu frames)", sub.size() / 2);
    // FLAC is lossless too: a range of the gap fixture equals the same slice of the full decode.
    unsigned fr = 0;
    auto flac = DecodeAll(root + L"\\gap2\\a.flac", fr);
    auto fd = OpenRange(OpenDecoder(root + L"\\gap2\\a.flac", 48000, error), 0.5, 1.5);
    size_t ff = (size_t)std::llround(0.5 * fr), ft = (size_t)std::llround(1.5 * fr);
    std::vector<float> fgot = fd ? ReadAllFrom(*fd) : std::vector<float>();
    bool flacSame = fd && ft <= flac.size() / 2 && fgot.size() == (ft - ff) * 2 &&
                    std::equal(fgot.begin(), fgot.end(), flac.begin() + ff * 2);
    Check(flacSame, L"RANGE flac [0.5, 1.5) frames=%zu exact=%d", fgot.size() / 2, flacSame);
}

void TestSoundEffects() {
    // Every sound, at two device rates and both levels: finite, below full scale, short, starting
    // and ending at silence (no clicks), and the same for the same seed.
    bool ok = true;
    size_t longest = 0;
    for (unsigned rate : {44100u, 48000u})
        for (int ducked = 0; ducked < 2; ++ducked)
            for (int i = 0; i < (int)Sfx::Count; ++i) {
                auto s = sfx::Render((Sfx)i, i == (int)Sfx::Rate ? 10 : 3, rate, 42, ducked != 0);
                float peak = 0;
                for (float v : s) peak = std::isfinite(v) ? std::max(peak, std::fabs(v)) : 2.0f;
                bool good = !s.empty() && peak > 0.05f && peak < 0.96f && s.size() < rate * 2.5 && std::fabs(s.front()) < 0.01f &&
                            std::fabs(s.back()) < 0.001f;
                if (!good) fwprintf(g_log, L"  sound %d rate=%u ducked=%d: %zu samples, peak %.3f\n", i, rate, ducked, s.size(), peak);
                ok = ok && good;
                longest = std::max(longest, s.size() * 1000 / rate);
            }
    Check(ok, L"SFX all sounds render cleanly (longest %zu ms)", longest);
    Check(sfx::Render(Sfx::Dice, 0, 48000, 7, false) == sfx::Render(Sfx::Dice, 0, 48000, 7, false) &&
              sfx::Render(Sfx::Dice, 0, 48000, 7, false) != sfx::Render(Sfx::Dice, 0, 48000, 8, false),
          L"SFX same seed, same sound; another seed varies it");
    size_t one = sfx::Render(Sfx::Rate, 2, 48000, 1, false).size(), five = sfx::Render(Sfx::Rate, 10, 48000, 1, false).size();
    Check(five > one, L"SFX five stars play a longer run than one (%zu > %zu samples)", five, one);

    // The DELTARUNE theme's sounds are embedded as FLAC. Frame counts and FNV-1a hashes of the 16-bit
    // samples come from the game's original WAVs, so anything short of an exact decode fails.
    if (!kExtraThemes) {
        testlog::Skip(L"SFX DELTARUNE sounds: this build leaves out the extra themes (build.bat themes)");
        return;
    }
    struct GameSound { int id; size_t frames; uint32_t hash; };
    const GameSound sounds[] = {
        {IDR_SND_SELECT, 4127, 0x388e9c2e}, {IDR_SND_MENUMOVE, 516, 0xe4874518}, {IDR_SND_NOISE, 1600, 0x151f08c2},
        {IDR_SND_SAVE, 12480, 0xc1fcc2be}, {IDR_SND_BELL, 10400, 0x15f077fa}, {IDR_SND_ERROR, 12448, 0x6dce609c},
        {IDR_SND_CANTSELECT, 5335, 0x3a2cfb27}, {IDR_SND_SWING, 16028, 0xdba96761}, {IDR_SND_LEVELUP, 21689, 0x2a14c4ee},
        {IDR_SND_TXTQ, 905, 0xcacb6601}, {IDR_SND_QUEEN_BITCRUSHLAUGH, 29932, 0xc18fdbaf}};
    bool exact = true;
    for (const GameSound& s : sounds) {
        unsigned rate = 0;
        std::vector<float> stereo = app::LoadGameSound(s.id, rate);
        uint32_t hash = 0x811C9DC5;
        bool mono = true;
        for (size_t i = 0; i + 1 < stereo.size(); i += 2) {
            hash = (hash ^ (uint16_t)(int16_t)std::lround(stereo[i] * 32768)) * 0x01000193;
            mono = mono && stereo[i + 1] == stereo[i];
        }
        bool good = rate == 22050 && stereo.size() == s.frames * 2 && hash == s.hash && mono;
        if (!good) fwprintf(g_log, L"  sound %d: %u Hz, %zu frames, hash %08x\n", s.id, rate, stereo.size() / 2, hash);
        exact = exact && good;
    }
    Check(exact, L"SFX DELTARUNE sounds decode to the game's exact samples (%zu sounds)", std::size(sounds));
}

namespace {

std::string Be24(size_t v) {
    return std::string{(char)(v >> 16), (char)(v >> 8), (char)v};
}

// A WAV smpl chunk with one loop, start and end (inclusive) in frames.
std::string SmplChunk(uint32_t start, uint32_t endInclusive) {
    return LeBytes(0, 4) + LeBytes(0, 4) + LeBytes(125000, 4) + LeBytes(60, 4) + LeBytes(0, 4) + LeBytes(0, 4) +
           LeBytes(0, 4) + LeBytes(1, 4) + LeBytes(0, 4) +  // header; the loop count is the eighth field
           LeBytes(0, 4) + LeBytes(0, 4) + LeBytes(start, 4) + LeBytes(endInclusive, 4) + LeBytes(0, 4) + LeBytes(0, 4);
}

// A FLAC file with only STREAMINFO (44.1 kHz) and a Vorbis comment block: all ReadLoopPoints reads.
std::string FlacWithComments(const std::vector<std::string>& comments) {
    std::string info(34, '\0');
    info[10] = 0x0A;
    info[11] = '\xC4';
    info[12] = 0x40;  // 44100 Hz in the top 20 bits of bytes 10-12
    std::string vc = LeBytes(1, 4) + "x" + LeBytes((uint32_t)comments.size(), 4);
    for (const std::string& c : comments) vc += LeBytes((uint32_t)c.size(), 4) + c;
    return std::string("fLaC", 4) + std::string("\x00", 1) + Be24(34) + info + std::string("\x84", 1) + Be24(vc.size()) + vc;
}
}  // namespace

// Game-music loops: the WAV loop tags, the FLAC comment tags, and the timeline they make.
void TestLoops(const std::wstring& scratch) {
    const unsigned rate = 8000;
    const std::string pcm = StereoRamp16(4000);
    auto loopOf = [&](const std::wstring& file, int64_t& s, int64_t& e) { return ReadLoopPoints(scratch + L"\\" + file, s, e); };
    auto write = [&](const char* name, const std::string& bytes) {
        return WriteTestBytes(scratch + L"\\" + std::wstring(name, name + strlen(name)), bytes);
    };
    int64_t s = 0, e = 0;

    // WAV: the smpl loop 1000..1999 (inclusive) is frames [1000, 2000).
    write("loop.wav", WavFile(rate, 2, pcm, RiffChunk("smpl", SmplChunk(1000, 1999))));
    bool wavOk = loopOf(L"loop.wav", s, e) && s == 1000 && e == 2000;
    write("plain.wav", WavFile(rate, 2, pcm, ""));
    bool noLoop = !loopOf(L"plain.wav", s, e);
    write("badloop.wav", WavFile(rate, 2, pcm, RiffChunk("smpl", SmplChunk(1000, 499))));
    bool wavBad = !loopOf(L"badloop.wav", s, e);
    Check(wavOk && noLoop && wavBad, L"LOOP tags WAV smpl: [%lld, %lld) (expect [1000, 2000)), plain WAV none, end before start ignored",
          (long long)s, (long long)e);

    // FLAC comments: LOOPLENGTH, LOOPEND (which wins over a LOOPLENGTH), key case, and bad values.
    write("length.flac", FlacWithComments({"LOOPSTART=1000", "LOOPLENGTH=1000"}));
    bool lengthOk = loopOf(L"length.flac", s, e) && s == 1000 && e == 2000;
    write("end.flac", FlacWithComments({"loopstart=1000", "LOOPLENGTH=999", "LoopEnd=2500"}));
    bool endOk = loopOf(L"end.flac", s, e) && s == 1000 && e == 2500;
    write("negative.flac", FlacWithComments({"LOOPSTART=-5", "LOOPEND=2000"}));
    bool negative = !loopOf(L"negative.flac", s, e);
    write("backwards.flac", FlacWithComments({"LOOPSTART=2000", "LOOPEND=1500"}));
    bool backwards = !loopOf(L"backwards.flac", s, e);
    Check(lengthOk && endOk && negative && backwards, L"LOOP tags FLAC comments: LOOPLENGTH=[%lld, %lld) LOOPEND wins, bad values ignored",
          (long long)s, (long long)e);

    // Decode: intro [0, 1000), the loop twice (frames 1000..2999), then 0.1 s (800 frames) of fade.
    std::wstring err;
    unsigned r = 0;
    auto file = DecodeAll(scratch + L"\\loop.wav", r);
    const size_t total = 1000 + 2 * 1000 + 800;
    auto d = OpenLooped(OpenDecoder(scratch + L"\\loop.wav", 48000, err), 1000, 2000, 2, 0.1);
    std::vector<float> out = d ? ReadAllFrom(*d) : std::vector<float>();
    double worst = 0;
    if (out.size() == total * 2) {
        for (size_t v = 0; v < total; ++v) {
            size_t f = v < 1000 ? v : 1000 + (v - 1000) % 1000;  // the file frame on the timeline
            float g = v < 3000 ? 1.0f : 1.0f - (float)(v - 3000 + 1) / 800.0f;
            for (size_t ch = 0; ch < 2; ++ch) worst = std::max(worst, (double)std::fabs(out[v * 2 + ch] - file[f * 2 + ch] * g));
        }
    }
    bool seam = out.size() >= 2002 * 2 && out[2000 * 2] == file[1000 * 2] && out[2001 * 2] == file[1001 * 2];
    Check(out.size() == total * 2 && worst < 1e-6 && seam, L"LOOP decode: %zu frames (expect %zu), seam exact=%d, worst error %.2e",
          out.size() / 2, total, seam, worst);

    // Seeks land on the timeline: frame 2500 is loop frame 1500 of the file.
    auto sd = OpenLooped(OpenDecoder(scratch + L"\\loop.wav", 48000, err), 1000, 2000, 2, 0.1);
    bool seekOk = sd && sd->Seek(2500.0 / rate);
    float buf[10 * 2] = {};
    size_t n = seekOk ? sd->Read(buf, 10) : 0;
    bool seekExact = n == 10 && std::equal(buf, buf + 20, file.begin() + 1500 * 2);
    bool duration = sd && std::fabs(sd->Duration() - (double)total / rate) < 1e-9;
    Check(seekExact && duration, L"LOOP seek to frame 2500 reads file frame 1500 exact=%d, duration %.4f s (expect 0.4750)", seekExact,
          sd ? sd->Duration() : 0.0);

    auto forever = OpenLooped(OpenDecoder(scratch + L"\\loop.wav", 48000, err), 1000, 2000, 0, 0);
    bool infiniteOk = forever && forever->Duration() == 0.5 && forever->Seek(1500.0 / rate);
    std::vector<float> repeated(3500 * 2);
    infiniteOk = infiniteOk && forever->Read(repeated.data(), 3500) == 3500;
    if (infiniteOk) for (size_t i = 0; i < 3500; ++i)
        for (size_t ch = 0; ch < 2; ++ch)
            infiniteOk &= repeated[i * 2 + ch] == file[(1000 + (500 + i) % 1000) * 2 + ch];
    Check(infiniteOk, L"LOOP endless sections retain the track duration and wrap samples exactly over multiple passes");
    Check(forever && !forever->Seek(NAN) && !forever->Seek(INFINITY) && !forever->Seek(1e30) &&
              !OpenLooped(OpenDecoder(scratch + L"\\loop.wav", 48000, err), 1, INT64_MAX, 1000, 0) &&
              !OpenLooped(OpenDecoder(scratch + L"\\loop.wav", 48000, err), 1000, 2000, 2, NAN),
          L"LOOP invalid times and overflowing frame counts are rejected");
    auto range = OpenRange(OpenDecoder(scratch + L"\\loop.wav", 48000, err), 0.1, 0.3);
    Check(range && !range->Seek(NAN) && !range->Seek(INFINITY) && !range->Seek(1e30) &&
              !OpenRange(OpenDecoder(scratch + L"\\loop.wav", 48000, err), 0, INFINITY),
          L"RANGE invalid times are rejected before conversion to sample frames");

    // The engine uses the same tags: the reported duration is the looped one, and 0 turns loops off.
    if (HeadlessTests()) {
        testlog::Skip(L"loop duration on a physical output: headless mode; offline loop samples were checked");
        return;
    }
    Engine engine;
    if (!engine.Init(nullptr, err)) {
        testlog::Skip(L"loop duration on a physical output: no audio device available");
        return;
    }
    engine.SetVolume(0);
    engine.SetLoops(2, 0.1);
    engine.Play(PlayItem{1, scratch + L"\\loop.wav"});
    Pump(300);
    double looped = engine.Duration();
    engine.Stop();
    Pump(100);
    engine.SetLoops(0, 0.1);
    engine.Play(PlayItem{2, scratch + L"\\loop.wav"});
    Pump(300);
    double plain = engine.Duration();
    engine.Stop();
    Pump(100);
    engine.Shutdown();
    Check(std::fabs(looped - 0.475) < 0.002 && std::fabs(plain - 0.5) < 0.002,
          L"LOOP engine duration: %.3f s with loops (expect 0.475), %.3f s with loops off (expect 0.500)", looped, plain);
}

namespace {

std::string BeBytes(uint64_t v, int n) {
    std::string s;
    for (int i = n - 1; i >= 0; --i) s += (char)((v >> (8 * i)) & 0xFF);
    return s;
}

// The FLAC with its metadata replaced by its STREAMINFO and `blocks` (type, body). Audio is unchanged.
std::string FlacWithBlocks(const std::string& flac, const std::vector<std::pair<int, std::string>>& blocks) {
    if (flac.compare(0, 4, "fLaC") != 0) return {};
    std::string info;
    size_t pos = 4;
    while (pos + 4 <= flac.size()) {
        uint8_t h = (uint8_t)flac[pos];
        size_t len = (size_t)(uint8_t)flac[pos + 1] << 16 | (size_t)(uint8_t)flac[pos + 2] << 8 | (uint8_t)flac[pos + 3];
        if ((h & 0x7F) == 0) info = flac.substr(pos, 4 + len);
        pos += 4 + len;
        if (h & 0x80) break;
    }
    if (info.empty()) return {};
    info[0] = (char)(info[0] & 0x7F);
    std::string out = "fLaC" + info;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const std::string& body = blocks[i].second;
        out += (char)((i + 1 == blocks.size() ? 0x80 : 0) | blocks[i].first);
        out += BeBytes(body.size(), 3) + body;
    }
    return out + flac.substr(std::min(pos, flac.size()));
}

// A Vorbis comment block (vendor string "cordinal") holding `comments` ("NAME=value").
std::string CommentBlock(const std::vector<std::string>& comments) {
    std::string body = LeBytes(8, 4) + "cordinal" + LeBytes(static_cast<uint32_t>(comments.size()), 4);
    for (const auto& c : comments) body += LeBytes(static_cast<uint32_t>(c.size()), 4) + c;
    return body;
}

// The binary CUESHEET block: two audio tracks, the second at 0.4 s.
std::string BinaryCueBlock(uint32_t rate) {
    std::string b(128 + 8 + 1 + 258, '\0');
    b += std::string(1, (char)2);  // number of tracks
    auto track = [](uint64_t offset, int number, const char* isrc) {
        return BeBytes(offset, 8) + std::string(1, (char)number) + std::string(isrc, 12) + std::string(1, '\0') +
               std::string(13, '\0') + std::string(1, (char)1) +  // one index point
               BeBytes(0, 8) + std::string(1, (char)1) + std::string(3, '\0');
    };
    b += track(0, 1, "USAB10000001");
    b += track((uint64_t)rate * 4 / 10, 2, "USAB10000002");
    return b;
}

std::string Utf16Text(const std::wstring& w, bool bigEndian) {
    std::string s = bigEndian ? "\xFE\xFF" : "\xFF\xFE";
    for (wchar_t c : w) {
        uint16_t u = (uint16_t)c;
        char hi = (char)(u >> 8), lo = (char)(u & 0xFF);
        s += bigEndian ? hi : lo;
        s += bigEndian ? lo : hi;
    }
    return s;
}

// The 1.1 MB limit and bad input: none of these may crash or hang.
void CueGarbage(const std::wstring& folder) {
    unsigned seed = 12345;
    auto next = [&seed] {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) & 0x7FFF;
    };
    bool survived = true;
    for (int round = 0; round < 300; ++round) {
        std::string junk;
        size_t n = next() % 3000;
        for (size_t i = 0; i < n; ++i) junk += (char)(next() & 0xFF);
        CueSheet sheet;
        survived &= ParseCue(junk, folder, sheet) || !sheet.TrackCount();
    }
    const char* broken[] = {"FILE \"Ramp.wav\"\nTRACK 01 AUDIO\nINDEX 01 00:0",
                            "TRACK\nINDEX\nFILE\n",
                            "FILE \"x\" WAVE\nTRACK 99 AUDIO\nINDEX 01 99:99:99\n",
                            "TRACK 01 AUDIO\nTITLE \"no file\"\nINDEX 01 00:00:00\n",
                            "FILE \"\" WAVE\nTRACK 01 AUDIO\n",
                            "FILE \"x\" WAVE\nTRACK 01 AUDIO\nINDEX 00 00:00:10\n",
                            "FILE \"x\" WAVE\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n"};
    for (const char* text : broken) {
        CueSheet sheet;
        survived &= ParseCue(text, folder, sheet) || !sheet.TrackCount();
    }
    CueSheet pregap;
    ParseCue("FILE \"x\" WAVE\nTRACK 01 AUDIO\nINDEX 00 00:00:10\n", folder, pregap);
    Check(survived && pregap.TrackCount() == 1 && std::fabs(pregap.files[0].tracks[0].start - 10 / 75.0) < 1e-9,
          L"CUE garbage: random bytes and broken lines are survived; a pregap-only track starts at its INDEX 00");

    std::string big = "FILE \"x\" WAVE\n";
    char line[64];
    for (int i = 0; i < 10000; ++i) {
        snprintf(line, sizeof line, "TRACK %02d AUDIO\nINDEX 01 00:%02d:00\n", (i % 99) + 1, i % 60);
        big += line;
    }
    CueSheet many;
    bool manyOk = ParseCue(big, folder, many);
    Check(manyOk && many.TrackCount() == 999,
          L"CUE 10,000 TRACK lines: parsed without a hang, capped at %zu tracks", many.TrackCount());

    // Hand-made sheets repeat numbers or restart them for each FILE: no song is dropped.
    CueSheet repeated;
    ParseCue("FILE \"a.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 01 AUDIO\nINDEX 01 01:00:00\n"
             "FILE \"b.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n",
             folder, repeated);
    bool renumbered = repeated.TrackCount() == 3 && repeated.files.size() == 2 && repeated.files[0].tracks.size() == 2 &&
                      repeated.files[0].tracks[1].number == 2 && repeated.files[1].tracks[0].number == 3;
    Check(renumbered, L"CUE repeated TRACK numbers take the next free number (%zu tracks)", repeated.TrackCount());

    CueSheet huge;
    Check(!ParseCue(std::string(1024 * 1024 + 1, 'x'), folder, huge), L"CUE over 1 MB is rejected");
}
}  // namespace

// CUE sheets: fields, encodings (CP-1252, UTF-8 BOM/plain, UTF-16 LE/BE) and times, sub-track
// ranges and their decode slices, folder scans that list only tracks, tag/performer inheritance,
// multi-file sheets, rescan-on-edit, and fuzzed/garbage input that must never crash or hang.
void TestCue(const std::wstring& media, const std::wstring& scratch) {
    const std::wstring dir = scratch + L"\\cue", album = dir + L"\\Album", enc = dir + L"\\enc";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW(album.c_str(), nullptr);
    CreateDirectoryW(enc.c_str(), nullptr);

    // Fixtures: a 2 s ramp WAV with a CP-1252 cue, FLACs with embedded sheets, a multi-file cue,
    // and a cue whose FILE says .wav for a .flac on disk.
    unsigned sineRate = 0;
    auto sine = DecodeAll(media + L"\\sine.flac", sineRate);
    const std::string sineFlac = ReadTestBytes(media + L"\\sine.flac");
    const std::string rampWav = WavFile(44100, 2, StereoRamp16(88200), "");
    WriteTestBytes(album + L"\\Ramp.wav", rampWav);
    const std::string rampCue =
        "REM DATE 1999\r\nREM GENRE \"Jazz Fusion\"\r\nREM COMMENT \"ripped\"\r\nREM DISCNUMBER 2\r\nREM TOTALDISCS 3\r\n"
        "CATALOG 0724349583629\r\nPERFORMER \"Caf\xE9 Band\"\r\nTITLE \"Live \xE9t\xE9\"\r\n"
        "FILE \"Ramp.wav\" WAVE\r\n"
        "  TRACK 01 AUDIO\r\n    TITLE \"One\"\r\n    PERFORMER \"Caf\xE9 Band\"\r\n    SONGWRITER \"Writer A\"\r\n"
        "    ISRC USAB12345678\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:00:00\r\n"
        "  TRACK 02 AUDIO\r\n    TITLE \"Two & Friends\"\r\n    INDEX 00 00:00:60\r\n    INDEX 01 00:00:70\r\n"
        "  TRACK 03 AUDIO\r\n    TITLE Three\r\n    INDEX 01 00:01:20\r\n";
    WriteTestBytes(album + L"\\Ramp.cue", rampCue);
    CopyFileW((media + L"\\sine.flac").c_str(), (album + L"\\Plain.flac").c_str(), FALSE);
    CopyFileW((media + L"\\sine.flac").c_str(), (album + L"\\Swap.flac").c_str(), FALSE);
    CopyFileW((media + L"\\sine.flac").c_str(), (album + L"\\Second.flac").c_str(), FALSE);
    CopyFileW((media + L"\\sine.flac").c_str(), (album + L"\\Third.flac").c_str(), FALSE);
    WriteTestBytes(album + L"\\Swap.cue", "FILE \"Swap.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n"
                                          "  TRACK 02 AUDIO\r\n    INDEX 01 00:01:00\r\n");
    WriteTestBytes(album + L"\\Multi.cue", "FILE \"Second.flac\" WAVE\nTRACK 01 AUDIO\nTITLE \"Alpha\"\nINDEX 01 00:00:00\n"
                                           "FILE \"Third.flac\" WAVE\nTRACK 02 AUDIO\nTITLE \"Beta\"\nINDEX 01 00:00:00\n");
    WriteTestBytes(album + L"\\Embedded.flac",
                   FlacWithBlocks(sineFlac, {{4, CommentBlock({"CUESHEET=TRACK 01 AUDIO\nTITLE \"Emb A\"\nINDEX 01 00:00:00\n"
                                                              "TRACK 02 AUDIO\nTITLE \"Emb B\"\nINDEX 01 00:00:30\n"})}}));
    WriteTestBytes(album + L"\\Binary.flac", FlacWithBlocks(sineFlac, {{5, BinaryCueBlock(sineRate)}}));

    // Sheet fields and times, parsed from the CP-1252 file.
    CueSheet sheet;
    bool loaded = LoadCueFile(album + L"\\Ramp.cue", sheet);
    const double t2 = 70 / 75.0, t3 = 1 + 20 / 75.0;
    auto same = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    bool fields = loaded && sheet.date == "1999" && sheet.genre == "Jazz Fusion" && sheet.comment == "ripped" &&
                  sheet.discNumber == 2 && sheet.totalDiscs == 3 && sheet.catalog == "0724349583629" &&
                  sheet.performer == "Caf\xC3\xA9 Band" && sheet.title == "Live \xC3\xA9t\xC3\xA9" && sheet.files.size() == 1;
    Check(fields, L"CUE fields: DATE, GENRE, COMMENT, DISCNUMBER, TOTALDISCS, CATALOG, disc PERFORMER and TITLE (CP-1252 read)");
    bool tracks = loaded && sheet.files[0].tracks.size() == 3 && sheet.files[0].tracks[0].title == "One" &&
                  sheet.files[0].tracks[1].title == "Two & Friends" && sheet.files[0].tracks[2].title == "Three" &&
                  sheet.files[0].tracks[0].isrc == "USAB12345678" && sheet.files[0].tracks[0].songwriter == "Writer A" &&
                  sheet.files[0].tracks[0].performer == "Caf\xC3\xA9 Band" && sheet.files[0].tracks[1].performer.empty();
    Check(tracks, L"CUE tracks: quoted and unquoted titles, PERFORMER, SONGWRITER, ISRC, pregap INDEX 00 ignored");
    bool times = loaded && same(sheet.files[0].tracks[0].start, 0) && same(sheet.files[0].tracks[0].end, t2) &&
                 same(sheet.files[0].tracks[1].start, t2) && same(sheet.files[0].tracks[1].end, t3) &&
                 same(sheet.files[0].tracks[2].start, t3) && sheet.files[0].tracks[2].end == 0;
    Check(times, L"CUE times: INDEX 01 at 75 frames a second, each track ends where the next starts, the last runs to the end");

    // Encodings: UTF-8 BOM, UTF-16 LE and BE with BOM, UTF-8 without BOM, and the ANSI code page.
    const std::wstring encBody = L"FILE \"Ramp.wav\" WAVE\nTRACK 01 AUDIO\nTITLE \"Ü\"\nINDEX 01 00:00:00\n";
    const std::string utf8Body = "FILE \"Ramp.wav\" WAVE\nTRACK 01 AUDIO\nTITLE \"\xC3\x9C\"\nINDEX 01 00:00:00\n";
    const std::string ansiBody = "FILE \"Ramp.wav\" WAVE\nTRACK 01 AUDIO\nTITLE \"\xDC\"\nINDEX 01 00:00:00\n";
    WriteTestBytes(enc + L"\\bom8.cue", "\xEF\xBB\xBF" + utf8Body);
    WriteTestBytes(enc + L"\\utf16le.cue", Utf16Text(encBody, false));
    WriteTestBytes(enc + L"\\utf16be.cue", Utf16Text(encBody, true));
    WriteTestBytes(enc + L"\\utf8.cue", utf8Body);
    WriteTestBytes(enc + L"\\ansi.cue", ansiBody);
    bool encodings = true;
    for (const wchar_t* name : {L"bom8.cue", L"utf16le.cue", L"utf16be.cue", L"utf8.cue", L"ansi.cue"}) {
        CueSheet e;
        encodings &= LoadCueFile(enc + L"\\" + name, e) && e.files[0].tracks.size() == 1 &&
                     e.files[0].tracks[0].title == "\xC3\x9C";
    }
    Check(encodings, L"CUE encodings: UTF-8 BOM, UTF-16 LE and BE, UTF-8 without BOM and ANSI all read as \xDC");

    // Sub-track ranges and the sheets behind them.
    double s = -1, e = -1;
    std::wstring ramp = album + L"\\Ramp.wav";
    bool ranges = SubtrackRange(SubtrackPath(ramp, 1), s, e) && same(s, 0) && same(e, t2) &&
                  SubtrackRange(SubtrackPath(ramp, 2), s, e) && same(s, t2) && same(e, t3) &&
                  SubtrackRange(SubtrackPath(ramp, 3), s, e) && same(s, t3) && e == 0;
    Check(ranges, L"CUE ranges: SubtrackRange gives [start, end) for tracks 1, 2 and 3");
    bool missing = !SubtrackRange(SubtrackPath(ramp, 9), s, e) && !SubtrackRange(album + L"\\Plain.flac|1", s, e);
    Check(missing, L"CUE lookups fail for a missing track number and for a file with no sheet");

    // ExpandSubtracks and folder scans: a covered file is only its tracks.
    auto ramp3 = ExpandSubtracks(ramp);
    Check(ramp3.size() == 3 && ramp3[0] == SubtrackPath(ramp, 1) && ExpandSubtracks(album + L"\\Plain.flac").empty() &&
              ExpandSubtracks(album + L"\\Embedded.flac").size() == 2 && ExpandSubtracks(album + L"\\Binary.flac").size() == 2 &&
              ExpandSubtracks(album + L"\\Swap.flac").size() == 2 && ExpandSubtracks(album + L"\\Third.flac").size() == 1,
          L"CUE ExpandSubtracks: 3 for Ramp.wav, 2 for each embedded or Swap sheet, none for a plain FLAC");
    std::vector<ScanFile> scan;
    ListAudioFiles(album, scan);
    auto listed = [&scan](const std::wstring& p) {
        return std::find_if(scan.begin(), scan.end(), [&](const ScanFile& f) { return f.path == p; }) != scan.end();
    };
    bool scanned = listed(SubtrackPath(album + L"\\Ramp.wav", 3)) && listed(album + L"\\Plain.flac") &&
                   listed(SubtrackPath(album + L"\\Swap.flac", 2)) && listed(SubtrackPath(album + L"\\Third.flac", 2)) &&
                   listed(SubtrackPath(album + L"\\Binary.flac", 1)) && !listed(album + L"\\Ramp.wav") &&
                   !listed(album + L"\\Swap.flac") && !listed(album + L"\\Embedded.flac") && !listed(album + L"\\Binary.flac") &&
                   !listed(album + L"\\Third.flac");
    uint64_t wavSize = ReadTestBytes(ramp).size();
    bool scanSize = std::any_of(scan.begin(), scan.end(), [&](const ScanFile& f) {
        return f.path == SubtrackPath(ramp, 1) && f.size == wavSize;
    });
    Check(scanned && scanSize, L"CUE scanner: %zu entries, covered audio listed only as tracks carrying their "
          L"file's size, plain files as themselves", scan.size());

    // Decoding each range matches the same slice of the whole file.
    auto slice = [](const std::vector<float>& whole, unsigned rate, double start, double end, const std::vector<float>& got) {
        size_t from = (size_t)std::llround(start * rate);
        size_t to = end > 0 ? (size_t)std::llround(end * rate) : whole.size() / 2;
        return got.size() == (to - from) * 2 && std::equal(got.begin(), got.end(), whole.begin() + from * 2);
    };
    unsigned rampRate = 0;
    auto ramps = DecodeAll(ramp, rampRate);
    std::vector<float> got;
    std::wstring error;
    bool decodes = true;
    for (int n = 1; n <= 3; ++n) {
        double start = n == 1 ? 0 : n == 2 ? t2 : t3, end = n == 1 ? t2 : n == 2 ? t3 : 0;
        auto d = OpenTrackDecoder(SubtrackPath(ramp, n), 48000, error);
        got = d ? ReadAllFrom(*d) : std::vector<float>();
        decodes &= d && slice(ramps, rampRate, start, end, got);
    }
    Check(decodes, L"CUE decode: tracks 1, 2 and 3 of Ramp.wav are exactly their slices");
    struct Embedded {
        const wchar_t* file;
        int n;
        double start, end;
    };
    bool embedded = true;
    for (Embedded c : {Embedded{L"Embedded.flac", 1, 0, 0.4}, Embedded{L"Embedded.flac", 2, 0.4, 0}, Embedded{L"Binary.flac", 2, 0.4, 0},
                       Embedded{L"Swap.flac", 2, 1.0, 0}}) {
        auto d = OpenTrackDecoder(SubtrackPath(album + L"\\" + c.file, c.n), 48000, error);
        got = d ? ReadAllFrom(*d) : std::vector<float>();
        embedded &= d && slice(sine, sineRate, c.start, c.end, got);
    }
    Check(embedded, L"CUE decode: embedded FLAC sheets (text and binary) and a .wav-named cue on a .flac match their slices");
    auto gone = OpenTrackDecoder(SubtrackPath(ramp, 9), 48000, error);
    Check(!gone && !error.empty(), L"CUE decode: a track whose sheet is gone fails instead of playing the file");

    // Tags: the sheet's values over the file's, and the tracks' durations add up to the file's.
    TagData t1, t2tags, t3tags, whole;
    bool read = ReadTags(SubtrackPath(ramp, 1), t1) && ReadTags(SubtrackPath(ramp, 2), t2tags) &&
                ReadTags(SubtrackPath(ramp, 3), t3tags) && ReadTags(ramp, whole);
    Check(read && t1.title == "One" && t1.artist == "Caf\xC3\xA9 Band" && t1.album == "Live \xC3\xA9t\xC3\xA9" &&
              t1.albumArtist == "Caf\xC3\xA9 Band" && t1.date == "1999" && t1.genre == "Jazz Fusion" &&
              t1.comment == "ripped" && t1.composer == "Writer A" && t1.track == 1 && t1.trackTotal == 3 && t1.disc == 2 &&
              t1.discTotal == 3 && std::isnan(t1.rgTrackGain) && t1.audioHash == 0 && t1.sampleRate == 44100,
          L"CUE tags: track 1 has the sheet's title, artists, album, date, genre, comment, number, total and disc");
    Check(t2tags.artist == "Caf\xC3\xA9 Band" && t2tags.title == "Two & Friends" && t2tags.track == 2,
          L"CUE tags: a track without PERFORMER takes the album artist as its artist");
    double sum = t1.duration + t2tags.duration + t3tags.duration;
    Check(read && same(t1.duration, t2) && same(t2tags.duration, t3 - t2) && same(sum, whole.duration) && whole.duration > 1.9,
          L"CUE tags: durations %.6f + %.6f + %.6f = %.6f, the file's %.6f", t1.duration, t2tags.duration,
          t3tags.duration, sum, whole.duration);

    // Multi-file cue: each track's end is the next track's start in the same file, so both are open-ended.
    CueSheet multi;
    bool multiOk = LoadCueFile(album + L"\\Multi.cue", multi) && multi.files.size() == 2 && multi.TrackCount() == 2 &&
                   multi.files[0].tracks[0].end == 0 && multi.files[1].tracks[0].end == 0;
    TagData mt;
    multiOk = multiOk && ReadTags(SubtrackPath(album + L"\\Third.flac", 2), mt) && mt.trackTotal == 2 && mt.title == "Beta";
    Check(multiOk, L"CUE multi-file: tracks are split across FILE lines, each ending at its own file's end");

    // A cue edited after the scan makes its tracks newer, so a rescan re-reads them.
    HANDLE h = CreateFileW((album + L"\\Ramp.cue").c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    SYSTEMTIME st{};
    st.wYear = 2100;
    st.wMonth = st.wDay = 1;
    FILETIME future{};
    SystemTimeToFileTime(&st, &future);
    bool touched = h != INVALID_HANDLE_VALUE && SetFileTime(h, nullptr, nullptr, &future);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    uint64_t futureTime = (uint64_t)future.dwHighDateTime << 32 | future.dwLowDateTime;
    std::vector<ScanFile> rescan;
    ListAudioFiles(album, rescan);
    bool newer = touched && std::any_of(rescan.begin(), rescan.end(), [&](const ScanFile& f) {
        return f.path == SubtrackPath(ramp, 1) && f.time == futureTime;
    });
    Check(newer, L"CUE rescan: a track takes the newer write time of its cue");

    CueGarbage(album);
}
