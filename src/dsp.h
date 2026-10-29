// Signal processing: graphic/parametric EQ, headphone crossfeed, the output limiter, and EBU R128
// and true-peak measurement (used by the ReplayGain scanner). Works on interleaved stereo float; filter gains are dB and loudness is LUFS.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include <string>
#include <string_view>

constexpr int kEqBands = 10;
constexpr float kEqFrequencies[kEqBands] = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
constexpr int kParametricBands = 20;
constexpr float kGraphicQ = 1.41f;  // the graphic bands' width: about one octave

// Display-only analysis of 2048 interleaved stereo frames, on a logarithmic 40 Hz..20 kHz axis.
// Values are 0..1 over 72 dB, tilted 3 dB/octave. Does not change the playback samples.
void SpectrumLevels(const float* stereo, unsigned rate, float* levels, size_t bands);
struct AudioVisuals {
    static constexpr size_t kFrames = 2048, kBands = 40;
    std::vector<float> wave = std::vector<float>(kFrames * 2);
    std::array<float, kBands> levels{};
    std::array<float, kBands> peaks{}, peakAge{};
    double time = -1;  // output clock at the middle of wave; negative for untimed test input
    // Foobar's scope spans 300 ms at 100% amplitude. Keep the FFT's shorter window at the end.
    size_t Prepare(unsigned rate);
    std::vector<float> Curve(size_t pixels) const;
    bool Update(bool active, unsigned rate, double elapsed = 1.0 / 60);
    bool Moving() const;
};
// The live seek bar and fountain share a fast note attack and a slower fall. Feed only newly
// audible PCM: the response then holds on pause and is independent of the display's frame rate.
// This is display state only; it never changes playback samples or the whole-track peaks.
class WaveformMotion {
public:
    void Add(const float* stereo, size_t frames, unsigned rate);
    float Level() const;
    void Shape(uint8_t* peaks, size_t buckets) const;

private:
    unsigned rate_ = 0, block_ = 0, count_ = 0;
    double power_ = 0;
    float baseline_ = 0, envelope_ = 0;
    float attack_ = 0, release_ = 0, follow_ = 0;
};

enum class EqFilterType : uint8_t { Peak, LowShelf, HighShelf };
struct EqFilter {
    EqFilterType type = EqFilterType::Peak;
    float frequency = 1000, gain = 0, q = 0.707f;
    bool enabled = true;
    bool operator==(const EqFilter&) const = default;
};

struct EqSettings {
    bool enabled = false;
    float preamp = 0;  // dB
    std::array<float, kEqBands> gains{};  // dB, -12..+12
    bool parametric = false, crossfeed = false;
    int filterCount = 0;
    std::array<EqFilter, kParametricBands> filters{};
    bool operator==(const EqSettings&) const = default;
};

// AutoEq / Equalizer APO parametric text. Failure leaves settings unchanged; error names the line.
bool ParseAutoEq(std::string_view text, EqSettings& settings, std::wstring& error);
std::string FormatAutoEq(const EqSettings& settings);

struct EqPreset {
    const wchar_t* name;
    std::array<float, kEqBands> gains;
};
const std::vector<EqPreset>& EqPresets();
// The peaking-filter gains (at kEqFrequencies, kGraphicQ) that make the combined curve meet every
// graphic slider at its band center, compensating for neighbouring bands' overlap.
std::array<float, kEqBands> GraphicFilterGains(const std::array<float, kEqBands>& sliders, unsigned rate);

