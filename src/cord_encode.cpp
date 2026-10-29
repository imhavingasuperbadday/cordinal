// Encoder for cordinal's own format (docs/cord-format.md): exact samples from a PcmReader into the
// container (cord.h) with the CL03 lossless codec (cord_codec.h). Choices are made in floating
// point, but every bit written follows the integer rules the decoder applies, so the file decodes
// the same everywhere. Per frame it takes the best of the four stereo pairings; per channel the
// best of a windowed LPC and a least-squares one, each at an order picked by its estimated cost
// and settled by exact counts. The counts are Rice bits, a close stand-in for the range coder's.
// The winner's residuals then pass through the block's cascade before they are coded.
#include "audio_kernels.h"
#include "cord.h"
#include "cord_codec.h"
#include "decoder.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

namespace {
using namespace cord;

// Blocks of 65,536 frames (1.5 s at 44.1 kHz) give the cascade time to converge: 0.4% smaller
// than 32,768 and 1.2% than 16,384 on the corpus, and a seek still decodes only a few milliseconds.
constexpr uint32_t kBlockLength = 65536, kFrameLength = 4096;
// PAD after INFO, so the first tags written later need not move the audio.
constexpr uint64_t kPadBytes = 4096;
// The cascades each block is tried with (Encoder::Preset).
constexpr unsigned kVariants = 2;

// The range encoder for a block's decisions: low keeps 33 bits so a carry can
// ripple into bytes already settled, which wait (cache_ and pending_) until no carry can reach
// them. Finish writes exactly the bytes the decoder will read.
class RangeEncoder {
public:
    void Bit(uint16_t& probability, unsigned bit) {
        const uint32_t bound = (range_ >> kProbabilityBits) * probability;
        if (!bit) {
            range_ = bound;
            probability += (uint16_t)(((1u << kProbabilityBits) - probability) >> kAdaptation);
        } else {
            low_ += bound;
            range_ -= bound;
            probability -= (uint16_t)(probability >> kAdaptation);
        }
        if (range_ < kRangeTop) {
            range_ <<= 8;
            ShiftLow();
        }
    }
    void Finish() {
        for (int i = 0; i < 5; ++i) ShiftLow();
    }
    void Clear() {
        bytes_.clear();
        low_ = 0;
        range_ = 0xFFFFFFFF;
        cache_ = 0;
        pending_ = 1;
    }
    const std::vector<uint8_t>& Bytes() const { return bytes_; }
private:
    void ShiftLow() {
        if ((uint32_t)low_ < 0xFF000000u || (low_ >> 32)) {
            const uint8_t carry = (uint8_t)(low_ >> 32);
            uint8_t byte = cache_;
            do {
                bytes_.push_back((uint8_t)(byte + carry));
                byte = 0xFF;
            } while (--pending_);
            cache_ = (uint8_t)(low_ >> 24);
        }
        ++pending_;
        low_ = (low_ & 0x00FFFFFF) << 8;
    }
    std::vector<uint8_t> bytes_;
    uint64_t low_ = 0, pending_ = 1;
    uint32_t range_ = 0xFFFFFFFF;
    uint8_t cache_ = 0;
};

// MSB-first bits of one block's raw stream.
class BitWriter {
public:
    void Put(uint32_t v, unsigned n) {
        if (!n) return;
        acc_ = acc_ << n | (v & (n == 32 ? ~0u : (1u << n) - 1));
        pending_ += n;
        while (pending_ >= 8) {
            pending_ -= 8;
            bytes_.push_back((uint8_t)(acc_ >> pending_));
        }
    }
    void Align() { Put(0, (8 - pending_) & 7); }
    void Clear() { bytes_.clear(); pending_ = 0; }
    const std::vector<uint8_t>& Bytes() const { return bytes_; }
private:
    std::vector<uint8_t> bytes_;
    uint64_t acc_ = 0;
    unsigned pending_ = 0;
};

// CorrelateAvx2's sums in plain code, in the same lanes and order, so both give the same doubles.
void Correlate(const double* x, size_t n, ptrdiff_t from, bool history, unsigned maxLag, double* out) {
    for (unsigned lag = 0; lag <= maxLag; ++lag) {
        ptrdiff_t i = history ? from : std::max<ptrdiff_t>(from, lag);
        double lane[8] = {};
        for (; i + 8 <= (ptrdiff_t)n; i += 8)
            for (unsigned j = 0; j < 8; ++j) lane[j] = lane[j] + x[i + j] * x[i + j - lag];
        double tail = 0;
        for (; i < (ptrdiff_t)n; ++i) tail += x[i] * x[i - lag];
        const double c0 = lane[0] + lane[4], c1 = lane[1] + lane[5], c2 = lane[2] + lane[6], c3 = lane[3] + lane[7];
        out[lag] = ((c0 + c2) + (c1 + c3)) + tail;
    }
}

class Encoder {
public:
    Encoder(const Info& info, const CascadeSpec* cascade);
    // One block of interleaved samples: its CRC-32, then its payload.
    void Block(const int32_t* pcm, size_t frames, std::vector<uint8_t>& out);
private:
    // How one channel of one frame is coded, and what that costs in bits.
    struct Plan {
        unsigned coding = kVerbatim, wasted = 0, order = 0, precision = 0, shift = 0, k0 = 0;
        int32_t coefficients[kMaxOrder]{};
        uint64_t bits = UINT64_MAX;
    };
    // One coded channel of a searched frame: its plan, width, samples (for constant and verbatim
    // coding) and the winning predictor's folded residuals.
    struct Coded {
        Plan plan;
        unsigned width = 0;
        std::vector<int32_t> x;
        std::vector<uint32_t> folded;
    };
    struct Searched {
        unsigned mode = kLeftRight;
        size_t n = 0;
        Coded channel[kMaxChannels];
    };
    // The predictor and stereo search for one frame, which no cascade choice affects; then the
    // frame written under the current cascade.
    void Search(size_t offset, size_t n, Searched& frame);
    void Keep(Coded& coded, const Plan& plan, const int32_t* x, size_t n, unsigned width, std::vector<uint32_t>& folded);
    void Write(const Searched& frame);
    // x[-kMaxOrder .. n): history, then the frame. The winner's folded residuals land in `folded`.
    Plan Choose(const int32_t* x, size_t n, unsigned width, std::vector<uint32_t>& folded);
    bool Evaluate(const int32_t* y, size_t n, unsigned order, const int32_t* coefficients, unsigned precision,
                  unsigned shift, unsigned header, Plan& best, std::vector<uint32_t>& folded);
    unsigned Analyze(const int32_t* y, size_t n, unsigned maxOrder);
    unsigned LeastSquares(const int32_t* y, size_t n, unsigned maxOrder);
    void Emit(const Plan& plan, const int32_t* x, size_t n, unsigned width, const std::vector<uint32_t>& folded,
              unsigned channel);
    CascadeSpec Preset(size_t frames, unsigned variant) const;
    void WriteCascade();
    std::vector<Searched> searched_;
    CascadeSpec spec_;
    const CascadeSpec* override_;           // EncodeOptions::cascade
    Cascade cascades_[kMaxChannels];
    std::vector<int32_t> crossInput_;       // the first coded channel's LPC residuals in this frame
    std::vector<uint32_t> cascaded_;        // the residuals the cascade leaves, as coded
    bool overflow_ = false;                 // a residual the cascade left needs more than 32 bits
    const Info& info_;
    size_t stride_;                         // kMaxOrder of zeros, then the block, per channel
    std::vector<int32_t> planar_, side_, mid_, shifted_;
    std::vector<double> doubles_;           // shifted_ as doubles, for the correlations and the AVX2 residual kernel
    std::vector<uint32_t> folded_[4], trial_;
    std::vector<double> window_, signal_;
    // Each order's predictor and residual energy: from the windowed autocorrelation (Analyze) and
    // from least squares over the frame and its history (LeastSquares).
    double lpc_[kMaxOrder + 1][kMaxOrder]{}, error_[kMaxOrder + 1]{};
    double ls_[kMaxOrder + 1][kMaxOrder]{}, lsError_[kMaxOrder + 1]{};
    BitWriter bits_, bestBits_;
    RangeEncoder decisions_, bestDecisions_;
    Models models_;
    bool fast_ = nativeaudio::FastAudioKernels();
};

Encoder::Encoder(const Info& info, const CascadeSpec* cascade)
    : override_(cascade), info_(info), stride_(kMaxOrder + info.blockLength) {
    planar_.assign(stride_ * info.channels, 0);
    side_.resize(kMaxOrder + info.frameLength);
    mid_.resize(kMaxOrder + info.frameLength);
    shifted_.resize(kMaxOrder + info.frameLength);
    doubles_.resize(kMaxOrder + info.frameLength);
    for (auto& f : folded_) f.resize(info.frameLength);
    trial_.resize(info.frameLength);
    signal_.resize(info.frameLength);
    crossInput_.resize(info.frameLength);
    cascaded_.resize(info.frameLength);
}

void Encoder::Block(const int32_t* pcm, size_t frames, std::vector<uint8_t>& out) {
    const unsigned channels = info_.channels;
    for (unsigned c = 0; c < channels; ++c)
        for (size_t i = 0; i < frames; ++i) planar_[c * stride_ + kMaxOrder + i] = pcm[i * channels + c];
    const size_t count = (frames + info_.frameLength - 1) / info_.frameLength;
    if (searched_.size() < count) searched_.resize(count);
    for (size_t f = 0; f < count; ++f) {
        const size_t o = f * info_.frameLength;
        Search(o, std::min<size_t>(info_.frameLength, frames - o), searched_[f]);
    }
    // Each cascade the encoder knows is tried over the searched frames, and the smallest block
    // kept: the filters' best step sizes depend on the material (docs/cord-format.md). A residual
    // the cascade leaves can outgrow 32 bits only with the widest samples; when every cascade does,
    // the block is coded without one.
    size_t smallest = SIZE_MAX;
    const unsigned variants = override_ ? 1 : kVariants;
    for (unsigned variant = 0; variant <= variants; ++variant) {
        if (variant == variants && smallest != SIZE_MAX) break;
        bits_.Clear();
        decisions_.Clear();
        models_.Reset();
        spec_ = variant == variants ? CascadeSpec{} : override_ ? *override_ : Preset(frames, variant);
        if (info_.channels != 2) spec_.crossTaps = 0;  // only a stereo pair has a cross filter
        WriteCascade();
        overflow_ = false;
        for (size_t f = 0; f < count; ++f) Write(searched_[f]);
        if (overflow_) continue;
        bits_.Align();
        decisions_.Finish();
        const size_t size = bits_.Bytes().size() + decisions_.Bytes().size();
        if (size < smallest) {
            smallest = size;
            std::swap(bits_, bestBits_);
            std::swap(decisions_, bestDecisions_);
        }
    }
    // The payload: the decisions' length and bytes, then the raw stream; the CRC covers all of it.
    const std::vector<uint8_t>&rc = bestDecisions_.Bytes(), &raw = bestBits_.Bytes();
    out.assign(8, 0);
    for (unsigned i = 0; i < 4; ++i) out[4 + i] = (uint8_t)(rc.size() >> (8 * i));
    out.insert(out.end(), rc.begin(), rc.end());
    out.insert(out.end(), raw.begin(), raw.end());
    const uint32_t crc = Crc32(out.data() + 4, out.size() - 4);
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(crc >> (8 * i));
}

// The cascade measured best on the corpus for its decode cost (docs/cord-format.md, milestone 3):
// a 16-tap cross filter for stereo pairs, then 256 taps and 16. Its step sizes come in two sets:
// the first suits the Vorbis-decoded corpus, the second real masters, and each block keeps
// whichever codes it smaller. The history scale keeps a block's residuals mostly inside the
// filters' 16-bit history: the second difference stands in for them, generously, and only loud or
// wide samples need any shift.
CascadeSpec Encoder::Preset(size_t frames, unsigned variant) const {
    static constexpr unsigned kShifts[kVariants][3] = {{12, 11, 12}, {13, 10, 14}};
    CascadeSpec spec;
    spec.stages = 2;
    spec.taps[0] = 256;
    spec.shifts[0] = kShifts[variant][0];
    spec.taps[1] = 16;
    spec.shifts[1] = kShifts[variant][1];
    if (info_.channels == 2) {
        spec.crossTaps = 16;
        spec.crossShift = kShifts[variant][2];
    }
    uint64_t sum = 0;
    for (unsigned c = 0; c < info_.channels; ++c) {
        const int32_t* x = planar_.data() + c * stride_ + kMaxOrder;
        for (size_t i = 2; i < frames; ++i) {
            const int64_t d = (int64_t)x[i] - 2 * (int64_t)x[i - 1] + x[i - 2];
            sum += (uint64_t)(d < 0 ? -d : d);
        }
    }
    const uint64_t mean = frames > 2 ? sum / ((frames - 2) * info_.channels) : 0;
    spec.scale = (unsigned)std::clamp((int)std::bit_width(mean) - 11, 0, 15);
    return spec;
}

void Encoder::WriteCascade() {
    bits_.Put(spec_.stages, 2);
    for (unsigned s = 0; s < spec_.stages; ++s) {
        bits_.Put(spec_.taps[s] / 16 - 1, 7);
        bits_.Put(spec_.shifts[s], 4);
    }
    if (info_.channels == 2) {
        bits_.Put(spec_.crossTaps / 16, 2);
        if (spec_.crossTaps) bits_.Put(spec_.crossShift, 4);
    }
    if (spec_.Any()) bits_.Put(spec_.scale, 4);
    for (unsigned c = 0; c < info_.channels; ++c) cascades_[c].Configure(spec_, info_.channels == 2 && c == 1);
}

void Encoder::Keep(Coded& coded, const Plan& plan, const int32_t* x, size_t n, unsigned width,
                   std::vector<uint32_t>& folded) {
    coded.plan = plan;
    coded.width = width;
    if (plan.coding == kPredicted) {
        std::swap(coded.folded, folded);
        folded.resize(info_.frameLength);
    } else {
        coded.x.assign(x, x + n);
    }
}

void Encoder::Search(size_t o, size_t n, Searched& frame) {
    const unsigned bits = info_.bits;
    frame.n = n;
    frame.mode = kLeftRight;
    if (info_.channels != 2) {
        for (unsigned c = 0; c < info_.channels; ++c) {
            const int32_t* x = planar_.data() + c * stride_ + kMaxOrder + o;
            Keep(frame.channel[c], Choose(x, n, bits, folded_[0]), x, n, bits, folded_[0]);
        }
        return;
    }
    const int32_t *l = planar_.data() + kMaxOrder + o, *r = l + stride_;
    int32_t *side = side_.data() + kMaxOrder, *mid = mid_.data() + kMaxOrder;
    const Plan left = Choose(l, n, bits, folded_[0]), right = Choose(r, n, bits, folded_[1]);
    unsigned mode = kLeftRight;
    uint64_t cost = left.bits + right.bits;
    Plan sidePlan, midPlan;
    if (bits <= 31) {
        // Side and mid over the history too, as the decoder restates it.
        for (ptrdiff_t i = -(ptrdiff_t)kMaxOrder; i < (ptrdiff_t)n; ++i) {
            int64_t a, b;
            Decorrelate(kMidSide, l[i], r[i], a, b);
            mid[i] = (int32_t)a;
            side[i] = (int32_t)b;
        }
        sidePlan = Choose(side, n, bits + 1, folded_[2]);
        midPlan = Choose(mid, n, bits, folded_[3]);
        const uint64_t costs[] = {cost, left.bits + sidePlan.bits, sidePlan.bits + right.bits, midPlan.bits + sidePlan.bits};
        for (unsigned m = 1; m < 4; ++m)
            if (costs[m] < cost) { cost = costs[m]; mode = m; }
    }
    frame.mode = mode;
    Coded &a = frame.channel[0], &b = frame.channel[1];
    switch (mode) {
    case kLeftRight: Keep(a, left, l, n, bits, folded_[0]); Keep(b, right, r, n, bits, folded_[1]); break;
    case kLeftSide: Keep(a, left, l, n, bits, folded_[0]); Keep(b, sidePlan, side, n, bits + 1, folded_[2]); break;
    case kSideRight: Keep(a, sidePlan, side, n, bits + 1, folded_[2]); Keep(b, right, r, n, bits, folded_[1]); break;
    default: Keep(a, midPlan, mid, n, bits, folded_[3]); Keep(b, sidePlan, side, n, bits + 1, folded_[2]); break;
    }
}

void Encoder::Write(const Searched& frame) {
    if (info_.channels == 2) bits_.Put(frame.mode, 2);
    for (unsigned c = 0; c < info_.channels; ++c) {
        const Coded& coded = frame.channel[c];
        Emit(coded.plan, coded.x.data(), frame.n, coded.width, coded.folded, c);
    }
}

Encoder::Plan Encoder::Choose(const int32_t* x, size_t n, unsigned width, std::vector<uint32_t>& folded) {
    Plan best;
    if (std::all_of(x + 1, x + n, [&](int32_t v) { return v == x[0]; })) {
        best.coding = kConstant;
        best.bits = 3 + width;
        return best;
    }
    uint32_t any = 0;
    for (size_t i = 0; i < n; ++i) any |= (uint32_t)x[i];
    best.wasted = (unsigned)std::countr_zero(any);  // fewer than width: some sample is nonzero
    const unsigned w = width - best.wasted, header = 2 + (best.wasted ? 6 : 1);
    best.bits = header + (uint64_t)n * w;
    int32_t* y = shifted_.data() + kMaxOrder;
    for (ptrdiff_t i = -(ptrdiff_t)kMaxOrder; i < (ptrdiff_t)n; ++i) y[i] = x[i] >> best.wasted;
    for (ptrdiff_t i = -(ptrdiff_t)kMaxOrder; i < (ptrdiff_t)n; ++i) doubles_[kMaxOrder + i] = y[i];
    Evaluate(y, n, 0, nullptr, 0, 0, header, best, folded);
    if (n < 32) return best;
    const unsigned maxOrder = std::min<unsigned>(kMaxOrder, (unsigned)(n / 8));
    // FLAC's coefficient precision for this frame length.
    const unsigned precision = n <= 192 ? 7 : n <= 384 ? 8 : n <= 576 ? 9 : n <= 1152 ? 10 : n <= 2304 ? 11 : n <= 4608 ? 12 : 13;
    int32_t q[kMaxOrder];
    unsigned shift = 0;
    const double* winner = nullptr;  // the coefficients the best plan was quantized from
    auto attempt = [&](const double* a, unsigned order, unsigned bits) {
        // Error feedback, as FLAC does, keeps the rounding from accumulating along the filter.
        double largest = 0;
        for (unsigned j = 0; j < order; ++j) largest = std::max(largest, std::fabs(a[j]));
        if (!(largest > 0) || !std::isfinite(largest)) return;
        int exponent = 0;
        std::frexp(largest, &exponent);
        shift = (unsigned)std::clamp((int)bits - 1 - exponent, 0, 31);
        const double limit = std::ldexp(1.0, (int)bits - 1), scale = std::ldexp(1.0, (int)shift);
        double carried = 0;
        for (unsigned j = 0; j < order; ++j) {
            carried += a[j] * scale;
            const double v = std::clamp(std::nearbyint(carried), -limit, limit - 1);
            carried -= v;
            q[j] = (int32_t)v;
        }
        if (Evaluate(y, n, order, q, bits, shift, header, best, folded)) winner = a;
    };
    // The order with the fewest estimated bits: half a bit per sample for each halving of the
    // residual energy, against the coefficients' own bits. Exact counts then settle the orders
    // around it.
    auto search = [&](const double (*a)[kMaxOrder], const double* error, unsigned found, unsigned spread, unsigned bits) {
        unsigned guess = 1;
        double cheapest = INFINITY;
        for (unsigned p = 1; p <= found; ++p) {
            const double perSample = error[p] > 0 ? std::max(0.0, 0.5 * std::log2(error[p] * 0.5 / (double)n)) : 0.0;
            const double estimate = perSample * (double)n + p * bits;
            if (estimate < cheapest) { cheapest = estimate; guess = p; }
        }
        for (unsigned p = guess > spread ? guess - spread : 1; p <= std::min(found, guess + spread); ++p) attempt(a[p], p, bits);
    };
    // The windowed predictor at its estimated order, then least squares around its own: together
    // they beat either alone, and least squares wins on tonal frames by a wide margin.
    if (const unsigned found = Analyze(y, n, maxOrder)) search(lpc_, error_, found, 0, precision);
    if (const unsigned found = LeastSquares(y, n, maxOrder)) search(ls_, lsError_, found, 1, 15);
    // 15-bit coefficients for the windowed predictor if it won at FLAC's precision.
    if (winner && best.precision != 15) attempt(winner, best.order, 15);
    return best;
}

// Least squares over the frame and its history (the covariance method): for every order up to
// maxOrder, the predictor that minimises the squared residual the decoder will actually see, in
// ls_, with that residual's energy in lsError_. Unlike the windowed autocorrelation it doesn't
// smear tonal frames' spectral lines. Returns the highest order solved.
unsigned Encoder::LeastSquares(const int32_t* y, size_t n, unsigned maxOrder) {
    // A block's first frame predicts its first samples from silence. Their errors would dominate
    // a squared-error fit, so the fit starts after them.
    const ptrdiff_t start = std::all_of(y - kMaxOrder, y, [](int32_t v) { return v == 0; }) ? (ptrdiff_t)maxOrder : 0;
    // c[j][k] = the sum over the fitted samples of y[i - j] * y[i - k]. The first row is summed;
    // each later entry follows from the one up and to the left by moving the span back a sample.
    double c[kMaxOrder + 1][kMaxOrder + 1];
    (fast_ ? CorrelateAvx2 : Correlate)(doubles_.data() + kMaxOrder, n, start, true, maxOrder, c[0]);
    for (unsigned j = 0; j < maxOrder; ++j)
        for (unsigned k = j; k < maxOrder; ++k)
            c[j + 1][k + 1] = c[j][k] + (double)y[start - 1 - (ptrdiff_t)j] * y[start - 1 - (ptrdiff_t)k] -
                              (double)y[(ptrdiff_t)n - 1 - (ptrdiff_t)j] * y[(ptrdiff_t)n - 1 - (ptrdiff_t)k];
    if (!(c[0][0] > 0)) return 0;
    // Cholesky of the normal equations, column by column, with a little diagonal loading so a pure
    // tone's singular system still solves. Forward substitution alongside gives each order's
    // residual energy: what remains of c[0][0] after the squares of the solved components.
    double l[kMaxOrder][kMaxOrder], z[kMaxOrder], energy = c[0][0];
    const double load = 1e-9 * c[1][1] + 1e-9;
    unsigned solved = 0;
    for (unsigned j = 0; j < maxOrder; ++j) {
        double d = c[j + 1][j + 1] + load;
        for (unsigned m = 0; m < j; ++m) d -= l[j][m] * l[j][m];
        if (!(d > load * 1e-3)) break;
        l[j][j] = std::sqrt(d);
        for (unsigned i = j + 1; i < maxOrder; ++i) {
            double s = c[j + 1][i + 1];
            for (unsigned m = 0; m < j; ++m) s -= l[i][m] * l[j][m];
            l[i][j] = s / l[j][j];
        }
        double s = c[0][j + 1];
        for (unsigned m = 0; m < j; ++m) s -= l[j][m] * z[m];
        z[j] = s / l[j][j];
        energy -= z[j] * z[j];
        lsError_[j + 1] = std::max(energy, 0.0);
        solved = j + 1;
    }
    // Each order's coefficients by back substitution over its leading part of the factor.
    for (unsigned p = 1; p <= solved; ++p)
        for (unsigned i = p; i-- > 0;) {
            double s = z[i];
            for (unsigned m = i + 1; m < p; ++m) s -= l[m][i] * ls_[p][m];
            ls_[p][i] = s / l[i][i];
        }
    return solved;
}

// Folds the residuals of one predictor and counts its exact cost; keeps it if it beats `best`.
bool Encoder::Evaluate(const int32_t* y, size_t n, unsigned order, const int32_t* coefficients, unsigned precision,
                       unsigned shift, unsigned header, Plan& best, std::vector<uint32_t>& folded) {
    uint32_t* f = trial_.data();
    if (fast_) {
        if (!FoldResidualsAvx2(doubles_.data() + kMaxOrder, n, coefficients, order, shift, f)) return false;
    } else {
        for (size_t i = 0; i < n; ++i) {
            const int64_t e = y[i] - Predict(y + i, coefficients, order, shift);
            if (e > INT32_MAX || e < -INT32_MAX) return false;
            f[i] = Fold(e);
        }
    }
    // The initial parameter is the one the first 16 residuals settle on.
    const size_t first = std::min<size_t>(16, n);
    uint64_t sum = 0;
    for (size_t i = 0; i < first; ++i) sum += ((uint64_t)f[i] + 1) >> 1;
    unsigned k0 = 0;
    while (k0 < 31 && ((uint64_t)first << k0) < sum) ++k0;
    uint64_t bits = header + 6 + (order ? 9 + (uint64_t)order * precision : 0) + 5;
    Rice rice;
    rice.Seed(k0);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t quotient = f[i] >> rice.k;
        bits += quotient < kEscape ? quotient + 1 + rice.k : kEscape + 32;
        if (bits >= best.bits) return false;
        rice.Update(f[i]);
    }
    best.coding = kPredicted;
    best.order = order;
    best.precision = precision;
    best.shift = shift;
    best.k0 = k0;
    best.bits = bits;
    std::copy_n(coefficients, order, best.coefficients);
    std::swap(trial_, folded);
    return true;
}

