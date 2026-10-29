// In-process AAC-LC and MP3 encoding, shared by exports, device copies and browser data saver.
// The codec sources are built into cordinal (encode/); no system encoder or external process is
// used. PCM arrives in bounded 48 kHz, 16-bit stereo chunks and leaves as ADTS or raw MP3 frames.
#pragma once
#include "encode/glint.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>

namespace aac {
using Sender = std::function<bool(const char*, size_t)>;
constexpr unsigned kEncoderDelay = 2048;  // AAC lookahead and MDCT overlap, trimmed by the M4A writer

class Encoder {
public:
    ~Encoder() { if (aac_) glint_aac_destroy(aac_); if (mp3_) glint_destroy(mp3_); }
    Encoder() = default;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    bool Open(unsigned kbps, bool mp3 = false) {
        if (aac_ || mp3_) return false;
        if (mp3) {
            glint_config cfg{};
            cfg.sample_rate = 48000; cfg.num_channels = 2; cfg.mode = GLINT_JOINT;
            cfg.bitrate = (int)kbps; cfg.simd = GLINT_SIMD_SSE2; cfg.quality = GLINT_QUALITY_NORMAL;
            mp3_ = glint_create(&cfg);
            if (mp3_) block_ = (size_t)glint_samples_per_frame(mp3_);
        } else {
            glint_aac_config cfg{};
            cfg.sample_rate = 48000; cfg.num_channels = 2; cfg.bitrate = (int)kbps;
            cfg.quality = GLINT_QUALITY_NORMAL;
            aac_ = glint_aac_create(&cfg);
            if (aac_) block_ = (size_t)glint_aac_samples_per_frame(aac_);
        }
        return block_ && block_ <= left_.size();
    }
    bool Write(const char* bytes, size_t size, const Sender& send, const std::atomic<bool>& cancel) {
        if (!block_ || finished_ || size % 4) return false;
        for (size_t at = 0; at < size && !cancel;) {
            const size_t n = std::min(block_ - used_, (size - at) / 4);
            for (size_t i = 0; i < n; ++i) {
                const auto* p = (const uint8_t*)bytes + at + i * 4;
                left_[used_ + i] = (int16_t)(p[0] | p[1] << 8);
                right_[used_ + i] = (int16_t)(p[2] | p[3] << 8);
            }
            used_ += n; at += n * 4; frames += n;
            if (used_ == block_ && !Emit(send, cancel)) return false;
        }
        return !cancel;
    }
    bool Finish(const Sender& send, const std::atomic<bool>& cancel) {
        if (!block_ || finished_ || cancel) return false;
        if (used_) {
            std::fill(left_.begin() + used_, left_.begin() + block_, (int16_t)0);
            std::fill(right_.begin() + used_, right_.begin() + block_, (int16_t)0);
            if (!Emit(send, cancel)) return false;
        }
        finished_ = true;
        int size = 0;
        const uint8_t* bytes = aac_ ? glint_aac_flush(aac_, &size) : glint_flush(mp3_, &size);
        return !cancel && size >= 0 && (!size || (bytes && send((const char*)bytes, (size_t)size)));
    }
    uint64_t frames = 0;
private:
    bool Emit(const Sender& send, const std::atomic<bool>& cancel) {
        if (cancel) return false;
        const int16_t* channels[]{left_.data(), right_.data()};
        int size = 0;
        const uint8_t* bytes = aac_ ? glint_aac_encode(aac_, channels, &size) : glint_encode(mp3_, channels, &size);
        used_ = 0;
        return size >= 0 && (!size || (bytes && send((const char*)bytes, (size_t)size)));
    }
    glint_aac_t aac_ = nullptr;
    glint_t mp3_ = nullptr;
    size_t block_ = 0, used_ = 0;
    bool finished_ = false;
    std::array<int16_t, 1152> left_{}, right_{};
};
}  // namespace aac
