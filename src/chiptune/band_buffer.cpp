// Band-limited resampler. A chip's level change (Steps) or sample (Samples) is scattered into
// the output by a windowed-sinc impulse (16 taps, 64 phases); for Steps the impulses are summed
// (integrated) into band-limited steps, and a one-pole high-pass removes the chips' DC offset.
// A 32.32 fixed-point position keeps exact input-to-output timing across frames.
#include "band_buffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

void BandBuffer::Setup(double inRate, double outRate, double cutoff, Input input, size_t maxFrameOut, double highPass,
                       float gain) {
    constexpr double kPi = 3.14159265358979323846;
    const double ratio = outRate / inRate;
    factor_ = (uint64_t)std::llround(ratio * 4294967296.0);
    steps_ = input == Input::Steps;
    scale_ = gain * (steps_ ? 1.0f : (float)ratio);  // a sample's impulse covers 1/inRate seconds

    // Windowed sinc, 16 taps centered 7.5 samples late, one row per 1/64 sample of offset.
    // Each row sums to 1, so a step settles exactly on its new level.
    const double fc = std::min(cutoff / outRate, 0.5);
    const double half = kTaps / 2.0 + 0.5;
    for (int p = 0; p <= kPhases; ++p) {
        double sum = 0;
        for (int j = 0; j < kTaps; ++j) {
            const double x = j - (double)p / kPhases - (kTaps / 2.0 - 0.5);
            const double sinc = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
            const double window = 0.42 + 0.5 * std::cos(kPi * x / half) + 0.08 * std::cos(2 * kPi * x / half);
            kernel_[p][j] = (float)(sinc * window);
            sum += kernel_[p][j];
        }
        for (int j = 0; j < kTaps; ++j) kernel_[p][j] = (float)(kernel_[p][j] / sum);
    }
    // Chips output 0..max levels, not a centered signal: a one-pole high-pass removes the DC.
    hpKeep_ = (float)std::exp(-2 * kPi * highPass / outRate);
    acc_.assign(maxFrameOut + kTaps + 4, 0.0f);
    maxFrameIn_ = (uint32_t)std::max(1.0, (maxFrameOut - 2) / ratio);
    Clear();
}

void BandBuffer::Clear() {
    std::fill(acc_.begin(), acc_.end(), 0.0f);
    pos_ = 0;
    level_ = 0;
    hpIn_ = hpOut_ = 0;
}

void BandBuffer::Add(uint32_t t, float value) {
    const uint64_t at = pos_ + t * factor_;
    const size_t i = (size_t)(at >> 32);
    if (i + kTaps > acc_.size()) return;  // past the frame limit; only a damaged file gets here
    const float f = (float)(uint32_t)at * (kPhases / 4294967296.0f);
    // The product can round up to exactly kPhases for the largest fractions; keep the second
    // row inside the table and let the weight reach its final row instead.
    const int p = std::min((int)f, kPhases - 1);
    const float w = std::min(f - (float)p, 1.0f);
    const float* a = kernel_[p];
    const float* b = kernel_[p + 1];
    float* d = &acc_[i];
    value *= scale_;
    for (int j = 0; j < kTaps; ++j) d[j] += value * (a[j] + (b[j] - a[j]) * w);
}

void BandBuffer::EndFrame(uint32_t t) {
    pos_ += t * factor_;
    const uint64_t limit = (uint64_t)(acc_.size() - kTaps) << 32;
    if (pos_ > limit) pos_ = limit;
}

void BandBuffer::Read(float* out, size_t n, size_t stride) {
    n = std::min(n, Available());
    for (size_t k = 0; k < n; ++k) {
        float v = acc_[k];
        if (steps_) {
            level_ += v;
            v = (float)level_;
        }
        const float y = v - hpIn_ + hpKeep_ * hpOut_;
        hpIn_ = v;
        hpOut_ = y;
        out[k * stride] += y;
    }
    Shift(n);
}

void BandBuffer::Skip(size_t n) {
    n = std::min(n, Available());
    for (size_t k = 0; k < n; ++k) {
        float v = acc_[k];
        if (steps_) {
            level_ += v;
            v = (float)level_;
        }
        hpOut_ = v - hpIn_ + hpKeep_ * hpOut_;
        hpIn_ = v;
    }
    Shift(n);
}

void BandBuffer::Shift(size_t n) {
    const size_t used = std::min(acc_.size(), Available() + kTaps + 2);
    std::memmove(acc_.data(), acc_.data() + n, (used - n) * sizeof(float));
    std::fill(acc_.begin() + (used - n), acc_.begin() + used, 0.0f);
    pos_ -= (uint64_t)n << 32;
}
