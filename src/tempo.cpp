// Tempo detection: a mono energy envelope is sampled at 11 kHz, compressed, and differentiated
// into onsets. The autocorrelation of those onsets is scored at every music tempo, with the
// second and third beats added so a steady pulse beats a single quiet bar. The fastest strong
// peak wins, which keeps a half-time reading from winning over the beat people count.
#include "tempo.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace tempo {
namespace {

constexpr double kAnalysisRate = 11025.0;
constexpr double kMinBpm = 50, kMaxBpm = 210;
constexpr double kMinSeconds = 4.0;
constexpr double kMinScore = 0.2;   // below this there is no repeating beat
constexpr double kPeakShare = 0.7;  // a beat this close to the best one still counts
constexpr double kLagStep = 0.02;   // analysis steps between candidate lags

}  // namespace

void Detector::Begin(unsigned rate) {
    rate_ = rate;
    step_ = rate ? rate / kAnalysisRate : 0;
    take_ = 0;
    dc_ = low_ = 0;
    dcCoef_ = rate ? (float)(1 - std::exp(-2 * 3.141592653589793 * 30 / rate)) : 0;
    lowCoef_ = rate ? (float)(1 - std::exp(-2 * 3.141592653589793 * 2000 / rate)) : 0;
    sum_ = 0;
    write_ = filled_ = sinceHop_ = 0;
    previous_ = 0;
    onset_.clear();
    onset_.reserve(rate ? (size_t)(kAnalyzeSeconds * kAnalysisRate / kHop) : 0);
    limit_ = rate ? (size_t)(kAnalyzeSeconds * kAnalysisRate / kHop) : 0;
    std::fill(std::begin(square_), std::end(square_), 0.0f);
}

void Detector::Add(const float* stereo, size_t frames) {
    if (!rate_ || !step_ || onset_.size() >= limit_) return;
    for (size_t i = 0; i < frames; ++i) {
        const float mono = (stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f;
        dc_ += dcCoef_ * (mono - dc_);
        low_ += lowCoef_ * (mono - dc_ - low_);
        take_ += 1.0;
        if (take_ < step_) continue;
        take_ -= step_;
        const float sample = low_;
        const float square = sample * sample;
        sum_ += square - square_[write_];
        square_[write_] = square;
        if (++write_ == kFrame) write_ = 0;
        if (filled_ < kFrame) {
            if (++filled_ < kFrame) continue;
            sinceHop_ = 0;
        } else if (++sinceHop_ < kHop) {
            continue;
        } else {
            sinceHop_ = 0;
        }
        const float energy = (float)(sum_ / kFrame);
        const float level = std::log1p(energy * 1000.0f);
        onset_.push_back(std::max(0.0f, level - previous_));
        previous_ = level;
    }
}

int Detector::Bpm() const {
    const size_t count = onset_.size();
    if (count < (size_t)(kMinSeconds * kAnalysisRate / kHop)) return 0;
    double mean = 0;
    for (float value : onset_) mean += value;
    mean /= count;
    std::vector<double> centered(count);
    double energy = 0;
    for (size_t i = 0; i < count; ++i) {
        centered[i] = onset_[i] - mean;
        energy += centered[i] * centered[i];
    }
    if (!(energy > 1e-9)) return 0;

    const double frameRate = kAnalysisRate / kHop;
    const double minLag = 60 * frameRate / kMaxBpm, maxLag = 60 * frameRate / kMinBpm;
    const int maxAcf = (int)std::min<double>(count - 1, std::ceil(3 * maxLag) + 2);
    std::vector<double> acf((size_t)maxAcf + 1, 0);
    for (int lag = 1; lag <= maxAcf; ++lag) {
        double sum = 0;
        for (size_t i = 0; i + lag < count; ++i) sum += centered[i] * centered[i + lag];
        acf[(size_t)lag] = sum / energy;
    }
    // Read the autocorrelation between whole-frame lags, so a tempo that doesn't land on one
    // frame (most of them) isn't measured low.
    auto at = [&](double lag) {
        if (!(lag > 0) || lag > maxAcf) return 0.0;
        const int whole = (int)lag;
        const double part = lag - whole;
        return acf[(size_t)whole] * (1 - part) +
               (whole + 1 <= maxAcf ? acf[(size_t)whole + 1] * part : 0);
    };
    auto score = [&](double lag) { return at(lag) + 0.5 * at(2 * lag) + 0.25 * at(3 * lag); };

    std::vector<std::pair<double, double>> candidates;  // lag, score
    for (double lag = minLag; lag <= maxLag; lag += kLagStep) candidates.emplace_back(lag, score(lag));
    if (candidates.size() < 3) return 0;
    double best = 0;
    for (const auto& [lag, value] : candidates) best = std::max(best, value);
    if (best < kMinScore) return 0;

    // Candidates run fastest to slowest. A peak's half tempo is often a little stronger, so take
    // the fastest local maximum that stays close to the best score rather than the strongest one.
    size_t chosen = 0;
    bool found = false;
    for (size_t i = 1; i + 1 < candidates.size(); ++i) {
        const double value = candidates[i].second;
        if (value >= kPeakShare * best && value >= candidates[i - 1].second &&
            value >= candidates[i + 1].second) {
            chosen = i;
            found = true;
            break;
        }
    }
    if (!found) {
        for (size_t i = 0; i < candidates.size(); ++i)
            if (candidates[i].second == best) {
                chosen = i;
                break;
            }
    }
    const double left = candidates[chosen - (chosen > 0 ? 1 : 0)].second;
    const double middle = candidates[chosen].second;
    const double right = candidates[chosen + (chosen + 1 < candidates.size() ? 1 : 0)].second;
    const double denominator = left - 2 * middle + right;
    double delta = denominator ? 0.5 * (left - right) / denominator : 0;
    delta = std::clamp(delta, -0.5, 0.5) * kLagStep;
    const double lag = candidates[chosen].first + delta;
    const double bpm = 60 * frameRate / lag;
    if (!(bpm >= kMinBpm && bpm <= kMaxBpm)) return 0;
    return (int)std::lround(bpm);
}

}  // namespace tempo
