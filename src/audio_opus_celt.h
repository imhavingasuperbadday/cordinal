// CELT layer decoder facade (RFC 6716 section 4.3): pimpl over the algorithm in
// audio_opus_celt.cpp. Owns the overlap-add, energy-prediction and post-filter
// history that must persist across packets; State is a copyable snapshot of it.
#pragma once
#include "audio_opus_range.h"
#include <memory>
#include <vector>

namespace nativeaudio {
class OpusCelt {
public:
    OpusCelt();
    ~OpusCelt();
    // Clears overlap, energy and filter history for a fresh stream.
    void Reset();
    struct State;
    // Snapshot of all decoder state; needed to seek, since Opus packets have no
    // keyframe notion and depend on prior history.
    std::shared_ptr<const State> Save() const;
    void Load(const State& state);
    // Decodes one CELT frame (or the CELT high band of a hybrid frame) from
    // `range` into interleaved 48 kHz floats. `lm` selects the 120<<lm frame
    // size, `first`/`last` bound the coded CELT bands (0-21), `channels` is 1-2.
    bool Decode(OpusRange& range,unsigned channels,unsigned lm,unsigned first,unsigned last,std::vector<float>& out);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace nativeaudio
