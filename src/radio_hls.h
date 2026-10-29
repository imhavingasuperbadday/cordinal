// HLS playlists and audio segments, shared by the Windows and Linux radio workers.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace radio {
struct HlsRange { uint64_t offset = 0, size = 0; };
struct HlsSegment {
    std::string url, init, key, initKey;
    HlsRange range, initRange;
    std::array<uint8_t, 16> iv{};
    std::array<uint8_t, 16> initIv{};
    uint64_t sequence = 0;
    bool explicitIv = false, discontinuity = false;
    bool gap = false;
};
struct HlsPlaylist {
    std::vector<HlsSegment> segments;
    std::string variant;
    unsigned target = 6;
    bool ended = false;
};
std::string ResolveRadioUrl(std::string_view base, std::string_view relative);
bool ParseHls(std::string_view text, const std::string& base, HlsPlaylist& playlist, std::wstring& problem);
// State survives segment boundaries; transport headers and MP4 sample tables never reach a decoder.
class HlsAudio {
public:
    bool Init(std::string_view bytes);
    bool Segment(std::string_view bytes, std::string& audio);
    void Discontinuity();
private:
    bool Transport(std::string_view bytes, std::string& audio);
    bool Fragment(std::string_view bytes, std::string& audio);
    int pmt_ = -1, audioPid_ = -1, continuity_ = -1;
    std::string pat_, pmtBytes_, pesHeader_;
    size_t pesLeft_ = 0;
    uint32_t track_ = 0, sampleSize_ = 0;
    unsigned config_ = 0;
};
bool DecryptRadioAes(std::string& bytes, const std::string& key, std::array<uint8_t, 16> iv);
bool PlayHls(std::string url, std::string playlist, const std::atomic<bool>& cancel,
             const std::function<bool(std::string_view)>& push,
             const std::function<bool(unsigned)>& wait, std::wstring& problem);
}  // namespace radio