// Tukey(0.5)-windowed autocorrelation and Levinson-Durbin: the predictor of every order up to
// maxOrder in lpc_, with its residual energy in error_. Returns the highest order found.
unsigned Encoder::Analyze(const int32_t* y, size_t n, unsigned maxOrder) {
    if (window_.size() != n) {
        window_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)i / (double)(n - 1), edge = std::min(t, 1 - t);
            window_[i] = edge < 0.25 ? 0.5 * (1 - std::cos(2 * 3.14159265358979323846 * edge / 0.5)) : 1.0;
        }
    }
    for (size_t i = 0; i < n; ++i) signal_[i] = y[i] * window_[i];
    double r[kMaxOrder + 1];
    (fast_ ? CorrelateAvx2 : Correlate)(signal_.data(), n, 0, false, maxOrder, r);
    if (r[0] <= 0) return 0;
    double a[kMaxOrder]{}, error = r[0];
    for (unsigned p = 1; p <= maxOrder; ++p) {
        double acc = r[p];
        for (unsigned j = 0; j + 1 < p; ++j) acc -= a[j] * r[p - 1 - j];
        const double k = acc / error;
        for (unsigned j = 0; j < (p - 1) / 2; ++j) {
            const double t = a[j];
            a[j] -= k * a[p - 2 - j];
            a[p - 2 - j] -= k * t;
        }
        if ((p - 1) & 1) a[(p - 1) / 2] -= k * a[(p - 1) / 2];
        a[p - 1] = k;
        error *= 1 - k * k;
        std::copy_n(a, p, lpc_[p]);
        error_[p] = error;
        if (error <= 0) return p;
    }
    return maxOrder;
}

