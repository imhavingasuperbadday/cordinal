// A PulseAudio client speaking the native protocol directly (no libpulse), for systems that run
// PulseAudio rather than PipeWire, or where only pipewire-pulse's socket is reachable (a sandbox).
// It connects to $PULSE_SERVER (unix:/path) or $XDG_RUNTIME_DIR/pulse/native, authenticates with
// the cookie in $PULSE_COOKIE or ~/.config/pulse/cookie (or none, which pipewire-pulse and a
// default PulseAudio accept from the same user), and plays one float32 stereo stream, fed on the
// server's requests. The server resamples and mixes it.
#pragma once
#include "sound_stream.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lx::pulse {

std::unique_ptr<sound::Stream> OpenStream(const sound::Params& params, sound::Callbacks callbacks,
                                          std::function<void()> onFailure, std::string& error);
// False when no PulseAudio server answers.
bool ListSinks(std::vector<sound::Sink>& out);
std::string SocketPath();

// Runs the protocol's reply parsers over `data` (for fuzzing). Returns how many parsed.
int FuzzParse(const uint8_t* data, size_t size);
// Well-formed messages to mutate: 0 a sink info list, 1 a create-stream reply, 2 server info.
std::vector<uint8_t> FuzzSeed(int which);

}  // namespace lx::pulse
