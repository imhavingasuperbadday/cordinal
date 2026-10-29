// Live decoder checks: feed real encoded audio in short reads and compare the resulting PCM with
// the existing file decoder. The stream never pretends it has a seekable duration.
#include "selftest_common.h"
#include "audio_live.h"
#include "audio_packets.h"

namespace {
class FragmentedSource final : public LiveAudioSource {
public:
    explicit FragmentedSource(std::string bytes) : bytes_(std::move(bytes)) {}
    size_t Read(uint8_t* out, size_t bytes) override {
        size_t n = std::min({bytes, size_t(7), bytes_.size() - at_});
        memcpy(out, bytes_.data() + at_, n);
        at_ += n;
        return n;
    }
    void Abort() override { at_ = bytes_.size(); }
    std::wstring Problem() const override { return L"The stream ended"; }
private:
    std::string bytes_;
    size_t at_ = 0;
};

// Reframe the same raw AAC packets as ADTS, as a live station does. This keeps the test independent
// of FFmpeg at runtime and uses the regular AAC fixture's audio as its reference.
std::string Adts(const std::wstring& path) {
    nativeaudio::PacketFile file;
    if (!file.OpenMp4(path) || file.codec != nativeaudio::AudioAac || file.config.size() < 2) return {};
    unsigned object = file.config[0] >> 3;
    unsigned index = ((file.config[0] & 7) << 1) | (file.config[1] >> 7);
    unsigned channels = (file.config[1] >> 3) & 15;
    if (object != 2 || index >= 13 || !channels || channels > 7) return {};
    std::string out;
    for (size_t i = 0; i < std::min<size_t>(file.packets.size(), 100); ++i) {
        const auto& packet = file.packets[i];
        const uint8_t* data = file.Data(i);
        size_t n = packet.bytes + 7;
        if (!data || n > 8191) return {};
        const uint8_t h[] = {0xff, 0xf1, (uint8_t)(((object - 1) << 6) | (index << 2) | (channels >> 2)),
                             (uint8_t)(((channels & 3) << 6) | (n >> 11)), (uint8_t)(n >> 3),
                             (uint8_t)(((n & 7) << 5) | 0x1f), 0xfc};
        out.append((const char*)h, sizeof(h));
        out.append((const char*)data, packet.bytes);
    }
    return out;
}

bool MatchesFile(Decoder& live, const std::vector<float>& expected, double& best, size_t& got) {
    std::vector<float> actual(48000 * 2);
    got = live.Read(actual.data(), actual.size() / 2);
    if (got != actual.size() / 2 || expected.size() < actual.size() + 8192 || live.Duration() != 0 || live.Seek(1))
        return false;
    // Container priming differs from a stream joined at an arbitrary frame. Find that initial
    // offset, then compare several thousand contiguous samples after the codec state has settled.
    best = 1e100;
    for (int offset = -2048; offset <= 2048; ++offset) {
        double error = 0;
        for (int i = 10000; i < 11024; i += 8) {
            double d = actual[(size_t)i * 2] - expected[(size_t)(i + offset) * 2];
            error += d * d;
        }
        best = std::min(best, error);
    }
    return std::isfinite(best) && best < 0.001;
}
}  // namespace

void TestLiveAudio(const std::wstring& media) {
    testlog::Cases cases;
    for (int codec = 0; codec < 2; ++codec) {
        const std::wstring path = media + (codec ? L"\\sine.m4a" : L"\\sine.mp3");
        unsigned rate = 0;
        auto expected = DecodeAll(path, rate);
        std::string bytes = codec ? Adts(path) : ReadTestBytes(path);
        size_t byteCount = bytes.size();
        auto source = std::make_shared<FragmentedSource>(std::move(bytes));
        auto live = OpenLiveAudio(source);
        double best = -1; size_t got = 0;
        bool ok = live && live->Rate() == rate && MatchesFile(*live, expected, best, got);
        if (!ok && g_log) fwprintf(g_log, L"live %ls: bytes=%zu, opened=%d, rate=%u/%u, frames=%zu, error=%g\n",
            codec ? L"AAC" : L"MP3", byteCount, live != nullptr, live ? live->Rate() : 0, rate, got, best);
        cases.Add(ok, codec ? L"AAC-LC" : L"MP3");
    }
    Check(cases.Passed(), L"live MP3 and AAC-LC decode fragmented input with the file decoder's sound and no seeking%ls",
          cases.failed.c_str());
}
