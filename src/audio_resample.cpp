// Band-limited sinc resampler used by the playback pipeline to convert a
// decoder's rate to the output device rate. A polyphase table with a Kaiser
// window is built once per rate pair: one exact row per rational phase where
// that fits (44.1 <-> 48 kHz and every whole-number ratio), otherwise 256 rows
// with linear interpolation between them. The fractional cursor (idx_/num_)
// carries position across pushes so streaming stays sample-accurate. The kernel
// grows for downsampling so high source rates retain the same transition width.
#include "decoder.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <emmintrin.h>
#include "audio_kernels.h"

// ---------------------------------------------------------------------------
// Resampler

namespace {
// Zeroth-order modified Bessel function via its series; used for the Kaiser
// window that controls the filter's stopband rolloff.
double BesselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 30; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}
}  // namespace

// Build the polyphase table for inRate -> outRate. The ratio is reduced by its
// GCD first; an unchanged reduced pair keeps the existing table. Cutoff sits at
// the lower of the two rates with a little headroom to tame aliasing.
//
// Every output frame lands on one of out_ rational phases. When a row for each of
// them, computed exactly, fits in the memory the 257 interpolated rows would take,
// those are used instead: no interpolation error between phases (which held the
// passband to about 95 dB SNR), and the rows saved pay for a kernel 25% longer
// with a Kaiser beta of 12.3 instead of 10. That keeps the same transition band,
// so the same treble, while the stopband and passband ripple move from about
// -100 dB to about -120 dB. The longer kernel costs a little arithmetic, no memory.
void Resampler::Init(unsigned inRate, unsigned outRate) {
    fast_=nativeaudio::FastAudioKernels();
    inRate_ = inRate;
    unsigned a = inRate, b = outRate;
    while (b) {
        unsigned t = a % b;
        a = b;
        b = t;
    }
    if (a) {
        inRate /= a;
        outRate /= a;
    }
    if (inRate == in_ && outRate == out_ && (table_ || inRate == outRate)) {
        Reset();
        return;
    }
    in_ = inRate;
    out_ = outRate;
    table_.reset();
    if (in_ != out_) {
        const double ratio = std::min(1.0, (double)outRate / inRate);
        // A short fixed kernel loses treble and barely rejects aliases at high downsampling
        // ratios. Keep its width in output frames, with a bound for unusual file/speed rates.
        auto width = [ratio](double outputTaps) { return std::ceil(outputTaps / ratio / 8) * 8; };
        const unsigned interpolatedTaps = (unsigned)std::min(8192.0, width(128));
        const double exactTaps = width(160);
        exact_ = out_ <= (unsigned)kPhases && exactTaps <= 8192 &&
                 out_ * exactTaps <= (kPhases + 1.0) * interpolatedTaps;
        taps_ = exact_ ? (unsigned)exactTaps : interpolatedTaps;
        // Kaiser's beta for about 120 dB of rejection, and for about 100 dB.
        const double beta = exact_ ? 12.27 : 10.0, half = taps_ / 2.0;
        const double cutoff = ratio * 0.95;
        const unsigned rows = exact_ ? out_ : kPhases + 1, step = exact_ ? out_ : kPhases;
        table_.reset(new float[rows * taps_]);
        const double normalization = BesselI0(beta);
        for (unsigned p = 0; p < rows; ++p) {
            double frac = (double)p / step, sum = 0;
            float* row = &table_[p * taps_];
            for (unsigned k = 0; k < taps_; ++k) {
                double x = (double)k - (taps_ / 2 - 1) - frac;
                double s = x == 0 ? 1.0 : std::sin(3.14159265358979 * cutoff * x) / (3.14159265358979 * cutoff * x);
                double r = x / half;
                double w = std::fabs(r) >= 1 ? 0 : BesselI0(beta * std::sqrt(1 - r * r)) / normalization;
                row[k] = (float)(cutoff * s * w);
                sum += row[k];
            }
            for (unsigned k = 0; k < taps_; ++k) row[k] = (float)(row[k] / sum);  // unity DC gain
        }
        // Since p=floor(num*kPhases/out_) >= num, ascending writes can reuse
        // this allocation without clobbering a phase still needed as input.
        if (fast_ && exact_) nativeaudio::AudioResampleLayoutAvx2(table_.get(),taps_,out_);
        else if (fast_ && out_<=kPhases) nativeaudio::AudioResamplePrepareAvx2(table_.get(),taps_,out_);
    }
    Reset();
}

// Zero the filter history; the cursor starts half a kernel in.
void Resampler::Reset() {
    hist_.assign(taps_, 0.0f);  // zero history before the first sample
    idx_ = taps_ / 2;
    num_ = 0;
}

