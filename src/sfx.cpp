// Sound effect synthesis (see sfx.h). Everything is built from three things: decaying sine
// partials (mallets, plinks, ticks, the stamp), filtered noise bursts (paper, cards, dice) and a
// triangle-wave whistle for the cardinal.
#include "sfx.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Major pentatonic from C5 (degree 0); negative degrees go below.
double Degree(int d) {
    static const int kSemitones[] = {0, 2, 4, 7, 9};
    int octave = d >= 0 ? d / 5 : -((4 - d) / 5);
    return 523.2511 * std::pow(2.0, octave + kSemitones[d - octave * 5] / 12.0);
}

struct Tone {
    double freq = 440;
    double to = 0, glide = 0.03;      // exponential pitch glide to `to` over `glide` s (0 = none)
    double amp = 1;
    double attack = 0.002, decay = 0.1;  // raised-cosine attack, then exponential decay (time constant)
    double fmIndex = 0, fmDecay = 0.02;  // FM brightness (modulator at the same pitch) that fades out
};

// A mono buffer one sound is drawn into. `detuneOctaves` bounds the random pitch offset applied
// to every tone, so two plays of the same sound differ without drifting out of tune.
class Canvas {
public:
    Canvas(unsigned rate, uint32_t seed, double detuneOctaves) : rate_(rate), rng_(seed) {
        detune_ = std::pow(2.0, Rand(-detuneOctaves, detuneOctaves));
    }

