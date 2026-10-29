// DSP checks of the self test (see selftest.cpp): loudness (EBU Tech 3341), true peak, the peak
// limiter, the ReplayGain model, the effects after the EQ (reverb, tempo, smart volume) and waveform peaks.
#include "selftest_common.h"

#include <winioctl.h>
#include <xmmintrin.h>

namespace {
void TestWaveformMotion() {
    auto tone = [](unsigned rate, double seconds, float amplitude, double frequency = 440) {
        std::vector<float> pcm((size_t)std::lround(rate * seconds) * 2);
        for (size_t i = 0; i < pcm.size() / 2; ++i)
            pcm[i * 2] = pcm[i * 2 + 1] = amplitude * (float)std::sin(6.28318530718 * frequency * i / rate);
        return pcm;
    };
    testlog::Cases attacks, falls, dynamics, cadence, stereo;
    for (unsigned rate : {44100u, 48000u, 96000u, 192000u}) {
        for (double frequency : {80., 440., 1760.}) {
            std::wstring name = std::to_wstring(rate) + L"/" + std::to_wstring((int)frequency);
            auto quiet = tone(rate, 0.3, 0.03f, frequency), note = tone(rate, 0.02, 0.5f, frequency);
            WaveformMotion motion;
            motion.Add(quiet.data(), quiet.size() / 2, rate);
            float before = motion.Level();
            motion.Add(note.data(), note.size() / 2, rate);
            float hit = motion.Level();
            attacks.Add(before < 0.15f && hit > 0.65f && hit - before > 0.5f, name);
            auto silence = tone(rate, 0.08, 0);
            motion.Add(silence.data(), silence.size() / 2, rate);
            float tail = motion.Level();
            motion.Add(silence.data(), silence.size() / 2, rate);
            float later = motion.Level();
            motion.Add(silence.data(), silence.size() / 2, rate);
            falls.Add(tail > hit * 0.35f && tail < hit && later < tail && motion.Level() < 0.25f, name);
            motion.Add(note.data(), note.size() / 2, rate);
            attacks.Add(motion.Level() > 0.65f, name + L" repeated note");

            WaveformMotion soft, loud;
            auto softNote = tone(rate, 1, 0.125f, frequency), loudNote = tone(rate, 1, 0.5f, frequency);
            soft.Add(softNote.data(), softNote.size() / 2, rate);
            loud.Add(loudNote.data(), loudNote.size() / 2, rate);
            dynamics.Add(soft.Level() > 0.15f && loud.Level() > 2.5f * soft.Level(), name);

            WaveformMotion split, opposite;
            for (size_t first = 0; first < loudNote.size() / 2;) {
                size_t count = std::min<size_t>(137, loudNote.size() / 2 - first);
                split.Add(loudNote.data() + first * 2, count, rate);
                first += count;
            }
            cadence.Add(split.Level() == loud.Level(), name);
            for (size_t i = 1; i < loudNote.size(); i += 2) loudNote[i] = -loudNote[i];
            opposite.Add(loudNote.data(), loudNote.size() / 2, rate);
            stereo.Add(opposite.Level() == loud.Level(), name);
        }
    }
    Check(attacks.Passed(), L"live waveforms swell within 20 ms on bass and melodic note attacks, including repeated notes%ls",
          attacks.failed.c_str());
    Check(falls.Passed(), L"live note attacks release smoothly and settle between accents%ls", falls.failed.c_str());
    Check(dynamics.Passed(), L"live waveforms keep soft notes visible with more contrast against strong notes%ls",
          dynamics.failed.c_str());
    Check(cadence.Passed(), L"live response is identical across PCM chunk sizes and display refresh schedules%ls",
          cadence.failed.c_str());
    Check(stereo.Passed(), L"live note response preserves opposite-phase stereo energy%ls", stereo.failed.c_str());

    WaveformMotion motion;
    auto note = tone(48000, 0.02, 0.5f);
    motion.Add(note.data(), note.size() / 2, 48000);
    std::array<uint8_t, 5> shape{0, 32, 96, 180, 255}, held = shape;
    motion.Shape(shape.data(), shape.size());
    motion.Shape(held.data(), held.size());
    Check(shape == held && shape.front() > 50 && shape.back() > 165 &&
              std::is_sorted(shape.begin(), shape.end()) && shape.front() < shape.back(),
          L"a note lifts the whole live shape while retaining its texture; drawing twice holds the same frame");
    motion = {};
    motion.Shape(shape.data(), shape.size());
    Check(motion.Level() == 0 && std::all_of(shape.begin(), shape.end(), [](uint8_t v) { return v == 0; }),
          L"resetting the live response clears the old note and its release tail");
    std::vector<float> invalid(960, std::numeric_limits<float>::quiet_NaN());
    for (size_t i = 1; i < invalid.size(); i += 2) invalid[i] = std::numeric_limits<float>::infinity();
    motion.Add(invalid.data(), invalid.size() / 2, 48000);
    Check(motion.Level() == 0, L"non-finite PCM cannot create a live visual pulse");
}
}

void TestVisuals() {
    TestWaveformMotion();
    AudioVisuals frame;
    Check(!frame.Update(false, 48000) && !frame.Moving() &&
              std::all_of(frame.wave.begin(), frame.wave.end(), [](float v) { return v == 0; }),
          L"VIS stopped audio is silent and does not keep the display timer running");
    testlog::Cases tones;
    for (unsigned rate : {44100u, 48000u, 96000u}) {
        frame = {};
        frame.Prepare(rate);
        for (size_t i = 0; i < frame.wave.size() / 2; ++i)
            frame.wave[i * 2] = frame.wave[i * 2 + 1] = (float)(0.5 * std::sin(6.28318530718 * 1000 * i / rate));
        const auto original = frame.wave;
        bool moving = frame.Update(true, rate);
        size_t peak = std::max_element(frame.levels.begin(), frame.levels.end()) - frame.levels.begin();
        double frequency = 50 * std::pow(1.37, peak * 20.0 / frame.levels.size());
        tones.Add(moving && frame.wave == original && frame.levels[peak] > 0.7f &&
                      frequency > 750 && frequency < 1300, std::to_wstring(rate));
    }
    Check(tones.Passed(), L"VIS a 1 kHz tone appears in the matching spectrum band at each output rate%ls",
          tones.failed.c_str());
    const auto levels = frame.levels;
    bool falling = frame.Update(false, 48000);
    Check(falling && frame.levels != levels &&
              std::all_of(frame.wave.begin(), frame.wave.end(), [](float v) { return v == 0; }),
          L"VIS stop clears the waveform while spectrum peaks finish their hold and fall");
    for (int i = 0; i < 160; ++i) frame.Update(false, 48000);
    Check(!frame.Moving(), L"VIS stopped spectrum settles completely");
    frame.wave[0] = std::numeric_limits<float>::quiet_NaN();
    frame.wave[1] = std::numeric_limits<float>::infinity();
    frame.Update(true, 48000);
    Check(std::all_of(frame.wave.begin(), frame.wave.end(), [](float v) { return std::isfinite(v); }) &&
              std::all_of(frame.levels.begin(), frame.levels.end(), [](float v) { return std::isfinite(v); }),
          L"VIS non-finite display samples cannot escape into drawing coordinates");

    testlog::Cases windows;
    for (unsigned rate : {44100u, 48000u, 96000u, 192000u, 384000u}) {
        size_t frames = frame.Prepare(rate);
        windows.Add(frames == (size_t)rate * 3 / 10 && frame.wave.size() == frames * 2, std::to_wstring(rate));
    }
    Check(windows.Passed(), L"VIS the scope retains 300 ms at each output rate rather than stretching the FFT window%ls",
          windows.failed.c_str());
    frame.Prepare(48000);
    for (size_t i = 0; i < frame.wave.size() / 2; ++i)
        frame.wave[i * 2] = frame.wave[i * 2 + 1] = (float)(0.02 * std::sin(6.28318530718 * 10 * i / 48000));
    auto quiet = frame.Curve(587);
    for (float& v : frame.wave) v *= 20;
    auto loud = frame.Curve(587);
    bool fixed = true;
    for (size_t i = 0; i < quiet.size(); ++i) fixed &= std::fabs(loud[i] - quiet[i] * 20) < 1e-6f;
    Check(fixed && *std::max_element(quiet.begin(), quiet.end()) < 0.02001f &&
              *std::max_element(loud.begin(), loud.end()) > 0.39f,
          L"VIS the scope uses a fixed linear amplitude so quiet music is not boosted to full height");
    for (size_t i = 0; i < frame.wave.size() / 2; ++i) frame.wave[i * 2 + 1] = -frame.wave[i * 2];
    auto cancelled = frame.Curve(587);
    Check(std::all_of(cancelled.begin(), cancelled.end(), [](float v) { return v == 0; }),
          L"VIS the reference's mono trace averages signed stereo samples");
    frame.wave = {0, 0, 1, 1, 0, 0, -1, -1};
    auto weighted = frame.Curve(3), zoom = frame.Curve(8);
    Check(std::fabs(weighted[0] - 0.25f) < 1e-6f && std::fabs(weighted[1] - 0.5f) < 1e-6f &&
              std::fabs(weighted[2] + 0.75f) < 1e-6f && zoom[0] == 0.25f && zoom[1] == 0.75f && zoom[7] == -1,
          L"VIS fractional pixel spans use weighted means and sub-sample spans interpolate their midpoint");
    frame.wave = {1, 1, -1, -1, 1, 1, -1, -1};
    auto averaged = frame.Curve(2);
    Check(averaged[0] == 0 && averaged[1] == 0,
          L"VIS rapid alternation averages to silence instead of aliasing into a full-height trace");
    frame = {};
    frame.Prepare(48000);
    for (size_t i = 0; i < frame.wave.size() / 2; ++i)
        frame.wave[i * 2] = frame.wave[i * 2 + 1] = (float)(0.5 * std::sin(2 * 3.141592653589793 * 50 * i / 48000));
    frame.Update(true, 48000);
    const float height = frame.levels[0];
    frame.Update(false, 48000, 0.2);
    bool held = frame.levels[0] == 0 && frame.peaks[0] == height;
    frame.Update(false, 48000, 0.2);
    Check(std::fabs(height - (60 - 6.0206f) / 66) < 0.002 && held &&
              std::fabs(frame.peaks[0] - (height - (0.4f - 0.333f) * 30 / 66)) < 1e-5,
          L"VIS foobar's bass band uses fixed dB scaling, holds 333 ms, then falls at 30 dB per second");
    auto slow = frame, fast = frame;
    for (int i = 0; i < 12; ++i) slow.Update(false, 48000, 1.0 / 30);
    for (int i = 0; i < 24; ++i) fast.Update(false, 48000, 1.0 / 60);
    Check(std::fabs(slow.peaks[0] - fast.peaks[0]) < 1e-5,
          L"VIS spectrum peak motion follows elapsed time at both 30 and 60 frames per second");
    auto pulseCenter = [&](double time) {
        frame.time = time;
        for (size_t i = 0; i < frame.wave.size() / 2; ++i) {
            double at = time - 0.15 + (double)i / 48000;
            frame.wave[i * 2] = frame.wave[i * 2 + 1] = std::fabs(at - 1.0) < 0.002 ? 0.8f : 0;
        }
        auto curve = frame.Curve(600);
        double sum = 0, moment = 0;
        for (size_t i = 0; i < curve.size(); ++i) { sum += curve[i]; moment += curve[i] * i; }
        return moment / sum;
    };
    double first = pulseCenter(0.94), next = pulseCenter(0.956);
    Check(std::fabs(first - next - 32) < 0.6,
          L"VIS consecutive scope frames scroll an audible pulse left by the playback clock's 32 pixels");
}

