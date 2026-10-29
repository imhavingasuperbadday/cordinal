// Headless engine checks of the self test (see selftest.cpp): gapless switching, seek and pause,
// sample conversion, exclusive output and the 12 ms fade, under a hidden window and muted.
#include "selftest_common.h"
#include "audio_live.h"
#include <bit>
#include <cfenv>

namespace {

// --- engine (headless, muted)

// Each engine event paired with its arrival time and token; EventProc records them and services
// NeedNext by pointing the engine at the next file, so the gapless timing checks can be read back.
struct Events {
    std::vector<std::pair<EngineEvent, unsigned long long>> list;
    std::vector<ULONGLONG> times;
    Engine* engine = nullptr;
    bool applicationCommands = false;
    std::wstring dir;
    std::vector<PlayItem> cueItems;  // tokens 100, 101, ...: each one is followed by the next
};
Events* g_events = nullptr;
class RadioBytes final : public LiveAudioSource {
public:
    explicit RadioBytes(std::string bytes) : bytes_(std::move(bytes)) {}
    size_t Read(uint8_t* out, size_t bytes) override {
        if (cancel_) return 0;
        size_t n = std::min({bytes, bytes_.size() - at_, size_t(287)});
        memcpy(out, bytes_.data() + at_, n); at_ += n; return n;
    }
    void Abort() override { cancel_ = true; }
    std::wstring Problem() const override { return {}; }
private:
    const std::string bytes_;
    size_t at_ = 0;
    std::atomic<bool> cancel_{false};
};

LRESULT CALLBACK EventProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && g_events && g_events->applicationCommands) {
        app::OnCommand(LOWORD(wp), HIWORD(wp), (HWND)lp);
        return 0;
    }
    if (msg == WM_ENGINE && g_events) {
        auto e = (EngineEvent)wp;
        auto token = (unsigned long long)lp;
        g_events->list.emplace_back(e, token);
        g_events->times.push_back(GetTickCount64());
        if (e == EngineEvent::NeedNext || e == EngineEvent::Error) {
            if (token == 1) {
                PlayItem b{2, g_events->dir + L"\\b.flac"};
                g_events->engine->SetNext(1, &b);
            } else if (token >= 100 && token + 1 < 100 + g_events->cueItems.size()) {
                PlayItem next = g_events->cueItems[token + 1 - 100];
                g_events->engine->SetNext(token, &next);
            } else {
                g_events->engine->SetNext(token, nullptr);
            }
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

// Exclusive output's sample conversion must give back exactly the integers the decoders read.
void TestSampleConversion() {
    std::vector<float> clipInput;
    for (uint32_t bits:{0u,0x80000000u,0x7F800000u,0xFF800000u,0x7FC12345u,0xFF812345u,
                       1u,0x80000001u,0x3F800001u,0xBF800001u,0x3F7FFFFFu,0xBF7FFFFFu})
        clipInput.push_back(std::bit_cast<float>(bits));
    uint32_t random=261020;
    for (unsigned i=0;i<65536;++i) {
        random^=random<<13; random^=random>>17; random^=random<<5;
        clipInput.push_back(std::bit_cast<float>(random));
    }
    testlog::Cases rounded16;
    // A fresh thread starts the production dither at its documented seed. The
    // scalar oracle carries that state across calls, including odd-sized blocks.
    std::thread conversion([&] {
        uint32_t noise = 0x9E3779B9u;
        std::vector<float> exact;
        for (int v = -32768; v <= 32767; ++v) exact.push_back((float)v / 32768);
        for (uint32_t bits : {0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC12345u,
                              0x80000000u, 0x43800000u, 0xC3800000u})
            exact.push_back(std::bit_cast<float>(bits));
        auto mixed = exact;
        // Just below 256 still permits half-integers after scaling. It must
        // turn dither on even though the sample itself is beyond clipping.
        mixed.push_back(std::nextafter(256.0f, 0.0f));
        const unsigned csr = _mm_getcsr();
        for (unsigned denormals : {0u, 0x8000u, 0x40u, 0x8040u})
        for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
            std::fesetround(mode);
            _mm_setcsr((_mm_getcsr() & ~0x8040u) | denormals);
            for (const auto* input : {&exact, &mixed, &clipInput})
            for (size_t count : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(4), size_t(7), input->size()}) {
                std::vector<int16_t> expected(count + 2, 12345), actual = expected;
                bool isExact = true;
                for (size_t i = 0; i < count; ++i) {
                    const float v = (*input)[i];
                    isExact &= !std::isfinite(v) || v * 32768.0 == std::nearbyint(v * 32768.0);
                }
                for (size_t i = 0; i < count; ++i) {
                    double dither = 0;
                    if (!isExact) {
                        noise = noise * 1664525u + 1013904223u; dither = (noise >> 8) / 16777216.0;
                        noise = noise * 1664525u + 1013904223u; dither -= (noise >> 8) / 16777216.0;
                    }
                    const float v = (*input)[i];
                    expected[i + 1] = (int16_t)(std::isfinite(v) ?
                        (int64_t)std::clamp(std::nearbyint(v * 32768.0 + dither), -32768.0, 32767.0) : 0);
                }
                ConvertSamples(input->data(), count, SampleFormat::Int16, actual.data() + 1);
                rounded16.Add(actual == expected, std::to_wstring(mode) + L"/" + std::to_wstring(denormals) +
                              L"/" + std::to_wstring(count) +
                              (input == &exact ? L" exact" : input == &mixed ? L" mixed" : L" raw float bits"));
            }
        }
        _mm_setcsr(csr);
    });
    conversion.join();
    Check(rounded16.Passed(), L"OUTPUT 16-bit blocks retain scalar rounding, denormal handling, dither sequence and guards%ls",
          rounded16.failed.c_str());
    bool exactClip=true;
    for (size_t count:{size_t(0),size_t(1),size_t(2),size_t(3),size_t(4),size_t(7),size_t(31),clipInput.size()-2}) {
        auto expected=clipInput,actual=clipInput;
        for (size_t i=1;i<count+1;++i) expected[i]=std::clamp(expected[i],-1.0f,1.0f);
        ClipPcm(actual.data()+1,count);
        exactClip&=memcmp(expected.data(),actual.data(),actual.size()*sizeof(float))==0;
    }
    Check(exactClip,L"OUTPUT clipping preserves all float bits against std::clamp, including NaNs, zeros, infinities, tails and unaligned spans");
    bool exactInteger = true;
    const int rounding = std::fegetround();
    for (int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        std::fesetround(mode);
        for (auto format : {SampleFormat::Int24, SampleFormat::Int24In32, SampleFormat::Int32})
        for (size_t count : {size_t(0), size_t(1), size_t(3), size_t(4), size_t(7), clipInput.size() - 1}) {
            const unsigned width = SampleBytes(format);
            std::vector<uint8_t> expected(count * width + 2, 0xA5), actual = expected;
            const bool wide = format == SampleFormat::Int32;
            const double scale = wide ? 2147483648.0 : 8388608.0;
            for (size_t i = 0; i < count; ++i) {
                float sample = clipInput[i + 1];
                int64_t v = std::isfinite(sample) ? (int64_t)std::clamp(std::nearbyint(sample * scale), -scale, scale - 1) : 0;
                if (format == SampleFormat::Int24In32) v *= 256;
                for (unsigned byte = 0; byte < width; ++byte) expected[1 + i * width + byte] = (uint8_t)((uint64_t)v >> (byte * 8));
            }
            ConvertSamples(clipInput.data() + 1, count, format, actual.data() + 1);
            exactInteger &= actual == expected;
        }
    }
    std::fesetround(rounding);
    Check(exactInteger, L"OUTPUT integer blocks match scalar rounding in every mode, including non-finite values, saturation, tails and guards");
    bool ok24 = true;
    std::vector<int32_t> values;
    for (int v = -8388608; v <= 8388607; v += 977) values.push_back(v);
    values.push_back(8388607);
    std::vector<float> in24;
    for (int v : values) in24.push_back((float)v / 8388608.0f);
    std::vector<int32_t> out32(in24.size());
    std::vector<uint8_t> out24(in24.size() * 3);
    ConvertSamples(in24.data(), in24.size(), SampleFormat::Int24In32, out32.data());
    ConvertSamples(in24.data(), in24.size(), SampleFormat::Int24, out24.data());
    for (size_t i = 0; i < values.size(); ++i) {
        int32_t packed = (int32_t)((uint32_t)out24[i * 3] | (uint32_t)out24[i * 3 + 1] << 8 | (uint32_t)out24[i * 3 + 2] << 16) << 8 >> 8;
        ok24 &= out32[i] == values[i] * 256 && packed == values[i];
    }
    Check(ok24, L"OUTPUT 24-bit sample conversion preserves exact integers in packed and 32-bit formats");
    // Processed audio between 16-bit steps is dithered: a level a quarter step above zero, which
    // plain rounding would erase, survives on average, and the noise stays within one step.
    std::vector<float> fraction(48000, 0.25f / 32768.0f);
    std::vector<int16_t> dithered(fraction.size());
    ConvertSamples(fraction.data(), fraction.size(), SampleFormat::Int16, dithered.data());
    double mean = 0;
    bool bounded = true;
    for (int16_t v : dithered) {
        mean += v;
        bounded &= v >= -1 && v <= 1;
    }
    mean /= dithered.size();
    Check(bounded && std::fabs(mean - 0.25) < 0.02,
          L"OUTPUT 16-bit conversion of processed audio is TPDF-dithered: mean %.3f steps (0.25)", mean);
}

