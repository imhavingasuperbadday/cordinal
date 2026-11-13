// Tempo detection for the BPM column: an onset envelope from decoded audio, then the strongest
// repeating beat in a music-sized range. Shared by both front ends; the ReplayGain scanner feeds
// it while it already decodes, so a scan fills in BPM at no extra decode cost.
#pragma once
#include <cstddef>
#include <vector>

namespace tempo {

// Only the first minutes are analyzed: a song's tempo rarely changes later, and a DJ set or a
// radio recording would otherwise keep a large envelope for hours.
constexpr double kAnalyzeSeconds = 240.0;

class Detector {
public:
    void Begin(unsigned rate);
    // Adds interleaved stereo frames. Safe to call in any chunk size.
    void Add(const float* stereo, size_t frames);
    // The detected tempo in BPM, or 0 when the audio has no clear beat.
    int Bpm() const;

private:
    unsigned rate_ = 0;
    double step_ = 0;  // input samples per analysis sample
    double take_ = 0;
    float dc_ = 0, low_ = 0;  // 30 Hz high-pass (as a low-pass removed) and 2 kHz low-pass
    float dcCoef_ = 0, lowCoef_ = 0;
    static constexpr int kFrame = 256, kHop = 128;  // at 11025 Hz: 23 ms window, 11.6 ms steps
    float square_[kFrame] = {};
    double sum_ = 0;
    int write_ = 0, filled_ = 0, sinceHop_ = 0;
    float previous_ = 0;
    std::vector<float> onset_;
    size_t limit_ = 0;
};

}  // namespace tempo