// Graphic or parametric biquads (matched to the analog curve, see DesignFilter in dsp.cpp), followed
// by optional bs2b headphone crossfeed. A change made
// while playing crossfades from the old curve to the new one over kFadeSeconds, so dragging a
// slider, switching presets or turning the EQ off never clicks; changes arriving during a fade
// are applied when it ends. Active() stays true until a fade to bypass has finished.
class Equalizer {
public:
    void Configure(const EqSettings& settings, unsigned rate);
    void Process(float* stereo, size_t frames);
    bool Active() const { return active_; }
    // Highest steady-state gain of the complete curve, including preamp, rounded up to 0.05 dB.
    // Used off the render thread to reserve ReplayGain headroom; the output limiter catches transients.
    double PeakGainDb() const;
    // The filters' response at `hz` in dB, without the preamp or crossfeed (for drawing the curve).
    double FilterDb(double hz) const;

private:
    static constexpr double kFadeSeconds = 0.015;
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2] = {}, z2[2] = {};
    };
    // One complete curve with its filter state: preamp, biquads, then crossfeed.
    struct Chain {
        std::array<Biquad, kParametricBands> bands{};
        std::array<bool, kParametricBands> used{};
        std::array<double, kParametricBands> centers{};  // radians/sample, for narrow-peak search seeds
        double preampGain = 1;
        bool enabled = false, crossfeed = false;
        double loA0 = 0, loB1 = 0, hiA0 = 0, hiA1 = 0, hiB1 = 0, crossGain = 1;  // bs2b coefficients
        double lo[2]{}, hi[2]{}, asis[2]{};
        void Run(double& left, double& right);
    };
    void Apply(const EqSettings& settings, bool fade);
    Chain chain_, previous_;
    EqSettings settings_, pending_;
    bool pendingSet_ = false, active_ = false;
    size_t fade_ = 0, fadeLength_ = 0;
    unsigned rate_ = 0;
};

enum class ReverbPreset : uint8_t { Off, Room, Hall, Plate, Count };
struct ReverbSettings {
    ReverbPreset preset = ReverbPreset::Off;
    float mix = 0.4f;  // wet level added to the unchanged dry signal, 0..1 (1: as loud as the dry)
    bool operator==(const ReverbSettings&) const = default;
};

// Feedback-delay-network reverb: pre-delay, then eight modulated delay lines mixed by a Householder
// matrix, with a damping low-pass and the decay gain in each loop. Freeverb's comb filters would be
// smaller but ring metallic. Below ~150 Hz is cut from the send, so the low end stays dry and clear.
// Turning it off fades the send, not the output: the tail rings out. Clear() (a seek or stop) and a
// preset change duck only the wet output for a few ms, so neither clicks. Active() holds until the
// tail has decayed below audibility; in silence it then sleeps, and wakes on the next sound.
class Reverb {
public:
    void Configure(const ReverbSettings& settings, unsigned rate);
    void Process(float* stereo, size_t frames);
    void Clear();
    bool Active() const { return active_; }

private:
    static constexpr int kLines = 8;
    void Build(ReverbPreset preset);
    ReverbSettings settings_;
    ReverbPreset built_ = ReverbPreset::Off;
    unsigned rate_ = 0;
    std::vector<float> lines_, pre_;  // kLines buffers of size_ each, then the pre-delay
    size_t size_ = 0, prePos_ = 0, preDelay_ = 0, pos_ = 0;
    std::array<float, kLines> length_{}, depth_{}, gain_{}, low_{}, cos_{}, sin_{}, rotCos_{}, rotSin_{};
    float damp_ = 0, outScale_ = 0;
    double hp_[5]{}, hpZ_[2]{};
    float send_ = 0, wet_ = 1, mix_ = 0, step_ = 0;
    bool active_ = false, reset_ = false;
    size_t quietFrames_ = 0;
};

