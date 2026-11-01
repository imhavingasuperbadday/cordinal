// cordinal MP3 (MPEG-1 Layer III) encoder.
//
// The encoder is written from the ISO/IEC 11172-3 transform, quantization and
// bitstream definitions. The only tables imported from the decoder are the
// normative Huffman code trees, the prototype filter and the scalefactor-band
// edges, all of which are format data rather than decoder logic.
#pragma once
#include <cstdint>
#include <vector>

namespace cordinal::encode {

class Mp3Encoder {
public:
    Mp3Encoder();
    ~Mp3Encoder();
    Mp3Encoder(const Mp3Encoder&) = delete;
    Mp3Encoder& operator=(const Mp3Encoder&) = delete;

    // sample_rate: 32000, 44100 or 48000. channels: 1 or 2.
    // bitrate: a standard MPEG-1 Layer III rate in kbit/s.
    bool Open(int sample_rate, int channels, int bitrate);
    bool Ready() const { return ready_; }
    int SamplesPerFrame() const { return 1152; }
    int SampleRate() const { return sample_rate_; }
    int Channels() const { return channels_; }
    int Bitrate() const { return bitrate_; }

    // Encodes exactly SamplesPerFrame() interleaved-float samples per channel.
    // channel_data[0] is left (or mono), channel_data[1] is right.
    // Appends one complete MP3 frame to `frame`.
    bool Encode(const float* const* channel_data, std::vector<uint8_t>& frame);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    int sample_rate_ = 0;
    int channels_ = 0;
    int bitrate_ = 0;
};

}  // namespace cordinal::encode
