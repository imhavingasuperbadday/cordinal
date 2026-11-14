// A PipeWire client speaking the native protocol directly (no libpipewire or SPA plugins). It
// connects to $PIPEWIRE_RUNTIME_DIR or $XDG_RUNTIME_DIR /pipewire-0 (or $PIPEWIRE_REMOTE), reads
// the registry and the "default" metadata for the sinks, and plays by exporting a client node of
// its own, as pipewire-jack does: two mono 32-bit float output ports (FL and FR, PipeWire's DSP
// format, which a sink's ports take as is), shared-memory buffers the server allocates, and the
// eventfd activation protocol that runs it in the server's graph each cycle. The node links itself
// to the chosen sink (or the default one, followed as it changes), so the session manager isn't
// needed for routing, and the samples reach the sink unconverted when the graph runs at the
// stream's rate. When it runs at another rate the stream resamples on its side.
#pragma once
#include "sound_stream.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lx::pw {

std::unique_ptr<sound::Stream> OpenStream(const sound::Params& params, sound::Callbacks callbacks,
                                          std::function<void()> onFailure, std::string& error);
// False when no PipeWire server answers.
bool ListSinks(std::vector<sound::Sink>& out);

// The socket path the client connects to.
std::string SocketPath();

// Runs every message parser of the protocol over `data` as if it had arrived (for fuzzing: no
// fds, no server). Returns how many parsed as well-formed.
int FuzzParse(const uint8_t* data, size_t size);

}  // namespace lx::pw
