// WASAPI output following the Windows default device (on Linux: a PipeWire or PulseAudio stream,
// see linux/audio_linux.cpp). Renders interleaved stereo float.
//
// Shared mode: through the Windows mixer at the device's mix rate (the engine resamples), in the
// device's own stereo mix format when that is float or 16/24/32-bit PCM. Any other mix layout is
// submitted as stereo float and WASAPI converts.
// Exclusive mode: the device is opened at the rate the engine asks for, trying the widest integer
// format first and then float, with no Windows mixing or resampling. Bit-perfect when the engine
// doesn't change the samples either.
// Upmix (shared mode only): on a device with more than two speakers, the stream is opened in the
// device's own layout and the stereo render is spread over it by an Upmixer (upmix.h).
// ASIO mode (Windows): an ASIO driver chosen by name (asio.h) plays the first two of its outputs at the
// rate the engine asks for, in the driver's own sample type and buffer size, bypassing Windows audio.
// Bit-perfect like exclusive mode. Falls back to shared output when the driver can't be opened.
#pragma once
#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

enum class OutputMode { Shared, Exclusive, Asio };

#ifndef _WIN32
// An audio sink the sound server (PipeWire or PulseAudio) offers, for choosing where cordinal plays.
struct OutputDevice {
    std::string node;         // node.name, what the stream is pointed at
    std::string description;  // what to show
    std::string alsaCard;     // API ALSA card number, used to match an optional headset's USB parent
    bool systemDefault = false;
};
// The sinks right now (asks the server; empty without one).
std::vector<OutputDevice> ListOutputDevices();
#endif
enum class SampleFormat : uint8_t { Float32, Int32, Int24In32, Int24, Int16 };

// Exclusive output tries these in order and keeps the first format the device accepts.
inline constexpr SampleFormat kExclusiveFormats[] = {
    SampleFormat::Int32, SampleFormat::Int24In32, SampleFormat::Int24, SampleFormat::Float32,
    SampleFormat::Int16};

// Shared-mode mix layout, detached from WAVEFORMATEX so the choice can be tested without a device.
// `pcm` is false for IEEE float. `validBits` 0 means the container width. `blockAlign` is the bytes
// in one interleaved frame; 0 leaves packing to the container width.
struct MixLayout {
    unsigned channels = 0;
    unsigned bits = 0;
    unsigned validBits = 0;
    unsigned blockAlign = 0;
    bool pcm = false;
};

// Stereo float, or stereo PCM at 16, 24 or 32 bits. nullopt when shared mode should submit float32
// and let WASAPI convert: a surround mix, or a width this player does not pack.
inline std::optional<SampleFormat> NativeMixFormat(MixLayout layout) {
    if (layout.channels != 2) return std::nullopt;
    const unsigned valid = layout.validBits ? layout.validBits : layout.bits;
    const unsigned align = layout.blockAlign;
    if (!layout.pcm) {
        if (layout.bits == 32 && valid == 32 && (align == 0 || align == 8)) return SampleFormat::Float32;
        return std::nullopt;
    }
    if (valid == 16 && layout.bits == 16 && (align == 0 || align == 4)) return SampleFormat::Int16;
    if (valid == 32 && layout.bits == 32 && (align == 0 || align == 8)) return SampleFormat::Int32;
    if (valid == 24 && layout.bits == 24 && (align == 0 || align == 6)) return SampleFormat::Int24;
    if (valid == 24 && layout.bits == 32 && (align == 0 || align == 8)) return SampleFormat::Int24In32;
    return std::nullopt;
}

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
    AudioOutput();  // out of line: PipewireState is complete only in audio_linux.cpp
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
    // ASIO output doesn't play through the Windows default device, so it doesn't follow it.
    void DefaultDeviceChanged() {
        if (mode_ == OutputMode::Asio && wantedMode_ == OutputMode::Asio) return;
        reopen_ = true;
        wake();
    }

    // Switches between shared, exclusive and ASIO output (re-opens the device; ResetFn follows).
    void SetMode(OutputMode mode);
    OutputMode Mode() const { return mode_; }  // the mode actually in use
    OutputMode WantedMode() const { return wantedMode_; }
    // Exclusive and ASIO modes: re-open the device at `rate` (blocking until done) and return the rate
    // it runs at, which differs when the device can't do `rate`. Shared mode: returns Rate().
    unsigned RequestRate(unsigned rate);
    SampleFormat Format() const { return format_; }
    // Spreads stereo over every speaker of a surround device in shared mode (re-opens the device).
    void SetUpmix(bool upmix);
    // The layout being upmixed to ("5.1"), or empty while playing plain stereo.
    std::string UpmixLabel() const;
    std::wstring Problem() const;  // why exclusive or ASIO output isn't in use, or isn't at the asked rate
#ifdef _WIN32
    // The ASIO driver to use, by its registry name; empty picks the first installed one. Re-opens the
    // device when ASIO output is in use.
    void SetAsioDriver(const std::wstring& name);
    std::wstring AsioDriver() const;  // the wanted name
    std::wstring DeviceName() const;  // the ASIO driver playing, empty otherwise
    // Plays on a network speaker (speakers.h, an id from speakers::DeviceId) instead of this computer's
    // device, whatever the mode; empty comes back. The speaker is retried while it can't be reached.
    void SetSpeaker(const std::string& id);
    long AsioType() const { return asioType_; }  // its sample type (asio::SampleType), -1 when not ASIO
    // Opens the playing ASIO driver's own settings (buffer size, clock). False when no driver is loaded.
    bool ShowAsioPanel();
#endif
    // The network speaker chosen as output (SetSpeaker, or SetDevice on Linux), empty for none.
    std::wstring SpeakerName() const;
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
    // For the sound stream's callbacks.
    size_t RenderInto(float* out, size_t frames, unsigned long long& sourcePos);
    void SetPlayed(unsigned long long pos) { played_ = pos; }
    void ClearProblem();  // a network speaker that failed is playing again
    void SetGraphRate(unsigned graphRate);
#endif

private:
#ifdef _WIN32
    void Run();
    void RunSpeaker(unsigned long long& sourcePos);
    std::string SpeakerId() const;
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
    std::atomic<bool> wantedUpmix_{false};
    std::string upmixLabel_;  // guarded by problemMx_
    std::atomic<bool> holding_{false};
    std::atomic<bool> waiting_{false};  // lost device, retrying the open
    std::atomic<SampleFormat> format_{SampleFormat::Float32};
#ifdef _WIN32
    std::atomic<long> asioType_{-1};
    std::atomic<bool> panel_{false};
    std::wstring asioDriver_, deviceName_;  // guarded by problemMx_
    std::string speaker_;                   // guarded by problemMx_
#endif
    std::atomic<unsigned long long> played_{0}, flushPos_{0};
    mutable std::mutex problemMx_;
    std::wstring problem_;
    bool ok_ = false;
    std::wstring error_;
};