void TestEqualizer() {
    EqSettings settings;
    settings.crossfeed = true;
    std::wstring error;
    bool parsed = ParseAutoEq("\xEF\xBB\xBF# AutoEq\r\nPreamp: -6.25 dB\r\n"
        "Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.7\n"
        "Filter 2: ON PK Fc 1200 Hz Gain -3 dB Q 2.1 # notch\n"
        "Filter 3: OFF HSC Fc 8000 Hz Gain 2 dB Q 0.71\n", settings, error);
    Check(parsed && settings.enabled && settings.parametric && settings.crossfeed && settings.filterCount == 3 &&
              settings.preamp == -6.25f && settings.filters[0].type == EqFilterType::LowShelf &&
              settings.filters[1].q == 2.1f && !settings.filters[2].enabled,
          L"AutoEq imports preamp, shelves, Q, disabled filters, comments and UTF-8 BOM");
    const std::string saved = FormatAutoEq(settings);
    EqSettings roundTrip;
    Check(ParseAutoEq(saved, roundTrip, error) && FormatAutoEq(roundTrip) == saved,
          L"AutoEq text round trips all filter parameters");
    bool rejected = true;
    for (const char* bad : {"GraphicEQ: 20 1; 30 2", "Preamp: -3 dB", "Filter 1: ON PK Fc nan Hz Gain 1 dB Q 1",
            "Filter 1: ON PK Fc 1000 Hz Gain inf dB Q 1", "Filter 1: ON PK Fc 0 Hz Gain 1 dB Q 1",
            "Filter 1: ON PK Fc 1000 Hz Gain 1 dB Q 0", "Filter 1: ON PK Fc 1000 Hz Gain 1 dB Q 1 junk",
            "Filter 1: ON LP Fc 1000 Hz Gain 1 dB Q 1", "Preamp: -99 dB\nFilter 1: ON PK Fc 100 Hz Gain 1 dB Q 1"}) {
        rejected &= !ParseAutoEq(bad, settings, error) && !error.empty() && FormatAutoEq(settings) == saved;
    }
    std::string many;
    for (int i = 1; i <= 20; ++i)
        many += "Filter " + std::to_string(i) + ": ON PK Fc 1000 Hz Gain 0 dB Q 1\n";
    rejected &= ParseAutoEq(many, roundTrip, error) && roundTrip.filterCount == 20;
    rejected &= !ParseAutoEq(many + "Filter 21: ON PK Fc 1000 Hz Gain 0 dB Q 1", roundTrip, error) &&
                roundTrip.filterCount == 20;
    Check(rejected, L"AutoEq rejects malformed, unsupported, non-finite and oversized profiles without partial changes");

    auto response = [](EqSettings s, double frequency, unsigned rate = 48000) {
        Equalizer eq;
        eq.Configure(s, rate);
        std::vector<float> samples(rate * 2);
        for (unsigned i = 0; i < rate; ++i) samples[i * 2] = (float)(0.1 * std::sin(2 * 3.14159265358979 * frequency * i / rate));
        // Process irregular chunks too: filter state must survive callback boundaries.
        for (size_t at = 0; at < rate;) {
            size_t frames = std::min<size_t>(137, rate - at);
            eq.Process(samples.data() + 2 * at, frames);
            at += frames;
        }
        double left = 0, right = 0;
        for (unsigned i = rate / 2; i < rate; ++i) {
            left += samples[i * 2] * samples[i * 2];
            right += samples[i * 2 + 1] * samples[i * 2 + 1];
        }
        return std::pair<double, double>{std::sqrt(left / (rate / 2) / 0.005), std::sqrt(right / (rate / 2) / 0.005)};
    };
    settings = {};
    ParseAutoEq("Filter 1: ON PK Fc 1000 Hz Gain 6 dB Q 2", settings, error);
    bool peaks = true;
    for (unsigned rate : {32000u, 44100u, 48000u, 96000u}) {
        auto gain = response(settings, 1000, rate);
        peaks &= std::fabs(20 * std::log10(gain.first) - 6) < 0.02 && gain.second == 0;
    }
    double narrow = response(settings, 3000).first;
    settings.filters[0].q = 0.3f;
    Check(peaks && response(settings, 3000).first > narrow + 0.3,
          L"parametric peak reaches specified gain at multiple rates, Q changes bandwidth, channels stay independent");
    bool graphic = true;
    for (unsigned rate : {44100u, 48000u, 96000u}) for (int band = 0; band < kEqBands; ++band) {
        EqSettings single;
        single.enabled = true;
        for (float gain : {-12.0f, 6.0f, 12.0f}) {
            single.gains[band] = gain;
            auto measured = response(single, kEqFrequencies[band], rate);
            graphic &= std::fabs(20 * std::log10(measured.first) - gain) < 0.03 && measured.second == 0;
        }
    }
    Check(graphic, L"all ten graphic EQ bands reach their specified boost/cut at 44.1, 48 and 96 kHz");
    // Neighbouring sliders don't add up: each lands on its own value at its center. And the top band
    // a 16 kHz filter keeps its width near Nyquist (the bilinear design squeezed +12 to about +2 at 10 kHz).
    EqSettings zigzag;
    zigzag.enabled = true;
    zigzag.gains = {6, 6, -6, 9, 9, 9, 0, -12, 3, 6};
    double worstBand = 0;
    for (int band = 0; band < kEqBands; ++band)
        worstBand = std::max(worstBand, std::fabs(20 * std::log10(response(zigzag, kEqFrequencies[band], 44100).first) - zigzag.gains[band]));
    EqSettings top;
    ParseAutoEq("Filter 1: ON PK Fc 16000 Hz Gain 12 dB Q 1.41", top, error);
    const double skirt = 20 * std::log10(response(top, 10000, 44100).first);
    Check(worstBand < 0.1 && skirt > 3.5,
          L"graphic sliders land within %.2f dB of their values with neighbours set; a +12 dB 16 kHz peak gives %.1f dB at 10 kHz",
          worstBand, skirt);
    settings.filters[0] = {EqFilterType::LowShelf, 1000, 6, 0.707f, true};
    auto low = response(settings, 40).first, lowFlat = response(settings, 10000).first;
    settings.filters[0].type = EqFilterType::HighShelf;
    auto high = response(settings, 10000).first, highFlat = response(settings, 40).first;
    Check(std::fabs(20 * std::log10(low) - 6) < 0.05 && std::fabs(lowFlat - 1) < 0.01 &&
              std::fabs(20 * std::log10(high) - 6) < 0.05 && std::fabs(highFlat - 1) < 0.01,
          L"shelves reach specified bass/treble gain and leave the opposite end flat");
    settings.filters[0].enabled = false;
    settings.preamp = -6;
    Check(std::fabs(20 * std::log10(response(settings, 1000).first) + 6) < 0.01,
          L"disabled filters are bypassed while profile preamp remains active");
    settings = {};
    settings.crossfeed = true;
    auto bass = response(settings, 100), treble = response(settings, 10000);
    const double feed = 20 * std::log10(bass.second / bass.first);
    Check(feed < -4.4 && feed > -5.5 && treble.second < bass.second * 0.1 && treble.first > 0.75,
          L"crossfeed works with EQ off and feeds bass into the opposite ear at the bs2b level: %.2f dB", feed);
    Equalizer eq;
    eq.Configure(settings, 48000);
    std::vector<float> mono(8192, 0.25f);
    eq.Process(mono.data(), mono.size() / 2);
    Check(std::fabs(mono.back() - 0.25f) < 1e-6, L"crossfeed preserves steady mono level");
    eq.Configure({}, 48000);
    std::vector<float> fadeOut(2 * 48000 / 50, 0.25f);
    eq.Process(fadeOut.data(), fadeOut.size() / 2);
    float bypass[] = {0.123f, -0.456f};
    eq.Process(bypass, 1);
    Check(!eq.Active() && bypass[0] == 0.123f && bypass[1] == -0.456f,
          L"disabling EQ and crossfeed fades out, then restores bit-exact bypass");

    // Changing the curve while playing crossfades instead of restarting filters: the output moves
    // no faster than the signal itself does (a reset filter state would jump by about the signal).
    bool smooth = true;
    for (bool parametric : {false, true}) for (bool toggle : {false, true}) {
        EqSettings before, after;
        before.enabled = after.enabled = true;
        before.parametric = after.parametric = parametric;
        before.filterCount = after.filterCount = 1;
        before.gains[2] = 12;
        after.gains[2] = -12;
        before.filters[0] = {EqFilterType::LowShelf, 150, 12, 0.7f, true};
        after.filters[0] = {EqFilterType::Peak, 300, -12, 2, true};
        if (toggle) after.enabled = false;
        std::vector<float> sine(48000 * 2);
        for (size_t i = 0; i < sine.size() / 2; ++i)
            sine[i * 2] = sine[i * 2 + 1] = (float)(0.2 * std::sin(2 * 3.14159265358979 * 125 * i / 48000));
        Equalizer live;
        live.Configure(before, 48000);
        live.Process(sine.data(), 24000);
        double steady = 0;
        for (size_t i = 12000; i < 24000; ++i) steady = std::max(steady, (double)std::fabs(sine[i * 2] - sine[i * 2 - 2]));
        // Several changes in quick succession, as a dragged slider sends them.
        for (size_t at = 24000; at < 48000; at += 137) {
            if (at < 26000) live.Configure((at / 137) % 2 ? after : before, 48000);
            live.Process(sine.data() + at * 2, std::min<size_t>(137, 48000 - at));
        }
        live.Configure(after, 48000);
        double jump = 0;
        for (size_t i = 24000; i < 48000; ++i) jump = std::max(jump, (double)std::fabs(sine[i * 2] - sine[i * 2 - 2]));
        smooth &= steady > 0 && jump < steady * 1.5;
    }
    Check(smooth, L"EQ changes and switching it off while playing crossfade without clicks");
    const unsigned csr = _mm_getcsr();
    settings = {};
    settings.enabled = true;
    settings.gains[0] = 12;
    eq.Configure(settings, 48000);
    std::vector<float> tail(48000 * 2 * 4);
    tail[0] = 1;
    eq.Process(tail.data(), tail.size() / 2);
    Check(_mm_getcsr() == csr && tail.back() == 0.0f,
          L"EQ flushes denormals only while it runs: the caller's floating-point mode is restored");
    eq.Configure(settings, 0);
    Check(!eq.Active(), L"zero sample rate safely bypasses processing");

    settings = {};
    settings.enabled = settings.crossfeed = true;
    settings.gains[1] = 8;
    std::vector<float> steady(48000 * 2);
    for (size_t i = 0; i < steady.size() / 2; ++i)
        steady[i * 2] = (float)(0.1 * std::sin(2 * 3.14159265358979 * 62 * i / 48000));
    auto uninterrupted = steady, refreshed = steady;
    Equalizer reference, reconfigured;
    reference.Configure(settings, 48000);
    reconfigured.Configure(settings, 48000);
    reference.Process(uninterrupted.data(), uninterrupted.size() / 2);
    for (size_t at = 0; at < refreshed.size() / 2; at += 137) {
        reconfigured.Configure(settings, 48000);
        reconfigured.Process(refreshed.data() + at * 2, std::min<size_t>(137, refreshed.size() / 2 - at));
    }
    Check(uninterrupted == refreshed, L"reapplying unchanged EQ preserves biquad tails and crossfeed history exactly");

    std::vector<float> signal(48000 * 2);
    for (size_t i = 0; i < signal.size() / 2; ++i)
        signal[i * 2] = signal[i * 2 + 1] = (float)(0.8 * std::sin(2 * 3.14159265358979 * 1000 * i / 48000));
    Check(RenderPlayerPcm(signal.data(), signal.size() / 2, 48000, {}, false) == signal,
          L"production renderer retains exact PCM with EQ and ReplayGain off");
    settings = {};
    settings.enabled = true;
    settings.preamp = -6;
    auto attenuated = RenderPlayerPcm(signal.data(), signal.size() / 2, 48000, settings, false);
    float attenuatedPeak = 0;
    for (float sample : attenuated) attenuatedPeak = std::max(attenuatedPeak, std::fabs(sample));
    Check(std::fabs(attenuatedPeak - 0.8f * std::pow(10.0f, -6.0f / 20)) < 0.0001f,
          L"production renderer applies the regular EQ preamp at the requested dB gain");
    bool protectedOutput = true, affectedOutput = true;
    for (bool parametric : {false, true}) {
        settings = {};
        settings.enabled = true;
        settings.parametric = parametric;
        settings.filterCount = 1;
        settings.filters[0] = {EqFilterType::Peak, 1000, 12, 1.41f, true};
        settings.gains[5] = 12;
        for (bool replayGain : {false, true}) {
            auto rendered = RenderPlayerPcm(signal.data(), signal.size() / 2, 48000, settings, replayGain);
            float peak = 0;
            protectedOutput &= rendered.size() == signal.size();
            for (float sample : rendered) {
                peak = std::max(peak, std::fabs(sample));
                protectedOutput &= std::isfinite(sample) && std::fabs(sample) < 1.0f;
            }
            affectedOutput &= peak > 0.95f && peak < 1.0f;
        }
    }
    Check(protectedOutput && affectedOutput,
          L"production renderer limits boosted graphic/parametric EQ with ReplayGain on or off instead of hard clipping");

    settings = {};
    settings.enabled = true;
    settings.gains[4] = settings.gains[5] = 6;
    eq.Configure(settings, 48000);
    const double overlap = eq.PeakGainDb();
    double measuredPeak = -100;
    for (int frequency = 400; frequency <= 1200; frequency += 10)
        measuredPeak = std::max(measuredPeak, 20 * std::log10(response(settings, frequency).first));
    Check(overlap > 6 && overlap < 12 && overlap >= measuredPeak - 0.01 && overlap < measuredPeak + 0.08,
          L"EQ headroom follows the combined overlapping-band response: %.2f dB, measured %.2f (not sum of boosts)",
          overlap, measuredPeak);
    ParseAutoEq("Preamp: -6 dB\nFilter 1: ON PK Fc 1777 Hz Gain 12 dB Q 100", settings, error);
    bool accuratePeaks = true;
    for (unsigned rate : {32000u, 44100u, 48000u, 96000u}) {
        eq.Configure(settings, rate);
        accuratePeaks &= std::fabs(eq.PeakGainDb() - 6) < 0.06;
    }
    settings.preamp = -12;
    eq.Configure(settings, 48000);
    Check(accuratePeaks && std::fabs(eq.PeakGainDb()) < 0.06,
          L"EQ headroom finds very narrow parametric peaks at multiple rates and includes the AutoEq preamp once");
    settings.enabled = false;
    settings.crossfeed = true;
    eq.Configure(settings, 48000);
    Check(eq.PeakGainDb() == 0, L"disabled filters and independent crossfeed do not consume ReplayGain EQ headroom");
}

