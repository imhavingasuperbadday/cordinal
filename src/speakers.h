// Network speakers: UPnP/DLNA media renderers (TVs, AV receivers, smart speakers, and players such as
// foobar2000, Kodi or BubbleUPnP on another device) used as cordinal's audio output. cordinal sends
// what its engine renders, after EQ, ReplayGain, crossfade and effects, as one endless lossless stream
// (16-bit WAV, or DLNA LPCM for a renderer that only takes that) which the speaker pulls from a small
// HTTP server here, and drives the speaker with AVTransport. Gapless playback, the queue and every
// sound setting therefore work as they do on the computer's own speakers.
//
// The speaker is the clock: rendering follows what it reads, at most a few seconds ahead of real
// time, and what it reports as played (GetPositionInfo) is turned back into cordinal's position. Pause
// and resume are passed on (Pause, Play); a seek or a track started by hand starts a fresh stream so
// the speaker drops what it had buffered. Volume stays cordinal's own.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace speakers {

struct Speaker {
    std::string udn;          // "uuid:...", the device's lasting identity
    std::string name, model;
    std::string location;     // the URL of its device description
    std::string transport;    // AVTransport control URL
    std::string transportType = "urn:schemas-upnp-org:service:AVTransport:1";
    std::string connection;   // ConnectionManager control URL; may be empty
    std::string connectionType = "urn:schemas-upnp-org:service:ConnectionManager:1";
};

// An output device id for a speaker, as settings keep it and AudioOutput takes it:
// "upnp:<udn> <location> <name>". The location is tried first; the udn finds the speaker again
// when its address changed.
std::string DeviceId(const Speaker& speaker);
inline bool IsSpeakerId(std::string_view id) { return id.substr(0, 5) == "upnp:"; }
// The parts of an id; false when it isn't one.
bool ParseDeviceId(std::string_view id, Speaker& speaker);

// A renderer's device description: false unless it has an AVTransport service. `location` resolves
// relative control URLs.
bool ParseDescription(std::string_view xml, const std::string& location, Speaker& speaker);
// Fetches and parses the description at `location` (a few seconds at most).
bool Describe(const std::string& location, Speaker& speaker);
// Asks every local network for media renderers for `ms` milliseconds, then reads their descriptions.
// Sorted by name, one entry per device. Blocks; call it on a worker.
std::vector<Speaker> Discover(unsigned ms);

// Called on the stream's thread, like a sound server's callbacks (linux/sound_stream.h): render
// fills `frames` interleaved stereo frames at the stream's rate; timing comes first on each cycle,
// with how many of the frames rendered since the stream opened the speaker has played (or that were
// dropped by Flush).
struct Callbacks {
    std::function<void(float* out, uint32_t frames)> render;
    std::function<void(uint64_t played)> timing;
};

class Stream {
public:
    // Returns at once: finding the speaker and starting it happen on the stream's own threads, so
    // the caller (the UI thread, on Linux) never waits on the network. Null only when no local
    // socket can be opened (`error` says why). `onFailure` is called from the stream's thread when
    // Failed() becomes true, never after the stream is destroyed.
    static std::unique_ptr<Stream> Open(const std::string& id, unsigned rate, bool active, Callbacks callbacks,
                                        std::function<void()> onFailure, std::string& error);
    ~Stream();  // tells the speaker to stop; no callback runs after it returns
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    void SetActive(bool active);  // pause or resume the speaker and the rendering
    void Flush();                 // drop what the speaker has buffered: it gets a fresh stream
    bool Failed() const;          // the speaker can't be found, refused the stream, or keeps dropping it
    std::string Name() const;     // the speaker's name
    std::string Problem() const;  // why it failed, in words
    unsigned Rate() const;

    struct Impl;
private:
    explicit Stream(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> p_;
};

}  // namespace speakers
