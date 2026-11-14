// Backend choice for sound_stream.h: PipeWire's native protocol first, then PulseAudio's.
#include "sound_stream.h"

#include "pulse_native.h"
#include "pw_native.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace lx::sound {

bool BackendAllowed(const char* name) {
    const char* only = getenv("CORDINAL_AUDIO");
    return !only || !*only || !strcmp(only, name);
}

std::unique_ptr<Stream> Open(const Params& params, Callbacks callbacks, std::function<void()> onFailure,
                             std::string& error) {
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