void TestLoudness() {
    // EBU Tech 3341 case 1: stereo 1 kHz sine at -23 dBFS reads -23.0 LUFS.
    const unsigned rate = 48000;
    LoudnessMeter meter(rate);
    std::vector<float> buf(rate * 2);
    double amp = std::pow(10.0, -23.0 / 20.0);
    for (int sec = 0; sec < 20; ++sec) {
        for (unsigned i = 0; i < rate; ++i)
            buf[i * 2] = buf[i * 2 + 1] = (float)(amp * std::sin(2 * 3.14159265358979 * 1000 * (sec * (double)rate + i) / rate));
        meter.Add(buf.data(), rate);
    }
    double lufs = meter.Integrated();
    Check(std::fabs(lufs + 23.0) < 0.1, L"LOUDNESS EBU 3341 sine: %.2f LUFS (expected -23.00), ReplayGain %+.2f dB", lufs,
          kReplayGainReference - lufs);
}

void TestTruePeak() {
    // Independent original scalar FIR oracle: stereo lanes, ring wrap, arbitrary chunking,
    // startup and Reset must all keep exactly the same reported peaks.
    struct Reference {
        double coefficients[4][24]{}, history[2][24]{}, peak=0;
        int pos=0; bool started=false;
        Reference() {
            const double pi=3.14159265358979323846;
            for (int p=0;p<4;++p) {
                double total=0;
                for (int k=0;k<24;++k) {
                    double x=k-11-p/4.0;
                    coefficients[p][k]=(x==0?1:std::sin(pi*x)/(pi*x))*(0.54-0.46*std::cos(2*pi*k/23));
                    total+=coefficients[p][k];
                }
                for (double& c:coefficients[p]) c/=total;
            }
        }
        void Add(const float* in,size_t frames) {
            if (!started && frames) {
                for (int ch=0;ch<2;++ch) for (double& x:history[ch]) x=in[ch];
                started=true;
            }
            for (size_t n=0;n<frames;++n) {
                for (int ch=0;ch<2;++ch) {
                    double x=in[n*2+ch]; peak=std::max(peak,std::fabs(x)); history[ch][pos]=x;
                    for (int p=1;p<4;++p) {
                        double sum=0;
                        for (int k=0;k<24;++k) { int at=pos-k; if (at<0) at+=24; sum+=coefficients[p][k]*history[ch][at]; }
                        peak=std::max(peak,std::fabs(sum));
                    }
                }
                pos=(pos+1)%24;
            }
        }
    } reference;
    TruePeak optimized;
    uint32_t seed=123;
    std::vector<float> noise(16384);
    for (float& x:noise) x=(float)((int32_t)(seed=1664525*seed+1013904223)/2147483648.0);
    bool identical=true;
    for (size_t at=0;at<noise.size()/2;) {
        size_t frames=std::min(size_t(1+at%137),noise.size()/2-at);
        reference.Add(noise.data()+at*2,frames); optimized.Add(noise.data()+at*2,frames);
        identical &= optimized.Peak()==(float)reference.peak;
        at+=frames;
    }
    optimized.Reset(); reference=Reference{};
    reference.Add(noise.data(),17); optimized.Add(noise.data(),17);
    Check(identical && optimized.Peak()==(float)reference.peak,L"TRUE PEAK SIMD matches original scalar FIR exactly across stereo, wrap, chunks and reset");

    // A sine at fs/4 with a 45-degree phase: the samples are all +/-0.7071, but the waveform
    // peaks at 1.0 between them. Sample peak cannot see that; true peak must.
    const unsigned rate = 48000;
    TruePeak tp;
    std::vector<float> buf(rate * 2);
    for (unsigned i = 0; i < rate; ++i) {
        double v = std::sin(2 * 3.14159265358979 * (rate / 4.0) * i / rate + 3.14159265358979 / 4);
        buf[i * 2] = buf[i * 2 + 1] = (float)v;
    }
    tp.Add(buf.data(), rate);
    Check(tp.Peak() > 0.95f && tp.Peak() < 1.05f, L"TRUE PEAK inter-sample (fs/4 sine): %.4f (samples are 0.7071)",
          tp.Peak());

    // Fade the edges so startup ringing cannot hide a missing half-sample phase.
    TruePeak faded;
    for (unsigned i = 0; i < rate; ++i) {
        const float envelope = std::min({1.0f, i / 512.0f, (rate - 1 - i) / 512.0f});
        buf[i * 2] *= envelope;
        buf[i * 2 + 1] *= envelope;
    }
    for (unsigned i = 0; i < rate; i += 127) faded.Add(buf.data() + i * 2, std::min(127u, rate - i));
    Check(std::fabs(faded.Peak() - 1.0f) < 0.005f,
          L"TRUE PEAK half-sample peaks with faded edges and split blocks: %.4f", faded.Peak());

    // A DC signal reads its own value, and an impulse reads 1.0.
    TruePeak dc;
    std::vector<float> flat(200, 0.5f);
    dc.Add(flat.data(), 100);
    Check(std::fabs(dc.Peak() - 0.5f) < 0.001f, L"TRUE PEAK DC: %.4f", dc.Peak());
    TruePeak impulse;
    std::vector<float> one(200, 0.0f);
    one[100] = one[101] = 1.0f;
    impulse.Add(one.data(), 100);
    Check(std::fabs(impulse.Peak() - 1.0f) < 0.001f, L"TRUE PEAK impulse: %.4f", impulse.Peak());
}