    double Rand(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng_); }
    std::vector<float>& Samples() { return s_; }

    void Add(double t0, const Tone& t) {
        float* out = Span(t0, t.attack + t.decay * 7);
        size_t n = Frames(t.attack + t.decay * 7);
        double phase = 0, mod = 0, f0 = t.freq * detune_, f1 = (t.to > 0 ? t.to : t.freq) * detune_;
        for (size_t i = 0; i < n; ++i) {
            double time = i / (double)rate_;
            double f = t.to > 0 ? f0 * std::pow(f1 / f0, std::min(time / t.glide, 1.0)) : f0;
            double index = t.fmIndex * std::exp(-time / t.fmDecay);
            out[i] += (float)(t.amp * Envelope(time, t.attack, t.decay) * std::sin(phase + index * std::sin(mod)));
            phase += 2 * kPi * f / rate_;
            mod += 2 * kPi * f / rate_;
        }
    }

    // White noise through a band (two one-pole high-passes at `low`, two low-passes at `high`).
    void Noise(double t0, double amp, double attack, double decay, double low, double high) {
        float* out = Span(t0, attack + decay * 7);
        size_t n = Frames(attack + decay * 7);
        double a = 1 - std::exp(-2 * kPi * low / rate_), b = 1 - std::exp(-2 * kPi * high / rate_);
        double l1 = 0, l2 = 0, h1 = 0, h2 = 0;
        for (size_t i = 0; i < n; ++i) {
            double x = Rand(-1, 1);
            l1 += a * (x - l1);
            double hp = x - l1;
            l2 += a * (hp - l2);
            hp -= l2;
            h1 += b * (hp - h1);
            h2 += b * (h1 - h2);
            out[i] += (float)(amp * 2 * Envelope(i / (double)rate_, attack, decay) * h2);
        }
    }

    // Instruments.
    void Mallet(double t, double f, double amp, bool ducked) {
        Add(t, {.freq = f, .amp = amp, .attack = 0.002, .decay = ducked ? 0.05 : 0.11});
        Add(t, {.freq = f * 3.93, .amp = amp * 0.12, .attack = 0.001, .decay = 0.02});  // the bar's first overtone
        Noise(t, amp * 0.1, 0.0005, 0.003, 1500, 5000);                                // the mallet hitting it
    }
    void Plink(double t, double f, double amp, bool ducked) {
        Add(t, {.freq = f, .amp = amp, .attack = 0.001, .decay = ducked ? 0.06 : 0.14, .fmIndex = 1.6, .fmDecay = 0.015});
    }
    void Tick(double t, double amp, double low = 1050, double high = 2500) {
        Add(t, {.freq = low * Rand(0.93, 1.07), .amp = amp, .attack = 0.0005, .decay = 0.011});
        Add(t, {.freq = high * Rand(0.93, 1.07), .amp = amp * 0.5, .attack = 0.0005, .decay = 0.005});
        Noise(t, amp * 0.3, 0.0003, 0.002, 800, 5000);
    }

    // The cardinal. Its song is a run of pure whistled slurs; each note is a pitch contour in the
    // bird's own range (about 1.8 to 4 kHz), here an octave and a bit lower and "pixelated": the
    // pitch holds for a 1/60 s frame and snaps to the nearest semitone, so the slurs come out as
    // tiny staircases on a soft triangle wave.
    struct Point {
        double t, f;
    };
    void Whistle(double t0, std::initializer_list<Point> contour, double amp, double transpose = 1) {
        const Point* p = contour.begin();
        size_t count = contour.size();
        double dur = p[count - 1].t;
        float* out = Span(t0, dur);
        size_t n = Frames(dur);
        double phase = 0.25, lp = 0, a = 1 - std::exp(-2 * kPi * 4500 / rate_);
        for (size_t i = 0; i < n; ++i) {
            double time = i / (double)rate_;
            double frame = (std::floor(time * 60) + 0.5) / 60;
            size_t k = 1;
            while (k + 1 < count && p[k].t < frame) ++k;
            double u = std::clamp((frame - p[k - 1].t) / (p[k].t - p[k - 1].t), 0.0, 1.0);
            double bird = p[k - 1].f * std::pow(p[k].f / p[k - 1].f, u);
            double semis = std::round(12 * std::log2(bird / 2.4 * transpose / 440));
            double f = 440 * std::pow(2.0, semis / 12) * detune_;
            double tri = 1 - 4 * std::abs(phase - std::floor(phase) - 0.5);
            lp += a * (tri - lp);
            double in = std::min(1.0, time / 0.012), outEnv = std::min(1.0, (dur - time) / 0.02);
            double env = (0.5 - 0.5 * std::cos(kPi * in)) * (0.5 - 0.5 * std::cos(kPi * outEnv)) * (0.8 + 0.2 * std::sin(kPi * time / dur));
            out[i] += (float)(amp * env * lp);
            phase += f / rate_;
        }
    }
    void Cheer(double t, double amp, double transpose = 1) {
        Whistle(t, {{0, 4000}, {0.07, 2900}, {0.2, 1900}}, amp, transpose);
    }
    void WhatCheer(double t, double amp) {
        Whistle(t, {{0, 1900}, {0.08, 2700}, {0.12, 3900}}, amp * 0.8);
        Whistle(t + 0.14, {{0, 4100}, {0.1, 2900}, {0.32, 1800}}, amp);
    }
    void Birdy(double t, double amp) { Whistle(t, {{0, 3600}, {0.06, 2400}, {0.1, 2900}}, amp); }

    // Scales the sound so its loudest 50 ms reach `db` RMS (equal loudness for very different
    // sounds; the peak stays below 0.95), trims the silent tail and fades the last 3 ms.
    void Finish(double db) {
        size_t window = Frames(0.05);
        double loudest = 0, sum = 0;
        float peak = 0;
        for (size_t i = 0; i < s_.size(); ++i) {
            sum += (double)s_[i] * s_[i];
            if (i >= window) sum -= (double)s_[i - window] * s_[i - window];
            loudest = std::max(loudest, sum);
            peak = std::max(peak, std::abs(s_[i]));
        }
        if (peak <= 0) return;
        double gain = std::min(std::pow(10.0, db / 20) / std::sqrt(loudest / window), 0.95 / peak);
        for (float& v : s_) v *= (float)gain;
        size_t end = s_.size();
        while (end > 0 && std::abs(s_[end - 1]) < 3e-4f) --end;
        s_.resize(end);
        size_t fade = std::min(end, Frames(0.003));
        for (size_t i = 0; i < fade; ++i) s_[end - 1 - i] *= (float)(i / (double)fade);
    }

private:
    size_t Frames(double seconds) const { return (size_t)(seconds * rate_); }
    float* Span(double t0, double dur) {
        size_t start = Frames(t0), end = start + Frames(dur);
        if (s_.size() < end) s_.resize(end);
        return s_.data() + start;
    }
    static double Envelope(double t, double attack, double decay) {
        if (t < attack) return 0.5 - 0.5 * std::cos(kPi * t / attack);
        return std::exp(-(t - attack) / decay);
    }

    unsigned rate_;
    std::mt19937 rng_;
    double detune_ = 1;
    std::vector<float> s_;
};

}  // namespace