void Encoder::Emit(const Plan& plan, const int32_t* x, size_t n, unsigned width, const std::vector<uint32_t>& source,
                   unsigned channel) {
    BitWriter& bits = bits_;
    RangeEncoder& decisions = decisions_;
    // The cascade turns the LPC residual into the one coded, so the initial parameter is chosen
    // again from what it leaves. The first channel of a stereo pair records its LPC residuals for
    // the second channel's cross filter: zeros when it isn't predicted.
    const bool cascade = plan.coding == kPredicted && spec_.Any();
    const bool record = info_.channels == 2 && spec_.crossTaps && channel == 0, cross = spec_.crossTaps && channel == 1;
    const std::vector<uint32_t>& folded = cascade ? cascaded_ : source;
    unsigned k0 = plan.k0;
    if (record && plan.coding != kPredicted) std::fill_n(crossInput_.begin(), n, 0);
    if (cascade && !(fast_ && FilterAvx2(cascades_[channel], source.data(), n, record ? crossInput_.data() : nullptr,
                                         cross ? crossInput_.data() : nullptr, cascaded_.data(), overflow_))) {
        for (size_t i = 0; i < n; ++i) {
            const int64_t input = Unfold(source[i]);
            if (record) crossInput_[i] = (int32_t)input;
            const int64_t e = cascades_[channel].Filter(input, cross ? crossInput_[i] : 0);
            if (e < INT32_MIN || e > INT32_MAX) overflow_ = true;
            cascaded_[i] = Fold(std::clamp<int64_t>(e, INT32_MIN, INT32_MAX));
        }
    }
    if (cascade) {
        const size_t first = std::min<size_t>(16, n);
        uint64_t sum = 0;
        for (size_t i = 0; i < first; ++i) sum += ((uint64_t)cascaded_[i] + 1) >> 1;
        k0 = 0;
        while (k0 < 31 && ((uint64_t)first << k0) < sum) ++k0;
    }
    bits.Put(plan.coding, 2);
    if (plan.wasted) {
        bits.Put(1, 1);
        bits.Put(plan.wasted - 1, 5);
    } else {
        bits.Put(0, 1);
    }
    const unsigned w = width - plan.wasted;
    if (plan.coding == kConstant) {
        bits.Put((uint32_t)x[0], w);
    } else if (plan.coding == kVerbatim) {
        for (size_t i = 0; i < n; ++i) bits.Put((uint32_t)(x[i] >> plan.wasted), w);
    } else {
        bits.Put(plan.order, 6);
        if (plan.order) {
            bits.Put(plan.precision - 1, 4);
            bits.Put(plan.shift, 5);
            for (unsigned j = 0; j < plan.order; ++j) bits.Put((uint32_t)plan.coefficients[j], plan.precision);
        }
        bits.Put(k0, 5);
        Rice rice;
        rice.Seed(k0);
        for (size_t i = 0; i < n; ++i) {
            const unsigned k = Split(rice.k);
            const uint32_t u = folded[i], quotient = u >> k;
            uint16_t* m = models_.For(k);
            for (unsigned q = 0; q < std::min(quotient, kEscape); ++q) decisions.Bit(m[std::min(q, 15u)], 1);
            if (quotient < kEscape) {
                decisions.Bit(m[std::min(quotient, 15u)], 0);
                if (k) {
                    decisions.Bit(m[16 + (quotient > 0)], (u >> (k - 1)) & 1);
                    bits.Put(u, k - 1);
                }
            } else {
                bits.Put(u, 32);
            }
            rice.Update(u);
        }
    }
}

