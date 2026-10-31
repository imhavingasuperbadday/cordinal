// WASAPI output following the Windows default device (on Linux: a PipeWire stream, see
// linux/audio_pipewire.cpp). Renders interleaved stereo float.
//
// Shared mode: through the Windows mixer at the device's mix rate (the engine resamples).
// Exclusive mode: the device is opened at the rate the engine asks for, in the best integer or
// float format it accepts, with no Windows mixing or resampling: bit-perfect when the engine
// doesn't change the samples either.
#pragma once
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class OutputMode { Shared, Exclusive };

#ifndef _WIN32
// An audio sink PipeWire offers, for choosing where cordinal plays.
struct OutputDevice {
    std::string node;         // node.name, what the stream is pointed at
    std::string description;  // what to show
};
// The sinks right now (asks the server; empty without one).
std::vector<OutputDevice> ListOutputDevices();
#endif
enum class SampleFormat : uint8_t { Float32, Int32, Int24In32, Int24, Int16 };

// float [-1, 1] -> the device format. Exact for samples that came from integers of that width.
void ConvertSamples(const float* in, size_t samples, SampleFormat fmt, void* out);
int SampleBytes(SampleFormat fmt);
int SampleBits(SampleFormat fmt);  // significant bits (32 for float)

// One render thread owns the device. The methods below only set atomic flags and wake that
// thread, except RequestRate and Stop, which block until it has responded. RenderFn runs on the
// render thread at device priority: it must not lock, allocate or block.
class AudioOutput {
public:
    // Called on the render thread. `out` must be filled with `frames` stereo frames.
    // Returns how many came from the source (the rest is silence) and sets `sourcePos`
    // to the source's read position after them.
    using RenderFn = std::function<size_t(float* out, size_t frames, unsigned long long& sourcePos)>;
    // Called on the render thread after the device was re-opened at a different rate on its own
    // (a new default device), not after RequestRate.
    using ResetFn = std::function<void(unsigned newRate)>;

#ifdef _WIN32
    ~AudioOutput() { Stop(); }
#else
    AudioOutput();  // out of line: PipewireState is complete only in audio_pipewire.cpp
    ~AudioOutput();
#endif
    bool Start(RenderFn render, ResetFn reset, std::wstring& error);
    void Stop();

    unsigned Rate() const { return rate_; }
    void SetPaused(bool paused) { paused_ = paused; wake(); }
    bool Paused() const { return paused_; }
    // Drops what the device has queued; playback continues from source position `pos`.
    void Flush(unsigned long long pos) { flushPos_ = pos; flush_ = true; wake(); }

    // Source position that the speakers have actually reached.
    unsigned long long PlayedPosition() const { return played_; }
    void DefaultDeviceChanged() { reopen_ = true; wake(); }

    // Switches between shared and exclusive output (re-opens the device; ResetFn follows).
    void SetMode(OutputMode mode);
    OutputMode Mode() const { return mode_; }  // the mode actually in use
    OutputMode WantedMode() const { return wantedMode_; }
    // Exclusive mode: re-opens the device at `rate` (blocking until done) and returns the rate it
    // runs at, which differs when the device can't do `rate`. Shared mode: returns Rate().
    unsigned RequestRate(unsigned rate);
    SampleFormat Format() const { return format_; }
    std::wstring Problem() const;  // why exclusive mode isn't in use, or isn't at the asked rate
    // True while a lost output device is being re-opened; playback is frozen until it returns.
    bool Waiting() const { return waiting_; }
    // False once a paused output has let the endpoint go (see Run). An exclusive stream keeps the
    // device claimed for as long as its IAudioClient exists, stopped or not, so a paused player
    // would otherwise block every other program from rendering.
    bool HoldingDevice() const { return holding_; }

#ifndef _WIN32
    // The sink to play on: a PipeWire node name, or empty for the system's default (which the session
    // manager moves as the user changes it). Re-opens the stream.
    void SetDevice(const std::string& node);
    // For the PipeWire stream's callback.
    size_t RenderInto(float* out, size_t frames, unsigned long long& sourcePos);
    void SetPlayed(unsigned long long pos) { played_ = pos; }
    void SetGraphRate(unsigned graphRate);
#endif

private:
#ifdef _WIN32
    void Run();
    void wake() { if (event_) SetEvent(event_); }
#else
    void wake();
    void Reopen();
    std::unique_ptr<struct PipewireState> state_;
    std::string device_;  // guarded by problemMx_
#endif

    RenderFn render_;
    ResetFn reset_;
    std::thread thread_;
    HANDLE event_ = nullptr, ready_ = nullptr, rateDone_ = nullptr;
    std::atomic<bool> quit_{false}, paused_{false}, flush_{false}, reopen_{false}, rateRequest_{false};
    std::atomic<unsigned> rate_{48000}, wantedRate_{0};
    std::atomic<OutputMode> mode_{OutputMode::Shared}, wantedMode_{OutputMode::Shared};
    std::atomic<bool> holding_{false};
    std::atomic<bool> waiting_{false};  // lost device, retrying the open
    std::atomic<SampleFormat> format_{SampleFormat::Float32};
    std::atomic<unsigned long long> played_{0}, flushPos_{0};
    mutable std::mutex problemMx_;
    std::wstring problem_;
    bool ok_ = false;
    std::wstring error_;
};