void TestPeakLimiter() {
    const unsigned rate = 48000;
    const double pi = 3.14159265358979;
    // Below full scale the limiter only delays: every sample comes out bit-exact, block sizes aside.
    PeakLimiter quiet;
    quiet.Configure(rate);
    std::vector<float> in(rate * 2), out;
    uint32_t seed = 1;
    // White noise is the worst case for overshoot between samples (up to 1.25x its sample peak), so
    // it stays at 0.75 to be under the ceiling between samples too.
    for (float& v : in) v = (float)(((seed = seed * 1664525u + 1013904223u) >> 8) / 16777216.0 * 1.5 - 0.75);
    out = in;
    for (size_t at = 0, block = 1; at < rate; at += block, block = block * 3 % 1021 + 1)
        quiet.Process(out.data() + at * 2, std::min<size_t>(block, rate - at));
    const size_t lag = quiet.Latency();
    bool exact = lag > 0 && lag < rate / 100;
    for (size_t i = lag * 2; i < out.size() && exact; ++i) exact = out[i] == in[i - lag * 2];
    Check(exact, L"LIMITER under full scale: bit-exact, delayed %zu frames", lag);
    // Engaged mid-song, the look-ahead holds the last frame out instead of inserting silence.
    PeakLimiter joined;
    joined.Configure(rate);
    joined.Prime(0.5f, -0.5f);
    std::vector<float> steady(rate / 10 * 2);
    for (size_t i = 0; i < steady.size(); i += 2) steady[i] = 0.5f, steady[i + 1] = -0.5f;
    joined.Process(steady.data(), steady.size() / 2);
    bool gapless = true;
    for (size_t i = 0; i < steady.size(); i += 2) gapless &= steady[i] == 0.5f && steady[i + 1] == -0.5f;
    Check(gapless, L"LIMITER engaged mid-song continues the waveform instead of inserting a silent gap");

    // A 1 kHz sine at +6 dB for half a second, between quiet passages: nothing leaves full scale,
    // the loud part sits just under the limiter's ceiling (limited, not clipped), and the quiet
    // parts are untouched except shortly after the loud part, while the gain recovers.
    PeakLimiter loud;
    loud.Configure(rate);
    std::vector<float> sig(rate * 2 * 2);
    auto level = [&](size_t i) { return i >= rate / 2 && i < rate ? 2.0 : 0.25; };
    for (size_t i = 0; i < sig.size() / 2; ++i) sig[i * 2] = sig[i * 2 + 1] = (float)(level(i) * std::sin(2 * pi * 1000 * i / rate));
    std::vector<float> res = sig;
    for (size_t at = 0; at < res.size() / 2; at += 480) loud.Process(res.data() + at * 2, 480);
    float maxOut = 0, loudPeak = 0, earlyError = 0, lateError = 0;
    for (size_t i = lag; i < res.size() / 2; ++i) {
        const size_t src = i - lag;
        const float y = res[i * 2];
        maxOut = std::max(maxOut, std::fabs(y));
        if (src >= rate / 2 + rate / 10 && src < rate) loudPeak = std::max(loudPeak, std::fabs(y));
        if (src < rate / 2 - lag) earlyError = std::max(earlyError, std::fabs(y - sig[src * 2]));
        if (src > rate + rate / 2) lateError = std::max(lateError, std::fabs(y - sig[src * 2]));
    }
    Check(maxOut <= 1.0f && loudPeak > 0.95f && earlyError == 0 && lateError < 0.001f,
          L"LIMITER +6 dB sine: max %.4f, loud part peaks %.4f, error before %.5f / 0.5 s after %.5f", maxOut,
          loudPeak, earlyError, lateError);

    // A single full-scale-plus click is caught too, with the gain ramping in ahead of it.
    PeakLimiter click;
    click.Configure(rate);
    std::vector<float> spike(4800 * 2, 0.5f);
    spike[2400 * 2] = spike[2400 * 2 + 1] = 3.0f;
    click.Process(spike.data(), 4800);
    float spikeMax = 0, step = 0;
    for (size_t i = 0; i < 4800; ++i) spikeMax = std::max(spikeMax, std::fabs(spike[i * 2]));
    for (size_t i = lag + 1; i < 4800; ++i)
        if (i - lag != 2400 && i - lag != 2401) step = std::max(step, std::fabs(spike[i * 2] - spike[(i - 1) * 2]));
    Check(spikeMax <= 1.0f && step < 0.02f, L"LIMITER click: max %.4f, largest step elsewhere %.4f", spikeMax, step);

    // A 12 kHz tone whose peaks fall between samples: its samples sit at 0.85, under the ceiling, but
    // the waveform reaches 1.2, so it is turned down rather than left to clip after conversion.
    PeakLimiter between;
    between.Configure(rate);
    std::vector<float> hidden(rate / 10 * 2);
    for (size_t i = 0; i < hidden.size() / 2; ++i)
        hidden[i * 2] = hidden[i * 2 + 1] = (float)(1.2 * std::sin(pi / 2 * i + pi / 4));
    between.Process(hidden.data(), hidden.size() / 2);
    float hiddenPeak = 0;
    for (size_t i = hidden.size() / 2; i < hidden.size(); ++i) hiddenPeak = std::max(hiddenPeak, std::fabs(hidden[i]));
    Check(hiddenPeak < 0.8f, L"LIMITER catches peaks between samples: a tone sampled at 0.85 peaking at 1.2 plays at %.3f",
          hiddenPeak);

    bool reservedPeaks = true;
    for (float reserve : {3.0f, 6.0f, 12.0f}) {
        auto protectedPcm = RenderPlayerPcm(sig.data(), sig.size() / 2, rate, {}, true, 1, reserve);
        float peak = 0;
        for (float sample : protectedPcm) peak = std::max(peak, std::fabs(sample));
        const float ceiling = 0.96605f * std::pow(10.0f, -reserve / 20);
        reservedPeaks &= peak <= ceiling + 1e-5f && peak > ceiling * 0.98f;
    }
    EqSettings flat;
    flat.enabled = true;
    Check(reservedPeaks &&
          RenderPlayerPcm(sig.data(), sig.size() / 2, rate, flat, false, 1, 6) ==
          RenderPlayerPcm(sig.data(), sig.size() / 2, rate, flat, false),
          L"production limiter preserves 3/6/12 dB headphone reserves on peaks; ReplayGain off removes the reserve");
    PeakLimiter reserved;
    reserved.Configure(rate, 6);
    auto belowReserve = in;
    for (float& sample : belowReserve) sample *= 0.25f;
    auto reservedOutput = belowReserve;
    reserved.Process(reservedOutput.data(), reservedOutput.size() / 2);
    bool reservedExact = true;
    for (size_t i = reserved.Latency() * 2; i < reservedOutput.size(); ++i)
        reservedExact &= reservedOutput[i] == belowReserve[i - reserved.Latency() * 2];
    Check(reservedExact, L"headphone reserve leaves audio below its ceiling bit-exact except for limiter delay");
}