// New input frames needed to drain every output sample the filter can currently
// produce; the player uses this to decide how far it may seek within the buffer.
double Resampler::PendingOutput() const {
    if (Passthrough()) return 0;
    double pos = idx_ + (double)num_ / out_;
    return std::max(0.0, ((double)hist_.size() / 2 - pos) * out_ / in_);
}

// Push appends to the caller's buffer (streamed output); Process overwrites it
// (one-shot conversion).
void Resampler::Push(const float* in, size_t frames, std::vector<float>& out) {
    Convert(in,frames,out,true);
}
void Resampler::Process(const float* in,size_t frames,std::vector<float>& out) {
    Convert(in,frames,out,false);
}
// Core streaming loop. in_/out_ are the reduced rates; step/remainder advance the
// input cursor by in_/out_ per output sample, and num_ carries the fractional
// part. The SSE2 path processes both channels at once; history older than half a
// kernel is dropped each call.
void Resampler::Convert(const float* in,size_t frames,std::vector<float>& out,bool append) {
    if (Passthrough()) {
        if (append) out.insert(out.end(), in, in + frames * 2);
        else { out.resize(frames*2);if (frames) memcpy(out.data(),in,frames*2*sizeof(float)); }
        return;
    }
    hist_.insert(hist_.end(), in, in + frames * 2);
    const size_t available = hist_.size() / 2;
    const float* h = hist_.data();
    const size_t begin=append?out.size():0;
    uint64_t distance=idx_+taps_/2<available?uint64_t(available-taps_/2-idx_)*out_-num_:0;
    const size_t count=(size_t)((distance+in_-1)/in_);
    out.resize(begin+count*2);
    const unsigned step=in_/out_,remainder=in_%out_;
    const double phaseScale=(double)kPhases/out_;
    if (fast_) {
        if (count) nativeaudio::AudioResampleAvx2(h,table_.get(),taps_,count,step,remainder,out_,idx_,num_,out.data()+begin);
    }
    else for (size_t frame=0;frame<count;++frame) {
        const float* s = h + (idx_ - (taps_ / 2 - 1)) * 2;
        __m128 a=_mm_setzero_ps(),b=a;
        if (exact_) {  // the row for this very phase
            const float* r = &table_[(size_t)num_ * taps_];
            for (unsigned k=0;k<taps_;k+=4) {
                __m128 w=_mm_loadu_ps(r+k);
                a=_mm_add_ps(a,_mm_mul_ps(_mm_loadu_ps(s+k*2),_mm_unpacklo_ps(w,w)));
                b=_mm_add_ps(b,_mm_mul_ps(_mm_loadu_ps(s+k*2+4),_mm_unpackhi_ps(w,w)));
            }
        } else {
            double f = (double)(unsigned)num_ * phaseScale;
            int p = (int)f;
            const float* r0 = &table_[p * taps_];
            const float* r1 = r0 + taps_;
            __m128 blend=_mm_set1_ps((float)(f - p));
            for (unsigned k=0;k<taps_;k+=4) {
                __m128 first=_mm_loadu_ps(r0+k),second=_mm_loadu_ps(r1+k);
                __m128 w=_mm_add_ps(first,_mm_mul_ps(_mm_sub_ps(second,first),blend));
                a=_mm_add_ps(a,_mm_mul_ps(_mm_loadu_ps(s+k*2),_mm_unpacklo_ps(w,w)));
                b=_mm_add_ps(b,_mm_mul_ps(_mm_loadu_ps(s+k*2+4),_mm_unpackhi_ps(w,w)));
            }
        }
        __m128 sum=_mm_add_ps(a,b);float lanes[4];_mm_storeu_ps(lanes,sum);
        out[begin+frame*2]=lanes[0]+lanes[2];out[begin+frame*2+1]=lanes[1]+lanes[3];
        num_+=remainder;idx_+=step;
        if (num_>=out_) { num_-=out_;++idx_; }
    }
    // Drop input no longer needed, keeping the filter's history.
    size_t keepFrom = idx_ > (size_t)(taps_ / 2 - 1) ? idx_ - (taps_ / 2 - 1) : 0;
    keepFrom = std::min(keepFrom, hist_.size() / 2);
    if (keepFrom > 0) {
        hist_.erase(hist_.begin(), hist_.begin() + (std::ptrdiff_t)(keepFrom * 2));
        idx_ -= keepFrom;
    }
}

// Drain the tail of the filter by pushing silence, then reset. Passthrough emits
// nothing.
void Resampler::Flush(std::vector<float>& out) {
    if (Passthrough()) return;
    std::vector<float> zeros(taps_);
    Push(zeros.data(), taps_ / 2, out);
    Reset();
}