// Tempo without a pitch change (WSOLA): 40 ms Hann-windowed segments overlap-added at 20 ms, each
// taken from within ±10 ms of where the tempo puts it, at the offset whose start best matches what
// the previous segment would have continued with. No FFT. Clean on speech and acceptable on music
// from about 0.75x to 1.5x. Interleaved stereo in and out; tempo 1 is not Active (the engine skips it).
class TimeStretch {
public:
    void Configure(unsigned rate, double tempo);  // also clears the state
    bool Active() const { return tempo_ != 1.0; }
    double Tempo() const { return tempo_; }
    void Reset();
    void Process(const float* in, size_t frames, std::vector<float>& out);  // replaces out
    void Flush(std::vector<float>& out);  // end of stream: appends what is held, then resets
    double PendingOutput() const;  // output frames still owed for input already pushed

private:
    void Run(std::vector<float>& out);
    double tempo_ = 1.0, nominal_ = 0;  // nominal_: input frame where the next segment belongs
    size_t hop_ = 0, search_ = 0;
    uint64_t base_ = 0, prev_ = 0;     // input frame of in_[0], and of the last segment used
    std::vector<float> in_, window_, tail_, mono_;
    bool primed_ = false;
};

// Loudness compensation tied to the volume (after the ISO 226 equal-loudness contours): ears lose
// bass, and a little treble, faster than the midrange as the level drops, so shelves at 150 Hz and
// 9 kHz rise as the volume falls (about 4 dB of bass at half volume on the slider). The target
// boost is below the attenuation, but a volume change can briefly outrun the shelves' glide; the
// output limiter covers that transition. At full volume it becomes inactive after the glide.
class LoudnessContour {
public:
    void Configure(unsigned rate);
    void Process(float* stereo, size_t frames, float volume, bool on);
    bool Active() const { return active_; }
    static float BassDb(float volume);   // the target boosts for a linear volume
    static float TrebleDb(float volume);

private:
    struct Shelf {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2] = {}, z2[2] = {};
    };
    void Design();
    Shelf bass_, treble_;
    float bassDb_ = 0, trebleDb_ = 0;
    unsigned rate_ = 0;
    bool active_ = false;
};

// Night mode: a slow two-way compressor for listening quietly. Passages below about -24 dBFS (as
// played, before the volume) are lifted by up to 12 dB and loud ones above -16 dBFS softened by up
// to 8 dB. Gain rises over seconds and falls within tens of ms, so a loud entry is caught at once;
// near-silence is left alone so noise isn't raised. The output limiter catches what a lifted
// transient overshoots.
class NightCompressor {
public:
    void Configure(unsigned rate);
    void Process(float* stereo, size_t frames, float volume, bool on);
    bool Active() const { return active_; }
    float LiftDb() const { return liftDb_; }  // negative while softening

private:
    double env_ = 0, attack_ = 0, release_ = 0;
    float liftDb_ = 0, gain_ = 1, rise_ = 0, fall_ = 0;
    bool active_ = false;
};

// ITU-R BS.1770 / EBU R128 integrated loudness, for the ReplayGain scanner. It also keeps the
// short-term loudness (3 s windows, 10 per second) for HighLoudness.
class LoudnessMeter {
public:
    explicit LoudnessMeter(unsigned rate);
    void Add(const float* stereo, size_t frames);
    double Integrated() const;  // LUFS, -inf for silence
    // How loud the loud parts are: the 95th percentile of the gated short-term loudness (the top
    // of the EBU Tech 3342 loudness range), LUFS; -inf when the track is under 3 s or silent.
    double HighLoudness() const;
    float Peak() const { return peak_; }
    const std::vector<double>& Blocks() const { return blocks_; }  // mean-square per 400 ms block
    std::vector<double> TakeBlocks() { return std::move(blocks_); }

    static double Gated(const std::vector<double>& blocks);  // for album gain over many tracks

private:
    struct Filter {
        double b0, b1, b2, a1, a2;
        double z1[2] = {}, z2[2] = {};
        double Run(double x, int ch) {
            double y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            return y;
        }
    };
    Filter shelf_{}, highpass_{};
    size_t stepFrames_ = 0, stepFill_ = 0;
    double stepSum_ = 0;
    std::array<double, 4> steps_{};  // only the last 400 ms can contribute to the next block
    std::array<double, 30> recent_{};  // the last 3 s of steps, for short-term loudness
    size_t stepCount_ = 0;
    std::vector<double> blocks_;
    std::vector<float> shortTerm_;  // mean-square per 3 s window, every 100 ms
    float peak_ = 0;
};

