// SILK layer decoder facade (RFC 6716 section 4.2): pimpl over the algorithm in
// audio_opus_silk.cpp. Owns LPC/LTP synthesis state and the resampler that lifts
// the 8/12/16 kHz internal signal to 48 kHz; State snapshots it for seeking.
#pragma once
#include "audio_opus_range.h"
#include <memory>
#include <vector>

namespace nativeaudio {
class OpusSilk {
public:
    OpusSilk();
    ~OpusSilk();
    // Clears synthesis history, stereo weights and resampler memory.
    void Reset();
    struct State;
    // Copyable snapshot of all decoder state; needed to seek (no keyframes).
    std::shared_ptr<const State> Save() const;
    void Load(const State& state);
    // Decodes one SILK frame (possibly several 20 ms subframes) from `range` at
    // `rate` Hz for `milliseconds` (10/20/40/60). Output is always 48 kHz.
    bool Decode(OpusRange& range,unsigned channels,unsigned rate,unsigned milliseconds,std::vector<float>& out);
    // Packet-loss concealment: synthesises a frame of the given duration from the
    // last good parameters. Same 48 kHz output format as Decode.
    bool Conceal(unsigned channels,unsigned rate,unsigned milliseconds,std::vector<float>& out);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace nativeaudio