// The ReplayGain model (RgGainFor): album gain first, loud albums only partly turned down, quiet
// ones lifted fully, peaks capped with a limiting budget that grows with the dynamic range.
void TestReplayGainModel() {
    auto& g = app::g;
    const bool on = g.rgOn;
    const float keep = g.rgKeep, preamp = g.rgPreamp, noInfo = g.rgPreampNoInfo;
    const EqSettings originalEq = g.eq;
    const float originalHeadroom = g.rgHeadphoneHeadroom;
    g.eq = {};
    g.rgHeadphoneHeadroom = 0;
    g.rgOn = true;
    g.rgKeep = 0.4f;
    g.rgPreamp = 0;
    g.rgPreampNoInfo = 0;
    auto make = [](const wchar_t* name, int16_t track, int16_t album, float peak) {
        TrackId id = g_lib.Add(std::wstring(L"C:\\RgModel\\") + name + L".flac");
        Track& t = g_lib[id];
        t.rgTrackGain = track;
        t.rgAlbumGain = album;
        t.rgTrackPeak = EncodeReplayPeak(peak);
        t.rgAlbumPeak = EncodeReplayPeak(peak);
        return id;
    };
    auto db = [](TrackId id) { return 20 * std::log10(app::RgGainFor(id)); };
    {
        Track t{};
        t.rgTrackGain = -1200; t.rgAlbumGain = -500;
        ReplayGainOptions o; o.keep = 0;
        auto gainDb = [&](ReplayGainMode mode, bool shuffled) {
            o.mode = mode; o.shuffleTracks = shuffled;
            return 20 * std::log10(replaygain::Gain(t, o, 0));
        };
        bool modes = std::fabs(gainDb(ReplayGainMode::Album, true) + 5) < 0.01 &&
                     std::fabs(gainDb(ReplayGainMode::Track, false) + 12) < 0.01 &&
                     std::fabs(gainDb(ReplayGainMode::PlaybackOrder, false) + 5) < 0.01 &&
                     std::fabs(gainDb(ReplayGainMode::PlaybackOrder, true) + 12) < 0.01;
        t.rgTrackGain = kNoGain;
        modes &= std::fabs(gainDb(ReplayGainMode::Track, false) + 5) < 0.01;
        t.rgTrackGain = -1200; t.rgAlbumGain = kNoGain;
        modes &= std::fabs(gainDb(ReplayGainMode::Album, false) + 12) < 0.01;
        Check(modes, L"ReplayGain chooses album, track or playback-order gain and falls back when missing");
    }
    auto high = [](double lu) { return (uint8_t)std::clamp(1 + (int)std::lround(lu * 4), 1, 255); };
    // A finale louder than its album keeps the album's gain, a loud album gets 60% of its cut.
    TrackId finale = make(L"finale", -1400, -1000, 1.0f);
    // A quiet, dynamic album is lifted all the way (its peak leaves room).
    TrackId quiet = make(L"quiet", 400, 500, 0.3f);
    // Quiet with a high peak and loud parts 1 LU above its average: lifted past the peak by the
    // limiting budget (only peaks 9+ dB clear of the loud parts may be limited, a little).
    TrackId peaky = make(L"peaky", 1000, 1000, 0.891f);
    g_lib[peaky].rgHigh = high(1.0);
    TrackId unknown = make(L"unknown", kNoGain, kNoGain, 0);
    TrackId single = make(L"single", -800, kNoGain, 1.0f);
    const double peakDb = 20 * std::log10(0.891f), loudParts = kReplayGainReference - 10.0 + 1.0,
                 budget = std::clamp((peakDb - loudParts - 9.0) / 3.0, 0.0, 3.0),
                 peakyGain = std::min(10.0, budget - peakDb);
    Check(std::fabs(db(finale) + 6.0) < 0.01 && std::fabs(db(quiet) - 5.0) < 0.01 &&
          std::fabs(db(peaky) - peakyGain) < 0.05 && std::fabs(db(unknown)) < 0.01 && std::fabs(db(single) + 4.8) < 0.01,
          L"RG MODEL finale %+.2f (expect -6), quiet %+.2f (+5), peaky %+.2f (%+.2f), unknown %+.2f, single %+.2f (-4.8)",
          db(finale), db(quiet), db(peaky), peakyGain, db(unknown), db(single));
    // A hot master (true peak past full scale) keeps more of its cut than a soft one, so a
    // wall-to-wall loud master - and the voice in it - stays as present as it was mastered.
    TrackId hot = make(L"hot", -600, -600, 1.4f);
    TrackId soft = make(L"soft", -600, -600, 0.9f);
    g_lib[hot].rgHigh = high(1.5);
    g_lib[soft].rgHigh = high(1.5);
    // An already-loud track with a wide range is not turned down twice for being dynamic.
    TrackId loudDynamic = make(L"louddyn", -600, -600, 0.5f);
    g_lib[loudDynamic].rgHigh = high(12.5);
    // A wide-range quiet track still has its lift held back, so its chorus doesn't hit the limiter.
    TrackId quietDynamic = make(L"quietdyn", 600, 600, 0.4f);
    g_lib[quietDynamic].rgHigh = high(12.5);
    Check(db(hot) > db(soft) + 1.0 && db(hot) > -2.0 && std::fabs(db(loudDynamic) + 3.6) < 0.05 &&
          std::fabs(db(quietDynamic) - 0.3) < 0.05,
          L"RG MODEL hot %+.2f keeps more than soft %+.2f; loud dynamic %+.2f (-3.6), quiet dynamic %+.2f (+0.3)",
          db(hot), db(soft), db(loudDynamic), db(quietDynamic));
    g.eq.enabled = true;
    g.eq.gains[5] = 6;
    const double boosted = db(peaky);
    Check(std::fabs(boosted - (peakyGain - 6)) < 0.06 && std::fabs(db(single) + 6) < 0.06,
          L"RG budgets EQ boosts before the limiter while retaining the track's existing limiting allowance");
    g.eq.preamp = -6;
    Check(std::fabs(db(peaky) - peakyGain) < 0.06 && g.eq.preamp == -6,
          L"AutoEq preamp that offsets its boost needs no second headroom cut and remains unchanged");
    g.eq.preamp = -12;
    Check(std::fabs(db(peaky) - std::min(10.0, peakyGain + 6)) < 0.06,
          L"negative net EQ gain makes its available headroom usable for the requested ReplayGain lift");
    g.eq = {};
    g.rgHeadphoneHeadroom = 6;
    Check(std::fabs(db(peaky) - (peakyGain - 6)) < 0.06,
          L"explicit headphone headroom shares the ReplayGain peak budget for onboard EQ");
    g.rgOn = false;
    Check(app::RgGainFor(peaky) == 1, L"turning ReplayGain off bypasses its EQ/headphone headroom adjustments");
    g.rgOn = true;
    g.rgHeadphoneHeadroom = 0;
    g.eq.enabled = true;
    g.eq.gains[5] = 6;
    g.rgPreampNoInfo = -3;
    Check(std::fabs(db(unknown) + 9) < 0.06, L"unmeasured tracks keep their fallback preamp and reserve positive EQ boost");
    g.rgPreampNoInfo = 0;

    // Exercise real renderer markers: track gain, EQ/preamp and crossfeed precede a single limiter.
    // Conservative measured peaks leave enough room that the limiter should only add its delay.
    TrackId combined = make(L"combined", 600, kNoGain, 0.6f);
    bool transparent = true;
    std::vector<float> source(48000 * 2);
    for (size_t i = 0; i < source.size() / 2; ++i)
        source[i * 2] = source[i * 2 + 1] = (float)(0.5 * std::sin(2 * 3.14159265358979 * 1000 * i / 48000));
    PeakLimiter delay;
    delay.Configure(48000);
    for (bool parametric : {false, true}) for (float eqPreamp : {0.0f, -6.0f})
        for (bool crossfeed : {false, true}) for (float headroom : {0.0f, 6.0f}) {
        g.eq = {};
        g.eq.enabled = true;
        g.eq.parametric = parametric;
        g.eq.crossfeed = crossfeed;
        g.eq.preamp = eqPreamp;
        g.eq.gains[5] = 6;
        g.eq.filterCount = 1;
        g.eq.filters[0] = {EqFilterType::Peak, 1000, 6, 1.41f, true};
        g.rgHeadphoneHeadroom = headroom;
        const float gain = app::RgGainFor(combined);
        auto rendered = RenderPlayerPcm(source.data(), source.size() / 2, 48000, g.eq, true, gain, headroom);
        auto expected = source;
        for (float& sample : expected) sample *= gain;
        Equalizer reference;
        reference.Configure(g.eq, 48000);
        reference.Process(expected.data(), expected.size() / 2);
        transparent &= rendered.size() == expected.size();
        for (size_t i = 4800; i < rendered.size() / 2; ++i) for (int ch = 0; ch < 2; ++ch)
            transparent &= std::isfinite(rendered[i * 2 + ch]) &&
                std::fabs(rendered[i * 2 + ch] - expected[(i - delay.Latency()) * 2 + ch]) < 1e-5f;
    }
    Check(transparent, L"production ReplayGain + graphic/AutoEq preamps + crossfeed + headset reserve avoid extra limiting and double gain");
    g.eq = {};
    g.rgHeadphoneHeadroom = 0;
    g.rgKeep = 0;
    const double full = db(finale);
    g.rgOn = false;
    Check(std::fabs(full + 10.0) < 0.01 && app::RgGainFor(finale) == 1.0f, L"RG MODEL keep nothing: %+.2f (-10); off: unity", full);
    // Album gain comes from the tracks' measurements (energy mean), once every playable track is
    // measured; an undecodable track doesn't hold the album back.
    auto member = [](const wchar_t* name, int16_t gain) {
        TrackId id = g_lib.Add(std::wstring(L"C:\\RgAlbum\\") + name + L".flac");
        g_lib[id].durationMs = 100000;
        g_lib[id].rgTrackGain = gain;
        g_lib[id].rgTrackPeak = EncodeReplayPeak(0.5f);
        return id;
    };
    TrackId a = member(L"a", -1000), b = member(L"b", -400), c = member(L"c", kNoGain);
    app::UpdateAlbumGains({a});
    const bool waits = g_lib[a].rgAlbumGain == kNoGain && g_lib[b].rgAlbumGain == kNoGain;
    g_lib[c].flags |= Track::kRgScanFailed;
    app::UpdateAlbumGains({a});
    g_lib[c].flags &= (uint16_t)~Track::kRgScanFailed;
    const double expect = kReplayGainReference - 10 * std::log10((std::pow(10.0, -0.8) + std::pow(10.0, -1.4)) / 2);
    Check(waits && std::abs(g_lib[a].rgAlbumGain - expect * 100) <= 1 && g_lib[b].rgAlbumGain == g_lib[a].rgAlbumGain &&
          g_lib[a].rgAlbumPeak == EncodeReplayPeak(0.5f),
          L"RG ALBUM waits for unmeasured tracks: %d; then %+.2f dB (expected %+.2f)", (int)waits,
          g_lib[a].rgAlbumGain / 100.0, expect);
    g_lib[a].flags |= Track::kRgScanned;
    g_lib[b].flags |= Track::kRgScanned;
    g_lib[c].flags |= Track::kRgScanFailed;
    g_lib[a].rgTrackGain = kNoGain;
    app::UpdateAlbumGains({a});
    Check(g_lib[a].rgTrackGain == kNoGain && g_lib[b].rgAlbumGain == kNoGain && !g_lib[b].rgAlbumPeak,
          L"RG clearing one measurement invalidates the album gain of its measured siblings");
    g.rgOn = on;
    g.rgKeep = keep;
    g.rgPreamp = preamp;
    g.rgPreampNoInfo = noInfo;
    g.eq = originalEq;
    g.rgHeadphoneHeadroom = originalHeadroom;
}