// Exclusive output opens the device at each track's own rate. It takes the audio device away
// from other programs for a moment, so it only runs when CORDINAL_TEST_EXCLUSIVE is set.
void TestExclusive(const std::wstring& root, HWND hwnd) {
    if (!GetEnvironmentVariableW(L"CORDINAL_TEST_EXCLUSIVE", nullptr, 0)) {
        testlog::Skip(L"exclusive output: set CORDINAL_TEST_EXCLUSIVE=1 to take the audio device");
        return;
    }
    Engine engine;
    std::wstring error;
    engine.Init(hwnd, error);
    engine.SetVolume(0);
    engine.SetExclusive(true);
    Pump(800);
    auto o = engine.Output();
    if (!o.exclusive) {
        testlog::Skip((L"exclusive output unavailable: " + o.problem).c_str());
        engine.Shutdown();
        return;
    }
    // Each track at its own rate, or (on devices that can't do it exclusively) another rate with
    // the reason reported.
    engine.Play(PlayItem{1, root + L"\\gap2\\a.flac"});
    Pump(1200);
    auto o1 = engine.Output();
    engine.Play(PlayItem{2, root + L"\\media\\sine.opus"});
    Pump(1500);
    auto o2 = engine.Output();
    bool flacOk = o1.exclusive && (o1.rate == 44100 || o1.problem.find(L"44100") != std::wstring::npos);
    bool opusOk = o2.exclusive && (o2.rate == 48000 || o2.problem.find(L"48000") != std::wstring::npos);
    Check(flacOk && opusOk, L"OUTPUT exclusive follows the track rate: FLAC %u Hz%ls%ls, Opus %u Hz, %d-bit%ls", o1.rate,
          o1.problem.empty() ? L"" : L" (", o1.problem.empty() ? L"" : (o1.problem + L")").c_str(), o2.rate, o2.bits,
          o2.floatFormat ? L" float" : L"");
    // A pause lets the endpoint go after a moment (see AudioOutput::Run); resuming must take it back,
    // still exclusive and at the same rate, or bit-perfect output would silently fall back to shared.
    engine.SetPaused(true);
    Pump(3500);
    engine.SetPaused(false);
    Pump(2000, [&] { return engine.Output().exclusive && engine.Output().rate == o2.rate; });
    auto o3 = engine.Output();
    Check(o3.exclusive && o3.rate == o2.rate,
          L"OUTPUT exclusive output comes back after the idle release: %u Hz, %ls", o3.rate,
          o3.exclusive ? L"exclusive" : L"shared");
    engine.Stop();
    Pump(200);
    engine.Shutdown();
}