// The file under construction: sequential writes through a buffer, patched at the end, and
// deleted unless Commit moves it into place.
class Output {
public:
    ~Output() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        if (!path_.empty()) DeleteFileW(path_.c_str());
    }
    bool Open(const std::wstring& path) {
        path_ = path;
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return handle_ != INVALID_HANDLE_VALUE;
    }
    bool Write(const void* p, size_t n) {
        buffer_.append((const char*)p, n);
        return buffer_.size() < (1 << 20) || Flush();
    }
    bool Write(const std::string& s) { return Write(s.data(), s.size()); }
    bool Patch(uint64_t at, const void* p, size_t n) {
        OVERLAPPED where{};
        where.Offset = (DWORD)at;
        where.OffsetHigh = (DWORD)(at >> 32);
        DWORD written = 0;
        return Flush() && WriteFile(handle_, p, (DWORD)n, &written, &where) && written == n;
    }
    bool Commit(const std::wstring& to) {
        const bool ok = Flush() && FlushFileBuffers(handle_);
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        // No MOVEFILE_REPLACE_EXISTING: a file that appeared meanwhile is never overwritten.
        if (!ok || !MoveFileExW(path_.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
        path_.clear();
        return true;
    }
private:
    bool Flush() {
        DWORD written = 0;
        const bool ok = buffer_.empty() ||
                        (WriteFile(handle_, buffer_.data(), (DWORD)buffer_.size(), &written, nullptr) && written == buffer_.size());
        buffer_.clear();
        return ok;
    }
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    std::string buffer_;
};

}  // namespace