namespace sfx {

std::vector<float> Render(Sfx sound, int arg, unsigned rate, uint32_t seed, bool ducked) {
    // Sounds whose notes walk the scale (the rating run, the queue) stay within a few cents so
    // the steps keep their intervals from one press to the next; the rest drift up to ~40 cents.
    bool scale = sound == Sfx::Rate || sound == Sfx::QueueAdd || sound == Sfx::PlayNext || sound == Sfx::QueueClear;
    Canvas c(rate, seed, scale ? 0.006 : 0.035);
    double level = -12;  // dB RMS of the loudest 50 ms; higher, brighter sounds sit a little lower
    switch (sound) {
        case Sfx::Rate: {
            int halves = std::clamp(arg, 1, 10), full = halves / 2;
            for (int i = 0; i < full; ++i)
                c.Mallet(i * 0.045 + c.Rand(0, 0.004), Degree(i), (i == full - 1 ? 1.0 : 0.75) * c.Rand(0.9, 1.0), ducked);
            if (halves % 2) c.Mallet(full * 0.045, Degree(full), 0.35, true);  // the half star: a grace note
            if (halves == 10) {
                if (ducked) c.Mallet(5 * 0.045, Degree(5), 0.7, true);
                else c.Whistle(0.27, {{0, 3800}, {0.05, 2800}, {0.13, 2000}}, 0.45);
            }
            break;
        }
        case Sfx::Unrate:
            c.Mallet(0, Degree(1), 0.8, ducked);
            c.Mallet(0.06, Degree(-2), 0.7, ducked);
            break;
        case Sfx::QueueAdd: c.Plink(0, Degree(std::clamp(arg - 1, 0, 9)), 1, ducked); break;
        case Sfx::PlayNext:
            c.Plink(0, Degree(2), 0.8, ducked);
            c.Plink(0.05, Degree(4), 1, ducked);
            break;
        case Sfx::QueueClear:
            for (int i = 0; i < 5; ++i) c.Plink(i * 0.028, Degree(4 - i), 1 - i * 0.1, true);
            break;
        case Sfx::TagsSaved:
            c.Add(0, {.freq = 160, .to = 90, .glide = 0.04, .amp = 1, .attack = 0.001, .decay = 0.045});
            c.Add(0, {.freq = 430, .amp = 0.4, .attack = 0.001, .decay = 0.018});
            c.Noise(0, 0.8, 0.0005, 0.012, 300, 2500);
            level = -11;
            break;
        case Sfx::Organized:
        case Sfx::Unorganized: {
            double t = 0, gap = 0.03;
            for (int i = 0; i < 11; ++i) {
                c.Noise(t, c.Rand(0.5, 0.9), 0.0003, 0.0025, 1000, 3500);
                c.Add(t, {.freq = c.Rand(2200, 3200), .amp = 0.15, .attack = 0.0003, .decay = 0.003});
                t += gap;
                gap *= 0.93;
            }
            c.Add(t + 0.01, {.freq = 620 * c.Rand(0.95, 1.05), .amp = 0.7, .attack = 0.001, .decay = 0.03});  // the deck squared up
            c.Noise(t + 0.01, 0.4, 0.0005, 0.008, 200, 2500);
            if (sound == Sfx::Unorganized) std::reverse(c.Samples().begin(), c.Samples().end());
            break;
        }
        case Sfx::Oops:
            c.Add(0, {.freq = Degree(1), .amp = 0.8, .attack = 0.003, .decay = 0.06, .fmIndex = 0.5});
            c.Add(0.11, {.freq = Degree(-1), .amp = 0.8, .attack = 0.003, .decay = 0.09, .fmIndex = 0.5});
            break;
        case Sfx::NewFiles:
            if (ducked) {
                c.Tick(0, 0.8);
                c.Tick(0.11, 0.6);
            } else {
                c.Cheer(0, 1);
                c.Cheer(0.3, 0.9, 0.97);
                level = -14;
            }
            break;
        case Sfx::Dice:
            for (int die = 0; die < 2; ++die) {
                double t = die * 0.018, gap = 0.075, amp = die ? 0.7 : 0.9;
                for (int bounce = 0; bounce < 6; ++bounce) {
                    c.Tick(t, amp, c.Rand(1600, 2400), c.Rand(3200, 4200));
                    t += gap * c.Rand(0.9, 1.1);
                    gap *= 0.68;
                    amp *= 0.72;
                }
            }
            break;
        case Sfx::Drop: {
            double f = std::max(220.0, 420 / (1 + 0.3 * std::log2(std::max(arg, 1))));
            c.Add(0, {.freq = f, .to = f * 2.8, .glide = 0.035, .amp = 1, .attack = 0.002, .decay = 0.035});
            level = -11;
            break;
        }
        case Sfx::Sparkle:
            for (int i = 0; i < 3; ++i)
                c.Add(i * 0.06, {.freq = Degree(7 + i), .amp = 0.5, .attack = 0.002, .decay = ducked ? 0.08 : 0.16, .fmIndex = 0.6});
            level = -15;
            break;
        case Sfx::Milestone:
            c.WhatCheer(0, 1);
            level = -14;
            break;
        case Sfx::CardinalSong:
            c.WhatCheer(0, 1);
            c.WhatCheer(0.62, 0.95);
            for (int i = 0; i < 4; ++i) c.Birdy(1.25 + i * 0.135 - i * i * 0.004, 0.85);
            level = -14;
            break;
        case Sfx::Count: break;
    }
    c.Finish(level);
    return std::move(c.Samples());
}

}  // namespace sfx