// 16-bit stereo WAV: a 1 kHz sine at half scale until `silentFrom` seconds, silence after.
std::string MakeSineWav(double seconds, double silentFrom) {
    const uint32_t rate = 48000, frames = (uint32_t)(seconds * rate);
    std::string data(frames * 4, '\0');
    for (uint32_t i = 0; i < frames; ++i) {
        int16_t s = i < silentFrom * rate
                        ? (int16_t)std::lround(16384 * std::sin(2 * 3.14159265358979 * 1000 * i / rate))
                        : 0;
        for (int c = 0; c < 2; ++c) {
            data[i * 4 + c * 2] = (char)(s & 0xFF);
            data[i * 4 + c * 2 + 1] = (char)((uint16_t)s >> 8);
        }
    }
    std::string wav = "RIFF";
    auto put32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) wav += (char)(v >> (8 * i)); };
    auto put16 = [&](uint16_t v) { wav += (char)(v & 0xFF); wav += (char)(v >> 8); };
    put32(36 + (uint32_t)data.size());
    wav += "WAVEfmt ";
    put32(16);
    put16(1);
    put16(2);
    put32(rate);
    put32(rate * 4);
    put16(4);
    put16(16);
    wav += "data";
    put32((uint32_t)data.size());
    return wav + data;
}

