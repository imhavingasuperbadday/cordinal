// Playback engine: decode thread -> tempo -> resampler (speed) -> ring buffer -> gain/EQ/effects ->
// limiter -> WASAPI, with gapless transitions and per-track ReplayGain.
//
// The engine identifies tracks by tokens chosen by the application. Events are posted to the
// notify window as WM_ENGINE (wParam = EngineEvent, lParam = token):
//   NeedNext     the engine started decoding `token`; answer with SetNext(token, ...)
//   Error        opening or seeking `token` failed (TakeError(token) has the text); answer with SetNext too
//   TrackStarted `token` became audible
//   Ended        everything has been played and there is no next track
//   Estimated    the quick loudness estimate of `token` is ready (TakeEstimate; see PlayItem::estimate)
//   Damaged      `token` failed while reading (TakeError(token) says where); queued playback continues
#pragma once
#include "dsp.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

constexpr UINT WM_ENGINE = WM_APP + 1;
enum class EngineEvent : WPARAM { NeedNext = 1, Error, TrackStarted, Ended, Estimated, Damaged };
class LiveAudioSource;

// One step of a click-free fade: moves `gain` toward `target` by at most `step`. The engine's
// fades run over about 12 ms (see Engine::SetPaused).
inline float StepFade(float gain, float target, float step) {
    return gain < target ? std::min(target, gain + step) : std::max(target, gain - step);
}

struct PlayItem {
    unsigned long long token = 0;
    std::wstring path;     // a file, or a sub-track "file|N" (see SplitSubtrack)
    float gain = 1;        // ReplayGain, linear
    double duration = 0;   // hint from the library, used when the decoder doesn't know
    // Play only [start, end) seconds of the file (a CUE track). end 0 means to the end of the file.
    // Position(), Duration() and Seek() are relative to start. A range turns loop points off.
    // start -1 means the track can't be located: it fails instead of playing the whole file.
    double start = 0, end = 0;
    // Optional repeating section, relative to the track's range. The full track timeline
    // stays visible; playback and seeks remain inside [loopStart, loopEnd).
    double loopStart = 0, loopEnd = 0;
    // No ReplayGain data yet: measure the first seconds as they decode and report Estimated.
    bool estimate = false;
    std::shared_ptr<LiveAudioSource> live;  // a non-seekable radio stream instead of a file
};

// One engine per process. Methods are UI-thread entry points: they enqueue commands for the
// decode thread or poke atomics for the render thread, and never block on playback itself.
class Engine {
public:
    Engine();
    ~Engine();
    // The UI can defer device/driver allocation until its first Play command.
    bool Init(HWND notify, std::wstring& error, bool deferOutput = false);
    void Shutdown();

    // `paused` opens the track at `start` without playing it: the first SetPaused(false) carries on
    // from there, so a saved place can be restored without a sound.
    void Play(const PlayItem& item, double start = 0, bool paused = false);
    // nullptr stops after `after`. Replacing a decoded-ahead next track preserves the current
    // song; samples already submitted to the output device cannot be recalled.
    void SetNext(unsigned long long after, const PlayItem* next);
    void Stop();
    // Pause, resume, seek, stop and a track started by hand fade the output out and in over 12 ms,
    // so they don't click. Gapless transitions aren't faded. Paused() reports the request at once.
    void SetPaused(bool paused);
    // An updated item applies edited section marks without starting a new listening session.
    void Seek(double seconds, const PlayItem* updated = nullptr);

