// Playback streams for the Linux build without a sound library: the clients in pw_native.cpp
// (PipeWire's native protocol) and pulse_native.cpp (PulseAudio's, which pipewire-pulse speaks as
// well), tried in that order. Both play interleaved stereo float at the rate the caller asks for;
// a stream that the server runs at another rate is resampled on the way (PipeWire: by the stream,
// with the engine's resampler; PulseAudio: by the server), so callers render one rate no matter
// what the device does, and are told the rate the server really runs at for reporting.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lx::sound {

struct Sink {
    std::string name;         // node.name / the PulseAudio sink name
    std::string description;  // what to show
    std::string alsaCard;     // the ALSA card number, when the server says
    bool systemDefault = false;
};

struct Params {
    std::string name = "cordinal";      // node and stream name
    std::string role = "Music";         // media.role
    unsigned rate = 48000;              // what the render callback produces
    unsigned latencyFrames = 960;       // wanted period, in frames at `rate`
    bool exclusive = false;             // ask the server to run the device at `rate`
    std::string target;                 // a Sink::name; empty follows the default
    bool active = true;                 // false opens it paused
};

// Called on the stream's thread, which must not block. Render fills `frames` interleaved stereo
// frames at Params::rate. Timing comes first on each cycle: `played` is how many of the frames
// rendered since the stream opened have reached the device (or were dropped by Flush);
// `serverRate` is the rate the server is running the device at (0 when it doesn't say).
struct Callbacks {
    std::function<void(float* out, uint32_t frames)> render;
    std::function<void(uint64_t played, unsigned serverRate)> timing;
};

class Stream {
public:
    virtual ~Stream() = default;  // stops the thread: no callback runs after it returns
    virtual void SetActive(bool active) = 0;
    // Drops what is queued on the way to the device, as far as the protocol allows (PipeWire: the
    // stream's own resampler queue; PulseAudio: the server's buffer).
    virtual void Flush() = 0;
    // The server went away, or refused the stream: open a new one.
    virtual bool Failed() const = 0;
    virtual const char* Backend() const = 0;  // "PipeWire" or "PulseAudio"
};

// The first backend that accepts the stream (none when no sound server answers; `error` says why).
// `onFailure` is called from the stream's thread when Failed() becomes true.
std::unique_ptr<Stream> Open(const Params& params, Callbacks callbacks, std::function<void()> onFailure,
                             std::string& error);
// The sinks of the first server that answers, sorted by description. Empty when none does.
std::vector<Sink> ListSinks();

// Which backends Open and ListSinks try, from CORDINAL_AUDIO ("pipewire", "pulse"; unset: both).
bool BackendAllowed(const char* name);

}  // namespace lx::sound
