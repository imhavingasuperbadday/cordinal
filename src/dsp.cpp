// Equalizer, EBU R128 loudness and true-peak DSP (see dsp.h). Everything works on interleaved
// stereo float in [-1, 1]; filter state is per-instance and the meters accumulate without clipping.
#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <emmintrin.h>

namespace {
constexpr double kPi = 3.14159265358979323846;
void Fft(std::complex<float>* a, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -6.2831853f / len;
        std::complex<float> step(std::cos(angle), std::sin(angle));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (int k = 0; k < len / 2; ++k) {
                auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

// Foobar's analyzer uses 200 ms, a Gaussian window, exact low-frequency DFTs and a -60..+6 dB
// display. Apollo's shorter Hann window and frequency tilt are a different visualizer.
void FoobarSpectrum(const std::vector<float>& stereo, unsigned rate, float* levels, size_t bands) {
    rate = std::max(8000u, rate);
    size_t count = std::min(stereo.size() / 2, (size_t)std::max(rate, 8000u) / 5);
    if (count < 2) { std::fill_n(levels, bands, 0.0f); return; }
    size_t first = (stereo.size() / 2 - count) / 2, n = 2;
    while (n * 2 <= count) n *= 2;
    std::vector<std::complex<float>> bins(n);
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
        double x = (i - (n - 1) * 0.5) * ((20.0 / 3) / (n - 1));
        float window = (float)std::exp(-0.5 * x * x);
        sum += window;
        bins[i] = (stereo[(first + i) * 2] + stereo[(first + i) * 2 + 1]) * 0.5f * window;
    }
    Fft(bins.data(), (int)n);
    std::vector<float> window(count);
    double dftSum = 0;
    for (size_t i = 0; i < count; ++i) {
        double x = (i - (count - 1) * 0.5) * ((20.0 / 3) / (count - 1));
        window[i] = (float)std::exp(-0.5 * x * x);
        dftSum += window[i];
    }
    auto frequency = [&](double band) { return 50 * std::pow(1.37, band * 20 / bands); };
    for (size_t band = 0; band < bands; ++band) {
        size_t low = std::min(n / 2, (size_t)std::lround(frequency(band - 0.5) * n / rate));
        size_t high = std::min(n / 2, (size_t)std::lround(frequency(band + 0.5) * n / rate));
        double magnitude = 0;
        if (low + 1 < high) {
            for (size_t i = low; i < high; ++i) magnitude = std::max(magnitude, (double)std::abs(bins[i]) * 2 / sum);
        } else if (frequency((double)band) <= rate * 0.5) {
            double angle = 2 * kPi * frequency((double)band) / rate;
            std::complex<double> phase{1, 0}, step{std::cos(angle), std::sin(angle)}, value{};
            for (size_t i = 0; i < count; ++i) {
                value += phase * (double)((stereo[(first + i) * 2] + stereo[(first + i) * 2 + 1]) * 0.5f * window[i]);
                phase *= step;
            }
            magnitude = std::abs(value) * 2 / dftSum;
        }
        levels[band] = (float)std::clamp((20 * std::log10(std::max(magnitude, 1e-8)) + 60) / 66, 0.0, 1.0);
    }
}
}

void SpectrumLevels(const float* stereo, unsigned rate, float* levels, size_t bands) {
    constexpr int n = 2048;
    std::complex<float> a[n];
    for (int i = 0; i < n; ++i) {
        float v = (stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f;
        a[i] = (std::isfinite(v) ? v : 0) * (0.5f - 0.5f * std::cos(6.2831853f * i / (n - 1)));
    }
    Fft(a, n);
    double hz = std::max(8000u, rate), top = std::min(20000.0, hz / 2);
    for (size_t x = 0; x < bands; ++x) {
        double f0 = 40 * std::pow(top / 40, (double)x / bands);
        double f1 = 40 * std::pow(top / 40, (double)(x + 1) / bands);
        int b0 = std::clamp((int)(f0 * n / hz), 1, n / 2 - 1);
        int b1 = std::clamp((int)(f1 * n / hz), b0, n / 2 - 1);
        float mag = 0;
        for (int b = b0; b <= b1; ++b) mag = std::max(mag, std::abs(a[b]));
        float db = 20 * std::log10(std::max(mag * 4 / n * (float)std::sqrt(f0 / 1000), 1e-7f));
        levels[x] = std::clamp((db + 72) / 72, 0.0f, 1.0f);
    }
}

size_t AudioVisuals::Prepare(unsigned rate) {
    size_t frames = std::max(kFrames, (size_t)std::clamp(rate, 8000u, 384000u) * 3 / 10);
    wave.resize(frames * 2);
    return frames;
}

std::vector<float> AudioVisuals::Curve(size_t pixels) const {
    std::vector<float> curve(pixels);
    const size_t frames = wave.size() / 2;
    if (!frames || !pixels) return curve;
    auto mono = [&](size_t i) {
        float v = (wave[i * 2] + wave[i * 2 + 1]) * 0.5f;
        return std::isfinite(v) ? v : 0;
    };
    const double step = (double)frames / pixels;
    double offset = 0;
    if (time >= 0) {
        // Keep the sample grid attached to the audio clock. Overlapping frames then scroll by
        // whole pixels instead of changing all their averages on every timer tick.
        double start = time - 0.15, pixel = 0.3 / pixels;
        offset = (std::round(start / pixel) * pixel - start) / 0.3 * frames;
    }
    for (size_t x = 0; x < pixels; ++x) {
        double first = std::max(0.0, x * step + offset), end = std::min((double)frames, (x + 1) * step + offset), value = 0;
        // Average the complete pixel span, including fractional edge samples. At high zoom,
        // interpolate its midpoint instead (foobar2000's 0x18011de30 sample reducer).
        if (step > 1) {
            for (size_t i = (size_t)first; i < frames && i < end; ++i)
                value += mono(i) * (std::min(end, (double)i + 1) - std::max(first, (double)i));
            value /= end - first;
        } else {
            double mid = (first + end) * 0.5;
            size_t i = std::min((size_t)mid, frames - 1);
            double f = mid - i;
            value = mono(i) * (1 - f) + mono(std::min(i + 1, frames - 1)) * f;
        }
        curve[x] = (float)value;
    }
    return curve;
}

bool AudioVisuals::Update(bool active, unsigned rate, double elapsed) {
    std::array<float, kBands> next{};
    if (active) {
        for (float& v : wave) if (!std::isfinite(v)) v = 0;
        FoobarSpectrum(wave, rate, next.data(), next.size());
    }
    else { std::fill(wave.begin(), wave.end(), 0.0f); time = -1; }
    for (size_t i = 0; i < levels.size(); ++i) {
        float previous = peakAge[i];
        peakAge[i] += (float)std::clamp(elapsed, 0.0, 10.0);
        float fall = (std::max(0.0f, peakAge[i] - 0.333f) - std::max(0.0f, previous - 0.333f)) * (30.0f / 66);
        levels[i] = next[i];
        peaks[i] = std::max(0.0f, peaks[i] - fall);
        if (levels[i] >= peaks[i]) { peaks[i] = levels[i]; peakAge[i] = 0; }
    }
    return active || Moving();
}

bool AudioVisuals::Moving() const {
    return std::any_of(peaks.begin(), peaks.end(), [](float x) { return x > 0; });
}

void WaveformMotion::Add(const float* stereo, size_t frames, unsigned rate) {
    if (!rate || !stereo) return;
    if (rate_ != rate) {
        *this = {};
        rate_ = rate;
        block_ = std::max(1u, rate / 200);  // 5 ms energy windows catch plucked notes as well as drums
        double seconds = (double)block_ / rate;
        attack_ = (float)(1 - std::exp(-seconds / 0.006));
        release_ = (float)(1 - std::exp(-seconds / 0.120));
        follow_ = (float)(1 - std::exp(-seconds / 0.100));
    }
    for (size_t i = 0; i < frames; ++i) {
        float left = stereo[i * 2], right = stereo[i * 2 + 1];
        left = std::isfinite(left) ? std::clamp(left, -1.f, 1.f) : 0;
        right = std::isfinite(right) ? std::clamp(right, -1.f, 1.f) : 0;
        power_ += std::max(left * left, right * right);  // opposite-phase stereo must not cancel
        if (++count_ != block_) continue;
        float amplitude = std::min(1.f, (float)std::sqrt(2 * power_ / block_));
        // A rising note gets a brief extra push; a sustained tone settles to its own level.
        float target = std::min(1.f, amplitude + 1.5f * std::max(0.f, amplitude - baseline_));
        baseline_ += (amplitude - baseline_) * follow_;
        envelope_ += (target - envelope_) * (target > envelope_ ? attack_ : release_);
        if (baseline_ < 1e-6f) baseline_ = 0;
        if (envelope_ < 1e-6f) envelope_ = 0;
        power_ = 0;
        count_ = 0;
    }
}

float WaveformMotion::Level() const {
    // A gentle knee keeps quiet passages visible while leaving more dynamic range than sqrt.
    return 2 * envelope_ / (1 + envelope_);
}

void WaveformMotion::Shape(uint8_t* peaks, size_t buckets) const {
    if (!peaks || !buckets) return;
    float maximum = *std::max_element(peaks, peaks + buckets), level = Level();
    for (size_t i = 0; i < buckets; ++i) {
        // Keep the recent waveform's texture, but move the whole display on the current note.
        float detail = maximum > 0 ? peaks[i] / maximum : 0;
        peaks[i] = (uint8_t)std::clamp(std::lround(255 * level * (0.35f + 0.65f * detail)), 0l, 255l);
    }
}

// Built-in presets; each gain array is in dB and matches kEqFrequencies.
const std::vector<EqPreset>& EqPresets() {
    static const std::vector<EqPreset> presets = {
        {L"Flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {L"Bass boost", {6, 5, 4, 2, 0, 0, 0, 0, 0, 0}},
        {L"Treble boost", {0, 0, 0, 0, 0, 1, 2, 4, 5, 6}},
        {L"Loudness", {5, 4, 2, 0, -1, -1, 0, 2, 4, 5}},
        {L"Rock", {4, 3, 2, 0, -1, -1, 1, 3, 4, 4}},
        {L"Pop", {-1, 1, 3, 4, 3, 1, 0, -1, -1, -1}},
        {L"Jazz", {3, 2, 1, 2, -1, -1, 0, 1, 2, 3}},
        {L"Classical", {4, 3, 2, 1, -1, -1, 0, 2, 3, 4}},
        {L"Electronic", {5, 4, 1, 0, -2, 1, 0, 1, 4, 5}},
        {L"Vocal", {-2, -2, -1, 1, 3, 4, 3, 1, 0, -1}},
        {L"Headphones", {3, 2, 0, -1, -1, 0, 1, 2, 3, 2}},
    };
    return presets;
}

namespace {
// Flushes denormals while filtering. A recursive filter's tail decays into the denormal range after
// some seconds of silence (a pause, a quiet intro), where every multiply costs ~100 cycles: enough
// for twenty biquads to starve the audio thread.
struct FlushDenormals {
    unsigned saved = _mm_getcsr();
    FlushDenormals() { _mm_setcsr(saved | 0x8040); }  // FTZ | DAZ
    ~FlushDenormals() { _mm_setcsr(saved); }
};

using Coefficients = std::array<double, 5>;  // b0, b1, b2, a1, a2 (a0 = 1)

// |H|^2 of a biquad at w radians/sample.
double Power(const Coefficients& c, double w) {
    const double co = std::cos(w), s = std::sin(w), c2 = 2 * co * co - 1, s2 = 2 * s * co;
    const double nr = c[0] + c[1] * co + c[2] * c2, ni = c[1] * s + c[2] * s2;
    const double dr = 1 + c[3] * co + c[4] * c2, di = c[3] * s + c[4] * s2;
    return std::max(nr * nr + ni * ni, 1e-300) / std::max(dr * dr + di * di, 1e-300);
}

// |H|^2 of RBJ's analog prototype at W = frequency / f0, with A = 10^(gain/40).
double PrototypePower(EqFilterType type, double W, double A, double q) {
    auto poly = [W](double s2, double s1, double s0) {  // |s2 (jW)^2 + s1 jW + s0|^2
        const double re = s0 - s2 * W * W, im = s1 * W;
        return re * re + im * im;
    };
    if (type == EqFilterType::Peak) return poly(1, A / q, 1) / poly(1, 1 / (A * q), 1);
    const double r = std::sqrt(A) / q;
    if (type == EqFilterType::LowShelf) return A * A * poly(1, r, A) / poly(A, r, 1);
    return A * A * poly(A, r, 1) / poly(1, r, A);
}

// RBJ cookbook (bilinear transform): exact at the center, but squeezed toward Nyquist, so a treble
// band comes out narrower than asked and a high shelf falls short above it.
Coefficients RbjDesign(EqFilterType type, double w0, double A, double q) {
    const double alpha = std::sin(w0) / (2 * q), c = std::cos(w0);
    double b0, b1, b2, a0, a1, a2;
    if (type == EqFilterType::Peak) {
        b0 = 1 + alpha * A; b1 = -2 * c; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * c; a2 = 1 - alpha / A;
    } else {
        const double t = 2 * std::sqrt(A) * alpha, k = type == EqFilterType::LowShelf ? 1 : -1;
        b0 = A * ((A + 1) - k * (A - 1) * c + t);
        b1 = 2 * k * A * ((A - 1) - k * (A + 1) * c);
        b2 = A * ((A + 1) - k * (A - 1) * c - t);
        a0 = (A + 1) + k * (A - 1) * c + t;
        a1 = -2 * k * ((A - 1) + k * (A + 1) * c);
        a2 = (A + 1) + k * (A - 1) * c - t;
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

// Vicanek, "Matched Second Order Digital Filters" (2016): the prototype's poles by impulse
// invariance, then zeros that match its magnitude exactly at DC, at Nyquist and at the center. The
// curve keeps its analog shape up to the top of the band. False when the zeros can't be placed.
bool MatchedDesign(EqFilterType type, double w0, double A, double q, Coefficients& c) {
    double wp = w0, qp = q;  // the poles' frequency and Q
    if (type == EqFilterType::Peak) qp = q * A;
    else wp = type == EqFilterType::LowShelf ? w0 / std::sqrt(A) : w0 * std::sqrt(A);
    const double z = 1 / (2 * qp), decay = std::exp(-z * wp);
    const double a1 = -2 * decay * (z <= 1 ? std::cos(std::sqrt(1 - z * z) * wp) : std::cosh(std::sqrt(z * z - 1) * wp));
    const double a2 = decay * decay;
    // |polynomial|^2 = P0 cos^2(w/2) + P1 sin^2(w/2) + P2 sin^2(w), for the poles (A*) and zeros (B*).
    const double A0 = (1 + a1 + a2) * (1 + a1 + a2), A1 = (1 - a1 + a2) * (1 - a1 + a2), A2 = -4 * a2;
    const double wm = std::min(w0, kPi * 0.9), p1 = std::sin(wm / 2) * std::sin(wm / 2), p0 = 1 - p1, p2 = 4 * p0 * p1;
    const double B0 = A0 * PrototypePower(type, 0, A, q), B1 = A1 * PrototypePower(type, kPi / w0, A, q);
    const double B2 = (PrototypePower(type, wm / w0, A, q) * (A0 * p0 + A1 * p1 + A2 * p2) - B0 * p0 - B1 * p1) / p2;
    const double s0 = std::sqrt(B0), s1 = std::sqrt(B1), W = (s0 + s1) / 2, d = W * W + B2;
    if (!(d >= 0)) return false;
    const double b0 = (W + std::sqrt(d)) / 2;
    c = {b0, (s0 - s1) / 2, -B2 / (4 * b0), a1, a2};
    return std::all_of(c.begin(), c.end(), [](double v) { return std::isfinite(v); });
}

// The matched design nearly always follows the prototype more closely, by about half on average;
// RBJ stays for a shelf whose poles fall past Nyquist. Each is scored by its worst error across
// the band, which costs a few hundred cosines per filter on a change.
Coefficients DesignFilter(EqFilterType type, double f0, double gainDb, double q, unsigned rate) {
    const double A = std::pow(10.0, gainDb / 40), w0 = 2 * kPi * f0 / rate;
    const Coefficients rbj = RbjDesign(type, w0, A, q);
    Coefficients matched;
    if (!MatchedDesign(type, w0, A, q, matched)) return rbj;
    auto error = [&](const Coefficients& c) {
        double worst = 0;
        for (int k = 0; k < 24; ++k) {
            const double f = 20 * std::pow(rate * 0.49 / 20, k / 23.0);
            worst = std::max(worst, std::fabs(10 * std::log10(Power(c, 2 * kPi * f / rate) /
                                                              PrototypePower(type, f / f0, A, q))));
        }
        return worst;
    };
    return error(matched) <= error(rbj) ? matched : rbj;
}
}  // namespace

// Neighbouring graphic bands overlap: set to its slider alone, two +6 dB bands make about +8 between
// and at them. Solve for filter gains whose combined response lands on every slider at its band
// center (Liski and Valimaki's accurate cascade graphic EQ), refined a few times because a biquad's
// dB response isn't linear in its gain.
std::array<float, kEqBands> GraphicFilterGains(const std::array<float, kEqBands>& sliders, unsigned rate) {
    std::array<float, kEqBands> out{};
    if (!rate) return out;
    double target[kEqBands], gain[kEqBands], f0[kEqBands], w[kEqBands];
    bool any = false;
    for (int i = 0; i < kEqBands; ++i) {
        target[i] = gain[i] = std::isfinite(sliders[i]) ? std::clamp((double)sliders[i], -40.0, 40.0) : 0;
        any |= std::fabs(target[i]) > 0.01;
        f0[i] = std::clamp((double)kEqFrequencies[i], 1.0, rate * 0.49);
        w[i] = 2 * kPi * f0[i] / rate;
    }
    if (!any) return out;
    for (int pass = 0;; ++pass) {
        double m[kEqBands][kEqBands + 1] = {}, worst = 0;  // interaction per dB of gain | the error left
        for (int j = 0; j < kEqBands; ++j) {
            const double reference = std::fabs(gain[j]) > 0.5 ? gain[j] : 12;
            const Coefficients unit = DesignFilter(EqFilterType::Peak, f0[j], reference, kGraphicQ, rate);
            const Coefficients actual =
                reference == gain[j] ? unit : DesignFilter(EqFilterType::Peak, f0[j], gain[j], kGraphicQ, rate);
            for (int i = 0; i < kEqBands; ++i) {
                m[i][j] = 10 * std::log10(Power(unit, w[i])) / reference;
                m[i][kEqBands] -= 10 * std::log10(Power(actual, w[i]));
            }
        }
        for (int i = 0; i < kEqBands; ++i) worst = std::max(worst, std::fabs(m[i][kEqBands] += target[i]));
        if (worst < 0.005 || pass == 5) break;
        for (int col = 0; col < kEqBands; ++col) {  // Gauss-Jordan elimination with partial pivoting
            int pivot = col;
            for (int r = col + 1; r < kEqBands; ++r)
                if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
            if (std::fabs(m[pivot][col]) < 1e-9) return out;  // can't happen for these bands; stay flat
            std::swap(m[pivot], m[col]);
            for (int r = 0; r < kEqBands; ++r) {
                if (r == col) continue;
                const double f = m[r][col] / m[col][col];
                for (int k = col; k <= kEqBands; ++k) m[r][k] -= f * m[col][k];
            }
        }
        for (int j = 0; j < kEqBands; ++j) gain[j] = std::clamp(gain[j] + m[j][kEqBands] / m[j][j], -40.0, 40.0);
    }
    for (int i = 0; i < kEqBands; ++i) out[i] = (float)gain[i];
    return out;
}

// Builds the curve for `s` (DesignFilter: peaking and Q-based shelf filters) and crossfades to it. Out-of-band
// centers are limited below Nyquist so profiles remain usable when exclusive output changes the
// sample rate. A filter in use before and after keeps its state, so the new curve starts settled.
void Equalizer::Configure(const EqSettings& s, unsigned rate) {
    if (rate != rate_) {
        // A new stream: nothing is playing through the old curve, so there is nothing to fade.
        rate_ = rate;
        chain_ = {};
        fade_ = 0;
        pendingSet_ = false;
        fadeLength_ = std::max<size_t>(1, (size_t)(rate * kFadeSeconds));
        Apply(s, false);
    } else if (fade_) {
        pending_ = s;
        pendingSet_ = true;
    } else Apply(s, true);
}

void Equalizer::Apply(const EqSettings& s, bool fade) {
    if (fade && s == settings_) return;  // keeps every tail exactly as it was
    settings_ = s;
    const unsigned rate = rate_;
    Chain next;
    next.enabled = s.enabled && rate > 0;
    const auto graphic = !s.parametric && next.enabled ? GraphicFilterGains(s.gains, rate) : std::array<float, kEqBands>{};
    next.crossfeed = s.crossfeed && rate > 0;
    next.preampGain = next.enabled && std::isfinite(s.preamp) ? std::pow(10.0, std::clamp(s.preamp, -60.0f, 24.0f) / 20.0) : 1;
    for (int i = 0; i < kParametricBands; ++i) {
        EqFilter f;
        if (s.parametric) {
            f = s.filters[i];
            f.enabled = f.enabled && i < s.filterCount;
        } else {
            f.enabled = i < kEqBands;
            if (f.enabled) { f.frequency = kEqFrequencies[i]; f.gain = graphic[i]; f.q = kGraphicQ; }
        }
        double gain = std::isfinite(f.gain) ? std::clamp((double)f.gain, -40.0, 40.0) : 0;
        double f0 = rate ? std::clamp((double)f.frequency, 1.0, std::max(1.0, rate * 0.49)) : 0;
        next.used[i] = next.enabled && f.enabled && std::isfinite(f.frequency) && std::isfinite(f.q) &&
                       f.q >= 0.05f && f.q <= 100 && std::fabs(gain) > 0.01;
        if (!next.used[i]) continue;
        Biquad& b = next.bands[i];
        next.centers[i] = 2 * kPi * f0 / rate;
        const Coefficients c = DesignFilter(f.type, f0, gain, f.q, rate);
        b.b0 = c[0]; b.b1 = c[1]; b.b2 = c[2]; b.a1 = c[3]; b.a2 = c[4];
        if (chain_.used[i]) {
            std::copy_n(chain_.bands[i].z1, 2, b.z1);
            std::copy_n(chain_.bands[i].z2, 2, b.z2);
        }
    }
    if (next.crossfeed) {
        // bs2b (Bauer stereophonic-to-binaural) at its default level: 700 Hz, 4.5 dB feed. The
        // opposite ear gets a low-passed copy; the direct path gets the matching high shelf, so
        // the overall tone stays even, and the sum is normalised to unity gain for mono bass.
        const double feed = 4.5, gainLow = -feed * 5 / 6 - 3, gainHigh = feed / 6 - 3;
        const double lo = std::pow(10.0, gainLow / 20), hi = 1 - std::pow(10.0, gainHigh / 20);
        const double cutHigh = 700 * std::pow(2.0, (gainLow - 20 * std::log10(hi)) / 12);
        const double xLow = std::exp(-2 * kPi * 700 / rate), xHigh = std::exp(-2 * kPi * cutHigh / rate);
        next.loA0 = lo * (1 - xLow);
        next.loB1 = xLow;
        next.hiA0 = 1 - hi * (1 - xHigh);
        next.hiA1 = -xHigh;
        next.hiB1 = xHigh;
        next.crossGain = 1 / (1 - hi + lo);
        if (chain_.crossfeed) {
            std::copy_n(chain_.lo, 2, next.lo);
            std::copy_n(chain_.hi, 2, next.hi);
            std::copy_n(chain_.asis, 2, next.asis);
        }
    }
    const bool audible = chain_.enabled || chain_.crossfeed || next.enabled || next.crossfeed;
    if (fade && audible) {
        previous_ = chain_;
        fade_ = fadeLength_;
    }
    chain_ = next;
    active_ = fade_ || chain_.enabled || chain_.crossfeed;
}

double Equalizer::FilterDb(double hz) const {
    if (!chain_.enabled || !rate_) return 0;
    const double w = 2 * kPi * std::clamp(hz, 0.0, rate_ / 2.0) / rate_;
    double db = 0;
    for (int i = 0; i < kParametricBands; ++i)
        if (chain_.used[i]) {
            const Biquad& b = chain_.bands[i];
            db += 10 * std::log10(Power({b.b0, b.b1, b.b2, b.a1, b.a2}, w));
        }
    return db;
}

double Equalizer::PeakGainDb() const {
    const Chain& c = chain_;
    if (!c.enabled) return 0;
    const double preamp = 20 * std::log10(c.preampGain);
    if (std::none_of(c.used.begin(), c.used.end(), [](bool used) { return used; })) return preamp;
    auto gain = [&](double w) { return preamp + FilterDb(w * rate_ / (2 * kPi)); };
    // Log spacing finds broad maxima; exact band centers keep a high-Q peak from falling between
    // samples. Refine local maxima so overlapping filters are budgeted as one response, not summed boosts.
    std::array<double, 515 + kParametricBands> frequencies{}, response{};
    size_t count = 0;
    frequencies[count++] = 0;
    for (int i = 0; i <= 512; ++i)
        frequencies[count++] = 2 * kPi * std::pow(std::max(1.0, rate_ / 2.0), i / 512.0) / rate_;
    for (int i = 0; i < kParametricBands; ++i) if (c.used[i]) frequencies[count++] = c.centers[i];
    std::sort(frequencies.begin(), frequencies.begin() + count);
    double peak = -1e9;
    for (size_t i = 0; i < count; ++i) peak = std::max(peak, response[i] = gain(frequencies[i]));
    for (size_t i = 1; i + 1 < count; ++i) {
        if (response[i] < response[i - 1] || response[i] < response[i + 1]) continue;
        double lo = frequencies[i - 1], hi = frequencies[i + 1];
        for (int step = 0; step < 32; ++step) {
            const double a = lo + (hi - lo) / 3, b = hi - (hi - lo) / 3;
            if (gain(a) < gain(b)) lo = a;
            else hi = b;
        }
        peak = std::max(peak, gain((lo + hi) / 2));
    }
    return std::ceil(peak * 20 - 1e-6) / 20;
}

// Preamp, then the bands in series, each in transposed direct form II (one state pair per
// channel), then crossfeed. Doubles throughout: the biquads would ring badly in float. A disabled
// chain leaves the frame untouched.
void Equalizer::Chain::Run(double& left, double& right) {
    double v[2] = {left * preampGain, right * preampGain};
    for (int i = 0; i < kParametricBands; ++i) {
        if (!used[i]) continue;
        Biquad& b = bands[i];
        for (int ch = 0; ch < 2; ++ch) {
            double y = b.b0 * v[ch] + b.z1[ch];
            b.z1[ch] = b.b1 * v[ch] - b.a1 * y + b.z2[ch];
            b.z2[ch] = b.b2 * v[ch] - b.a2 * y;
            v[ch] = y;
        }
    }
    if (crossfeed) {
        for (int ch = 0; ch < 2; ++ch) {
            lo[ch] = loA0 * v[ch] + loB1 * lo[ch];
            hi[ch] = hiA0 * v[ch] + hiA1 * asis[ch] + hiB1 * hi[ch];
            asis[ch] = v[ch];
        }
        v[0] = (hi[0] + lo[1]) * crossGain;
        v[1] = (hi[1] + lo[0]) * crossGain;
    }
    left = v[0];
    right = v[1];
}

void Equalizer::Process(float* x, size_t frames) {
    if (!active_) return;
    FlushDenormals flush;
    for (size_t n = 0; n < frames; ++n) {
        if (!fade_ && pendingSet_) {
            pendingSet_ = false;
            Apply(pending_, true);
        }
        double left = x[n * 2], right = x[n * 2 + 1];
        if (fade_) {
            // Both curves run on the same input; the output moves linearly from the old to the new.
            double oldLeft = left, oldRight = right;
            previous_.Run(oldLeft, oldRight);
            chain_.Run(left, right);
            const double t = (double)(fadeLength_ - --fade_) / fadeLength_;
            left = oldLeft + (left - oldLeft) * t;
            right = oldRight + (right - oldRight) * t;
        } else chain_.Run(left, right);
        x[n * 2] = (float)left;
        x[n * 2 + 1] = (float)right;
    }
    if (!fade_ && !pendingSet_) active_ = chain_.enabled || chain_.crossfeed;
}

// ---------------------------------------------------------------------------
// Reverb. Each line's loop: read (modulated, interpolated), damp, scale by the decay gain, mix
// through the Householder matrix (I - 2/N), add the send, write. The wet output is normalised by the
// network's steady-state energy, so presets with long and short decays sit at a similar level.

namespace {
struct ReverbShape { float scale, seconds, dampHz, preMs, depthMs; };
constexpr ReverbShape kReverbShapes[] = {
    {0.45f, 0.7f, 7000, 6, 0.12f},   // Room
    {1.0f, 2.4f, 5000, 22, 0.3f},    // Hall
    {0.7f, 1.6f, 10000, 1, 0.2f},    // Plate: dense and bright, almost no pre-delay
};
constexpr float kLineMs[8] = {31.7f, 37.9f, 41.3f, 45.1f, 52.7f, 59.3f, 66.1f, 72.9f};
constexpr float kLfoHz[8] = {0.31f, 0.43f, 0.57f, 0.67f, 0.79f, 0.89f, 1.03f, 1.13f};
// Orthogonal sign patterns, so the two outputs are decorrelated and the send isn't the Householder
// matrix's own eigenvector.
constexpr float kInSign[8] = {1, -1, -1, 1, -1, 1, 1, -1};
constexpr float kLeftSign[8] = {1, -1, 1, -1, 1, -1, 1, -1};
constexpr float kRightSign[8] = {1, 1, -1, -1, 1, 1, -1, -1};

float Toward(float value, float target, float step) {
    return value < target ? std::min(target, value + step) : std::max(target, value - step);
}
}  // namespace

void Reverb::Configure(const ReverbSettings& s, unsigned rate) {
    const bool on = s.preset > ReverbPreset::Off && s.preset < ReverbPreset::Count && rate > 0;
    settings_ = s;
    if (!on) settings_.preset = ReverbPreset::Off;  // the send fades; the tail rings out as built
    settings_.mix = std::isfinite(s.mix) ? std::clamp(s.mix, 0.0f, 1.0f) : 0;
    if (rate != rate_) {  // a new stream: nothing to ring out
        rate_ = rate;
        step_ = rate ? 1.0f / (0.02f * rate) : 1;
        active_ = reset_ = false;
    }
    if (!on) return;
    if (!active_) {
        Build(settings_.preset);
        send_ = 0;
        wet_ = 1;
        mix_ = settings_.mix;
        active_ = true;
    } else if (settings_.preset != built_) reset_ = true;  // duck, rebuild, come back
}

void Reverb::Clear() {
    if (active_) reset_ = true;
}

void Reverb::Build(ReverbPreset preset) {
    built_ = preset;
    const ReverbShape& s = kReverbShapes[(int)preset - 1];
    const double r = rate_;
    double loss = 0, longest = 0;
    for (int i = 0; i < kLines; ++i) {
        length_[i] = (float)std::max(16.0, std::floor(kLineMs[i] * s.scale * r / 1000));
        depth_[i] = (float)(s.depthMs * r / 1000 * (i % 2 ? 1.0 : 0.7));
        const double g = std::pow(10.0, -3.0 * length_[i] / (s.seconds * r));
        gain_[i] = (float)g;
        loss += 1 - g * g;
        longest = std::max(longest, (double)length_[i] + 2 * depth_[i]);
        const double w = 2 * kPi * kLfoHz[i] / r;
        rotCos_[i] = (float)std::cos(w);
        rotSin_[i] = (float)std::sin(w);
        cos_[i] = (float)std::cos(i * 0.8);
        sin_[i] = (float)std::sin(i * 0.8);
        low_[i] = 0;
    }
    for (size_ = 1; size_ < longest + 4; size_ <<= 1) {}
    lines_.assign(size_ * kLines, 0.0f);
    pos_ = 0;
    preDelay_ = (size_t)(s.preMs * r / 1000);
    pre_.assign(preDelay_ + 1, 0.0f);
    prePos_ = 0;
    damp_ = (float)std::exp(-2 * kPi * std::min((double)s.dampHz, r * 0.45) / r);
    // Twice the energy-preserving level: the damping in the loops takes about that much from a full mix.
    outScale_ = (float)std::sqrt(loss / kLines) * 2;
    // A Butterworth high-pass at 150 Hz on the send: the bass stays dry.
    const double w0 = 2 * kPi * 150 / r, alpha = std::sin(w0) / (2 * 0.70710678), co = std::cos(w0), a0 = 1 + alpha;
    hp_[0] = (1 + co) / 2 / a0;
    hp_[1] = -(1 + co) / a0;
    hp_[2] = hp_[0];
    hp_[3] = -2 * co / a0;
    hp_[4] = (1 - alpha) / a0;
    hpZ_[0] = hpZ_[1] = 0;
    quietFrames_ = 0;
}

void Reverb::Process(float* x, size_t frames) {
    const float sendTarget = settings_.preset != ReverbPreset::Off ? 1.0f : 0.0f;
    if (!active_) {
        // Asleep in silence with a preset on: wake, empty, on the first sound. Nothing rings yet, so
        // the send starts fully open.
        if (sendTarget == 0 || std::all_of(x, x + frames * 2, [](float s) { return s == 0; })) return;
        Build(settings_.preset);
        send_ = wet_ = 1;
        mix_ = settings_.mix;
        active_ = true;
    }
    FlushDenormals flush;
    const size_t mask = size_ - 1;
    float loudest = 0, input = 0;
    for (size_t n = 0; n < frames; ++n) {
        float* s = x + n * 2;
        wet_ = Toward(wet_, reset_ ? 0.0f : 1.0f, step_);
        if (reset_ && wet_ == 0) {  // ducked: start again empty, with the current preset if any
            reset_ = false;
            if (settings_.preset == ReverbPreset::Off) {
                active_ = false;
                return;
            }
            Build(settings_.preset);
        }
        send_ = Toward(send_, sendTarget, step_);
        mix_ = Toward(mix_, settings_.mix, step_);
        input = std::max(input, std::fabs(s[0]) + std::fabs(s[1]));
        const float in = (s[0] + s[1]) * 0.5f * send_;
        const float delayed = pre_[prePos_];
        pre_[prePos_] = in;
        if (++prePos_ == pre_.size()) prePos_ = 0;
        const double h = hp_[0] * delayed + hpZ_[0];
        hpZ_[0] = hp_[1] * delayed - hp_[3] * h + hpZ_[1];
        hpZ_[1] = hp_[2] * delayed - hp_[4] * h;
        const float feed = (float)h * 0.35355339f;  // 1/sqrt(8): the send's energy, spread over the lines
        float y[kLines], sum = 0;
        for (int i = 0; i < kLines; ++i) {
            const float c = cos_[i] * rotCos_[i] - sin_[i] * rotSin_[i];
            sin_[i] = sin_[i] * rotCos_[i] + cos_[i] * rotSin_[i];
            cos_[i] = c;
            const float at = (float)(pos_ + size_) - (length_[i] + depth_[i] * (1 + sin_[i]));
            const size_t k = (size_t)at;
            const float frac = at - (float)k;
            const float* line = &lines_[i * size_];
            const float a = line[k & mask], b = line[(k + 1) & mask];
            const float v = a + (b - a) * frac;
            low_[i] = v + damp_ * (low_[i] - v);
            y[i] = low_[i] * gain_[i];
            sum += y[i];
        }
        sum *= 2.0f / kLines;
        float left = 0, right = 0;
        for (int i = 0; i < kLines; ++i) {
            lines_[i * size_ + pos_] = y[i] - sum + feed * kInSign[i];
            left += y[i] * kLeftSign[i];
            right += y[i] * kRightSign[i];
        }
        pos_ = (pos_ + 1) & mask;
        const float wet = outScale_ * wet_ * mix_;
        s[0] += left * wet;
        s[1] += right * wet;
        loudest = std::max(loudest, std::fabs(left) + std::fabs(right));
    }
    for (int i = 0; i < kLines; ++i) {  // the oscillators drift in float; keep them on the circle
        const float norm = 1.0f / std::sqrt(cos_[i] * cos_[i] + sin_[i] * sin_[i]);
        cos_[i] *= norm;
        sin_[i] *= norm;
    }
    // Off (or fed silence) and the tail gone (-100 dB) for a tenth of a second: stop costing anything,
    // and let a paused or stopped output idle.
    if ((send_ == 0 || input == 0) && loudest < 1e-5f) {
        quietFrames_ += frames;
        if (quietFrames_ >= rate_ / 10) active_ = reset_ = false;
    } else quietFrames_ = 0;
}

// ---------------------------------------------------------------------------
// WSOLA. Output frame k * hop_ stands for input frame nominal_ at that time; nominal_ advances by
// hop_ * tempo per segment. A segment's first half overlaps the previous segment's second half
// (tail_), and the periodic Hann window sums to exactly 1 across that overlap.

void TimeStretch::Configure(unsigned rate, double tempo) {
    tempo_ = std::isfinite(tempo) ? std::clamp(tempo, 0.25, 4.0) : 1.0;
    hop_ = std::max<size_t>(64, rate / 50);
    search_ = std::max<size_t>(16, rate / 100);
    window_.resize(hop_ * 2);
    for (size_t n = 0; n < window_.size(); ++n) window_[n] = (float)(0.5 - 0.5 * std::cos(kPi * n / hop_));
    tail_.assign(hop_ * 2, 0.0f);
    Reset();
}

void TimeStretch::Reset() {
    in_.clear();
    base_ = prev_ = 0;
    nominal_ = 0;
    primed_ = false;
    std::fill(tail_.begin(), tail_.end(), 0.0f);
}

double TimeStretch::PendingOutput() const {
    if (!Active()) return 0;
    return std::max(0.0, ((double)base_ + in_.size() / 2 - nominal_) / tempo_);
}

void TimeStretch::Process(const float* in, size_t frames, std::vector<float>& out) {
    out.clear();
    in_.insert(in_.end(), in, in + frames * 2);
    Run(out);
}

void TimeStretch::Flush(std::vector<float>& out) {
    if (!hop_ || (in_.empty() && !primed_)) return Reset();
    const double end = (double)base_ + in_.size() / 2;
    in_.resize(in_.size() + (hop_ + search_ + 1) * 4, 0.0f);  // enough silence to pass the end
    std::vector<float> more;
    Run(more);
    // What was made from the padding past the end is cut, so the output length matches the input.
    const size_t extra = (size_t)std::clamp(std::llround((nominal_ - end) / tempo_), 0ll, (long long)more.size() / 2);
    out.insert(out.end(), more.begin(), more.end() - (ptrdiff_t)extra * 2);
    Reset();
}

void TimeStretch::Run(std::vector<float>& out) {
    if (!hop_) return;
    const size_t length = hop_ * 2;
    for (;;) {
        const uint64_t end = base_ + in_.size() / 2;
        const uint64_t center = (uint64_t)std::llround(nominal_);
        if (center + (primed_ ? search_ : 0) + length > end) break;
        uint64_t best = std::max<uint64_t>(center, base_);
        if (primed_) {
            // The candidate whose start best matches how the previous segment would have gone on
            // (normalised cross-correlation on the mono mix): coarsely every other frame, then refined.
            const uint64_t lo = std::max<uint64_t>(base_, center > search_ ? center - search_ : 0), hi = center + search_;
            const uint64_t target = prev_ + hop_;
            mono_.resize((size_t)(end - base_));
            for (uint64_t f = std::min(lo, target); f < end; ++f) {
                const float* p = &in_[(size_t)(f - base_) * 2];
                mono_[(size_t)(f - base_)] = p[0] + p[1];
            }
            const float* t = &mono_[(size_t)(target - base_)];
            auto score = [&](uint64_t c, size_t stride) {
                const float* m = &mono_[(size_t)(c - base_)];
                double dot = 0, energy = 1e-9;
                for (size_t n = 0; n < hop_; n += stride) {
                    dot += (double)m[n] * t[n];
                    energy += (double)m[n] * m[n];
                }
                return dot / std::sqrt(energy);
            };
            double top = -1e300;
            for (uint64_t c = lo; c <= hi; c += 2) {
                const double v = score(c, 2);
                if (v > top) { top = v; best = c; }
            }
            const uint64_t coarse = best;
            top = -1e300;
            for (uint64_t c = coarse > lo ? coarse - 1 : lo; c <= std::min(hi, coarse + 1); ++c) {
                const double v = score(c, 1);
                if (v > top) { top = v; best = c; }
            }
        }
        const float* x = &in_[(size_t)(best - base_) * 2];
        const size_t at = out.size();
        out.resize(at + hop_ * 2);
        for (size_t n = 0; n < hop_; ++n)
            for (int ch = 0; ch < 2; ++ch) {
                out[at + n * 2 + ch] = tail_[n * 2 + ch] + (primed_ ? window_[n] : 1.0f) * x[n * 2 + ch];
                tail_[n * 2 + ch] = window_[n + hop_] * x[(n + hop_) * 2 + ch];
            }
        prev_ = best;
        nominal_ += hop_ * tempo_;
        primed_ = true;
    }
    // Keep what the next search can reach and the previous segment's continuation.
    const double reach = std::floor(nominal_) - (double)search_;
    uint64_t keep = primed_ ? std::min<uint64_t>(prev_ + hop_, reach > 0 ? (uint64_t)reach : 0) : (uint64_t)std::floor(nominal_);
    keep = std::clamp<uint64_t>(keep, base_, base_ + in_.size() / 2);
    in_.erase(in_.begin(), in_.begin() + (ptrdiff_t)(keep - base_) * 2);
    base_ = keep;
}

// ---------------------------------------------------------------------------
// Loudness compensation. The shelves glide at most 0.05 dB per 32 frames, so a volume drag or a
// toggle never clicks; coefficients are redesigned only while they move.

float LoudnessContour::BassDb(float volume) {
    const float attenuation = volume > 0.001f ? std::min(60.0f, -20 * std::log10(volume)) : 60.0f;
    return std::clamp(attenuation * 0.35f, 0.0f, 12.0f);
}

float LoudnessContour::TrebleDb(float volume) {
    const float attenuation = volume > 0.001f ? std::min(60.0f, -20 * std::log10(volume)) : 60.0f;
    return std::clamp(attenuation * 0.1f, 0.0f, 4.0f);
}

void LoudnessContour::Configure(unsigned rate) {
    if (rate == rate_) return;
    rate_ = rate;
    bass_ = treble_ = {};
    bassDb_ = trebleDb_ = 0;
    active_ = false;
}

void LoudnessContour::Design() {
    // RBJ rather than DesignFilter: this runs every 32 frames while the shelves glide, and at most
    // 4 dB of treble shelf is too gentle for the bilinear squeeze to matter.
    Coefficients c = RbjDesign(EqFilterType::LowShelf, 2 * kPi * 150 / rate_, std::pow(10.0, bassDb_ / 40.0), 0.5);
    bass_.b0 = c[0]; bass_.b1 = c[1]; bass_.b2 = c[2]; bass_.a1 = c[3]; bass_.a2 = c[4];
    c = RbjDesign(EqFilterType::HighShelf, 2 * kPi * std::min(9000.0, rate_ * 0.45) / rate_, std::pow(10.0, trebleDb_ / 40.0), 0.5);
    treble_.b0 = c[0]; treble_.b1 = c[1]; treble_.b2 = c[2]; treble_.a1 = c[3]; treble_.a2 = c[4];
}

void LoudnessContour::Process(float* x, size_t frames, float volume, bool on) {
    const float bassTarget = on && volume < 1.0f ? BassDb(volume) : 0, trebleTarget = on && volume < 1.0f ? TrebleDb(volume) : 0;
    if (!rate_ || (!active_ && bassTarget == 0 && trebleTarget == 0)) return;
    active_ = true;
    FlushDenormals flush;
    for (size_t at = 0; at < frames; at += 32) {
        const float bass = Toward(bassDb_, bassTarget, 0.05f), treble = Toward(trebleDb_, trebleTarget, 0.05f);
        if (bass != bassDb_ || treble != trebleDb_) {
            bassDb_ = bass;
            trebleDb_ = treble;
            Design();
        }
        for (size_t n = at; n < std::min(frames, at + 32); ++n)
            for (int ch = 0; ch < 2; ++ch) {
                double v = x[n * 2 + ch];
                for (Shelf* f : {&bass_, &treble_}) {
                    const double y = f->b0 * v + f->z1[ch];
                    f->z1[ch] = f->b1 * v - f->a1 * y + f->z2[ch];
                    f->z2[ch] = f->b2 * v - f->a2 * y;
                    v = y;
                }
                x[n * 2 + ch] = (float)v;
            }
    }
    if (bassDb_ == 0 && trebleDb_ == 0 && bassTarget == 0 && trebleTarget == 0) {
        active_ = false;
        bass_ = treble_ = {};
    }
}

// ---------------------------------------------------------------------------
// Night mode. The detector follows the mean square of what plays, divided back to before the
// volume, with a 50 ms attack and 400 ms release. Below -24 dBFS the gain closes 60% of the gap
// (fading to nothing between -60 and -70 dBFS); above -16 dBFS it takes off half the excess. It
// rises at 3 dB/s and falls at 240 dB/s, so a loud entry is caught at once and a lift creeps in.

void NightCompressor::Configure(unsigned rate) {
    const double r = std::max(rate, 1u);
    attack_ = 1 - std::exp(-1 / (0.05 * r));
    release_ = 1 - std::exp(-1 / (0.4 * r));
    rise_ = (float)(3.0 * 16 / r);
    fall_ = (float)(240.0 * 16 / r);
}

void NightCompressor::Process(float* x, size_t frames, float volume, bool on) {
    if ((!active_ && !on) || rise_ == 0) return;
    active_ = true;
    FlushDenormals flush;
    const double inverse = 1.0 / std::max(volume, 1e-3f);
    for (size_t at = 0; at < frames; at += 16) {
        const size_t count = std::min<size_t>(16, frames - at);
        for (size_t n = at; n < at + count; ++n) {
            const double l = x[n * 2] * inverse, r = x[n * 2 + 1] * inverse, ms = (l * l + r * r) / 2;
            env_ += (ms - env_) * (ms > env_ ? attack_ : release_);
        }
        float target = 0;
        if (on) {
            const double level = 10 * std::log10(env_ + 1e-12);
            if (level < -24) target = (float)(std::min(12.0, (-24 - level) * 0.6) * std::clamp((level + 70) / 10, 0.0, 1.0));
            else if (level > -16) target = (float)std::max(-8.0, (-16 - level) * 0.5);
        }
        liftDb_ = Toward(liftDb_, target, target > liftDb_ ? rise_ : fall_);
        const float next = liftDb_ == 0 ? 1.0f : (float)std::pow(10.0, liftDb_ / 20.0);
        const float step = (next - gain_) / count;
        for (size_t n = at; n < at + count; ++n) {
            gain_ += step;
            x[n * 2] *= gain_;
            x[n * 2 + 1] *= gain_;
        }
        gain_ = next;
    }
    if (!on && liftDb_ == 0) {
        active_ = false;
        env_ = 0;
    }
}

// ---------------------------------------------------------------------------
// BS.1770 K-weighting, with coefficients derived for any sample rate (as in libebur128).

LoudnessMeter::LoudnessMeter(unsigned rate) {
    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = std::tan(kPi * f0 / rate);
    double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    shelf_ = {(Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
              2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    f0 = 38.13547087602444;
    Q = 0.5003270373238773;
    K = std::tan(kPi * f0 / rate);
    a0 = 1.0 + K / Q + K * K;
    highpass_ = {1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0};
    stepFrames_ = rate / 10;  // 100 ms steps; blocks are 4 steps (400 ms, 75% overlap)
}

// K-weights each sample (shelf then high-pass), then folds it into 400 ms blocks of 100 ms steps,
// so consecutive blocks overlap by 75%. peak_ is the meter's own sample peak, not the true peak.
void LoudnessMeter::Add(const float* x, size_t frames) {
    FlushDenormals flush;  // silence between tracks would otherwise slow the scan down
    for (size_t n = 0; n < frames; ++n) {
        double sum = 0;
        for (int ch = 0; ch < 2; ++ch) {
            float s = x[n * 2 + ch];
            peak_ = std::max(peak_, std::fabs(s));
            double y = highpass_.Run(shelf_.Run(s, ch), ch);
            sum += y * y;
        }
        stepSum_ += sum;
        if (++stepFill_ == stepFrames_) {
            recent_[stepCount_ % recent_.size()] = stepSum_ / stepFrames_;
            steps_[stepCount_++ % 4] = stepSum_ / stepFrames_;
            stepSum_ = 0;
            stepFill_ = 0;
            if (stepCount_ >= recent_.size()) {
                double window = 0;
                for (double s : recent_) window += s;
                shortTerm_.push_back((float)(window / recent_.size()));
            }
            if (stepCount_ >= 4) {
                size_t k = stepCount_;
                blocks_.push_back((steps_[(k - 1) % 4] + steps_[(k - 2) % 4] + steps_[(k - 3) % 4] + steps_[(k - 4) % 4]) / 4);
            }
        }
    }
}

// The BS.1770 double gate: drop blocks below -70 LUFS, then those below (mean - 10 LU), and take
// the loudness of what is left. Silence reads -inf. Public so an album gain can gate all tracks.
double LoudnessMeter::Gated(const std::vector<double>& blocks) {
    auto lufs = [](double ms) { return -0.691 + 10 * std::log10(ms); };
    const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sum = 0;
    size_t count = 0;
    for (double b : blocks)
        if (b > absGate) { sum += b; ++count; }
    if (!count) return -std::numeric_limits<double>::infinity();
    double relGate = sum / count * std::pow(10.0, -1.0);  // -10 LU
    sum = 0;
    count = 0;
    for (double b : blocks)
        if (b > absGate && b > relGate) { sum += b; ++count; }
    return count ? lufs(sum / count) : -std::numeric_limits<double>::infinity();
}

double LoudnessMeter::Integrated() const { return Gated(blocks_); }

// EBU Tech 3342: short-term values below -70 LUFS, then those more than 20 LU under the mean of
// the rest, are dropped (silence and fades); the 95th percentile of what is left is how loud the
// loud parts are.
double LoudnessMeter::HighLoudness() const {
    auto lufs = [](double ms) { return -0.691 + 10 * std::log10(ms); };
    const double absGate = std::pow(10.0, (-70.0 + 0.691) / 10.0);
    double sum = 0;
    size_t count = 0;
    for (float s : shortTerm_)
        if (s > absGate) { sum += s; ++count; }
    if (!count) return -std::numeric_limits<double>::infinity();
    const double relGate = sum / count * std::pow(10.0, -2.0);  // -20 LU
    std::vector<float> kept;
    for (float s : shortTerm_)
        if (s > absGate && s > relGate) kept.push_back(s);
    if (kept.empty()) return -std::numeric_limits<double>::infinity();
    const size_t at = std::min(kept.size() - 1, (size_t)(kept.size() * 0.95));
    std::nth_element(kept.begin(), kept.begin() + (ptrdiff_t)at, kept.end());
    return lufs(kept[at]);
}

// ---------------------------------------------------------------------------
// True peak: 4x polyphase windowed-sinc interpolation. Phase 0 is the sample itself; phases
// 1..3 estimate the waveform between samples. Each phase is normalised to unity gain so a DC
// signal reads its own value rather than a windowing error.

namespace {
double Sinc(double x) { return x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x); }
}

TruePeak::TruePeak() {
    // Phase 0 is the original sample, so the remaining phases must be quarter,
    // half and three-quarter samples from an integer delay, even with an even tap count.
    const double center = kTaps / 2 - 1;
    for (int p = 0; p < kPhases; ++p) {
        double sum = 0;
        for (int k = 0; k < kTaps; ++k) {
            const double x = (k - center) - p / (double)kPhases;
            const double window = 0.54 - 0.46 * std::cos(2.0 * kPi * k / (kTaps - 1));
            coeff_[p][k] = Sinc(x) * window;
            sum += coeff_[p][k];
        }
        if (sum != 0)
            for (int k = 0; k < kTaps; ++k) coeff_[p][k] /= sum;
    }
}

void TruePeak::Add(const float* stereo, size_t frames) {
    // Start from a steady signal rather than silence: a file does not begin with a step from
    // nothing, and it keeps the first samples from ringing a false peak into the result.
    if (!started_ && frames) {
        for (auto& sample : hist_)
            for (int ch = 0; ch < 2; ++ch) sample[ch] = stereo[ch];
        started_ = true;
    }
    const __m128d sign = _mm_set1_pd(-0.0);
    __m128d maximum = _mm_set1_pd(peak_);
    for (size_t n = 0; n < frames; ++n) {
        const __m128d sample = _mm_cvtps_pd(_mm_loadl_pi(_mm_setzero_ps(), (const __m64*)(stereo + n * 2)));
        _mm_storeu_pd(hist_[pos_], sample);
        _mm_storeu_pd(hist_[pos_ + kTaps], sample);
        maximum = _mm_max_pd(_mm_andnot_pd(sign, sample), maximum);
        __m128d sum1 = _mm_setzero_pd(), sum2 = _mm_setzero_pd(), sum3 = _mm_setzero_pd();
        for (int k = 0; k < kTaps; ++k) {
            // Each lane is one channel; each phase retains the scalar accumulation order.
            const __m128d history = _mm_loadu_pd(hist_[pos_ + k]);
            sum1 = _mm_add_pd(sum1, _mm_mul_pd(_mm_set1_pd(coeff_[1][k]), history));
            sum2 = _mm_add_pd(sum2, _mm_mul_pd(_mm_set1_pd(coeff_[2][k]), history));
            sum3 = _mm_add_pd(sum3, _mm_mul_pd(_mm_set1_pd(coeff_[3][k]), history));
        }
        maximum = _mm_max_pd(_mm_andnot_pd(sign, sum1), maximum);
        maximum = _mm_max_pd(_mm_andnot_pd(sign, sum2), maximum);
        maximum = _mm_max_pd(_mm_andnot_pd(sign, sum3), maximum);
        if (--pos_ < 0) pos_ = kTaps - 1;
    }
    peak_ = _mm_cvtsd_f64(_mm_max_pd(maximum, _mm_unpackhi_pd(maximum, maximum)));
}

void TruePeak::Reset() {
    for (auto& ch : hist_)
        for (double& v : ch) v = 0;
    pos_ = 0;
    peak_ = 0;
    started_ = false;
}

// ---------------------------------------------------------------------------
// Peak limiter. The sliding minimum of the gain each frame needs over the look-ahead, averaged
// twice, is the target: at a loud frame every value either average spans is at or below what that
// frame needs, so the limit holds exactly there, while the ramp into it spans the whole look-ahead.
// The minimum queue only holds frames that need gain below 1, so an empty queue reads as 1. Levels
// run two frames behind the input, which the between-samples estimate needs to see past the frame.

void PeakLimiter::SetHeadroom(float headroomDb) {
    ceiling_ = kCeiling * (float)std::pow(10.0, -(std::isfinite(headroomDb) ? std::clamp(headroomDb, 0.0f, 24.0f) : 0) / 20.0);
}

void PeakLimiter::Prime(float left, float right) {
    for (size_t i = 0; i + 1 < delay_.size(); i += 2) {
        delay_[i] = left;
        delay_[i + 1] = right;
    }
    for (auto& frame : history_) frame[0] = left, frame[1] = right;
}

void PeakLimiter::Average::Reset(size_t length) {
    values.assign(length, 1.0f);
    sum = (double)length;
    below = pos = 0;
}

float PeakLimiter::Average::Push(float value) {
    float& old = values[pos];
    if (++pos == values.size()) pos = 0;
    below += (value < 1.0f) - (old < 1.0f);
    sum += (double)value - old;
    old = value;
    if (!below) {
        sum = (double)values.size();  // no drift once everything is back at 1
        return 1.0f;
    }
    return (float)(sum / values.size());
}

// The level of the frame two behind the newest: its larger sample, or more where the waveform
// overshoots between it and either neighbour (4-point half-sample interpolation). Shifts `frame` in.
float PeakLimiter::Level(const float* frame) {
    float level = 0;
    for (int ch = 0; ch < 2; ++ch) {
        const float a = history_[0][ch], b = history_[1][ch], c = history_[2][ch], d = history_[3][ch], e = frame[ch];
        const float before = (9 * (b + c) - a - d) * 0.0625f, after = (9 * (c + d) - b - e) * 0.0625f;
        level = std::max({level, std::fabs(c), std::fabs(before), std::fabs(after)});
        history_[0][ch] = b;
        history_[1][ch] = c;
        history_[2][ch] = d;
        history_[3][ch] = e;
    }
    return level;
}

void PeakLimiter::Configure(unsigned rate, float headroomDb) {
    SetHeadroom(headroomDb);
    length_ = std::max<size_t>(16, (size_t)(std::max(rate, 1u) * kLookaheadSeconds));
    delay_.assign((length_ + 1) * 2, 0.0f);
    delayPos_ = 0;
    for (auto& frame : history_) frame[0] = frame[1] = 0;
    first_.Reset(length_ / 2);
    second_.Reset(length_ - length_ / 2 + 1);  // the two spans overlap one frame: together length_
    minQueue_.assign(length_ + 1, {1.0f, 0});
    queueHead_ = queueSize_ = 0;
    frame_ = 0;
    gain_ = 1;
    sustain_ = 0;
    const double r = std::max(rate, 1u);
    fastRelease_ = (float)(1.0 - std::exp(-1.0 / (kFastRelease * r)));
    slowRelease_ = (float)(1.0 - std::exp(-1.0 / (kSlowRelease * r)));
    sustainRate_ = (float)(1.0 - std::exp(-1.0 / (kSustainSeconds * r)));
}

void PeakLimiter::Process(float* x, size_t frames) {
    if (!length_) return;
    const size_t delayFrames = length_ + 1, cap = minQueue_.size();
    // Not limiting and nothing over full scale in this block: only the delay applies.
    if (gain_ == 1.0f && !first_.below && !second_.below) {
        float saved[4][2];
        std::memcpy(saved, history_, sizeof(saved));
        bool over = false;
        for (size_t i = 0; i < frames && !over; ++i) over = Level(x + i * 2) > ceiling_;
        if (!over) {
            for (size_t i = 0; i < frames; ++i) {
                float* slot = &delay_[delayPos_ * 2];
                std::swap(slot[0], x[i * 2]);
                std::swap(slot[1], x[i * 2 + 1]);
                if (++delayPos_ == delayFrames) delayPos_ = 0;
            }
            frame_ += frames;
            queueSize_ = 0;
            sustain_ = 0;
            return;
        }
        std::memcpy(history_, saved, sizeof(saved));
    }
    for (size_t i = 0; i < frames; ++i) {
        float* s = x + i * 2;
        const uint64_t f = frame_++;
        while (queueSize_ && minQueue_[queueHead_].second + length_ <= f) {
            queueHead_ = (queueHead_ + 1) % cap;
            --queueSize_;
        }
        const float peak = Level(s);
        if (peak > ceiling_) {
            const float need = ceiling_ / peak;
            while (queueSize_ && minQueue_[(queueHead_ + queueSize_ - 1) % cap].first >= need) --queueSize_;
            minQueue_[(queueHead_ + queueSize_++) % cap] = {need, f};
        }
        const float h = queueSize_ ? minQueue_[queueHead_].first : 1.0f;
        const float target = second_.Push(first_.Push(h));
        sustain_ += ((target < 1.0f ? 1.0f : 0.0f) - sustain_) * sustainRate_;
        const float release = fastRelease_ + (slowRelease_ - fastRelease_) * sustain_;
        gain_ = target < gain_ ? target : gain_ + (target - gain_) * release;
        if (target == 1.0f && gain_ > 0.99999f) gain_ = 1.0f;
        float* slot = &delay_[delayPos_ * 2];
        const float l = slot[0], r = slot[1];
        slot[0] = s[0];
        slot[1] = s[1];
        if (++delayPos_ == delayFrames) delayPos_ = 0;
        s[0] = l * gain_;
        s[1] = r * gain_;
    }
}