bool cord::Encode(PcmReader& source, const std::wstring& path, std::wstring& error, const std::atomic<bool>* cancel,
                  const EncodeOptions& options) {
    const PcmFormat& format = source.Format();
    if (!format.rate || format.rate > 1048575 || !format.channels || format.channels > kMaxChannels || !format.bits ||
        format.bits > 32) {
        error = L"cordinal's format holds up to 8 channels of 1- to 32-bit integer samples.";
        return false;
    }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        error = L"The destination already exists, and it is never overwritten.";
        return false;
    }
    Info info;
    info.rate = format.rate;
    info.channels = format.channels;
    info.bits = format.bits;
    info.channelMask = format.channelMask;
    info.frames = format.frames;
    info.blockLength = kBlockLength;
    info.frameLength = kFrameLength;
    const uint64_t blocks = info.Blocks();
    Output out;
    if (!out.Open(path + L".cordinal-tmp")) {
        error = L"Couldn't create the file next to its destination.";
        return false;
    }
    // INFO's MD5, INDX and AUDI's size are patched once the audio is written.
    const uint64_t md5At = kHeaderBytes + kChunkHeaderBytes + 36;
    // The tags come first after INFO, then PAD, so the file reads as if it had been tagged later.
    std::string head = Header() + ChunkHeader(kInfo, kInfoBytes) + InfoBytes(info);
    if (!options.meta.empty()) head += ChunkHeader(kMeta, options.meta.size()) + options.meta;
    for (const std::string& picture : options.pictures) head += ChunkHeader(kPicture, picture.size()) + picture;
    head += ChunkHeader(kPad, kPadBytes) + std::string(kPadBytes, '\0') + ChunkHeader(kIndex, blocks * 4);
    const uint64_t indexAt = head.size(), audioAt = indexAt + blocks * 4;
    bool ok = out.Write(head);
    std::vector<uint32_t> sizes(blocks);
    ok = ok && out.Write(sizes.data(), sizes.size() * 4) && out.Write(ChunkHeader(kAudio, 0));
    // Blocks share no state, so up to `threads` of them are encoded at once: read in order, coded
    // side by side, written in order. The output is the same for any thread count.
    const unsigned threads = (unsigned)std::clamp<uint64_t>(options.threads, 1, std::max<uint64_t>(blocks, 1));
    std::vector<std::unique_ptr<Encoder>> encoders;
    std::vector<std::vector<int32_t>> pcm(threads);
    std::vector<std::vector<uint8_t>> coded(threads);
    std::vector<size_t> lengths(threads);
    for (unsigned t = 0; t < threads; ++t) {
        encoders.push_back(std::make_unique<Encoder>(info, options.cascade));
        pcm[t].resize((size_t)info.blockLength * info.channels);
    }
    uint64_t audioBytes = 0;
    for (uint64_t first = 0; ok && first < blocks; first += threads) {
        if (cancel && cancel->load()) {
            error = L"Cancelled.";
            return false;
        }
        const unsigned batch = (unsigned)std::min<uint64_t>(threads, blocks - first);
        for (unsigned t = 0; t < batch; ++t) {
            const size_t want = (size_t)std::min<uint64_t>(info.blockLength, info.frames - (first + t) * info.blockLength);
            size_t got = 0;
            while (got < want)
                if (const size_t n = source.Read(pcm[t].data() + got * info.channels, want - got)) got += n;
                else break;
            if (got < want) {
                error = L"The source ended early or is damaged.";
                return false;
            }
            lengths[t] = want;
        }
        std::vector<std::thread> workers;
        for (unsigned t = 1; t < batch; ++t)
            workers.emplace_back([&, t] {
                if (options.background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
                encoders[t]->Block(pcm[t].data(), lengths[t], coded[t]);
            });
        encoders[0]->Block(pcm[0].data(), lengths[0], coded[0]);
        for (auto& w : workers) w.join();
        for (unsigned t = 0; ok && t < batch; ++t) {
            sizes[first + t] = (uint32_t)coded[t].size();
            audioBytes += coded[t].size();
            ok = out.Write(coded[t].data(), coded[t].size());
        }
    }
    // Reading past the end runs the source's own checks and completes its MD5.
    if (ok && (source.Read(pcm[0].data(), 1) || source.Damaged())) {
        error = L"The source is damaged: its length or stored MD5 does not hold.";
        return false;
    }
    info.md5 = source.Digest();
    uint8_t size[8];
    for (unsigned i = 0; i < 8; ++i) size[i] = (uint8_t)(audioBytes >> (8 * i));
    ok = ok && out.Patch(md5At, info.md5.data(), 16) && (!blocks || out.Patch(indexAt, sizes.data(), sizes.size() * 4)) &&
         out.Patch(audioAt + 4, size, 8);
    if (!ok || !out.Commit(path)) {
        error = L"Couldn't write the file.";
        return false;
    }
    return true;
}