// A paused output must give the endpoint back. An exclusive stream keeps the device claimed for as
// long as its IAudioClient exists, stopped or not, so a paused player used to block every other
// program from rendering for the rest of the session: a browser tab reported an audio renderer
// error until the player was closed.
void TestOutputIdleRelease() {
    AudioOutput out;
    std::wstring error;
    std::atomic<unsigned long long> rendered{0};
    AudioOutput::RenderFn silent = [&rendered](float* buffer, size_t frames, unsigned long long& pos) {
        memset(buffer, 0, frames * 2 * sizeof(float));
        pos += frames;
        rendered += frames;
        return frames;
    };
    if (!out.Start(silent, nullptr, error)) {
        testlog::Skip((L"output idle release: no audio output (" + error + L")").c_str());
        return;
    }
    Check(out.HoldingDevice(), L"OUTPUT holds the device while it is playing");
    out.SetPaused(true);
    Pump(4000, [&] { return !out.HoldingDevice(); });
    const bool letGo = !out.HoldingDevice();
    const auto atRelease = rendered.load();
    Pump(400);
    Check(letGo && !out.HoldingDevice() && rendered.load() == atRelease,
          L"OUTPUT a paused output lets the endpoint go, so another program can take it");
    out.SetPaused(false);
    Pump(2000, [&] { return out.HoldingDevice(); });
    const auto resumed = rendered.load();
    Pump(300);
    Check(out.HoldingDevice() && rendered.load() > resumed,
          L"OUTPUT resuming takes the endpoint back and renders again (%u Hz)", out.Rate());
    out.Stop();
    Check(!out.HoldingDevice(), L"OUTPUT Stop releases the device");
}

// Pause, seek and stop fade the output over 12 ms (Render applies StepFade per frame). Driving
// Render headlessly isn't possible from here, so the fade step itself is checked: its length, its
// largest step, and that it is exact at unity.
void TestFadeRamp() {
    const float step = (float)(1.0 / (0.012 * 48000));
    float g = 1.0f, maxStep = 0;
    int down = 0;
    while (g > 0 && down < 10000) {
        float next = StepFade(g, 0.0f, step);
        maxStep = std::max(maxStep, g - next);
        g = next;
        ++down;
    }
    float h = 0, maxRise = 0;
    int up = 0;
    while (h < 1 && up < 10000) {
        float next = StepFade(h, 1.0f, step);
        maxRise = std::max(maxRise, next - h);
        h = next;
        ++up;
    }
    Check(down >= 575 && down <= 577 && g == 0 && maxStep <= step * 1.001f && up >= 575 && up <= 577 && h == 1 &&
              maxRise <= step * 1.001f,
          L"FADE 12 ms at 48 kHz: out in %d frames, in in %d frames, largest step %.2e (limit %.2e)", down, up,
          std::max(maxStep, maxRise), step);
    Check(StepFade(1.0f, 1.0f, step) == 1.0f && StepFade(0.25f, 0.25f, step) == 0.25f,
          L"FADE steady gain is unchanged (exact unity)");
}

