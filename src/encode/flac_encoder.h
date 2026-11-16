// cordinal FLAC encoder (RFC 9639): frames from interleaved integer PCM, and the STREAMINFO block.
// Each channel takes the cheapest of constant, verbatim, fixed and LPC prediction (orders up to 12,
// Tukey-windowed autocorrelation), with wasted bits and partitioned Rice codes; two-channel audio
// also tries left/side, side/right and mid/side. Streams stay within FLAC's streamable subset
// whenever the sample rate has a frame-header code.
#pragma once
#include "bitwriter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cordinal::encode {

class FlacEncoder {
public:
    static constexpr unsigned kBlockFrames = 4096;
    // FLAC's limits as this encoder writes them: 4..32 bits, 1..8 channels, 1..1,048,575 Hz and
    // fewer than 2^36 frames.
    static bool Supports(unsigned rate, unsigned channels, unsigned bits, uint64_t frames);
    FlacEncoder(unsigned rate, unsigned channels, unsigned bits);
    ~FlacEncoder();
    // Appends the next frame: `frames` interleaved, right-justified frames (PcmReader's layout),
    // kBlockFrames of them except in the stream's last frame.
    void Frame(const int32_t* pcm, size_t frames, std::string& out);
    // The smallest and largest frame written so far, in bytes; 0 before the first.
    uint32_t MinFrameBytes() const { return minFrame_; }
    uint32_t MaxFrameBytes() const { return maxFrame_; }

private:
    struct Plan;
    void Choose(const int32_t* x, size_t n, unsigned bits, Plan& plan);
    void Write(const Plan& plan, size_t n, unsigned bits);
    unsigned rate_, channels_, bits_;
    uint32_t number_ = 0, minFrame_ = 0, maxFrame_ = 0;
    std::vector<std::vector<int32_t>> channel_;
    std::vector<Plan> plans_;
    std::vector<uint32_t> trial_;
    std::vector<double> window_, windowed_;
    BitWriter writer_;
};

// The 34-byte STREAMINFO body. Frame sizes of 0 mean unknown, and an MD5 of zeros means none.
std::string FlacStreamInfo(unsigned rate, unsigned channels, unsigned bits, uint64_t frames, uint32_t minFrame,
                           uint32_t maxFrame, const std::array<uint8_t, 16>& md5);

}  // namespace cordinal::encode
