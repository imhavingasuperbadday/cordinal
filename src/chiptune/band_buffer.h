// Turns a chip's output into 44100 Hz samples without aliasing.
//
// A chip either changes its output level at exact clock times (square, triangle and noise
// channels) or produces a stream of samples at its own rate (the SNES DSP, the YM2612). Both
// are written as weighted impulses at their exact time: an impulse filtered by a windowed sinc
// lands on the nearest output samples, and summing (integrating) impulses of level changes
// gives band-limited steps. Times are in input units (chip clocks or chip samples) counted
// from the start of the current frame.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

class BandBuffer {
public:
    enum class Input { Steps, Samples };

    // inRate: input units per second. cutoff: the low-pass frequency in Hz. highPass: where the
    // output's DC blocker starts, in Hz (consoles' output stages cut bass too). gain: output scale.
    void Setup(double inRate, double outRate, double cutoff, Input input, size_t maxFrameOut = 4096,
               double highPass = 8, float gain = 1);
    void Clear();

    // A level change (Steps) or one sample (Samples) at input time t in this frame.
    void Add(uint32_t t, float value);
    // Ends the frame after t input units; the next frame's times start at zero.
    void EndFrame(uint32_t t);
    // Output samples that are complete.
    size_t Available() const { return (size_t)(pos_ >> 32); }
    // Adds n complete samples to out[0], out[stride], ... and removes them.
    void Read(float* out, size_t n, size_t stride);
    // Removes n complete samples without reading them.
    void Skip(size_t n);
    // The most input units a frame may hold so that its output fits the buffer.
    uint32_t MaxFrame() const { return maxFrameIn_; }

private:
    static constexpr int kTaps = 16, kPhases = 64;
    void Shift(size_t n);

    float kernel_[kPhases + 1][kTaps] = {};
    // Per-output-sample sum of scattered impulses; the last kTaps entries carry the filter tail
    // that spills into the next frame.
    std::vector<float> acc_;
    uint64_t factor_ = 0;  // output samples per input unit, 32.32 fixed point
    uint64_t pos_ = 0;     // frame start in output samples, 32.32 fixed point
    float scale_ = 1;
    bool steps_ = true;
    double level_ = 0;  // running sum of steps
    float hpIn_ = 0, hpOut_ = 0, hpKeep_ = 0;
    uint32_t maxFrameIn_ = 0;
};
