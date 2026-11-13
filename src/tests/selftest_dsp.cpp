// DSP checks of the self test (see selftest.cpp): loudness (EBU Tech 3341), true peak, the peak
// limiter, the ReplayGain model, the effects after the EQ (reverb, tempo, smart volume) and waveform peaks.
#include "selftest_common.h"
#include "tempo.h"
#include "wind_checks.h"

#include <winioctl.h>
#include <xmmintrin.h>

namespace {
void TestWaveformMotion() {
    testlog::Batch batch;
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
    batch.Next(L"live waveforms swell within 20 ms on bass and melodic note attacks and release smoothly "
               L"between accents");
    Check(attacks.Passed(), L"live waveforms swell within 20 ms on bass and melodic note attacks, including repeated notes%ls",
          attacks.failed.c_str());
    Check(falls.Passed(), L"live note attacks release smoothly and settle between accents%ls", falls.failed.c_str());
    batch.Next(L"live waveforms keep soft notes visible against strong ones and respond identically across "
               L"chunk sizes and phase");
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
    batch.Next(L"the live shape lifts with a note, holds the same frame twice, resets to silence and "
               L"ignores non-finite PCM");
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
    // Live shapes are normalized to their own loudest moment, so a dense master needs more
    // contrast between its quieter and louder columns than the whole-song peaks show.
    WaveformMotion lit;
    auto pulseNote = tone(48000, 0.02, 0.5f);
    lit.Add(pulseNote.data(), pulseNote.size() / 2, 48000);
    std::array<uint8_t, 5> textured{0, 32, 96, 180, 255};
    lit.Shape(textured.data(), textured.size());
    Check(textured[2] < textured.back() * 0.5f && std::is_sorted(textured.begin(), textured.end()),
          L"the live shape darkens its mid columns for contrast while keeping the loudest full height");
}
}

