// In-process AAC-LC and MP3 encoding, shared by exports, device copies and browser data saver.
// The codec sources are built into cordinal (encode/); no system encoder or external process is
// used. PCM arrives in bounded 48 kHz, 16-bit stereo chunks and leaves as ADTS or raw MP3 frames.
#pragma once
#include "encode/aac_lc_encoder.h"
#include "encode/mp3_encoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace aac {
using Sender = std::function<bool(const char*, size_t)>;
constexpr unsigned kEncoderDelay = 3072;  // priming and MDCT overlap, trimmed by the M4A writer

class Encoder {
public:
    Encoder() = default;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    bool Open(unsigned kbps, bool mp3 = false) {
        if (open_) return false;
        if (mp3) {
            if (!mp3_.Open(48000, 2, (int)kbps)) return false;
            block_ = (size_t)mp3_.SamplesPerFrame();
        } else {
            if (!aac_.Open(48000, 2, (int)kbps)) return false;
            block_ = (size_t)aac_.SamplesPerFrame();
        }
        open_ = true;
        return block_ > 0 && block_ <= left_.size();
    }

    bool Write(const char* bytes, size_t size, const Sender& send, const std::atomic<bool>& cancel) {
        if (!open_ || finished_ || size % 4) return false;
        for (size_t at = 0; at < size && !cancel;) {
            const size_t n = std::min(block_ - used_, (size - at) / 4);
            for (size_t i = 0; i < n; ++i) {
                const auto* p = (const uint8_t*)bytes + at + i * 4;
                const int16_t l = (int16_t)(p[0] | p[1] << 8);
                const int16_t r = (int16_t)(p[2] | p[3] << 8);
                left_[used_ + i] = (float)(l / 32768.0);
                right_[used_ + i] = (float)(r / 32768.0);
            }
            used_ += n;
            at += n * 4;
            frames += n;
            if (used_ == block_ && !Emit(send, cancel)) return false;
        }
        return !cancel;
    }

    bool Finish(const Sender& send, const std::atomic<bool>& cancel) {
        if (!open_ || finished_ || cancel) return false;
        if (used_) {
            std::fill(left_.begin() + used_, left_.begin() + block_, 0.f);
            std::fill(right_.begin() + used_, right_.begin() + block_, 0.f);
            if (!Emit(send, cancel)) return false;
        }
        if (aac_.Ready()) {
            std::vector<std::vector<uint8_t>> tail;
            aac_.Flush(tail);
            for (const auto& frame : tail)
                if (cancel || (!frame.empty() && !send((const char*)frame.data(), frame.size()))) return false;
        }
        finished_ = true;
        return !cancel;
    }

    uint64_t frames = 0;

private:
    bool Emit(const Sender& send, const std::atomic<bool>& cancel) {
        if (cancel) return false;
        const float* channels[]{left_.data(), right_.data()};
        std::vector<uint8_t> frame;
        const bool ok = aac_.Ready() ? aac_.Encode(channels, frame) : mp3_.Encode(channels, frame);
        used_ = 0;
        if (!ok) return false;
        return frame.empty() || send((const char*)frame.data(), frame.size());
    }

    cordinal::encode::AacLcEncoder aac_;
    cordinal::encode::Mp3Encoder mp3_;
    size_t block_ = 0, used_ = 0;
    bool open_ = false, finished_ = false;
    std::array<float, 1152> left_{}, right_{};
};
}  // namespace aac