// 4x oversampled true-peak detector (the ITU-R BS.1770-4 Annex 2 method: reconstruct the
// waveform between samples and take the largest value, including the samples themselves).
// A windowed-sinc polyphase interpolator is used, so the result is conservative for
// inter-sample peaks without over-reading typical material.
class TruePeak {
public:
    TruePeak();
    void Add(const float* stereo, size_t frames);
    float Peak() const { return (float)peak_; }
    void Reset();

private:
    static constexpr int kTaps = 24;    // per phase
    static constexpr int kPhases = 4;   // 4x oversampling
    double coeff_[kPhases][kTaps]{};
    // Mirrored newest-first stereo history makes each FIR window contiguous.
    double hist_[kTaps * 2][2]{};
    int pos_ = 0;
    double peak_ = 0;
    bool started_ = false;   // history filled from the first sample, so a file doesn't ring at t=0
};

constexpr double kReplayGainReference = -18.0;  // LUFS (ReplayGain 2.0)

// Look-ahead peak limiter for the output, in place of clipping: keeps interleaved stereo within
// full scale without the distortion a hard clip adds. Each frame's level counts the overshoot
// between samples too (a 2x true-peak estimate), and its gain is the lowest any frame in the next
// kLookahead needs (a sliding minimum), smoothed by two moving averages of half that length, so the
// gain eases in along an S-curve, has fully arrived by the time the loud frame comes out, and never
// steps; afterwards it recovers with a smooth release. Output is delayed by Latency() frames.
// While nothing is over full scale the samples pass through bit-exact (only delayed).
class PeakLimiter {
public:
    void Configure(unsigned rate, float headroomDb = 0);  // also clears the state
    // Lowers the ceiling by headroomDb without clearing the look-ahead, so it can change mid-song.
    void SetHeadroom(float headroomDb);
    // Fills the look-ahead with one frame. Engaging the limiter mid-song otherwise inserts
    // Latency() frames of silence (a click); holding the last frame out keeps the waveform joined.
    void Prime(float left, float right);
    void Process(float* stereo, size_t frames);
    float Reduction() const { return gain_; }  // the gain now applied, 1 = not limiting
    size_t Latency() const { return length_ ? length_ + 1 : 0; }  // frames

private:
    // Hold a fraction of a dB below full scale: the true-peak estimate catches most of the overshoot
    // between samples, and this little headroom covers what it misses, before the hard clip at the
    // output (where it would distort, voices most of all).
    static constexpr float kCeiling = 0.96605f;  // -0.3 dBFS
    float ceiling_ = kCeiling;
    // 3 ms is long enough that the gain change doesn't buzz on bass and short enough not to duck
    // ahead of a drum hit audibly. Release is fast after a brief peak and slows when limiting goes
    // on (a long loud passage), so sustained material, voices especially, doesn't pump.
    static constexpr double kLookaheadSeconds = 0.003, kFastRelease = 0.06, kSlowRelease = 0.5, kSustainSeconds = 0.3;
    // A moving average of gains that reads exactly 1 whenever none of its values is below 1.
    struct Average {
        std::vector<float> values;
        double sum = 0;
        size_t below = 0, pos = 0;
        void Reset(size_t length);
        float Push(float value);
    };
    float Level(const float* frame);
    size_t length_ = 0;                     // look-ahead frames
    std::vector<float> delay_;              // the last length_ + 1 input frames
    size_t delayPos_ = 0;
    float history_[4][2] = {};              // the newest input frames, for the between-samples estimate
    Average first_, second_;
    std::vector<std::pair<float, uint64_t>> minQueue_;  // monotonic queue (ring) for the minimum
    size_t queueHead_ = 0, queueSize_ = 0;
    uint64_t frame_ = 0;
    float gain_ = 1, fastRelease_ = 0, slowRelease_ = 0;
    float sustain_ = 0, sustainRate_ = 0;  // how much of the recent past was limited, 0..1
};