    void SetVolume(float linear);
    // Exclusive output plays each track at its own sample rate with nothing in between (no
    // Windows mixer, no resampling); bit-perfect at 100% volume with EQ and ReplayGain off.
    void SetExclusive(bool exclusive);
#ifndef _WIN32
    void SetOutputDevice(const std::string& node);  // a PipeWire sink by node name; empty is the default
#endif
    struct OutputInfo {
        bool exclusive = false, bitPerfect = false, ready = false, waiting = false;
        unsigned rate = 0;
        int bits = 32;         // device sample format: 16, 24 or 32
        bool floatFormat = true;
        std::wstring problem;  // why exclusive output isn't (fully) in use
    };
    OutputInfo Output() const;
    void SetEq(const EqSettings& eq);
    // Effects after the EQ (see dsp.h). The limiter covers them, and any of them sounding ends
    // bit-perfect output. Loudness compensation follows the volume.
    void SetReverb(const ReverbSettings& reverb);
    void SetSmartVolume(bool loudness, bool night);
    // Speed plays faster or slower with the pitch following (the resampler's ratio); tempo keeps
    // the pitch (TimeStretch). Position, Duration and Seek stay in the track's own time. A change
    // carries on from what is heard, with the 12 ms fade.
    void SetSpeed(double speed);
    void SetTempo(double tempo);
    double TakeEstimate(unsigned long long token);  // LUFS, NaN if none is waiting
    // glide: an estimated gain, applied as a slow level change rather than the usual quick one.
    void SetGain(unsigned long long token, float gain, bool glide = false);
    // A look-ahead peak limiter in place of clipping (see PeakLimiter); it delays the output by
    // 3 ms. EQ also enables it automatically; with both off the plain path stays bit-perfect.
    // Optional output reserve lowers the limiter ceiling for EQ applied inside a headset.
    void SetLimiter(bool on, float headroomDb = 0);
    // Game-music loop points (LOOPSTART/LOOPEND tags, WAV smpl chunks): the intro, then the loop
    // `count` times in total, then the loop again while fading out over fadeSeconds. 0 turns loop
    // points off. Applies to tracks opened after the call. Default: 2 passes, 8 s fade.
    void SetLoops(int count, double fadeSeconds);

    bool Paused() const;
    double Position() const;             // seconds into the audible track
    double Duration() const;
    float PlayingGain() const;           // the playing track's applied ReplayGain
    unsigned Rate() const;
    // Output history for Apollo's analyzer, up to 2048 interleaved stereo frames, oldest first.
    void SetScope(bool on);
    size_t Scope(float* out, size_t frames) const;
    // Pre-volume PCM centered on the audible clock, including decoded look-ahead. The buffer
    // holds 2 * frames; time is the center in output-frame seconds, stable while paused.
    size_t VisualSamples(float* out, size_t frames, double& time) const;
    // Live display heights: the last quarter-second's stereo texture swells on audible note
    // attacks and falls smoothly. Pre-volume, empty while stopped, held by the clock on pause.
    // No decoder work or history allocation; the whole-track waveform keeps its own peak scale.
    bool WaveformPeaks(uint8_t* out, size_t buckets) const;
    // Takes this token's pending problem, or the oldest when token is 0.
    std::wstring TakeError(unsigned long long token = 0);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Offline CPU throughput through the production decoder, resampler, ring and
// renderer. No audio device is opened and no wall-clock playback waits are used.
struct PipelineTiming { uint64_t inputFrames=0,outputFrames=0;unsigned inputRate=0;double seconds=0; };
bool BenchmarkPlayerPipeline(const std::wstring& path,unsigned outputRate,unsigned passes,PipelineTiming& result);
// Offline PCM through the production ring and renderer, including EQ, the effects after it and
// output protection.
struct RenderEffects {
    ReverbSettings reverb;
    float volume = 1;
    bool loudness = false, night = false;
    size_t volumeChangeFrame = SIZE_MAX;
    float volumeAfter = 1;
};
std::vector<float> RenderPlayerPcm(const float* stereo, size_t frames, unsigned rate,
                                 const EqSettings& eq, bool replayGainLimiter, float trackGain = 1,
                                 float outputHeadroomDb = 0, const RenderEffects* effects = nullptr);
// Final renderer clipping, preserving NaN payloads and signed zero like std::clamp.
void ClipPcm(float* samples,size_t count);
