// The MPEG-1 polyphase synthesis filterbank (ISO/IEC 11172-3), shared with other subband codecs:
// Musepack uses the same 32-band filter. Implemented in audio_mpeg.cpp, with its AVX2 kernels.
#pragma once
#include <memory>

namespace nativeaudio {
class Polyphase {
public:
    Polyphase();
    ~Polyphase();
    void Reset();
    // Eighteen consecutive time slots of one channel (0 or 1): in[18][32] subband samples in
    // [-1, 1] scale, out[18][32] PCM. The history carries on to the next call.
    void Synthesize18(const float* in, unsigned channel, float* out);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace nativeaudio
