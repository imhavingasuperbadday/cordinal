// Waveform peaks for the seek bar: each track is reduced to kPeakBuckets loudness values.
//
// Peaks are computed on one low-priority worker thread and cached in memory (a few recent
// results) and on disk under %LOCALAPPDATA%\cordinal\peaks, keyed on the file's path, size and
// time and on the range, so an edited file gets new peaks. The ReplayGain scanner stores peaks
// while it decodes anyway, so a scan leaves every track's waveform and moodbar ready.
#pragma once
#include <windows.h>

#include "theme_palette.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A fixed count per track, so the seek bar's resolution doesn't depend on the track's length.
constexpr int kPeakBuckets = 600;
// Cached peaks and the live waveform's texture use this curve to retain quiet detail. The live
// display's overall height follows its separate note envelope (WaveformMotion in dsp.h).
inline uint8_t CompandPeak(float linear) {
    if (!(linear > 0)) return 0;
    return (uint8_t)std::min(255.0f, std::sqrt(std::min(linear, 1.0f)) * 255.0f + 0.5f);
}
enum MoodBand { kMoodBass, kMoodMid, kMoodTreble, kMoodBands };
// Per bucket: the largest |sample| over both channels, and the RMS level of both channels, each
// sqrt-companded to 0..255 (0 = silence). The seek bar draws the RMS as the waveform's solid body
// and the peaks as its fainter edge, so a loud, dense master still shows its shape.
// The moodbar: per bucket, the energy of the bass (below about 250 Hz), the mids and the treble
// (above about 4 kHz), as log energy on one scale for the whole track with its loudest few percent
// clipped, so a single crash doesn't wash out the rest (0 = silence or far below the track).
struct Peaks {
    uint8_t v[kPeakBuckets];
    uint8_t rms[kPeakBuckets];
    uint8_t mood[kMoodBands][kPeakBuckets];
};

// The whole-song seek bar's looks (Preferences, "Seek bar", one up after Progress bar), in the order
// settings store them:
// the plain waveform, the moodbar as a flat stripe, and the waveform's columns in mood colours.
// The live waveform takes the same style, coloured by the audio being heard.
enum SeekStyle { kSeekWaveform, kSeekMoodbar, kSeekMoodWaveform, kSeekStyles };
// The moodbar's colours (Preferences, "Moodbar colours"): turned from the base colour (the theme's
// accent, or the chosen Waveform colour), the full colour wheel, the bands themselves as red (bass),
// green (mids) and blue (treble), red to yellow, violet to teal, blue to orange through grey (told
// apart with the common colour-vision deficiencies), or the base colour alone, standing out more
// from the background for treble.
enum MoodColors {
    kMoodTheme, kMoodVivid, kMoodSpectrum, kMoodWarm, kMoodCool, kMoodColorBlind, kMoodSingle, kMoodColorSchemes
};

// The plain waveform's colour (Preferences, "Waveform colour"): the theme's, or a named hue. It is
// also the base the moodbar's Theme, Vivid and Single colour schemes turn from.
enum WaveColors {
    kWaveTheme, kWaveRed, kWaveOrange, kWaveYellow, kWaveGreen, kWaveTeal, kWaveBlue, kWavePurple, kWavePink,
    kWaveGrey, kWaveColorChoices
};
// The chosen colour, its lightness moved until it stands out from `background` as much as an accent
// must (3:1); kNoRgb for kWaveTheme or an unknown choice.
Rgb WaveColorOn(int choice, Rgb background);
// The unplayed part in the waveform's own colour: faded most of the way into the background, so the
// playhead's split stays plain.
Rgb FadedWaveColor(Rgb played, Rgb background);
// What the Unplayed part setting offers.
enum WaveRestColors { kWaveRestTheme, kWaveRestFaded, kWaveRestChoices };

// Splits a mono mix of the audio into the three bands with cascaded one-pole filters: cheap and
// smooth, with no FFT. Primes itself on the first sample so a short read starts settled.
struct MoodSplit {
    void Begin(unsigned rate);
    // Adds the bass, mid and treble energy (sum of squares) of `frames` stereo frames to `sums`.
    void Add(const float* stereo, size_t frames, double sums[kMoodBands]);
    float a = 0, b = 0;  // the 250 Hz and 4 kHz coefficients
    // Each band's two stages: a low-pass below 250 Hz; a high-pass above 250 Hz then a low-pass
    // below 4 kHz; a high-pass above 4 kHz. A high-pass stage keeps its low-pass and subtracts it.
    float bass[2] = {}, midHigh[2] = {}, midLow[2] = {}, treble[2] = {};
    bool primed = false;
};

// Fills `out.mood` from per-bucket band energies (mean squares, kMoodBands per bucket; a negative
// value marks a bucket not decoded yet, left 0).
void FillMood(const double* energy, Peaks& out);

// The live moodbar's scale. The heard audio's band energies (Engine::WaveformBands) go on a ceiling
// and a balance that follow the music over a few seconds, so the colours don't flicker frame to
// frame. Fills the first `buckets` buckets of `out.mood` and returns the center for MoodOf.
class LiveMood {
public:
    float Scale(const float* energy, int buckets, Peaks& out);

private:
    double ceiling_ = 0, center_ = 0;
    bool started_ = false;
};

