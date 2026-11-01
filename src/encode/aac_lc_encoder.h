// cordinal AAC-LC encoder (ADTS output).
//
// Original implementation of the ISO/IEC 14496-3 low-complexity encoder:
// sine-window MDCT, band quantization, section/codebook selection, Huffman
// emission and ADTS framing. Only the normative codebooks and band edges are
// shared with cordinal's own AAC decoder.
#pragma once
#include <cstdint>
#include <vector>

namespace cordinal::encode {

class AacLcEncoder {
public:
    AacLcEncoder();
    ~AacLcEncoder();
    AacLcEncoder(const AacLcEncoder&) = delete;
    AacLcEncoder& operator=(const AacLcEncoder&) = delete;

    // sample_rate: 32000, 44100 or 48000. channels: 1 or 2.
    // bitrate is the target ADTS payload rate in kbit/s.
    bool Open(int sample_rate, int channels, int bitrate);
    bool Ready() const { return ready_; }
    int SamplesPerFrame() const { return 1024; }
    int SampleRate() const { return sample_rate_; }
    int Channels() const { return channels_; }
    int Bitrate() const { return bitrate_; }

    // Encodes one 1024-sample block per channel. One ADTS frame is produced per
    // call; the first two calls emit the codec's priming frames.
    bool Encode(const float* const* channel_data, std::vector<uint8_t>& frame);

    // Emits the final three frames after the last input block. Call exactly once.
    void Flush(std::vector<std::vector<uint8_t>>& frames);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    int sample_rate_ = 0;
    int channels_ = 0;
    int bitrate_ = 0;
};

}  // namespace cordinal::encode
