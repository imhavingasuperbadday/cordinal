// Cordinal Spectral Band Replication (SBR) and Parametric Stereo (PS) decoder,
// following ISO/IEC 14496-3 Subpart 4 (SBR) and Subpart 8 (PS).
#pragma once
#include "audio_bits.h"
#include <memory>

namespace nativeaudio {

class SbrDecoder {
public:
    SbrDecoder();
    ~SbrDecoder();
    SbrDecoder(SbrDecoder&&) noexcept;
    SbrDecoder& operator=(SbrDecoder&&) noexcept;

    void Init(unsigned coreRate, unsigned coreChannels, bool psHint = false);
    void Reset();

    // Parse an SBR FIL extension payload (extension_type 13 or 14) for `elem` (0..3).
    // Returns false only on malformed bitstream syntax; valid frames before the first
    // header arrives are tolerated (core-upsampled fallback).
    bool Parse(Bits& b, unsigned elem, unsigned elemChannels, bool crc);

    // Apply SBR (and PS if active on a single-channel stream) to `elemChannels` core
    // 1024-sample buffers, producing `outChannels` 2048-sample buffers.
    void ApplyElement(unsigned elem, unsigned elemChannels, const float* const* corePcm,
                      float* out0, float* out1, unsigned& outChannels);

    bool Active() const { return active_; }
    bool PsActive() const { return psActive_; }
    unsigned OutputRate() const { return coreRate_ * 2; }

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    unsigned coreRate_ = 0;
    bool active_ = false, psActive_ = false;
};

}  // namespace nativeaudio