void TestVisuals() {
    TestWaveformMotion();
    int wind = CheckWind();
    Check(wind == 0, L"the track list's wind stays still in silence, gusts on attacks and drifts off on pause (check %d)", wind);
    testlog::Batch batch;
    AudioVisuals frame;
    batch.Next(L"VIS stopped audio stays silent, a 1 kHz tone lands in its band at each rate, and stop "
               L"drains the display");
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
    batch.Next(L"VIS non-finite display samples cannot escape into drawing coordinates or spectrum levels");
    frame.wave[0] = std::numeric_limits<float>::quiet_NaN();
    frame.wave[1] = std::numeric_limits<float>::infinity();
    frame.Update(true, 48000);
    Check(std::all_of(frame.wave.begin(), frame.wave.end(), [](float v) { return std::isfinite(v); }) &&
              std::all_of(frame.levels.begin(), frame.levels.end(), [](float v) { return std::isfinite(v); }),
          L"VIS non-finite display samples cannot escape into drawing coordinates");

    batch.Next(L"VIS the scope keeps 300 ms at a fixed linear scale and averages signed stereo into "
               L"weighted spans at every rate");
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
    batch.Next(L"VIS spectrum peaks hold and fall in bass-band dB, follow elapsed time and scroll with the "
               L"playback clock");
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
    testlog::Batch batch;
    EqSettings settings;
    settings.crossfeed = true;
    std::wstring error;
    batch.Next(L"AutoEq imports preamp, shelves, Q and disabled filters from BOM'd text, round trips it and "
               L"rejects bad profiles");
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

    batch.Next(L"parametric and graphic bands and shelves reach their gains at every rate, and neighbouring "
               L"sliders stay independent");
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
    batch.Next(L"profile preamp and crossfeed work with filters off, mono level holds and disabling fades "
               L"to bit-exact bypass");
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
    batch.Next(L"EQ changes crossfade without clicks, denormals stay contained, a zero rate bypasses and "
               L"re-applying keeps tails");
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

    batch.Next(L"the production renderer stays exact with EQ off, applies the preamp, and limits boosted "
               L"output instead of clipping");
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
    batch.Next(L"EQ headroom follows the combined overlapping-band response, finds narrow peaks at every "
               L"rate and skips crossfeed");
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
    testlog::Batch batch;
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
    batch.Next(L"LOUDNESS EBU 3341 sine reads -23 LUFS and the ReplayGain reference follows it");
    double lufs = meter.Integrated();
    Check(std::fabs(lufs + 23.0) < 0.1, L"LOUDNESS EBU 3341 sine: %.2f LUFS (expected -23.00), ReplayGain %+.2f dB", lufs,
          kReplayGainReference - lufs);

    // Independent scalar oracle of the original per-channel K-weighting: the SIMD meter must
    // produce the same blocks, short-term values and peak bit for bit, across uneven chunks,
    // step boundaries mid-chunk, a rate that is not a multiple of ten, silence and a NaN.
    batch.Next(L"LOUDNESS SIMD meter matches the original scalar K-weighting exactly");
    struct Reference {
        struct Filter {
            double b0, b1, b2, a1, a2, z1[2]{}, z2[2]{};
            double Run(double x, int ch) {
                double y = b0 * x + z1[ch];
                z1[ch] = b1 * x - a1 * y + z2[ch];
                z2[ch] = b2 * x - a2 * y;
                return y;
            }
        } shelf{}, highpass{};
        size_t stepFrames, fill = 0, count = 0;
        double sum = 0, steps[4]{}, recent[30]{};
        std::vector<double> blocks;
        std::vector<float> shortTerm;
        float peak = 0;
        explicit Reference(unsigned rate) {
            const double pi = 3.14159265358979323846;
            double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            double K = std::tan(pi * f0 / rate);
            double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
            double a0 = 1.0 + K / Q + K * K;
            shelf = {(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                     2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
            f0 = 38.13547087602444;
            Q = 0.5003270373238773;
            K = std::tan(pi * f0 / rate);
            a0 = 1.0 + K / Q + K * K;
            highpass = {1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
            stepFrames = rate / 10;
        }
        void Add(const float* x, size_t frames) {
            for (size_t n = 0; n < frames; ++n) {
                double total = 0;
                for (int ch = 0; ch < 2; ++ch) {
                    float s = x[n * 2 + ch];
                    peak = std::max(peak, std::fabs(s));
                    double y = highpass.Run(shelf.Run(s, ch), ch);
                    total += y * y;
                }
                sum += total;
                if (++fill == stepFrames) {
                    recent[count % 30] = sum / stepFrames;
                    steps[count++ % 4] = sum / stepFrames;
                    sum = 0;
                    fill = 0;
                    if (count >= 30) {
                        double window = 0;
                        for (double s : recent) window += s;
                        shortTerm.push_back((float)(window / 30));
                    }
                    if (count >= 4)
                        blocks.push_back((steps[(count - 1) % 4] + steps[(count - 2) % 4] + steps[(count - 3) % 4] +
                                          steps[(count - 4) % 4]) / 4);
                }
            }
        }
    };
    bool identical = true;
    for (unsigned r : {44100u, 48000u, 22050u, 11025u}) {
        LoudnessMeter simd(r);
        Reference reference(r);
        uint32_t seed = r;
        std::vector<float> pcm((size_t)r * 9 * 2);
        for (size_t i = 0; i < pcm.size(); ++i) {
            const double level = (i / 2) % r < r / 3 ? 0.0 : 0.05 + 0.9 * ((i / 2) % 7919) / 7919.0;
            pcm[i] = (float)(level * (int32_t)(seed = 1664525 * seed + 1013904223) / 2147483648.0);
        }
        pcm[r * 5 + 1] = NAN;  // a NaN sample is not a peak
        for (size_t at = 0, frames = pcm.size() / 2; at < frames;) {
            const size_t n = std::min<size_t>(1 + (at * 7) % 6151, frames - at);
            simd.Add(pcm.data() + at * 2, n);
            reference.Add(pcm.data() + at * 2, n);
            at += n;
        }
        // Past the NaN the filters hold NaN in both versions; bit for bit, payloads included.
        const auto& blocks = simd.Blocks();
        identical &= blocks.size() == reference.blocks.size() && simd.Peak() == reference.peak;
        for (size_t i = 0; identical && i < blocks.size(); ++i)
            identical &= std::memcmp(&blocks[i], &reference.blocks[i], sizeof(double)) == 0;
    }
    // A clean signal with changing levels, in chunks that cross the step boundaries unevenly.
    LoudnessMeter simd(44100);
    Reference reference(44100);
    std::vector<float> music(44100 * 2 * 12);
    for (size_t i = 0; i < music.size(); ++i)
        music[i] = (float)(std::sin(i * 0.0137) * std::sin(i * 0.00007) * (0.2 + 0.7 * ((i / 88200) % 3) / 2.0));
    for (size_t at = 0, frames = music.size() / 2; at < frames;) {
        const size_t n = std::min<size_t>(4096 - at % 333, frames - at);
        simd.Add(music.data() + at * 2, n);
        reference.Add(music.data() + at * 2, n);
        at += n;
    }
    identical &= simd.Integrated() == LoudnessMeter::Gated(reference.blocks) && simd.Peak() == reference.peak &&
                 simd.Blocks() == reference.blocks && !reference.shortTerm.empty();
    Check(identical, L"LOUDNESS SIMD meter matches the original scalar K-weighting exactly");
}

void TestTruePeak() {
    testlog::Batch batch;
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
    // Music-like dynamics: a loud start, then quiet and swelling passages, a lone spike and a
    // NaN, so stretches that cannot beat the peak are skipped and the rest still interpolated.
    {
        TruePeak dynamic; Reference oracle;
        std::vector<float> swell(48000*2*3);
        for (size_t i=0;i<swell.size();++i) {
            const double t=i/2/48000.0, level=t<0.2?0.8:t<1?0.05:std::min(1.0,(t-1)*0.6);
            swell[i]=(float)(level*std::sin(i*0.37+(i&1))*std::sin(i*0.0011));
        }
        swell[48000*2+1001]=0.9f; swell[48000*2+5001]=NAN;
        for (size_t at=0,frames=swell.size()/2;at<frames;) {
            const size_t n=std::min<size_t>(1+(at*13)%700,frames-at);
            dynamic.Add(swell.data()+at*2,n); oracle.Add(swell.data()+at*2,n);
            identical &= dynamic.Peak()==(float)oracle.peak;
            at+=n;
        }
        TruePeak copy=dynamic; Reference tail=oracle;
        const float silence[24]{};
        tail.Add(silence,12);
        identical &= copy.FinalPeak()==(float)tail.peak;
    }
    optimized.Reset(); reference=Reference{};
    reference.Add(noise.data(),17); optimized.Add(noise.data(),17);
    batch.Next(L"TRUE PEAK SIMD matches the original scalar FIR exactly across stereo lanes, wrap, chunks "
               L"and reset");
    Check(identical && optimized.Peak()==(float)reference.peak,L"TRUE PEAK SIMD matches original scalar FIR exactly across stereo, wrap, chunks and reset");

    batch.Next(L"TRUE PEAK sees inter-sample and half-sample peaks, reads DC and impulses at their own "
               L"level");
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

    batch.Next(L"TRUE PEAK final queries drain delayed samples without changing streaming state");
    testlog::Cases endings;
    for (size_t frames : {1u, 2u, 7u, 12u, 23u, 24u, 127u}) {
        std::vector<float> terminal(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            terminal[i * 2] = (float)std::sin(i * 1.5707963267948966 + 0.7853981633974483);
            terminal[i * 2 + 1] = -terminal[i * 2] * 0.5f;
        }
        TruePeak actual;
        Reference oracle;
        actual.Add(terminal.data(), frames); oracle.Add(terminal.data(), frames);
        const float streaming = actual.Peak();
        float zeros[24]{};
        oracle.Add(zeros, 12);
        const float final = actual.FinalPeak();
        bool same = final == (float)oracle.peak && actual.Peak() == streaming && actual.FinalPeak() == final;
        actual.Add(terminal.data(), frames);
        Reference continued;
        continued.Add(terminal.data(), frames); continued.Add(terminal.data(), frames);
        same &= actual.Peak() == (float)continued.peak;
        actual.Reset();
        same &= actual.Peak() == 0 && actual.FinalPeak() == 0;
        endings.Add(same, std::to_wstring(frames) + L" frames");
    }
    Check(endings.Passed(), L"TRUE PEAK final queries include the tail, are repeatable and preserve Add/Reset%ls",
          endings.failed.c_str());
}

void TestPeakLimiter() {
    testlog::Batch batch;
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
    batch.Next(L"LIMITER under full scale delays bit-exactly and continues the waveform when engaged "
               L"mid-song");
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

    batch.Next(L"LIMITER catches loud passages, single clicks and peaks between samples without leaving "
               L"full scale");
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

    batch.Next(L"the production limiter preserves headphone reserves and leaves audio below the reserve "
               L"ceiling bit-exact");
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
    testlog::Batch batch;
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
    {
        const auto before = replaygain::LibraryTracks();
        const TrackId available = g_lib.Add(L"C:\\RgScope\\available.flac");
        const TrackId measured = g_lib.Add(L"C:\\RgScope\\measured.flac");
        const TrackId failed = g_lib.Add(L"C:\\RgScope\\retry.flac");
        const TrackId missing = g_lib.Add(L"C:\\RgScope\\missing.flac");
        const TrackId inbox = g_lib.Add(L"C:\\RgScope\\inbox.flac");
        g_lib.Add(L"C:\\RgScope\\playlist-only.flac");
        g_lib[available].flags = Track::kInLibrary;
        g_lib[measured].flags = Track::kInLibrary | Track::kRgScanned;
        g_lib[failed].flags = Track::kInLibrary | Track::kRgScanFailed;
        g_lib[missing].flags = Track::kInLibrary | Track::kMissing;
        g_lib[inbox].flags = Track::kInLibrary | Track::kInbox;
        auto expected = before;
        expected.insert(expected.end(), {available, measured, failed});
        batch.Next(L"RG library scans include measured tracks and retries, while excluding missing and Inbox files");
        Check(replaygain::HasLibraryTracks() && replaygain::LibraryTracks() == expected,
              L"RG scan library covers the available library independently of measurement flags");
    }
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
        batch.Next(L"ReplayGain chooses album, track or playback-order gain and falls back when one is "
                   L"missing");
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
    batch.Next(L"RG MODEL turns loud albums down partly, lifts quiet ones, budgets peaks and keeps hot "
               L"masters present");
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
    batch.Next(L"RG budgets EQ boosts, AutoEq preamp, negative net gain, headphone headroom, off bypass and "
               L"unmeasured tracks");
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
    batch.Next(L"production ReplayGain with EQ preamps, crossfeed and a headset reserve avoids extra "
               L"limiting and double gain");
    Check(transparent, L"production ReplayGain + graphic/AutoEq preamps + crossfeed + headset reserve avoid extra limiting and double gain");
    g.eq = {};
    g.rgHeadphoneHeadroom = 0;
    g.rgKeep = 0;
    const double full = db(finale);
    g.rgOn = false;
    batch.Next(L"RG MODEL keep-nothing and off give unity, album gain waits for measurements and clears "
               L"with them");
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

// A short noise burst on every beat: enough attack for the onset envelope and no tonal bias.
std::vector<float> TempoClicks(unsigned rate, double bpm, double seconds) {
    const size_t frames = (size_t)std::lround(rate * seconds);
    std::vector<float> pcm(frames * 2);
    const double period = 60.0 / bpm * rate;
    uint32_t seed = 12345;
    for (double at = 0; at < frames; at += period) {
        const size_t start = (size_t)at;
        const size_t burst = std::min<size_t>((size_t)(rate * 0.02), frames - start);
        for (size_t k = 0; k < burst; ++k) {
            seed = seed * 1664525u + 1013904223u;
            const float noise = ((seed >> 8) / 16777216.0f) * 2 - 1;
            const float value = 0.8f * noise * (float)std::exp(-(double)k / (0.004 * rate));
            pcm[(start + k) * 2] += value;
            pcm[(start + k) * 2 + 1] += value;
        }
    }
    return pcm;
}

// The same clicks as a 16-bit stereo WAV, for the decoder and scanner path.
std::string TempoWav(unsigned rate, double bpm, double seconds) {
    const auto pcm = TempoClicks(rate, bpm, seconds);
    std::string data(pcm.size() * 2, '\0');
    for (size_t i = 0; i < pcm.size(); ++i) {
        const int16_t sample = (int16_t)std::lround(std::clamp(pcm[i], -1.0f, 1.0f) * 32767);
        data[i * 2] = (char)((uint16_t)sample & 0xFF);
        data[i * 2 + 1] = (char)((uint16_t)sample >> 8);
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

void TestTempo() {
    testlog::Batch batch;
    batch.Next(L"BPM finds the beat of steady click tracks at 44.1, 48 and 96 kHz");
    testlog::Cases steady;
    for (unsigned rate : {44100u, 48000u, 96000u}) {
        for (int bpm : {60, 72, 90, 100, 120, 128, 140, 150, 160, 180, 200}) {
            tempo::Detector detector;
            detector.Begin(rate);
            const auto pcm = TempoClicks(rate, bpm, 8);
            detector.Add(pcm.data(), pcm.size() / 2);
            steady.Add(std::abs(detector.Bpm() - bpm) <= 1,
                       std::to_wstring(rate) + L"/" + std::to_wstring(bpm) + L" -> " + std::to_wstring(detector.Bpm()));
        }
    }
    Check(steady.Passed(), L"BPM detects every steady click track within one beat per minute%ls");
    batch.Next(L"BPM stays empty for silence, noise, a steady tone and a clip too short to count");
    bool empty = true;
    std::vector<float> silence(48000 * 2), noise(48000 * 2), tone(48000 * 2);
    uint32_t seed = 7;
    for (size_t i = 0; i < 48000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        noise[i * 2] = noise[i * 2 + 1] = ((seed >> 8) / 16777216.0f) * 2 - 1;
        tone[i * 2] = tone[i * 2 + 1] = 0.5f * (float)std::sin(6.28318530718 * 1000 * i / 48000);
    }
    for (auto* pcm : {&silence, &noise, &tone}) {
        tempo::Detector detector;
        detector.Begin(48000);
        detector.Add(pcm->data(), pcm->size() / 2);
        empty &= detector.Bpm() == 0;
    }
    tempo::Detector shortClip;
    shortClip.Begin(48000);
    const auto brief = TempoClicks(48000, 120, 2);
    shortClip.Add(brief.data(), brief.size() / 2);
    empty &= shortClip.Bpm() == 0;
    Check(empty, L"BPM never invents a tempo from silence, noise, a steady tone or two seconds of clicks");
    batch.Next(L"BPM gives the same answer when the audio arrives in uneven chunks");
    bool chunks = true;
    for (unsigned rate : {44100u, 48000u}) {
        const auto pcm = TempoClicks(rate, 128, 8);
        tempo::Detector whole, split;
        whole.Begin(rate);
        split.Begin(rate);
        whole.Add(pcm.data(), pcm.size() / 2);
        size_t at = 0;
        for (size_t chunk = 1; at < pcm.size() / 2; chunk = chunk * 3 + 7) {
            const size_t frames = std::min(chunk, pcm.size() / 2 - at);
            split.Add(pcm.data() + at * 2, frames);
            at += frames;
        }
        chunks &= split.Bpm() == whole.Bpm() && whole.Bpm() == 128;
    }
    Check(chunks, L"BPM is independent of how the decoder's reads are split");
}

void TestReplayGainScan(const std::wstring& media, const std::wstring& scratch) {
    testlog::Batch batch;
    std::atomic<bool> cancel{false};
    std::atomic<std::shared_ptr<Decoder>> current;
    auto measure = [&](const std::wstring& path) {
        TrackId id = g_lib.Add(path);
        return replaygain::Measure(g_lib.SnapshotPath(id), 0, 0, cancel, current, nullptr);
    };
    batch.Next(L"RG scans complete files without changing their bytes or retaining an open decoder");
    const std::wstring healthy = media + L"\\sine.flac";
    const auto original = ReadTestBytes(healthy);
    auto good = measure(healthy);
    Check(good.ok && std::isfinite(good.trackGain) && good.trackPeak > 0 && !current.load() &&
          ReadTestBytes(healthy) == original, L"RG accepts a complete scan and leaves the source unchanged");

    batch.Next(L"RG scans detect BPM once, keep a tag's BPM, and survive re-reading tags without one");
    const std::wstring tempoFile = scratch + L"\\rg-tempo.wav";
    if (Ready(WriteTestBytes(tempoFile, TempoWav(48000, 120, 8)), L"the tempo scan fixture is written")) {
        auto beat = measure(tempoFile);
        g_lib[beat.id].flags |= Track::kLoaded;
        g_lib[beat.id].bpm = 0;
        g_lib[beat.id].flags &= (uint16_t)~Track::kBpmDetected;
        replaygain::Apply(beat);
        const bool detected = beat.ok && beat.bpm >= 119 && beat.bpm <= 121 && g_lib[beat.id].bpm == beat.bpm &&
                              (g_lib[beat.id].flags & Track::kBpmDetected);
        TagData tags;
        g_lib[beat.id].bpm = 128;
        g_lib[beat.id].flags &= (uint16_t)~Track::kBpmDetected;
        replaygain::Apply(beat);
        const bool tagWins = g_lib[beat.id].bpm == 128 && !(g_lib[beat.id].flags & Track::kBpmDetected);
        g_lib[beat.id].bpm = 120;
        g_lib[beat.id].flags |= Track::kBpmDetected;
        g_lib.ApplyTags(beat.id, tags);
        const bool kept = g_lib[beat.id].bpm == 120 && (g_lib[beat.id].flags & Track::kBpmDetected);
        g_lib[beat.id].bpm = 0;
        g_lib[beat.id].flags &= (uint16_t)~Track::kBpmDetected;
        const auto picked = replaygain::AutoMeasureTracks({}, 10, true);
        const bool wanted = replaygain::NeedsTempo(beat.id) &&
                            std::find(picked.begin(), picked.end(), beat.id) != picked.end();
        Check(detected && tagWins && kept && wanted,
              L"RG scans fill BPM from the audio, let a tag override it, keep it through a tag read and pick it up when the column asks");
    }

    batch.Next(L"RG rejects a damaged stream after good audio and preserves the previous measurement");
    const std::wstring damaged = scratch + L"\\rg-damaged.flac";
    auto bytes = original;
    if (Ready(bytes.size() > 4096, L"the FLAC scan fixture has multiple audio frames")) {
        std::fill_n(bytes.begin() + bytes.size() / 2, 32, '\xFF');
        if (Ready(WriteTestBytes(damaged, bytes), L"the damaged scan fixture is written")) {
            std::wstring error;
            auto decoder = OpenDecoder(damaged, 48000, error);
            float block[4096 * 2];
            size_t frames = 0;
            if (decoder) while (size_t n = decoder->Read(block, 4096)) frames += n;
            if (Ready(decoder && decoder->Failed() && frames > decoder->Rate() / 2,
                      L"the damaged fixture decodes measurable audio before reporting failure")) {
                auto bad = measure(damaged);
                g_lib[bad.id].rgTrackGain = -325;
                g_lib[bad.id].rgTrackPeak = EncodeReplayPeak(0.5f);
                replaygain::Apply(bad);
                Check(!bad.ok && !current.load() && g_lib[bad.id].rgTrackGain == -325 &&
                      g_lib[bad.id].rgTrackPeak == EncodeReplayPeak(0.5f) &&
                      (g_lib[bad.id].flags & Track::kRgScanFailed) && ReadTestBytes(damaged) == bytes,
                      L"RG never saves a partial decode as a complete measurement");
            }
        }
    }

    batch.Next(L"RG true peaks at the end of a file agree with the same audio followed by silence");
    std::string terminal = MakeSineWav(1, 1);
    auto word = [&](size_t offset, int16_t value) {
        terminal[offset] = (char)(uint16_t)value;
        terminal[offset + 1] = (char)((uint16_t)value >> 8);
    };
    for (size_t i = 44; i + 1 < terminal.size(); i += 2) {
        int16_t value = (int16_t)((uint8_t)terminal[i] | (uint8_t)terminal[i + 1] << 8);
        word(i, (int16_t)(value / 32));
    }
    // Eight fs/4 samples have a 0.707 sample peak and an inter-sample peak near full scale.
    // The burst fits entirely inside the true-peak filter's delay at end of input.
    for (size_t i = 0; i < 8; ++i) {
        const int16_t value = i % 4 < 2 ? 23170 : -23170;
        word(terminal.size() - 32 + i * 4, value);
        word(terminal.size() - 30 + i * 4, (int16_t)-value);
    }
    std::string padded = terminal + std::string(4096 * 4, '\0');
    auto length = [&](size_t at, uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) padded[at + byte] = (char)(value >> (byte * 8));
    };
    length(4, (uint32_t)padded.size() - 8);
    length(40, (uint32_t)padded.size() - 44);
    const std::wstring end = scratch + L"\\rg-terminal.wav", after = scratch + L"\\rg-padded.wav";
    if (Ready(WriteTestBytes(end, terminal) && WriteTestBytes(after, padded), L"terminal peak fixtures are written")) {
        const auto last = measure(end), continued = measure(after);
        fwprintf(g_log, L"  terminal true peak %.6f, padded reference %.6f\n", last.trackPeak, continued.trackPeak);
        Check(last.ok && continued.ok && last.trackPeak > 0.95f &&
              std::abs(last.trackPeak - continued.trackPeak) < 1e-6f && ReadTestBytes(end) == terminal,
              L"RG includes the final inter-sample peaks instead of only the stored sample peak");
    }
    batch.Next(L"RG cancelled scans return no measurement and release the decoder");
    cancel = true;
    Check(!measure(healthy).ok && !current.load(), L"RG cancellation cannot produce a measurement");
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
// accumulator, and the disk cache's store/read-by-key behaviour. The moodbar rides along: its bands,
// its colours, and the same cache and CUE checks.
void TestPeaks(const std::wstring& scratch) {
    testlog::Batch batch;
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
    batch.Next(L"PEAKS turns a half-scale sine, a half-silent file and silence into the expected bucket "
               L"values");
    bool ok = ComputePeaks(sine, 0, 0, sinePeaks, nullptr);
    span(sinePeaks, 0, kPeakBuckets, lo, hi);
    Check(ok && lo >= 179 && hi <= 181, L"PEAKS sine at half scale: %d..%d (expected 180 in every bucket)", lo, hi);
    // Its RMS is half scale over the square root of two: sqrt(0.354) * 255 = 152.
    int rmsLo = 255, rmsHi = 0;
    for (int i = 0; i < kPeakBuckets; ++i) rmsLo = std::min(rmsLo, (int)sinePeaks.rms[i]), rmsHi = std::max(rmsHi, (int)sinePeaks.rms[i]);
    Check(ok && rmsLo >= 150 && rmsHi <= 154, L"PEAKS sine RMS at half scale: %d..%d (expected 152 in every bucket)", rmsLo, rmsHi);

    ok = ComputePeaks(half, 0, 0, halfPeaks, nullptr);
    span(halfPeaks, 0, kPeakBuckets / 2, lo, hi);
    int lo2 = 0, hi2 = 0;
    span(halfPeaks, kPeakBuckets / 2, kPeakBuckets, lo2, hi2);
    Check(ok && lo >= 179 && hi <= 181 && hi2 == 0, L"PEAKS half-silent: first half %d..%d, second half %d..%d", lo, hi, lo2, hi2);

    ok = ComputePeaks(silent, 0, 0, p, nullptr);
    span(p, 0, kPeakBuckets, lo, hi);
    const bool moodSilent = std::all_of(&p.mood[0][0], &p.mood[0][0] + sizeof(p.mood), [](uint8_t v) { return v == 0; });
    Check(ok && hi == 0 && moodSilent, L"PEAKS silence is all zero (max %d, moodbar %ls)", hi, moodSilent ? L"zero" : L"lit");

    // The moodbar's bands: a 100 Hz tone is bass, an 8 kHz tone treble, and a track that moves
    // from one to the other turns its hue from one side of the accent to the other.
    auto tones = [](std::initializer_list<double> hz, Peaks& out) {
        std::vector<float> audio;
        for (double f : hz)
            for (int i = 0; i < 48000 * 2; ++i) {
                float x = (float)(0.5 * std::sin(2 * 3.14159265358979 * f * i / 48000.0));
                audio.push_back(x), audio.push_back(x);
            }
        PeakAccumulator a;
        a.Begin(audio.size() / 2, 48000);
        a.Add(audio.data(), audio.size() / 2);
        return a.Finish(out);
    };
    auto bandMean = [](const Peaks& peaks, int band, int from, int to) {
        int sum = 0;
        for (int i = from; i < to; ++i) sum += peaks.mood[band][i];
        return sum / (to - from);
    };
    // The band split against the original member-state loop, bit for bit across uneven chunks.
    {
        struct Reference {
            float a, b, bass[2], midHigh[2], midLow[2], treble[2];
            void Add(const float* stereo, size_t frames, double sums[kMoodBands]) {
                double low = 0, mid = 0, high = 0;
                for (const float* end = stereo + frames * 2; stereo != end; stereo += 2) {
                    const float x = (stereo[0] + stereo[1]) * 0.5f;
                    const float l1 = bass[0] += a * (x - bass[0]);
                    const float l2 = bass[1] += a * (l1 - bass[1]);
                    const float m1 = x - (midHigh[0] += a * (x - midHigh[0]));
                    const float m2 = m1 - (midHigh[1] += a * (m1 - midHigh[1]));
                    const float m3 = midLow[0] += b * (m2 - midLow[0]);
                    const float m4 = midLow[1] += b * (m3 - midLow[1]);
                    const float t1 = x - (treble[0] += b * (x - treble[0]));
                    const float t2 = t1 - (treble[1] += b * (t1 - treble[1]));
                    low += (double)l2 * l2;
                    mid += (double)m4 * m4;
                    high += (double)t2 * t2;
                }
                sums[kMoodBass] += low;
                sums[kMoodMid] += mid;
                sums[kMoodTreble] += high;
                for (float* state : {bass, midHigh, midLow, treble})
                    for (int i = 0; i < 2; ++i)
                        if (std::fabs(state[i]) < 1e-20f) state[i] = 0;
            }
        };
        std::vector<float> audio(44100 * 2);
        uint32_t seed = 7;
        for (size_t i = 0; i < audio.size(); ++i)
            audio[i] = (float)((i / 2 % 9000 < 3000 ? 0.0 : 0.6) * (int32_t)(seed = 1664525 * seed + 1013904223) / 2147483648.0);
        MoodSplit split;
        split.Begin(44100);
        split.Add(audio.data(), 1, std::array<double, kMoodBands>{}.data());  // primes it on the first sample
        Reference reference{split.a, split.b};
        std::memcpy(reference.bass, split.bass, sizeof(split.bass));
        std::memcpy(reference.midHigh, split.midHigh, sizeof(split.midHigh));
        std::memcpy(reference.midLow, split.midLow, sizeof(split.midLow));
        std::memcpy(reference.treble, split.treble, sizeof(split.treble));
        double sums[kMoodBands]{}, expected[kMoodBands]{};
        for (size_t at = 1, frames = audio.size() / 2; at < frames;) {
            const size_t n = std::min<size_t>(1 + at % 1777, frames - at);
            split.Add(audio.data() + at * 2, n, sums);
            reference.Add(audio.data() + at * 2, n, expected);
            at += n;
        }
        batch.Next(L"MOODBAR band split matches the original loop exactly");
        Check(std::memcmp(sums, expected, sizeof(sums)) == 0 && std::memcmp(split.bass, reference.bass, sizeof(split.bass)) == 0 &&
                  std::memcmp(split.midLow, reference.midLow, sizeof(split.midLow)) == 0 &&
                  std::memcmp(split.treble, reference.treble, sizeof(split.treble)) == 0,
              L"MOODBAR band split matches the original loop exactly");
    }
    Peaks low{}, high{}, sweep{};
    batch.Next(L"MOODBAR splits bass and treble, colours by balance and keeps an accent's contrast");
    ok = tones({100}, low) && tones({8000}, high) && tones({100, 8000}, sweep);
    const int lowBass = bandMean(low, kMoodBass, 0, kPeakBuckets), lowMid = bandMean(low, kMoodMid, 0, kPeakBuckets),
              lowTreble = bandMean(low, kMoodTreble, 0, kPeakBuckets);
    const int highBass = bandMean(high, kMoodBass, 0, kPeakBuckets), highMid = bandMean(high, kMoodMid, 0, kPeakBuckets),
              highTreble = bandMean(high, kMoodTreble, 0, kPeakBuckets);
    Check(ok && lowBass >= 240 && lowMid + 50 < lowBass && lowTreble < 30 && highTreble >= 240 &&
              highMid + 20 < highTreble && highBass < 30,
          L"MOODBAR 100 Hz is bass (bass %d, mid %d, treble %d) and 8 kHz treble (bass %d, mid %d, treble %d)",
          lowBass, lowMid, lowTreble, highBass, highMid, highTreble);
    const float center = MoodCenter(sweep);
    const MoodTone bassy = MoodOf(sweep, 20, kPeakBuckets / 2 - 20, center),
                   bright = MoodOf(sweep, kPeakBuckets / 2 + 20, kPeakBuckets - 20, center);
    Check(bassy.bright < -0.9f && bright.bright > 0.9f && bassy.level > 0.8f && bright.level > 0.8f &&
              MoodOf(p, 0, kPeakBuckets, 0).level == 0,
          L"MOODBAR tones follow the balance: bass half %.2f, treble half %.2f, silence level %.2f", bassy.bright,
          bright.bright, MoodOf(p, 0, kPeakBuckets, 0).level);
    // Every hue-table scheme, turned from the accent or from any Waveform colour, keeps the accents'
    // contrast at full level and tells bass from treble; each Waveform colour keeps it too.
    testlog::Cases contrast;
    const int tableSchemes[] = {kMoodTheme, kMoodVivid, kMoodWarm, kMoodCool, kMoodColorBlind, kMoodSingle};
    for (const Palette* palette : {&LightPalette(), &DarkPalette()}) {
        for (int wave = kWaveTheme; wave < kWaveColorChoices; ++wave) {
            const Rgb chosen = WaveColorOn(wave, palette->surface), base = chosen != kNoRgb ? chosen : palette->accent;
            wchar_t name[48];
            swprintf_s(name, L"%ls wave %d", palette->dark ? L"dark" : L"light", wave);
            if (wave != kWaveTheme)
                contrast.Add(chosen != kNoRgb && Contrast(chosen, palette->surface) >= 3 &&
                                 FadedWaveColor(chosen, palette->surface) != chosen, name);
            for (int scheme : tableSchemes) {
                MoodPalette moods;
                moods.Make(base, palette->surface, scheme);
                swprintf_s(name, L"%ls wave %d scheme %d", palette->dark ? L"dark" : L"light", wave, scheme);
                for (int h = 0; h < MoodPalette::kHues; ++h) {
                    Rgb loud = moods.played[h][MoodPalette::kLevels - 1];
                    contrast.Add(Contrast(loud, palette->surface) >= 3 && moods.rest[h][MoodPalette::kLevels - 1] != loud,
                                 name);
                }
                contrast.Add(moods.Pick({0.9f, 1}, true) != moods.Pick({-0.9f, 1}, true) &&
                                 moods.Pick({0, 0}, true) == palette->surface, name);
            }
        }
    }
    Check(contrast.Passed() && WaveColorOn(kWaveTheme, 0x202020) == kNoRgb,
          L"MOODBAR and Waveform colours keep 3:1 on the player in every scheme, differ when unplayed and tell "
          L"bass from treble%ls",
          contrast.failed.c_str());

    // Spectrum puts the bands on red, green and blue; Vivid spreads its hues further than Theme.
    MoodPalette spectrum, vivid, theme;
    spectrum.Make(0xE04050, 0x202020, kMoodSpectrum);
    vivid.Make(0xE04050, 0x202020, kMoodVivid);
    theme.Make(0xE04050, 0x202020, kMoodTheme);
    MoodTone bassTone, trebleTone;
    bassTone.level = trebleTone.level = 1;
    bassTone.bands[kMoodBass] = trebleTone.bands[kMoodTreble] = 1;
    bassTone.bright = -1, trebleTone.bright = 1;
    const Rgb red = spectrum.Pick(bassTone, true), blue = spectrum.Pick(trebleTone, true);
    auto hueGap = [](Rgb a, Rgb b) {
        int d = 0;
        for (int shift : {16, 8, 0}) d += std::abs((int)(a >> shift & 0xFF) - (int)(b >> shift & 0xFF));
        return d;
    };
    const bool spectrumOk = red == 0xFF0000 && blue == 0x0000FF;
    // Vivid passes through more colours on its way from bass to treble than Theme does.
    auto travel = [&](const MoodPalette& m) {
        int sum = 0;
        for (int h = 1; h < MoodPalette::kHues; ++h)
            sum += hueGap(m.played[h - 1][MoodPalette::kLevels - 1], m.played[h][MoodPalette::kLevels - 1]);
        return sum;
    };
    const bool spread = travel(vivid) > travel(theme);
    // Every theme hue keeps its saturation: no table entry comes out gray (the conversion's sectors).
    bool colourful = true;
    for (int h = 0; h < MoodPalette::kHues; ++h) {
        Rgb c = theme.played[h][MoodPalette::kLevels - 1];
        int most = std::max({c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF}), least = std::min({c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF});
        colourful = colourful && most - least > 80;
    }
    Check(spectrumOk && spread && colourful,
          L"MOODBAR colour schemes: spectrum bass %06x, treble %06x; vivid spreads further; theme stays saturated %d",
          red, blue, (int)colourful);

    // The live scale follows the music: a bass tone lights the bass, and a following silence leaves
    // nothing lit. Smoothing turns a step into a ramp and keeps a flat run flat.
    LiveMood live;
    Peaks liveOut{};
    std::vector<float> liveEnergy(64 * kMoodBands);
    for (int i = 0; i < 64; ++i) liveEnergy[i * kMoodBands + kMoodBass] = 0.1f, liveEnergy[i * kMoodBands + kMoodTreble] = 1e-6f;
    live.Scale(liveEnergy.data(), 64, liveOut, 0);
    const bool liveLit = liveOut.mood[kMoodBass][10] == 255 && liveOut.mood[kMoodTreble][10] < 40;
    std::fill(liveEnergy.begin(), liveEnergy.end(), 0.0f);
    live.Scale(liveEnergy.data(), 64, liveOut, 1.0 / 30);
    const bool liveQuiet = liveOut.mood[kMoodBass][10] == 0;
    // The scale goes by the clock, not by frames: a loud moment, then a second of music 20 dB
    // quieter, leaves the same ceiling at 60 frames a second as at 30.
    auto settled = [](int fps) {
        LiveMood scale;
        Peaks out{};
        std::vector<float> energy(64 * kMoodBands, 1e-6f);
        for (int i = 0; i < 64; ++i) energy[i * kMoodBands + kMoodBass] = 0.1f;
        double clock = 0;
        scale.Scale(energy.data(), 64, out, clock);
        for (int i = 0; i < 64; ++i) energy[i * kMoodBands + kMoodBass] = 0.001f;
        for (int frame = 0; frame < fps; ++frame) scale.Scale(energy.data(), 64, out, clock += 1.0 / fps);
        return (int)out.mood[kMoodBass][10];
    };
    const int at30 = settled(30), at60 = settled(60);
    Check(std::abs(at30 - at60) <= 1 && at30 > 0 && at30 < 255,
          L"MOODBAR live scale falls by the clock: bass %d at 30 fps, %d at 60 fps", at30, at60);
    Peaks step{}, soft{};
    std::fill(step.v + kPeakBuckets / 2, step.v + kPeakBuckets, (uint8_t)200);
    std::fill(&step.mood[kMoodMid][0], &step.mood[kMoodMid][0] + kPeakBuckets, (uint8_t)90);
    SmoothPeaks(step, soft, 8);
    const int mid = kPeakBuckets / 2;
    const bool ramp = soft.v[mid - 20] == 0 && soft.v[mid + 20] == 200 && soft.v[mid] > 60 && soft.v[mid] < 140 &&
                      soft.v[mid - 4] < soft.v[mid] && soft.mood[kMoodMid][0] == 90 && soft.mood[kMoodMid][kPeakBuckets - 1] == 90;
    Check(liveLit && liveQuiet && ramp, L"MOODBAR live scale (lit %d, quiet %d) and smoothing (step at %d)",
          (int)liveLit, (int)liveQuiet, (int)soft.v[mid]);

    // Seek bar glow's pulse, frame by frame at 30 a second: steady music settles to a middle glow, a
    // beat over it jumps up at once and falls back within half a second, and a held clock holds it.
    {
    GlowPulse pulse;
    std::array<uint8_t, 256> quietRun{}, beatRun{};
    quietRun.fill(80);
    beatRun.fill(80);
    std::fill(beatRun.end() - 32, beatRun.end(), (uint8_t)250);
    double clock = 0;
    for (int frame = 0; frame < 90; ++frame) pulse.Step(quietRun.data(), 256, clock += 1.0 / 30);
    const float steady = pulse.Value();
    const float hit = pulse.Step(beatRun.data(), 256, clock += 1.0 / 30);
    const float held = pulse.Step(quietRun.data(), 256, clock);
    for (int frame = 0; frame < 15; ++frame) pulse.Step(quietRun.data(), 256, clock += 1.0 / 30);
    const float after = pulse.Value();
    Check(steady > 0.05f && steady < 0.4f && hit > steady + 0.4f && held == hit && after < steady + 0.15f,
          L"SEEK GLOW pulse: steady %.2f, beat %.2f, held %.2f, half a second later %.2f", steady, hit, held, after);
    }

    testlog::Cases ranges;
    batch.Next(L"PEAKS ranges preserve their samples, reject reversed bounds and cancellation, and CUE "
               L"tracks match the file's window");
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
    Check(trackOk && fileOk && hi == 0 && std::memcmp(trackPeaks.v, filePeaks.v, kPeakBuckets) == 0 &&
              std::memcmp(trackPeaks.mood, filePeaks.mood, sizeof(filePeaks.mood)) == 0,
          L"PEAKS CUE track [%.3f, %.3f): track=%d file=%d max %d (expected both decoded, max 0)", from, to,
          (int)trackOk, (int)fileOk, hi);

    // Unknown length: 20 s of float audio is enough to merge the fine buckets several times.
    std::vector<float> stereo(48000 * 2 * 20);
    for (size_t i = 0; i < stereo.size() / 2; ++i)
        stereo[i * 2] = stereo[i * 2 + 1] = (float)(0.5 * std::sin(2 * 3.14159265358979 * 1000 * i / 48000.0));
    PeakAccumulator acc;
    batch.Next(L"PEAKS the accumulator and partial previews keep the track timeline, merge blocks and "
               L"quick-preview any range");
    acc.Begin(0);
    for (size_t i = 0; i < stereo.size(); i += 4096 * 2) acc.Add(stereo.data() + i, std::min<size_t>(4096, stereo.size() / 2 - i / 2));
    Peaks accPeaks{};
    ok = acc.Finish(accPeaks);
    span(accPeaks, 0, kPeakBuckets, lo, hi);
    bool accRms = std::all_of(accPeaks.rms, accPeaks.rms + kPeakBuckets, [](uint8_t v) { return v >= 150 && v <= 154; });
    Check(ok && lo >= 179 && hi <= 181 && accRms, L"PEAKS accumulator, unknown length, 20 s: %d..%d, RMS %d (expected 152 after merges)",
          lo, hi, (int)accPeaks.rms[kPeakBuckets / 2]);

    // A partial decode stays in its place on the full track, including after the blocks merge.
    PeakAccumulator previewAcc;
    Peaks preview{};
    previewAcc.Begin(4 * 48000);
    bool previewOk = !previewAcc.Preview(preview);
    previewAcc.Add(stereo.data(), 48000);
    previewOk = previewOk && previewAcc.Preview(preview);
    span(preview, 0, kPeakBuckets / 4, lo, hi);
    span(preview, kPeakBuckets / 4, kPeakBuckets, lo2, hi2);
    previewOk = previewOk && lo >= 179 && hi <= 181 && hi2 == 0;
    std::vector<float> quiet(48000 * 2);
    previewAcc.Add(quiet.data(), 48000);
    previewOk = previewOk && previewAcc.Preview(preview);
    span(preview, 0, kPeakBuckets / 4, lo, hi);
    span(preview, kPeakBuckets / 4, kPeakBuckets, lo2, hi2);
    previewOk = previewOk && lo >= 179 && hi <= 181 && hi2 == 0;
    previewAcc.Begin(0);
    previewAcc.Add(stereo.data(), 4096);
    previewOk = previewOk && !previewAcc.Preview(preview);
    previewAcc.Begin(4);
    previewAcc.Add(stereo.data() + 24, 1);  // one half-scale frame, over a quarter of this short track
    previewOk = previewOk && previewAcc.Preview(preview);
    span(preview, 0, kPeakBuckets / 4, lo, hi);
    span(preview, kPeakBuckets / 4, kPeakBuckets, lo2, hi2);
    Check(previewOk && lo >= 179 && hi <= 181 && hi2 == 0,
          L"PEAKS partial waveforms stay on the known track timeline, including after blocks merge");

    struct SampledTrack final : Decoder {
        double position = 0;
        bool fast = true;
        int seeks = 0;
        bool Seek(double seconds) override { position = seconds; ++seeks; return true; }
        bool FastSeek() const override { return fast; }
        double Duration() const override { return 4; }
        unsigned Rate() const override { return 1000; }
        size_t Read(float* out, size_t frames) override {
            const float level[] = {0.1f, 0.3f, 0.6f, 0.9f};
            for (size_t i = 0; i < frames; ++i) {
                float value = level[std::clamp((int)position, 0, 3)];
                out[i * 2] = out[i * 2 + 1] = value;
                position += 0.001;
            }
            return frames;
        }
    } sampled;
    Peaks overview{};
    bool whole = PreviewPeaks(sampled, 0, 4, overview, nullptr) &&
                 std::abs((int)overview.v[75] - CompandPeak(0.1f)) <= 2 &&
                 std::abs((int)overview.v[225] - CompandPeak(0.3f)) <= 2 &&
                 std::abs((int)overview.v[375] - CompandPeak(0.6f)) <= 2 &&
                 std::abs((int)overview.v[525] - CompandPeak(0.9f)) <= 2;
    bool slice = PreviewPeaks(sampled, 1, 2, overview, nullptr) &&
                 std::abs((int)overview.v[150] - CompandPeak(0.3f)) <= 2 &&
                 std::abs((int)overview.v[450] - CompandPeak(0.6f)) <= 2;
    sampled.fast = false;
    int beforeSeek = sampled.seeks;
    bool bounded = !PreviewPeaks(sampled, 0, 4, overview, nullptr) && sampled.seeks == beforeSeek;
    sampled.fast = true;
    Check(whole && slice && bounded && !PreviewPeaks(sampled, 0, 4, overview, &cancelled),
          L"PEAKS quick preview represents the whole song and CUE range, skips slow seeks and honors cancellation");

    // The disk cache: a fresh service must answer from disk what the first one computed.
    const std::wstring cache = scratch + L"\\peaks_cache";
    std::error_code ec;
    std::filesystem::remove_all(cache, ec);  // a cache left by an earlier run would answer at once
    const uint64_t size = 1000, time = 42;
    StartPeaks(nullptr, 0, cache);
    uint32_t req = 0;
    Peaks got{};
    bool hit = GetPeaks(sine, size, time, 0, 0, got, req);
    batch.Next(L"PEAKS the worker queues and answers requests, and its disk cache round trips by file key "
               L"and range");
    Check(!hit && req != 0, L"PEAKS first request queues work (request %u)", req);
    for (int i = 0; i < 500 && !hit; ++i) {
        Sleep(20);
        hit = GetPeaks(sine, size, time, 0, 0, got, req);
    }
    Check(hit && std::memcmp(got.v, sinePeaks.v, kPeakBuckets) == 0, L"PEAKS queued result matches the direct computation");
    StopPeaks();
    StartPeaks(nullptr, 0, cache);
    hit = GetPeaks(sine, size, time, 0, 0, got, req);
    Check(hit && std::memcmp(got.v, sinePeaks.v, kPeakBuckets) == 0 &&
              std::memcmp(got.mood, sinePeaks.mood, sizeof(got.mood)) == 0 && sinePeaks.mood[kMoodMid][300] > 200,
          L"PEAKS cache round trip: a new service reads the disk copy, moodbar included");
    Peaks ranged{};
    std::memset(ranged.v, 77, sizeof(ranged.v));
    StorePeaks(sine, size, time, 0.5, 1.0, ranged);
    hit = GetPeaks(sine, size, time, 0.5, 1.0, got, req);
    Check(hit && std::memcmp(got.v, ranged.v, kPeakBuckets) == 0, L"PEAKS stored range is returned by its own key");
    hit = GetPeaks(sine, size + 1, time, 0, 0, got, req);
    Check(!hit, L"PEAKS a changed file size is a cache miss");
    // A library scan stores peaks for every track it decodes, so the disk cache has to keep itself
    // under budget on its own. Each entry is 3,012 bytes, so this writes well past 20 MB; the oldest
    // entries must be gone and the newest must have survived. It uses its own folder so the entries
    // the checks below rely on are not pruned away.
    batch.Next(L"PEAKS the disk cache stays within its byte budget while tracks are stored");
    const std::wstring bulkCache = scratch + L"\\peaks_cache_budget";
    std::filesystem::remove_all(bulkCache, ec);
    StartPeaks(nullptr, 0, bulkCache);
    for (uint64_t i = 0; i < 7500; ++i) StorePeaks(sine, 100000 + i, time, 0, 0, ranged);
    uint64_t cacheBytes = 0;
    size_t cacheFiles = 0;
    for (const auto& entry : std::filesystem::directory_iterator(bulkCache)) {
        if (entry.path().extension() != L".pk") continue;
        ++cacheFiles;
        cacheBytes += entry.file_size();
    }
    const std::wstring newest = bulkCache + L"\\" + PeaksKeyName(sine, 100000 + 7499, time) + L".pk";
    Check(cacheFiles > 0 && cacheBytes <= 21ull << 20 && GetFileAttributesW(newest.c_str()) != INVALID_FILE_ATTRIBUTES,
          L"PEAKS the disk cache stays within its budget (%zu files, %llu bytes)", cacheFiles,
          (unsigned long long)cacheBytes);
    StopPeaks();
    std::filesystem::remove_all(bulkCache, ec);
    StartPeaks(nullptr, 0, cache);
    // The Mood column asks from memory only: a miss queues the track, and its result then answers.
    Peaks listed{};
    bool listMiss = !ListPeaks(half, size + 50, time, 0, 0, listed), listHit = false;
    for (int i = 0; i < 500 && !listHit; ++i) {
        Sleep(20);
        listHit = ListPeaks(half, size + 50, time, 0, 0, listed);
    }
    Check(listMiss && listHit && std::memcmp(listed.v, halfPeaks.v, kPeakBuckets) == 0 &&
              std::memcmp(listed.mood, halfPeaks.mood, sizeof(listed.mood)) == 0,
          L"PEAKS the Mood column queues a missing track and then reads it from memory (miss %d, hit %d)",
          (int)listMiss, (int)listHit);
    StopPeaks();

    // The actual next song is warmed behind the current one, so the hand-off can read its
    // complete waveform without waiting for a second decode.
    StartPeaks(nullptr, 0, cache);
    uint32_t foreground = 0;
    bool front = !GetPeaks(sine, size + 10, time, 0, 0, got, foreground) && foreground;
    PrefetchPeaks(half, size + 11, time, 0, 0);
    const std::wstring prefetched = cache + L"\\" + PeaksKeyName(half, size + 11, time) + L".pk";
    ULONGLONG prefetchUntil = GetTickCount64() + 5000;
    while (GetFileAttributesW(prefetched.c_str()) == INVALID_FILE_ATTRIBUTES && GetTickCount64() < prefetchUntil) Sleep(1);
    uint32_t current = 0, future = 0;
    bool both = GetPeaks(sine, size + 10, time, 0, 0, got, current) &&
                std::memcmp(got.v, sinePeaks.v, kPeakBuckets) == 0 &&
                GetPeaks(half, size + 11, time, 0, 0, got, future) &&
                std::memcmp(got.v, halfPeaks.v, kPeakBuckets) == 0;
    batch.Next(L"PEAKS prefetch warms the next song without disturbing the current request");
    Check(front && both && !future, L"PEAKS prefetch leaves the current request intact and makes the next song ready");
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
    // Each sampled part has a different real peak; most of the sparse file remains silent.
    for (int i = 0; prepared && i < 32; ++i) {
        LARGE_INTEGER at{};
        at.QuadPart = header.size() + (uint64_t)(audioBytes / 4) * (2 * i + 1) / 64 * 4;
        int16_t amplitude = (int16_t)(1000 + i * 450);
        int16_t sample[] = {amplitude, amplitude};
        prepared = SetFilePointerEx(file, at, nullptr, FILE_BEGIN) &&
                   WriteFile(file, sample, sizeof(sample), &written, nullptr) && written == sizeof(sample);
    }
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
        uint32_t first = 0, intermediate = 0, next = 0;
        bool queued = !GetPeaks(longFile, end.QuadPart, time, 0, 0, got, first) && first;
        bool progressed = waitFor([&] { return GetPeakProgress(first, got); });
        bool started = progressed && isOpen();
        bool fullPreview = progressed && got.v[kPeakBuckets - 1] > got.v[kPeakBuckets / 4] && got.v[kPeakBuckets / 4] > 0;
        // Make two requests together so the second is rapid even on a busy test machine.
        bool switched = !GetPeaks(sine, size + 3, time, 0, 0, got, intermediate) && intermediate;
        PrefetchPeaks(half, size + 2, time, 0, 0);
        bool nextQueued = !GetPeaks(half, size + 2, time, 0, 0, got, next) && next;
        bool oldHidden = !GetPeakProgress(first, got);
        Sleep(150);
        uint32_t pending = 0;
        bool deferred = !GetPeaks(half, size + 2, time, 0, 0, got, pending) && pending == next;
        bool rapidPreview = GetPeakProgress(next, got) && got.v[kPeakBuckets / 4] > 100 &&
                            got.v[3 * kPeakBuckets / 4] == 0;
        bool nextReady = waitFor([&] { return GetPeaks(half, size + 2, time, 0, 0, got, next); });
        const std::wstring longCache = cache + L"\\" + PeaksKeyName(longFile, end.QuadPart, time) + L".pk";
        bool abandoned = GetFileAttributesW(longCache.c_str()) == INVALID_FILE_ATTRIBUTES;
        uint32_t retry = 0;
        bool retryQueued = !GetPeaks(longFile, end.QuadPart, time, 0, 0, got, retry) && retry && retry != first;
        bool restarted = waitFor([&] { return GetPeakProgress(retry, got); }) && isOpen();
        bool cached = GetPeaks(sine, size, time, 0, 0, got, next);
        bool closed = waitFor([&] { return !isOpen(); });
        uint32_t again = 0, afterCancel = 0;
        bool canRetry = !GetPeaks(longFile, end.QuadPart, time, 0, 0, got, again) && again;
        CancelPeaks();
        bool canReturn = !GetPeaks(longFile, end.QuadPart, time, 0, 0, got, afterCancel) && afterCancel && afterCancel != again;
        CancelPeaks();
        StopPeaks();
        batch.Next(L"PEAKS previews in full width while decoding and cancels rapid changes without caching "
                   L"partial results");
        Check(queued && started && progressed && fullPreview && switched && nextQueued && oldHidden && deferred && rapidPreview &&
                  nextReady && abandoned &&
                  retryQueued && restarted && cached && closed && canRetry && canReturn,
              L"PEAKS shows a full-width preview while decoding; rapid changes cancel without caching partial results "
              L"(queue=%d started=%d progress=%d preview=%d switched=%d next=%d old=%d delayed=%d rapid-preview=%d ready=%d abandoned=%d "
              L"retry=%d restarted=%d cached=%d closed=%d return=%d/%d)",
              queued, started, progressed, fullPreview, switched, nextQueued, oldHidden, deferred, rapidPreview, nextReady, abandoned,
              retryQueued, restarted, cached, closed, canRetry, canReturn);
    }
    DeleteFileW(longFile.c_str());
}

// Reverb, tempo, loudness compensation and night mode (dsp.h), alone and through the production renderer.
void TestEffects() {
    testlog::Batch batch;
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
    batch.Next(L"REVERB every preset rings and decays audibly; off rings out, a seek ducks the tail and "
               L"silence sleeps until sound");
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
    batch.Next(L"TEMPO keeps a tone's pitch and level, divides the length by the tempo, ignoring chunking");
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
    batch.Next(L"LOUDNESS is flat at full volume and lifts bass and treble towards -20 dB without exceeding "
               L"the attenuation");
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
    batch.Next(L"NIGHT lifts quiet passages, softens loud ones and ignores near-silence; effects off stay "
               L"exact and reverb is limited");
    Check(std::fabs(lifted - 9) < 0.5 && std::fabs(softened + 5) < 0.5 && hiss.LiftDb() == 0,
          L"NIGHT lifts a -39 dB passage by %.1f dB (expect 9), softens a -6 dB one by %.1f dB (expect -5), ignores -80 dB",
          lifted, softened);

    // Bitcrush: off is exact and asleep; on, every sample lands on the depth's levels and is held for
    // the rate's run of frames; the mix blends; turning it off glides out without a jump, then sleeps.
    const auto crushIn = tone(440, 0.7, 0.5);
    Bitcrusher idle;
    idle.Configure({}, rate);
    auto idleOut = crushIn;
    run(idle, idleOut);
    const size_t settled = rate / 20;  // past the 20 ms glide
    Bitcrusher four;
    four.Configure({true, 4, 48000, 1}, rate);
    auto fourOut = crushIn;
    run(four, fourOut);
    bool onLevels = true;
    for (size_t i = settled * 2; i < fourOut.size(); ++i)
        onLevels = onLevels && std::fabs(fourOut[i] * 8 - std::round(fourOut[i] * 8)) < 1e-5f;
    Bitcrusher held;
    held.Configure({true, 16, 6000, 1}, rate);
    auto heldOut = crushIn;
    run(held, heldOut);
    size_t changes = 0;  // a 440 Hz tone moves every frame, so changes come only at the 6 kHz hold rate
    for (size_t i = settled; i + 1 < heldOut.size() / 2; ++i) changes += heldOut[(i + 1) * 2] != heldOut[i * 2];
    const double perSecond = changes * (double)rate / (heldOut.size() / 2 - settled - 1);
    Bitcrusher half;
    half.Configure({true, 3, 48000, 0.5f}, rate);
    auto halfOut = crushIn;
    run(half, halfOut);
    const size_t probe = settled + 123;
    const float halfWant = (crushIn[probe * 2] + std::round(crushIn[probe * 2] * 4) / 4) / 2;
    Bitcrusher fading;
    fading.Configure({true, 3, 4000, 1}, rate);
    auto fadeOut = crushIn;
    fading.Process(fadeOut.data(), settled);
    fading.Configure({false, 3, 4000, 1}, rate);
    fading.Process(fadeOut.data() + settled * 2, fadeOut.size() / 2 - settled);
    // The glide off never steps further than the crushed signal itself already does.
    float crushedStep = 0, jump = 0;
    for (size_t i = settled / 2; i + 1 < settled; ++i)
        crushedStep = std::max(crushedStep, std::fabs(fadeOut[(i + 1) * 2] - fadeOut[i * 2]));
    for (size_t i = settled; i < settled + rate / 50; ++i) jump = std::max(jump, std::fabs(fadeOut[(i + 1) * 2] - fadeOut[i * 2]));
    bool tailExact = !fading.Active();
    for (size_t i = (settled + rate / 25) * 2; i < fadeOut.size(); ++i) tailExact = tailExact && fadeOut[i] == crushIn[i];
    batch.Next(L"BITCRUSH off is exact, quantizes and holds as set, mixes, and glides off to an exact bypass");
    Check(!idle.Active() && idleOut == crushIn && onLevels && fourOut != crushIn && std::fabs(perSecond - 6000) < 60 &&
              std::fabs(halfOut[probe * 2] - halfWant) < 1e-5f && jump <= crushedStep + 1e-4f && tailExact,
          L"BITCRUSH off exact %d, 4-bit levels %d, 6 kHz hold %.0f changes/s, 50%% mix %.4f (expect %.4f), off "
          L"glide step %.3f (crushed steps %.3f), exact after %d",
          idleOut == crushIn, onLevels, perSecond, halfOut[probe * 2], halfWant, jump, crushedStep, tailExact);

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
    RenderEffects crushed;
    crushed.bitcrush = {true, 6, 8000, 1};
    auto withCrush = RenderPlayerPcm(sine.data(), sine.size() / 2, rate, {}, false, 1, 0, &crushed);
    float crushPeak = 0;
    for (float s : withCrush) crushPeak = std::isfinite(s) ? std::max(crushPeak, std::fabs(s)) : 2;
    Check(withCrush.size() == sine.size() && withCrush != sine && crushPeak < 0.97f,
          L"BITCRUSH through the renderer changes the sound and stays under the limiter's ceiling (peak %.3f)", crushPeak);
}