// Waveform peaks: bucket values for a half-scale sine, silence and partial ranges, the CUE-track
// mapping (a track's waveform is a range of the file, asked for in file seconds), the streaming
// accumulator, and the disk cache's store/read-by-key behaviour.
void TestPeaks(const std::wstring& scratch) {
    const std::wstring sine = scratch + L"\\peaks_sine.wav", half = scratch + L"\\peaks_half.wav",
                       silent = scratch + L"\\peaks_silent.wav";
    WriteTestBytes(sine, MakeSineWav(2, 2));
    WriteTestBytes(half, MakeSineWav(2, 1));
    WriteTestBytes(silent, MakeSineWav(2, 0));
    // The sine peaks at exactly half scale: sqrt(0.5) * 255 = 180.
    auto span = [](const Peaks& p, size_t from, size_t to, int& lo, int& hi) {
        lo = 255, hi = 0;
        for (size_t i = from; i < to; ++i) lo = std::min(lo, (int)p.v[i]), hi = std::max(hi, (int)p.v[i]);
    };
    int lo = 0, hi = 0;
    Peaks sinePeaks{}, halfPeaks{}, p{};
    bool ok = ComputePeaks(sine, 0, 0, sinePeaks, nullptr);
    span(sinePeaks, 0, kPeakBuckets, lo, hi);
    Check(ok && lo >= 179 && hi <= 181, L"PEAKS sine at half scale: %d..%d (expected 180 in every bucket)", lo, hi);

    ok = ComputePeaks(half, 0, 0, halfPeaks, nullptr);
    span(halfPeaks, 0, kPeakBuckets / 2, lo, hi);
    int lo2 = 0, hi2 = 0;
    span(halfPeaks, kPeakBuckets / 2, kPeakBuckets, lo2, hi2);
    Check(ok && lo >= 179 && hi <= 181 && hi2 == 0, L"PEAKS half-silent: first half %d..%d, second half %d..%d", lo, hi, lo2, hi2);

    ok = ComputePeaks(silent, 0, 0, p, nullptr);
    span(p, 0, kPeakBuckets, lo, hi);
    Check(ok && hi == 0, L"PEAKS silence is all zero (max %d)", hi);

    testlog::Cases ranges;
    ok = ComputePeaks(half, 1.0, 2.0, p, nullptr);
    span(p, 0, kPeakBuckets, lo, hi);
    ranges.Add(ok && hi == 0, L"silent second");
    ok = ComputePeaks(sine, 0.5, 1.0, p, nullptr);
    span(p, 0, kPeakBuckets, lo, hi);
    ranges.Add(ok && lo >= 179 && hi <= 181, L"partial sine");
    ranges.Add(!ComputePeaks(sine, 1.0, 0.5, p, nullptr), L"reversed range");
    Check(ranges.Passed(), L"PEAKS ranges preserve their samples and reject reversed bounds%ls", ranges.failed.c_str());
    std::atomic<bool> cancelled{true};
    Check(!ComputePeaks(sine, 0, 0, p, &cancelled), L"PEAKS a cancelled computation reports failure");

    // A CUE track's waveform is its own range of the file, asked for in file seconds: exactly the
    // window the file itself yields for those seconds. Mapping the track's offset onto the range
    // a second time would seek past the track - the middle track below would decode nothing.
    const std::wstring cueDir = scratch + L"\\peaks_cue";
    std::error_code cueEc;
    std::filesystem::remove_all(cueDir, cueEc);
    std::filesystem::create_directories(cueDir, cueEc);
    const std::wstring cueAudio = cueDir + L"\\Peaks.wav";
    WriteTestBytes(cueAudio, MakeSineWav(4, 2));  // a sine for two seconds, then silence
    WriteTestBytes(cueDir + L"\\Peaks.cue", "FILE \"Peaks.wav\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n"
                                            "TRACK 02 AUDIO\nINDEX 01 00:02:00\nTRACK 03 AUDIO\nINDEX 01 00:03:00\n");
    const std::wstring track2 = SubtrackPath(cueAudio, 2);
    double from = -1, to = -1;
    Peaks trackPeaks{}, filePeaks{};
    bool trackOk = SubtrackRange(track2, from, to) && from > 0 && to > from &&
                   ComputePeaks(track2, from, to, trackPeaks, nullptr);
    bool fileOk = ComputePeaks(cueAudio, from, to, filePeaks, nullptr);
    span(trackPeaks, 0, kPeakBuckets, lo, hi);
    Check(trackOk && fileOk && hi == 0 && std::memcmp(trackPeaks.v, filePeaks.v, kPeakBuckets) == 0,
          L"PEAKS CUE track [%.3f, %.3f): track=%d file=%d max %d (expected both decoded, max 0)", from, to,
          (int)trackOk, (int)fileOk, hi);

    // Unknown length: 20 s of float audio is enough to merge the fine buckets several times.
    std::vector<float> stereo(48000 * 2 * 20);
    for (size_t i = 0; i < stereo.size() / 2; ++i)
        stereo[i * 2] = stereo[i * 2 + 1] = (float)(0.5 * std::sin(2 * 3.14159265358979 * 1000 * i / 48000.0));
    PeakAccumulator acc;
    acc.Begin(0);
    for (size_t i = 0; i < stereo.size(); i += 4096 * 2) acc.Add(stereo.data() + i, std::min<size_t>(4096, stereo.size() / 2 - i / 2));
    Peaks accPeaks{};
    ok = acc.Finish(accPeaks);
    span(accPeaks, 0, kPeakBuckets, lo, hi);
    Check(ok && lo >= 179 && hi <= 181, L"PEAKS accumulator, unknown length, 20 s: %d..%d", lo, hi);

    // The disk cache: a fresh service must answer from disk what the first one computed.
    const std::wstring cache = scratch + L"\\peaks_cache";
    std::error_code ec;
    std::filesystem::remove_all(cache, ec);  // a cache left by an earlier run would answer at once
    const uint64_t size = 1000, time = 42;
    StartPeaks(nullptr, 0, cache);
    uint32_t req = 0;
    Peaks got{};
    bool hit = GetPeaks(sine, size, time, 0, 0, got, req);
    Check(!hit && req != 0, L"PEAKS first request queues work (request %u)", req);
    for (int i = 0; i < 500 && !hit; ++i) {
        Sleep(20);
        hit = GetPeaks(sine, size, time, 0, 0, got, req);
    }
    Check(hit && std::memcmp(got.v, sinePeaks.v, kPeakBuckets) == 0, L"PEAKS queued result matches the direct computation");
    StopPeaks();
    StartPeaks(nullptr, 0, cache);
    hit = GetPeaks(sine, size, time, 0, 0, got, req);
    Check(hit && std::memcmp(got.v, sinePeaks.v, kPeakBuckets) == 0, L"PEAKS cache round trip: a new service reads the disk copy");
    Peaks ranged{};
    std::memset(ranged.v, 77, sizeof(ranged.v));
    StorePeaks(sine, size, time, 0.5, 1.0, ranged);
    hit = GetPeaks(sine, size, time, 0.5, 1.0, got, req);
    Check(hit && std::memcmp(got.v, ranged.v, kPeakBuckets) == 0, L"PEAKS stored range is returned by its own key");
    hit = GetPeaks(sine, size + 1, time, 0, 0, got, req);
    Check(!hit, L"PEAKS a changed file size is a cache miss");
    StopPeaks();

    // A sparse, long PCM file keeps the worker busy without writing a gigabyte to disk. Waiting
    // until the decoder has it open makes the cancellation check exercise in-flight work.
    const std::wstring longFile = scratch + L"\\peaks-long.wav";
    constexpr DWORD audioBytes = 1u << 30;
    HANDLE file = CreateFileW(longFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    bool sparse = file != INVALID_HANDLE_VALUE && DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &written, nullptr);
    std::string header = MakeSineWav(0, 0);
    auto put32 = [&](size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i) header[at + i] = (char)(value >> (8 * i));
    };
    put32(4, audioBytes + 36);
    put32(40, audioBytes);
    LARGE_INTEGER end{};
    end.QuadPart = audioBytes + header.size();
    bool prepared = sparse && WriteFile(file, header.data(), (DWORD)header.size(), &written, nullptr) && written == header.size() &&
                    SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!sparse) {
        testlog::Skip(L"PEAKS in-flight cancellation: filesystem lacks sparse-file support");
    } else if (Ready(prepared, L"PEAKS long sparse fixture is prepared")) {
        auto isOpen = [&] {
            HANDLE probe = CreateFileW(longFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
            if (probe == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
            CloseHandle(probe);
            return false;
        };
        auto waitFor = [&](auto predicate) {
            const ULONGLONG until = GetTickCount64() + 5000;
            while (!predicate() && GetTickCount64() < until) Sleep(1);
            return predicate();
        };
        StartPeaks(nullptr, 0, cache);
        uint32_t first = 0, next = 0;
        bool queued = !GetPeaks(longFile, end.QuadPart, time, 0, 0, got, first) && first;
        Sleep(75);
        bool deferred = !isOpen();
        Sleep(425);  // after the settling delay the worker opens the file
        bool started = isOpen();
        bool nextQueued = !GetPeaks(half, size + 2, time, 0, 0, got, next) && next;
        bool nextReady = waitFor([&] { return GetPeaks(half, size + 2, time, 0, 0, got, next); });
        const std::wstring longCache = cache + L"\\" + PeaksKeyName(longFile, end.QuadPart, time) + L".pk";
        bool abandoned = GetFileAttributesW(longCache.c_str()) == INVALID_FILE_ATTRIBUTES;
        uint32_t retry = 0;
        bool retryQueued = !GetPeaks(longFile, end.QuadPart, time, 0, 0, got, retry) && retry && retry != first;
        Sleep(500);
        bool restarted = isOpen();
        bool cached = GetPeaks(sine, size, time, 0, 0, got, next);
        bool closed = waitFor([&] { return !isOpen(); });
        uint32_t again = 0, afterCancel = 0;
        bool canRetry = !GetPeaks(longFile, end.QuadPart, time, 0, 0, got, again) && again;
        CancelPeaks();
        bool canReturn = !GetPeaks(longFile, end.QuadPart, time, 0, 0, got, afterCancel) && afterCancel && afterCancel != again;
        CancelPeaks();
        StopPeaks();
        Check(queued && deferred && started && nextQueued && nextReady && abandoned && retryQueued && restarted && cached && closed &&
                  canRetry && canReturn,
              L"PEAKS rapid changes wait to decode; switching, a cached song and stop cancel work without caching partial results "
              L"(queue=%d delayed=%d started=%d next=%d ready=%d abandoned=%d retry=%d restarted=%d cached=%d closed=%d return=%d/%d)",
              queued, deferred, started, nextQueued, nextReady, abandoned, retryQueued, restarted, cached, closed, canRetry, canReturn);
    }
    DeleteFileW(longFile.c_str());
}

// Reverb, tempo, loudness compensation and night mode (dsp.h), alone and through the production renderer.
void TestEffects() {
    constexpr unsigned rate = 48000;
    constexpr double pi = 3.14159265358979;
    auto run = [](auto& effect, std::vector<float>& pcm) {
        for (size_t at = 0; at < pcm.size() / 2; at += 137) effect.Process(pcm.data() + at * 2, std::min<size_t>(137, pcm.size() / 2 - at));
    };
    auto rms = [](const std::vector<float>& pcm, size_t from, size_t to) {
        double sum = 0;
        for (size_t i = from * 2; i < to * 2; ++i) sum += (double)pcm[i] * pcm[i];
        return std::sqrt(sum / std::max<size_t>(1, (to - from) * 2));
    };
    auto tone = [&](double hz, double amplitude, double seconds) {
        std::vector<float> pcm((size_t)(seconds * rate) * 2);
        for (size_t i = 0; i < pcm.size() / 2; ++i) pcm[i * 2] = pcm[i * 2 + 1] = (float)(amplitude * std::sin(2 * pi * hz * i / rate));
        return pcm;
    };
    uint32_t seed = 12345;
    auto noise = [&](double seconds) {  // the same in both channels, as most of a mix is
        std::vector<float> pcm((size_t)(seconds * rate) * 2);
        for (size_t i = 0; i < pcm.size(); i += 2) {
            seed = seed * 1664525 + 1013904223;
            pcm[i] = pcm[i + 1] = ((seed >> 8) / 16777216.0f - 0.5f) * 0.5f;
        }
        return pcm;
    };

    // Reverb: every preset rings after an impulse and decays, and at the default amount the wet signal
    // is loud enough to hear under steady music (the first amounts sat 19 dB down and went unnoticed).
    bool decays = true, audible = true;
    std::wstring levelText;
    for (ReverbPreset preset : {ReverbPreset::Room, ReverbPreset::Hall, ReverbPreset::Plate}) {
        Reverb reverb;
        reverb.Configure({preset, 1.0f}, rate);
        std::vector<float> impulse(rate * 3 * 2);
        impulse[rate / 20 * 2] = impulse[rate / 20 * 2 + 1] = 0.5f;  // after the send's 20 ms fade-in
        run(reverb, impulse);
        const double early = rms(impulse, rate / 10, rate / 2), late = rms(impulse, rate * 2, rate * 5 / 2);
        decays &= early > 1e-4 && late < early * 0.1 &&
                  std::all_of(impulse.begin(), impulse.end(), [](float s) { return std::isfinite(s); });
        Reverb steady;
        steady.Configure({preset, ReverbSettings{}.mix}, rate);
        auto dry = noise(4), wet = dry;
        run(steady, wet);
        for (size_t i = 0; i < wet.size(); ++i) wet[i] -= dry[i];
        const double db = 20 * std::log10(std::max(rms(wet, rate * 2, rate * 4) / rms(dry, rate * 2, rate * 4), 1e-12));
        audible &= db > -14 && db < -3;
        levelText += L" " + std::to_wstring((int)std::lround(db));
    }
    Check(decays && audible, L"REVERB every preset rings and decays; the default amount's wet level is -14..-3 dB (dB:%ls)",
          levelText.c_str());

    // Turned off, the tail rings out and the reverb stops; a seek ducks it within 50 ms; in silence
    // it sleeps (so the output can idle) and wakes on sound.
    Reverb ringing;
    ringing.Configure({ReverbPreset::Plate, 0.5f}, rate);
    auto music = noise(1);
    run(ringing, music);
    ringing.Configure({ReverbPreset::Off, 0.5f}, rate);
    std::vector<float> after(rate * 4 * 2);
    run(ringing, after);
    const double tail = rms(after, rate / 20, rate / 5);
    Reverb cleared;
    cleared.Configure({ReverbPreset::Hall, 0.5f}, rate);
    music = noise(1);
    run(cleared, music);
    cleared.Clear();
    std::vector<float> silence(rate * 3 / 10 * 2);
    run(cleared, silence);
    float residue = 0;
    for (size_t i = rate / 20 * 2; i < silence.size(); ++i) residue = std::max(residue, std::fabs(silence[i]));
    const bool slept = !cleared.Active();
    music = noise(0.5);
    run(cleared, music);
    Check(tail > 1e-3 && !ringing.Active() && residue == 0 && slept && cleared.Active(),
          L"REVERB off rings out (%.4f) then stops, a seek ducks the tail, and silence puts it to sleep", tail);

    // Tempo: the length follows the tempo, the pitch and level do not, and chunking doesn't matter.
    bool lengths = true, pitch = true, steadyLevel = true;
    for (double tempo : {0.75, 1.5, 2.0}) {
        auto in = tone(440, 0.5, 3);
        TimeStretch stretch;
        stretch.Configure(rate, tempo);
        std::vector<float> out, part;
        for (size_t at = 0; at < in.size() / 2; at += 2048) {
            stretch.Process(in.data() + at * 2, std::min<size_t>(2048, in.size() / 2 - at), part);
            out.insert(out.end(), part.begin(), part.end());
        }
        stretch.Flush(out);
        const double expected = in.size() / 2 / tempo;
        lengths &= std::fabs(out.size() / 2 - expected) < expected * 0.01;
        size_t from = out.size() / 8, to = out.size() / 2 * 7 / 8, crossings = 0;
        for (size_t i = from + 1; i < to; ++i) crossings += (out[i * 2 - 2] < 0) != (out[i * 2] < 0);
        const double hz = crossings / 2.0 / ((double)(to - from) / rate);
        pitch &= std::fabs(hz - 440) < 440 * 0.02;
        const double r = rms(out, from, to);
        steadyLevel &= r > 0.5 / std::sqrt(2.0) * 0.85 && r < 0.5 / std::sqrt(2.0) * 1.1;
    }
    auto song = noise(2);
    std::vector<float> whole, pieces, part;
    double owed = 0;
    for (size_t chunk : {size_t(137), size_t(4096)}) {
        TimeStretch stretch;
        stretch.Configure(rate, 1.25);
        auto& out = chunk == 137 ? pieces : whole;
        for (size_t at = 0; at < song.size() / 2; at += chunk) {
            stretch.Process(song.data() + at * 2, std::min<size_t>(chunk, song.size() / 2 - at), part);
            out.insert(out.end(), part.begin(), part.end());
            owed = std::max(owed, std::fabs(out.size() / 2 + stretch.PendingOutput() - (at + std::min<size_t>(chunk, song.size() / 2 - at)) / 1.25));
        }
        stretch.Flush(out);
    }
    Check(lengths && pitch && steadyLevel && whole == pieces && owed < 2,
          L"TEMPO keeps a tone's pitch and level, divides the length by the tempo, and ignores chunking");

    // Loudness compensation: flat at full volume; at -20 dB the bass and treble rise and the midrange
    // stays, and no boost ever exceeds the attenuation it follows.
    LoudnessContour contour;
    contour.Configure(rate);
    auto untouched = tone(1000, 0.5, 0.5);
    for (size_t at = 0; at < untouched.size() / 2; at += 137)
        contour.Process(untouched.data() + at * 2, std::min<size_t>(137, untouched.size() / 2 - at), 1.0f, true);
    auto gainAt = [&](double hz, float volume) {
        LoudnessContour c;
        c.Configure(rate);
        auto pcm = tone(hz, 0.1, 1.5), reference = pcm;
        for (size_t at = 0; at < pcm.size() / 2; at += 137) c.Process(pcm.data() + at * 2, std::min<size_t>(137, pcm.size() / 2 - at), volume, true);
        return 20 * std::log10(rms(pcm, rate, rate * 3 / 2) / rms(reference, rate, rate * 3 / 2));
    };
    const double low = gainAt(40, 0.1f), middle = gainAt(1000, 0.1f), high = gainAt(14000, 0.1f);
    bool belowAttenuation = true;
    for (float volume = 0.001f; volume < 1; volume *= 1.3f)
        belowAttenuation &= LoudnessContour::BassDb(volume) < -20 * std::log10(volume) &&
                            LoudnessContour::TrebleDb(volume) < -20 * std::log10(volume);
    Check(untouched == tone(1000, 0.5, 0.5) && !contour.Active() && low > 5.5 && low < 7.5 && std::fabs(middle) < 1 &&
              high > 1.2 && high < 2.2 && belowAttenuation,
          L"LOUDNESS is flat at full volume; at -20 dB it lifts 40 Hz by %.1f dB and 14 kHz by %.1f dB, 1 kHz by %.2f dB",
          low, high, middle);

    // A rapid volume rise can outpace the shelf glide. Exercise both EQ paths and ReplayGain
    // through the production renderer; the first case isolates the contour with all others off.
    auto rising = tone(40, 0.99, 3);
    RenderEffects rise;
    rise.volume = 0.1f; rise.loudness = true;
    rise.volumeChangeFrame = rate * 2; rise.volumeAfter = 1;
    bool safeRise = true;
    for (int eqMode = 0; eqMode < 3; ++eqMode) for (bool rg : {false, true}) for (bool nightOn : {false, true}) {
        EqSettings settings;
        if (eqMode) {
            settings.enabled = true; settings.parametric = eqMode == 2;
            settings.gains[0] = 3;
            settings.filterCount = 1;
            settings.filters[0] = {EqFilterType::Peak, 40, 3, 1.0f, true};
        }
        rise.night = nightOn;
        auto rendered = RenderPlayerPcm(rising.data(), rising.size() / 2, rate, settings, rg, 1, 0, &rise);
        safeRise &= rendered.size() == rising.size();
        for (float sample : rendered) safeRise &= std::isfinite(sample) && std::fabs(sample) < 0.98f;
    }
    Check(safeRise, L"volume rise with loudness compensation avoids hard clipping with ReplayGain, both EQ types and night mode");

    // Night mode: lifts a quiet passage, softens a loud one quickly, leaves near-silence alone. The
    // levels are the detector's (as played before the 0.5 volume); left and right are in quadrature, so
    // the mean square has no ripple.
    auto passage = [&](double db, double seconds) {
        const double a = std::pow(10.0, (db + 3) / 20.0) * 0.5;
        std::vector<float> pcm((size_t)(seconds * rate) * 2);
        for (size_t i = 0; i < pcm.size() / 2; ++i) {
            pcm[i * 2] = (float)(a * std::sin(2 * pi * 1000 * i / rate));
            pcm[i * 2 + 1] = (float)(a * std::cos(2 * pi * 1000 * i / rate));
        }
        return pcm;
    };
    auto nightRun = [&](NightCompressor& night, std::vector<float>& pcm) {
        for (size_t at = 0; at < pcm.size() / 2; at += 137) night.Process(pcm.data() + at * 2, std::min<size_t>(137, pcm.size() / 2 - at), 0.5f, true);
    };
    NightCompressor night, hiss;
    night.Configure(rate);
    hiss.Configure(rate);
    auto quiet = passage(-39, 6);
    nightRun(night, quiet);
    const double lifted = night.LiftDb();
    auto loud = passage(-6, 0.3);
    nightRun(night, loud);
    const double softened = night.LiftDb();
    auto floor = passage(-80, 4);
    nightRun(hiss, floor);
    Check(std::fabs(lifted - 9) < 0.5 && std::fabs(softened + 5) < 0.5 && hiss.LiftDb() == 0,
          L"NIGHT lifts a -39 dB passage by %.1f dB (expect 9), softens a -6 dB one by %.1f dB (expect -5), ignores -80 dB",
          lifted, softened);

    // The production renderer: everything off stays exact; reverb is covered by the limiter.
    auto sine = tone(1000, 0.98, 1);
    RenderEffects off, room;
    room.reverb = {ReverbPreset::Room, 0.7f};
    auto withReverb = RenderPlayerPcm(sine.data(), sine.size() / 2, rate, {}, false, 1, 0, &room);
    float peak = 0;
    for (float s : withReverb) peak = std::isfinite(s) ? std::max(peak, std::fabs(s)) : 2;
    Check(RenderPlayerPcm(sine.data(), sine.size() / 2, rate, {}, false, 1, 0, &off) == sine &&
              withReverb.size() == sine.size() && peak < 0.97f && peak > 0.9f,
          L"EFFECTS off leave the renderer exact; reverb on a near-full-scale tone stays under the ceiling (peak %.3f)", peak);
}