// Headless engine (muted, real device): gapless track switch timing, seek, pause holding position,
// a CUE-style range playing as one track, and the three CUE tracks of Ramp.wav as a gapless run.
void TestEngine(const std::wstring& gapDir, const std::wstring& cueDir, const std::wstring& media) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = EventProc;
    wc.lpszClassName = L"CordinalSelfTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    Engine engine;
    Events ev;
    ev.engine = &engine;
    ev.dir = gapDir;
    g_events = &ev;
    std::wstring error;
    bool ok = engine.Init(hwnd, error);
    Check(ok, L"ENGINE init rate=%u %ls", engine.Rate(), error.c_str());
    if (!ok) { g_events = nullptr; DestroyWindow(hwnd); return; }
    engine.SetVolume(0);  // silent test
    {
        PlayItem live; live.token = 950;
        live.live = std::make_shared<RadioBytes>(ReadTestBytes(media + L"\\sine.mp3"));
        engine.Play(live);
        Pump(3000, [&] { return engine.Position() > 0.35; });
        std::array<uint8_t, 256> peaks{};
        bool audible = engine.WaveformPeaks(peaks.data(), peaks.size()) &&
            *std::max_element(peaks.begin(), peaks.end()) > 8 && engine.Duration() == 0;
        Check(audible, L"radio waveform uses audible PCM even at zero volume and with no seekable duration");
        engine.SetPaused(true); Pump(180);
        engine.WaveformPeaks(peaks.data(), peaks.size());
        auto held = peaks; double position = engine.Position(); Pump(120);
        engine.WaveformPeaks(peaks.data(), peaks.size());
        bool frozen = peaks == held && engine.Position() == position;
        engine.SetPaused(false); Pump(160);
        Check(frozen && engine.Position() > position,
              L"radio pause holds the waveform's samples and resume advances the same audible clock");
        engine.Stop(); Pump(200);
        Check(!engine.WaveformPeaks(peaks.data(), peaks.size()) &&
                  *std::max_element(peaks.begin(), peaks.end()) == 0,
              L"stopping radio clears its waveform instead of retaining the last station");
        ev.list.clear(); ev.times.clear();
    }
    std::wstring visualFile = cueDir + L"\\visual-motion.wav";
    std::string visualWav = MakeSineWav(300, 0);
    for (int i = 0; i < 9 * 48000; ++i) {
        double t = i / 48000.0;
        double sweep = 200 * 3 / std::log(40.0) * (std::pow(40.0, t / 3) - 1);
        double sample = 0.35 * std::sin(6.28318530718 * 117.3 * t) +
                        0.15 * std::sin(6.28318530718 * sweep) * (0.55 + 0.45 * std::sin(6.28318530718 * 3.7 * t));
        if (t >= 3) sample = (0.06 + 0.055 * std::sin(6.28318530718 * 1.5 * t)) *
                            std::sin(6.28318530718 * 117.3 * t);
        if (t >= 8) sample = t >= 8.4 && t < 8.44 ? 0.5 * std::sin(6.28318530718 * 440 * t) : 0;
        int16_t value = (int16_t)std::lround(sample * 32767);
        for (int c = 0; c < 2; ++c) {
            visualWav[44 + i * 4 + c * 2] = (char)(value & 0xFF);
            visualWav[44 + i * 4 + c * 2 + 1] = (char)((uint16_t)value >> 8);
        }
    }
    if (Ready(WriteTestBytes(visualFile, visualWav), L"visualizer motion fixture is prepared")) {
        engine.Play(PlayItem{900, visualFile});
        Pump(2000, [&] { return engine.Position() > 0.2; });
        AudioVisuals frame;
        std::array<float, AudioVisuals::kBands> previous{};
        std::vector<float> previousCurve;
        int bars = 0, waves = 0, advancing = 0;
        double last = -1;
        float maximum = 0, retained = 1;
        for (int tick = 0; tick < 18; ++tick) {
            Pump(20);
            unsigned rate = engine.Rate();
            size_t frames = frame.Prepare(rate);
            bool active = engine.VisualSamples(frame.wave.data(), frames, frame.time) == frames;
            advancing += frame.time > last;
            last = frame.time;
            for (float v : frame.wave) maximum = std::max(maximum, std::fabs(v));
            float past = 0, future = 0;
            for (size_t i = 0; i < frames; ++i) {
                float v = std::fabs(frame.wave[i * 2]);
                if (i < frames / 2) past = std::max(past, v);
                else future = std::max(future, v);
            }
            retained = std::min({retained, past, future});
            frame.Update(active, rate, 0.02);
            bars += previous != frame.levels;
            previous = frame.levels;
            auto curve = frame.Curve(587);
            waves += curve != previousCurve;
            previousCurve = std::move(curve);
        }
        Check(maximum > 0.3f && retained > 0.3f && bars >= 14 && waves >= 14 && advancing >= 14,
              L"VIS muted live playback advances both complete plots (spectrum %d/18, scope %d/18, clock %d/18, past/future %.3f)",
              bars, waves, advancing, retained);
        engine.SetPaused(true);
        Pump(200);
        size_t frames = frame.Prepare(engine.Rate());
        engine.VisualSamples(frame.wave.data(), frames, frame.time);
        auto paused = frame.wave;
        double pausedAt = frame.time;
        Pump(120);
        engine.VisualSamples(frame.wave.data(), frames, frame.time);
        Check(frame.wave == paused && frame.time == pausedAt,
              L"VIS pause holds both the audible sample window and the waveform clock");
        engine.SetPaused(false);
        Pump(120);
        engine.VisualSamples(frame.wave.data(), frames, frame.time);
        Check(frame.time > pausedAt && frame.wave != paused, L"VIS resume advances the same visualization feed");
        engine.Seek(4);
        Pump(2000, [&] { return engine.Position() >= 4.4 && engine.Position() < 4.8; });
        std::array<uint16_t, 256> previousHeights{};
        int visibleChanges = 0, tallest = 0;
        for (int tick = 0; tick < 36; ++tick) {
            Pump(33);
            std::array<uint8_t, 256> peaks{};
            std::array<uint16_t, 256> heights{};
            engine.WaveformPeaks(peaks.data(), peaks.size());
            // A normal 26 px player has eleven pixels above its centre, with a fractional edge
            // pixel. Include that coverage: rounding it away used to hide quiet note releases.
            for (size_t b = 0; b < peaks.size(); ++b) {
                heights[b] = (uint16_t)std::max(255, 11 * peaks[b]);
                tallest = std::max(tallest, heights[b] / 255);
            }
            visibleChanges += tick && heights != previousHeights;
            previousHeights = heights;
        }
        Check(engine.Duration() == 300 && visibleChanges >= 20 && tallest >= 3,
              L"live quiet audio visibly changes in a 26 px bar early in a five-minute song (%d/35 frames, %d px)",
              visibleChanges, tallest);
        engine.Seek(8);
        Pump(2000, [&] { return engine.Position() >= 8.1 && engine.Position() < 8.3; });
        std::array<uint8_t, 256> pulse{};
        engine.WaveformPeaks(pulse.data(), pulse.size());
        bool cleanSeek = std::all_of(pulse.begin(), pulse.end(), [](uint8_t v) { return v == 0; });
        int before = 0, attack = 0, tail = 0;
        Pump(1200, [&] {
            engine.WaveformPeaks(pulse.data(), pulse.size());
            double position = engine.Position();
            int height = *std::max_element(pulse.begin(), pulse.end());
            if (position < 8.39) before = std::max(before, height);
            if (position >= 8.4 && position < 8.47) attack = std::max(attack, height);
            if (position >= 8.55 && position < 8.6) tail = std::max(tail, height);
            return position >= 8.7;
        });
        Check(cleanSeek && before == 0 && attack > 165 && tail > 20 && tail < attack,
              L"live response resets on seek and swells on the audible note, without decode-ahead pre-echo "
              L"(before %d, attack %d, release %d)", before, attack, tail);
        engine.Stop();
        Pump(200);
        ev.list.clear();
        ev.times.clear();
    }
    Pump(350);  // the initially idle endpoint must wake on the first Play command
    ULONGLONG t0 = GetTickCount64();
    engine.Play(PlayItem{1, gapDir + L"\\a.flac"});
    Pump(9000, [&] { return !ev.list.empty() && ev.list.back().first == EngineEvent::Ended; });
    auto at = [&](EngineEvent e, unsigned long long tok) -> double {
        for (size_t i = 0; i < ev.list.size(); ++i)
            if (ev.list[i].first == e && ev.list[i].second == tok) return (ev.times[i] - t0) / 1000.0;
        return -1;
    };
    double s1 = at(EngineEvent::TrackStarted, 1), s2 = at(EngineEvent::TrackStarted, 2), end = at(EngineEvent::Ended, 0);
    Check(s1 >= 0 && s2 > 0 && end > 0 && std::fabs((s2 - s1) - 2.5) < 0.15 && std::fabs((end - s1) - 6.0) < 0.2,
          L"ENGINE gapless a.flac -> b.flac: started %.2fs, switched after %.2fs (expect 2.50), ended after %.2fs (expect 6.00)",
          s1, s2 - s1, end - s1);
    Pump(350);
    auto endedEvent = std::find_if(ev.list.begin(), ev.list.end(), [](const auto& e) { return e.first == EngineEvent::Ended; });
    Check(endedEvent != ev.list.end() && std::none_of(endedEvent + 1, ev.list.end(),
          [](const auto& e) { return e.first == EngineEvent::TrackStarted; }),
          L"ENGINE does not restart a finished track after posting Ended");

    // Change the next choice after the engine has decoded into it. The current song must keep
    // advancing at the requested rate and reach its original end before the replacement or stop.
    testlog::Cases nextChanges;
    engine.SetVolume(0.00001f);
    engine.SetScope(true);
    auto scopeLevel = [](Engine& player) {
        float samples[1024 * 2]{};
        player.Scope(samples, 1024);
        double sum = 0;
        for (float value : samples) sum += value * value;
        return std::sqrt(sum / std::size(samples));
    };
    struct NextChange { bool stop; double tempo; };
    for (NextChange change : {NextChange{false, 1}, NextChange{true, 1}, NextChange{false, 1.5}}) {
        const bool stop = change.stop;
        ev.list.clear();
        ev.times.clear();
        engine.SetTempo(change.tempo);
        engine.Play(PlayItem{1, gapDir + L"\\a.flac"});
        Pump(5000, [&] {
            return std::find(ev.list.begin(), ev.list.end(), std::pair{EngineEvent::NeedNext, 2ull}) != ev.list.end();
        });
        const bool ahead = std::find(ev.list.begin(), ev.list.end(), std::pair{EngineEvent::NeedNext, 2ull}) != ev.list.end();
        const double before = engine.Position();
        const double level = scopeLevel(engine);
        const ULONGLONG changed = GetTickCount64();
        PlayItem replacement{9, gapDir + L"\\a.flac"};
        engine.SetNext(1, stop ? nullptr : &replacement);
        double lowest = level;
        for (int tick = 0; tick < 12; ++tick) {
            Pump(20);
            lowest = std::min(lowest, scopeLevel(engine));
        }
        const double advanced = engine.Position() - before;
        const double elapsed = (GetTickCount64() - changed) / 1000.0;
        Pump(6500, [&] { return !ev.list.empty() && ev.list.back().first == EngineEvent::Ended; });
        const double first = at(EngineEvent::TrackStarted, 1);
        const double changedTo = stop ? at(EngineEvent::Ended, 0) : at(EngineEvent::TrackStarted, 9);
        const bool noOldTrack = at(EngineEvent::TrackStarted, 2) < 0;
        nextChanges.Add(ahead && level > 1e-7 && lowest > level * (change.tempo == 1 ? 0.8 : 0.1) &&
                            std::fabs(advanced - elapsed * change.tempo) < 0.12 &&
                            first > 0 && changedTo > first &&
                            std::fabs(changedTo - first - 2.5 / change.tempo) < 0.18 && noOldTrack,
                        std::wstring(stop ? L"stop after current" : L"replace next") +
                            L" at " + std::to_wstring(change.tempo) + L"x tempo" +
                            L" (advanced " + std::to_wstring(advanced) + L" in " + std::to_wstring(elapsed) +
                            L" s; end/transition " + std::to_wstring(changedTo - first) + L" s)");
        engine.Stop();
        Pump(200);
    }
    Check(nextChanges.Passed(), L"ENGINE replacing or cancelling decoded-ahead audio leaves the current song uninterrupted%ls",
          nextChanges.failed.c_str());
    engine.SetScope(false);
    engine.SetVolume(0);
    engine.SetTempo(1);

    // Seek
    ev.list.clear();
    ev.times.clear();
    engine.Play(PlayItem{5, gapDir + L"\\b.mp3"});
    Pump(600);
    engine.Seek(2.0);
    Pump(400);
    double pos = engine.Position();
    Check(pos > 2.0 && pos < 2.6, L"ENGINE seek to 2.0 s -> position %.2f s", pos);
    PlayItem replaced{5, gapDir + L"\\b.mp3"};
    engine.Play(PlayItem{6, gapDir + L"\\b.mp3"});
    engine.Seek(2.0, &replaced);
    Pump(400);
    Check(engine.Position() < 1.0, L"ENGINE a seek bound to the previous track cannot seek its replacement");
    engine.SetPaused(true);
    Pump(200);  // the 12 ms fade out, then what the device had queued plays out
    double p1 = engine.Position();
    Pump(500);
    double p2 = engine.Position();
    Check(std::fabs(p2 - p1) < 0.02 && engine.Paused(), L"ENGINE pause holds position (%.3f -> %.3f)", p1, p2);
    engine.SetPaused(false);
    engine.Stop();
    Pump(200);

    // A CUE-style range: 0.5 s to 1.5 s of a.flac plays as a one-second track.
    Pump(350);  // leave the stopped endpoint and decode thread asleep before the next Play
    ev.list.clear();
    ev.times.clear();
    engine.Play(PlayItem{6, gapDir + L"\\a.flac", 1, 0, 0.5, 1.5});
    Pump(2000, [&] { return engine.Duration() > 0; });
    double rangeDuration = engine.Duration();
    Pump(5000, [&] { return !ev.list.empty() && ev.list.back().first == EngineEvent::Ended; });
    auto rangeAt = [&](EngineEvent e) -> double {
        for (size_t i = 0; i < ev.list.size(); ++i)
            if (ev.list[i].first == e) return ev.times[i] / 1000.0;
        return -1;
    };
    double rs = rangeAt(EngineEvent::TrackStarted), re = rangeAt(EngineEvent::Ended);
    Check(std::fabs(rangeDuration - 1.0) < 0.001 && rs > 0 && re > rs && std::fabs((re - rs) - 1.0) < 0.15,
          L"ENGINE range 0.5-1.5 s: duration %.3f s (expect 1.000), played %.2f s", rangeDuration, re - rs);
    engine.Stop();
    Pump(200);

    // The three CUE tracks of Ramp.wav (0.933 s, 0.333 s, 0.733 s) play as one gapless run.
    ev.list.clear();
    ev.times.clear();
    const std::wstring ramp = cueDir + L"\\Ramp.wav";
    ev.cueItems.clear();
    for (int n = 1; n <= 3; ++n) {
        double s = 0, e = 0;
        bool found = SubtrackRange(SubtrackPath(ramp, n), s, e);
        ev.cueItems.push_back(PlayItem{(unsigned long long)(100 + n - 1), SubtrackPath(ramp, n), 1,
                                       e > 0 ? e - s : 0, found ? s : 0, found ? e : 0});
    }
    engine.Play(ev.cueItems[0]);
    Pump(6000, [&] { return !ev.list.empty() && ev.list.back().first == EngineEvent::Ended; });
    auto cueAt = [&](EngineEvent e, unsigned long long tok) -> double {
        for (size_t i = 0; i < ev.list.size(); ++i)
            if (ev.list[i].first == e && ev.list[i].second == tok) return ev.times[i] / 1000.0;
        return -1;
    };
    double c1 = cueAt(EngineEvent::TrackStarted, 100), c2 = cueAt(EngineEvent::TrackStarted, 101),
           c3 = cueAt(EngineEvent::TrackStarted, 102);
    double cEnd = -1;
    for (size_t i = 0; i < ev.list.size(); ++i)
        if (ev.list[i].first == EngineEvent::Ended) cEnd = ev.times[i] / 1000.0;
    Check(c1 > 0 && c2 > c1 && c3 > c2 && std::fabs((c2 - c1) - 70 / 75.0) < 0.15 &&
              std::fabs((c3 - c2) - 1.0 / 3.0) < 0.15 && std::fabs((cEnd - c1) - 2.0) < 0.2,
          L"ENGINE CUE tracks: switches after %.2f s (expect 0.93) and %.2f s (expect 0.33), ended after %.2f s (expect 2.00)",
          c2 - c1, c3 - c2, cEnd - c1);
    ev.cueItems.clear();
    engine.Stop();
    Pump(200);

    PlayItem section{200, gapDir + L"\\a.flac"};
    section.loopStart = 0.5;
    section.loopEnd = 0.75;
    ev.list.clear();
    engine.Play(section);
    bool sectionOk = true;
    for (int i = 0; i < 8; ++i) {
        Pump(100);
        double p = engine.Position();
        sectionOk &= p >= 0.5 && p < 0.75 && std::fabs(engine.Duration() - 2.5) < 0.001;
    }
    sectionOk &= std::none_of(ev.list.begin(), ev.list.end(), [](const auto& e) { return e.first == EngineEvent::Ended; });
    engine.Seek(2.0);
    Pump(100);
    Check(sectionOk && engine.Position() >= 0.5 && engine.Position() < 0.75,
          L"ENGINE section repeats without ending and keeps the playhead and seeks within its marks");
    engine.SetPaused(true);
    Pump(200);
    section.loopStart = section.loopEnd = 0;
    engine.Seek(1.2, &section);
    Pump(200);
    Check(engine.Paused() && std::fabs(engine.Position() - 1.2) < 0.02,
          L"ENGINE clearing a section rebuilds playback at the requested full-track position while paused");
    section.loopStart = 1.0;
    section.loopEnd = 1.25;
    engine.Seek(0.1, &section);
    Pump(200);
    Check(engine.Paused() && std::fabs(engine.Position() - 1.0) < 0.02,
          L"ENGINE arming or editing a section takes effect immediately and preserves pause");
    engine.Stop();
    Pump(100);

    // Tempo and then speed at 2x, chosen while a track plays as the menu does: the position runs at
    // twice the clock in each track's own time, and the gapless b.flac (3.5 s) lasts half as long. The
    // slope is measured once the change is heard, so neither start-up nor the re-time counts.
    testlog::Cases rates;
    for (bool speed : {false, true}) {
        ev.list.clear();
        ev.times.clear();
        engine.Play(PlayItem{1, gapDir + L"\\a.flac"});
        Pump(3000, [&] { return engine.Position() > 0.05; });
        if (speed) engine.SetSpeed(2.0);
        else engine.SetTempo(2.0);
        Pump(400);
        LARGE_INTEGER frequency, first, last;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&first);
        const double from = engine.Position();
        Pump(800);
        QueryPerformanceCounter(&last);
        const double slope = (engine.Position() - from) / ((last.QuadPart - first.QuadPart) / (double)frequency.QuadPart);
        Pump(6000, [&] { return !ev.list.empty() && ev.list.back().first == EngineEvent::Ended; });
        t0 = 0;
        const double second = at(EngineEvent::TrackStarted, 2), ended = at(EngineEvent::Ended, 0);
        wchar_t detail[96];
        swprintf_s(detail, L"%ls runs at %.2fx, b.flac for %.2f s", speed ? L"speed" : L"tempo", slope, ended - second);
        rates.Add(second > 0 && std::fabs((ended - second) - 1.75) < 0.15 && std::fabs(slope - 2) < 0.1, detail);
        engine.SetSpeed(1);
        engine.SetTempo(1);
        engine.Stop();
        Pump(200);
    }
    Check(rates.Passed(), L"ENGINE at 2x speed and 2x tempo track time runs at twice the clock and b.flac plays for 1.75 s%ls",
          rates.failed.c_str());

    // An empty range reaches EOF without presenting any audio to the device. A marker at the
    // current output position must not announce it as audible before any sample is consumed.
    ev.list.clear();
    ev.times.clear();
    engine.Play(PlayItem{399, gapDir + L"\\a.flac", 1, 0, 0.5, 0.5});
    Pump(2000, [&] { return !ev.list.empty() && ev.list.back().first == EngineEvent::Ended; });
    Check(!ev.list.empty() && ev.list.back().first == EngineEvent::Ended &&
              std::none_of(ev.list.begin(), ev.list.end(), [](const auto& e) {
                  return e.first == EngineEvent::TrackStarted && e.second == 399;
              }), L"ENGINE an empty range ends without announcing audio that never played");
    engine.Stop();
    Pump(200);

    // Two damaged tracks decoded ahead must keep separate problems and still reach the good one.
    std::string bytes = ReadTestBytes(gapDir + L"\\a.flac");
    const std::wstring damaged = cueDir + L"\\damaged.flac";
    if (bytes.size() > 4096) std::fill_n(bytes.begin() + bytes.size() / 2, 64, '\xFF');
    Ready(bytes.size() > 4096 && WriteTestBytes(damaged, bytes), L"engine damaged FLAC fixture is prepared");
    std::wstring openError;
    auto broken = OpenDecoder(damaged, 48000, openError);
    uint64_t goodFrames = 0;
    float block[4096 * 2];
    if (broken) while (size_t n = broken->Read(block, 4096)) goodFrames += n;
    const double badSeek = broken ? (goodFrames + 0.5) / broken->Rate() : 0;
    broken.reset();
    auto damagedEvents = [&](unsigned long long token) {
        return std::count(ev.list.begin(), ev.list.end(), std::pair{EngineEvent::Damaged, token});
    };
    auto ended = [&] { return std::count(ev.list.begin(), ev.list.end(), std::pair{EngineEvent::Ended, 0ull}) > 0; };
    ev.cueItems = {{100, damaged}, {101, damaged}, {102, gapDir + L"\\a.flac"}};
    ev.list.clear();
    engine.Play(ev.cueItems.front());
    Pump(10000, ended);
    const std::wstring secondProblem = engine.TakeError(101), firstProblem = engine.TakeError(100);
    const bool continued = ended() && damagedEvents(100) == 1 && damagedEvents(101) == 1 && !damagedEvents(102) &&
              std::count(ev.list.begin(), ev.list.end(), std::pair{EngineEvent::TrackStarted, 102ull}) == 1 &&
              firstProblem.find(L"Playback failed at 0:") != std::wstring::npos &&
              secondProblem.find(L"Playback failed at 0:") != std::wstring::npos && engine.TakeError(100).empty() &&
              engine.TakeError(101).empty();
    std::wstring events;
    for (const auto& event : ev.list)
        events += L" " + std::to_wstring((unsigned)event.first) + L":" + std::to_wstring(event.second);
    ev.cueItems.clear();
    engine.Stop();
    Pump(100);
    ev.list.clear();
    engine.Play(PlayItem{403, damaged}, badSeek);
    Pump(2000, ended);
    const std::wstring seekProblem = engine.TakeError(403);
    Check(continued && badSeek > 0 && ended() &&
              std::count(ev.list.begin(), ev.list.end(), std::pair{EngineEvent::Error, 403ull}) == 1 &&
              seekProblem.find(L"Couldn't seek") != std::wstring::npos,
          L"ENGINE damaged tracks keep separate problems, continue to good audio and report failed seeks: "
          L"events%ls; problems=\"%ls\" / \"%ls\"; seek=\"%ls\"", events.c_str(),
          firstProblem.c_str(), secondProblem.c_str(), seekProblem.c_str());
    engine.Stop();
    Pump(100);

    // A track without ReplayGain data reports one loudness estimate from its first seconds.
    PlayItem unmeasured{400, gapDir + L"\\a.flac"};
    unmeasured.estimate = true;
    ev.list.clear();
    engine.Play(unmeasured);
    Pump(4000, [&] { return std::any_of(ev.list.begin(), ev.list.end(), [](const auto& e) { return e.first == EngineEvent::Estimated; }); });
    const double lufs = engine.TakeEstimate(400);
    Check(std::isfinite(lufs) && lufs > -60 && lufs < 0 && std::isnan(engine.TakeEstimate(400)),
          L"ENGINE quick loudness estimate of an unmeasured track: %.1f LUFS, taken once", lufs);
    engine.Stop();
    Pump(100);

    engine.Shutdown();
    g_events = nullptr;
    {
        using namespace app;
        auto savedLibrary = std::move(g_lib);
        auto savedPlaylists = std::move(g.playlists);
        auto savedTokens = std::move(g.tokens);
        auto savedQueue = std::move(g.queue);
        const auto savedIni = g.iniPath;
        const HWND savedWindow = g.hwnd, savedShuffleButton = g.buttons[3], savedRepeatButton = g.buttons[4];
        const auto savedMode = g.rgMode;
        const auto savedPlaying = g.playing;
        const auto savedPlayingToken = g.playingToken, savedDecodingToken = g.decodingToken;
        const auto savedFirst = g.firstValidToken, savedNext = g.nextToken;
        const int savedActive = g.active, savedPlayingPl = g.playingPl, savedShuffle = g.shuffle;
        const auto savedRepeat = g.repeat;
        auto savedShuffleQueue = std::move(g.shuffleQueue);
        const int savedShufflePl = g.shufflePl;
        const auto savedShuffleToken = g.shuffleToken;
        const bool savedShuffleStarted = g.shuffleStarted, savedSkipShuffle = g.skipAwareShuffle, savedRadio = g.radio;
        const bool savedRg = g.rgOn, savedEstimate = g.rgEstimate, savedSound = g.soundEffects;
        const bool savedStatsDirty = g.statsDirty, savedSearchPending = g.searchPending;
        const float savedKeep = g.rgKeep;
        g_lib = Library();
        const std::wstring tone = cueDir + L"\\order-tone.wav";
        const bool written = WriteTestBytes(tone, MakeSineWav(6, 6));
        TrackId id = g_lib.Add(tone);
        g_lib[id].durationMs = 6000;
        g_lib[id].rgAlbumGain = 0;
        g_lib[id].rgTrackGain = -3000;
        g.playlists = {{L"Order test", {id}}};
        g.active = g.playingPl = 0;
        g.iniPath = cueDir + L"\\order-settings.ini";
        g.hwnd = hwnd;
        g.buttons[3] = CreateWindowW(WC_BUTTONW, L"Shuffle", WS_CHILD | BS_OWNERDRAW, 0, 0, 1, 1, hwnd,
                                     (HMENU)(INT_PTR)IDC_SHUFFLE, GetModuleHandleW(nullptr), nullptr);
        g.buttons[4] = CreateWindowW(WC_BUTTONW, L"Repeat", WS_CHILD | BS_OWNERDRAW, 0, 0, 1, 1, hwnd,
                                     (HMENU)(INT_PTR)IDC_REPEAT, GetModuleHandleW(nullptr), nullptr);
        g.statsDirty = g.searchPending = false;
        g.rgOn = true;
        g.rgEstimate = g.soundEffects = false;
        g.rgKeep = 0;
        g.rgMode = ReplayGainMode::PlaybackOrder;
        g.shuffle = kShuffleOff;
        g.repeat = Repeat::Off;
        g.playing = id;
        const auto token = NewToken(id, 0, 0);
        g.firstValidToken = g.playingToken = g.decodingToken = token;
        ev.engine = &g.engine;
        ev.applicationCommands = true;
        ev.list.clear(); ev.times.clear();
        g_events = &ev;
        const bool initialized = g.engine.Init(hwnd, error);
        if (Ready(written && initialized && g.buttons[3] && g.buttons[4],
                  L"playback-order continuity fixture, controls and output are ready")) {
            g.engine.SetVolume(0.0001f);  // below audibility, while leaving the output samples measurable
            g.engine.SetLimiter(true);
            g.engine.SetScope(true);
            auto level = [&] {
                float samples[1024 * 2]{};
                g.engine.Scope(samples, 1024);
                double sum = 0;
                for (float value : samples) sum += value * value;
                return std::sqrt(sum / std::size(samples));
            };
            testlog::Cases controls;
            for (auto mode : {ReplayGainMode::Track, ReplayGainMode::PlaybackOrder}) {
                g.engine.Stop(); Pump(200);
                ev.list.clear(); ev.times.clear();
                g.rgMode = mode;
                g.shuffle = kShuffleOff;
                g.repeat = Repeat::Off;
                g.skipAwareShuffle = g.radio = false;
                g.shuffleQueue.clear();
                g.shuffleStarted = false;
                g.engine.Play(MakeItem(token, id));
                Pump(2500, [&] { return g.engine.Position() > 0.4; });
                const double beforeLevel = level(), beforePosition = g.engine.Position();
                const ULONGLONG changed = GetTickCount64();
                SendMessageW(g.buttons[3], BM_CLICK, 0, 0);
                double lowestLevel = beforeLevel;
                for (int tick = 0; tick < 15; ++tick) {
                    if (tick == 3) SendMessageW(g.buttons[4], BM_CLICK, 0, 0);
                    if (tick == 7) OnCommand(IDM_SKIP_SHUFFLE, 0, nullptr);
                    if (tick == 11) OnCommand(IDM_RADIO, 0, nullptr);
                    Pump(20);
                    lowestLevel = std::min(lowestLevel, level());
                }
                const double elapsed = (GetTickCount64() - changed) / 1000.0;
                const double afterLevel = level(), advanced = g.engine.Position() - beforePosition;
                controls.Add(g.shuffle == kShuffleTracks && g.repeat == Repeat::All && g.skipAwareShuffle && g.radio &&
                                 !g.engine.Paused() && beforeLevel > 1e-7 && lowestLevel > beforeLevel * 0.9 &&
                                 afterLevel > beforeLevel * 0.9 && afterLevel < beforeLevel * 1.1 &&
                                 std::fabs(advanced - elapsed) < 0.12,
                             std::wstring(mode == ReplayGainMode::Track ? L"track gain" : L"playback-order gain") +
                                 L" (level " + std::to_wstring(beforeLevel) + L" -> " + std::to_wstring(afterLevel) +
                                 L", advanced " + std::to_wstring(advanced) + L" in " + std::to_wstring(elapsed) + L" s)");
            }
            Check(controls.Passed(), L"SHUFFLE, REPEAT, skip-aware shuffle and radio leave the playing audio unchanged%ls",
                  controls.failed.c_str());
            {
                const auto savedLayout = g.layout;
                const auto savedTheme = g.themeMode;
                const auto savedSize = g.size;
                const UINT savedDpi = g.dpi;
                const bool savedDark = g.dark, savedLive = g.liveWaveform, savedFountain = g.liveFountain;
                const bool savedFull = g.fullLiveWaveform, savedStopAfter = g.stopAfterCurrent;
                g.dpi = 96; g.dark = true; g.size = ViewSize::Normal;
                g.liveWaveform = g.fullLiveWaveform = true; g.stopAfterCurrent = false;
                g.engine.SetVolume(0);
                g.engine.Play(PlayItem{901, visualFile});
                Pump(2000, [&] { return g.engine.Duration() == 300 && g.engine.Position() > 0.2; });
                g.engine.Seek(4);
                Pump(2000, [&] { return g.engine.Position() >= 4.4 && g.engine.Position() < 4.8; });
                HDC dc = CreateCompatibleDC(nullptr);
                BITMAPINFO info{};
                info.bmiHeader = {sizeof(BITMAPINFOHEADER), 120, -26, 1, 32, BI_RGB};
                void* pixels = nullptr;
                HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
                HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : nullptr;
                if (Ready(dc && bitmap, L"offscreen live player rendering is ready")) {
                    testlog::Cases motion;
                    for (bool fountain : {false, true}) for (Dock dock : {Dock::Top, Dock::Bottom}) {
                        g.layout.player = dock; g.liveFountain = fountain;
                        g.themeMode = fountain ? ThemeMode::Deltarune : ThemeMode::Dark;
                        std::vector<uint32_t> previous;
                        int changed = 0;
                        for (int tick = 0; tick < 12; ++tick) {
                            Pump(33);
                            // Hold the song fraction fixed, as it is for many frames in a long song.
                            // Only the audible samples may change these frames; the fountain clock is idle.
                            if (fountain) PaintFountain(dc, {0, 0, 120, 26}, 4, 116, 0.001);
                            else DrawWaveform(dc, {0, 0, 120, 26}, {4, 0, 116, 26}, 0.001);
                            GdiFlush();
                            const auto* first = (const uint32_t*)pixels;
                            std::vector<uint32_t> current(first, first + 120 * 26);
                            changed += tick && current != previous;
                            previous = std::move(current);
                        }
                        motion.Add(changed >= 8, std::wstring(fountain ? L"fountain" : L"bars") + L"/" +
                            std::to_wstring((int)dock) + L" (" + std::to_wstring(changed) + L"/11 frames)");
                    }
                    Check(motion.Passed(), L"quiet live bars and Dark Fountain animate in both 26 px bands with fixed progress%ls",
                          motion.failed.c_str());
                }
                if (old) SelectObject(dc, old);
                if (bitmap) DeleteObject(bitmap);
                if (dc) DeleteDC(dc);
                g.layout = savedLayout; g.themeMode = savedTheme; g.size = savedSize; g.dpi = savedDpi;
                g.dark = savedDark; g.liveWaveform = savedLive; g.liveFountain = savedFountain;
                g.fullLiveWaveform = savedFull; g.stopAfterCurrent = savedStopAfter;
            }
        }
        g.engine.Stop(); Pump(200); g.engine.Shutdown(); g_events = nullptr;
        ev.applicationCommands = false;
        DestroyWindow(g.buttons[3]);
        DestroyWindow(g.buttons[4]);
        DeleteFileW(tone.c_str());
        g_lib = std::move(savedLibrary);
        g.playlists = std::move(savedPlaylists);
        g.tokens = std::move(savedTokens);
        g.queue = std::move(savedQueue);
        g.iniPath = savedIni;
        g.hwnd = savedWindow; g.buttons[3] = savedShuffleButton; g.buttons[4] = savedRepeatButton;
        g.rgMode = savedMode;
        g.playing = savedPlaying;
        g.playingToken = savedPlayingToken; g.decodingToken = savedDecodingToken;
        g.firstValidToken = savedFirst; g.nextToken = savedNext;
        g.active = savedActive; g.playingPl = savedPlayingPl; g.shuffle = savedShuffle;
        g.repeat = savedRepeat;
        g.shuffleQueue = std::move(savedShuffleQueue); g.shufflePl = savedShufflePl; g.shuffleToken = savedShuffleToken;
        g.shuffleStarted = savedShuffleStarted; g.skipAwareShuffle = savedSkipShuffle; g.radio = savedRadio;
        g.rgOn = savedRg; g.rgEstimate = savedEstimate; g.soundEffects = savedSound; g.rgKeep = savedKeep;
        g.statsDirty = savedStatsDirty; g.searchPending = savedSearchPending;
    }
    {
        Engine deferred;
        ev.engine = &deferred;
        ev.list.clear(); ev.times.clear();
        g_events = &ev;
        const bool initialized = deferred.Init(hwnd, error, true);
        deferred.SetVolume(0);
        const bool stayedIdle = initialized && !deferred.Output().ready;
        deferred.Play(PlayItem{901, gapDir + L"\\a.flac"});
        Pump(2000, [&] { return deferred.Position() > 0.1; });
        Check(stayedIdle && deferred.Output().ready && deferred.Position() > 0.1,
              L"ENGINE deferred device opens on first Play and produces audible-position progress");
        deferred.Stop(); Pump(350); deferred.Shutdown(); g_events = nullptr;
    }
    TestOutputIdleRelease();
    TestExclusive(DirectoryOf(gapDir), hwnd);
    DestroyWindow(hwnd);
}
