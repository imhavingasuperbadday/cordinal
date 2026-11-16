// Backend choice for sound_stream.h: PipeWire's native protocol first, then PulseAudio's.
#include "sound_stream.h"

#include "pulse_native.h"
#include "pw_native.h"
#include "speakers.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace lx::sound {
namespace {
// A network speaker behind the sound stream interface: the engine renders into it as into a sink.
class SpeakerStream : public Stream {
public:
    explicit SpeakerStream(std::unique_ptr<speakers::Stream> stream) : stream_(std::move(stream)) {}
    void SetActive(bool active) override { stream_->SetActive(active); }
    void Flush() override { stream_->Flush(); }
    bool Failed() const override { return stream_->Failed(); }
    const char* Backend() const override { return "network speaker"; }
    std::string Problem() const override { return stream_->Problem(); }
private:
    std::unique_ptr<speakers::Stream> stream_;
};
}

bool BackendAllowed(const char* name) {
    const char* only = getenv("CORDINAL_AUDIO");
    return !only || !*only || !strcmp(only, name);
}

std::unique_ptr<Stream> Open(const Params& params, Callbacks callbacks, std::function<void()> onFailure,
                             std::string& error) {
    if (speakers::IsSpeakerId(params.target)) {
        speakers::Callbacks cb;
        cb.render = callbacks.render;
        cb.timing = [timing = callbacks.timing](uint64_t played) { timing(played, 0); };
        auto stream = speakers::Stream::Open(params.target, params.rate, params.active, std::move(cb), std::move(onFailure), error);
        if (!stream) return nullptr;
        return std::make_unique<SpeakerStream>(std::move(stream));
    }
    std::string why;
    if (BackendAllowed("pipewire")) {
        if (auto s = pw::OpenStream(params, callbacks, onFailure, why)) return s;
        error = why;
    }
    if (BackendAllowed("pulse")) {
        std::string pulseWhy;
        if (auto s = pulse::OpenStream(params, callbacks, onFailure, pulseWhy)) return s;
        error = error.empty() ? pulseWhy : error + "; " + pulseWhy;
    }
    if (error.empty()) error = "no sound server allowed by CORDINAL_AUDIO";
    return nullptr;
}

std::vector<Sink> ListSinks() {
    std::vector<Sink> sinks;
    bool answered = BackendAllowed("pipewire") && pw::ListSinks(sinks);
    if (!answered && BackendAllowed("pulse")) pulse::ListSinks(sinks);
    std::sort(sinks.begin(), sinks.end(), [](const Sink& a, const Sink& b) { return a.description < b.description; });
    return sinks;
}

}  // namespace lx::sound
