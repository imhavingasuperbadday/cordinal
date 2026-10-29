// Recognize a radio stream from its bytes, then replay the probe into the native live decoder.
#include "audio_live.h"

#include <array>

namespace {
class PrefixSource final : public LiveAudioSource {
public:
    PrefixSource(std::shared_ptr<LiveAudioSource> source, const std::array<uint8_t, 7>& prefix)
        : source_(std::move(source)), prefix_(prefix) {}
    size_t Read(uint8_t* out, size_t bytes) override {
        if (at_ == prefix_.size()) return source_->Read(out, bytes);
        size_t n = std::min(bytes, prefix_.size() - at_);
        memcpy(out, prefix_.data() + at_, n);
        at_ += n;
        return n;
    }
    void Abort() override { source_->Abort(); }
    std::wstring Problem() const override { return source_->Problem(); }
private:
    std::shared_ptr<LiveAudioSource> source_;
    std::array<uint8_t, 7> prefix_;
    size_t at_ = 0;
};
}  // namespace

std::unique_ptr<Decoder> OpenLiveAudio(std::shared_ptr<LiveAudioSource> source) {
    if (!source) return nullptr;
    std::array<uint8_t, 7> p{};
    if (!ReadLiveBytes(*source, p.data(), p.size()) || !SkipLiveId3(*source, p.data(), p.size())) return nullptr;
    for (size_t skipped = 0; skipped <= 65536; ++skipped) {
        bool sync = p[0] == 255 && (p[1] & 0xe0) == 0xe0;
        bool aac = sync && (p[1] & 0xf6) == 0xf0 && (p[2] >> 6) == 1 && ((p[2] >> 2) & 15) < 13;
        bool mpeg = sync && ((p[1] >> 3) & 3) != 1 && (p[1] & 6) && (p[2] >> 4) &&
                    (p[2] >> 4) != 15 && ((p[2] >> 2) & 3) != 3;
        if (aac || mpeg) {
            auto prefix = std::make_shared<PrefixSource>(std::move(source), p);
            return aac ? OpenLiveAac(prefix) : OpenLiveMpeg(prefix);
        }
        memmove(p.data(), p.data() + 1, p.size() - 1);
        if (!ReadLiveBytes(*source, p.data() + p.size() - 1, 1)) return nullptr;
    }
    return nullptr;
}