// The seek bar glow's pulse, 0..1, from the audio being heard (Engine::WaveformPeaks, oldest
// first). It jumps when the sound now stands above its level of the last second or so, a beat
// or an attack, and settles back over a fraction of a second; steady loud music keeps a middle
// glow. `seconds` is any clock; a paused clock holds the pulse where it is.
class GlowPulse {
public:
    float Step(const uint8_t* peaks, int buckets, double seconds);
    float Value() const { return value_; }

private:
    float fast_ = 0, slow_ = 0, value_ = 0;
    double last_ = -1;
};

// Blurs the outline and the colours over about `radius` buckets either side: the soft, rounded
// shape the Smooth seek bar setting draws.
void SmoothPeaks(const Peaks& in, Peaks& out, int radius);

// How a span of buckets sounds: `bright` from -1 (bassier than this track usually is) to 1
// (more treble), `level` from 0 (silence) to 1, and each band's mean, 0..1. `center` is MoodCenter
// of the same peaks.
struct MoodTone {
    float bright = 0, level = 0;
    float bands[kMoodBands] = {};
};
float MoodCenter(const Peaks& p, int buckets = kPeakBuckets);
MoodTone MoodOf(const Peaks& p, int first, int last, float center, int buckets = kPeakBuckets);

// The moodbar's colours for one scheme and accent on one background. Theme and Vivid turn a tone's
// hue from the accent's one way for bass and the other for treble, and its loudness sets how far it
// stands out from the background; their full-level colours keep the 3:1 contrast PRINCIPLES.md asks
// of accents. Spectrum mixes the bands into red, green and blue. The unplayed part is the same
// colours dimmed and desaturated. Tones are quantized, so a paint uses a bounded set of colours.
struct MoodPalette {
    static constexpr int kHues = 9, kLevels = 6;
    Rgb accent = kNoRgb, background = kNoRgb;  // what the table was made for
    int scheme = -1;
    Rgb played[kHues][kLevels], rest[kHues][kLevels];
    void Make(Rgb accent, Rgb background, int scheme = kMoodTheme);
    bool For(Rgb a, Rgb b, int s) const { return accent == a && background == b && scheme == s; }
    Rgb Pick(MoodTone tone, bool played) const;
};

class Decoder;
// Quickly samples a seekable track across its full length. The result is an approximation for
// display only; the whole-file decode supplies the exact peaks and is the only result cached.
bool PreviewPeaks(Decoder& decoder, double start, double seconds, Peaks& out, const std::atomic<bool>* cancel);

// Decodes the file, or only the [start, end) seconds of the file when end > 0, and fills `out`.
// A CUE track's range is passed in file seconds; peaks.cpp does not map it itself.
// Returns false when the file can't be decoded or `cancel` is set.
bool ComputePeaks(const std::wstring& path, double start, double end, Peaks& out, const std::atomic<bool>* cancel);

// Starts the background worker. `notify` gets `msg` (wParam = request id) as a queued computation
// progresses and when it finishes; call GetPeaks and GetPeakProgress again then.
// Until this runs, GetPeaks never queues.
void StartPeaks(HWND notify, UINT msg, const std::wstring& cacheDir);
void StopPeaks();
// Abandons the current and queued waveform requests when no track needs them any more.
void CancelPeaks();
// Warms one likely next track without interrupting the playing track's waveform.
void PrefetchPeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end);
void CancelPeakPrefetch();

// Returns true and fills `out` when the peaks are cached in memory or on disk. Otherwise queues
// the computation (newest request first; obsolete work is cancelled) and
// returns false with `requestId` set to the request (0 when nothing was queued).
bool GetPeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end, Peaks& out,
              uint32_t& requestId);

// Copies the decoded part of an in-flight request, positioned on the whole track's timeline.
// The unprocessed part is zero; these provisional peaks are never written to the cache.
bool GetPeakProgress(uint32_t requestId, Peaks& out);

// The Mood column: a track's peaks for a list cell from memory only, so painting never waits. A miss
// queues the track behind every seek bar request (disk, else a decode that yields to them) and
// returns false; the notify window then gets the peaks message with wParam 0.
bool ListPeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end, Peaks& out);

// Collects peaks while a file is decoded for another purpose (ReplayGain). Memory stays small
// for any length: fine buckets are merged as the file grows.
class PeakAccumulator {
public:
    void Begin(uint64_t totalFrames, unsigned rate = 48000);  // totalFrames 0 = unknown
    void Add(const float* stereo, size_t frames);
    bool Preview(Peaks& out) const;  // false until samples and a known length are available
    bool Finish(Peaks& out);  // false when no audio was seen

private:
    static constexpr size_t kMaxBlocks = 2 * kPeakBuckets;
    void Push();
    void Merge();
    std::vector<float> maxima_;    // one linear maximum per block of blockFrames_ frames
    std::vector<double> squares_;  // the same blocks' mean squared sample
    std::vector<double> bands_;    // the same blocks' mean band energies, kMoodBands per block
    MoodSplit split_;
    uint64_t blockFrames_ = 512;
    uint64_t totalFrames_ = 0;
    uint64_t inBlock_ = 0;
    float blockMax_ = 0;
    double blockSquares_ = 0;  // sum over the open block
    double blockBands_[kMoodBands] = {};
};

// Stores peaks computed elsewhere (memory and disk). Ignored until StartPeaks has run.
void StorePeaks(const std::wstring& path, uint64_t fileSize, uint64_t fileTime, double start, double end, const Peaks& p);

// The peaks cache key for a whole file as 16 hex digits: the same path+size+time identity, for
// settings that belong to one file's audio (the section in app_region.cpp) rather than to a range.
std::wstring PeaksKeyName(const std::wstring& path, uint64_t fileSize, uint64_t fileTime);
