// The lossless codec "CL03" (docs/cord-spec.md, frozen with version 1), as its encoder (cord_encode.cpp)
// and decoder (audio_cord.cpp, with an AVX2 copy of the residual loop in audio_avx.cpp) must both
// compute it: stereo decorrelation, the prediction, the adaptive cascade, the adaptive Rice
// parameter, and the range coder that carries each residual's modelled decisions. Every stage is
// exact on every path, in integers or in doubles whose every step is an exact integer, so every
// build decodes the same samples.
//
// A block (the unit of seeking) is a CRC-32 of its payload, then the payload: a u32 length, that
// many bytes of range-coded decisions, then a raw MSB-first bit stream padded with zero bits to a
// byte. Both streams run through the block's frames in step. The raw stream opens with the block's
// cascade (CascadeSpec); then come the frames. A frame holds frameLength frames (fewer at the end
// of the stream); its fields are raw bits:
//
//   stereo mode       2 bits, only for two channels: Stereo below; side modes need bits <= 31
//   per coded channel:
//     coding          2 bits: Coding below (3 is invalid)
//     wasted bits     1 bit; when set, 5 more bits hold the count minus 1. Samples are coded
//                     shifted right by that many zero bits; w = the channel's width minus them
//     constant        one w-bit signed value
//     verbatim        one w-bit signed value per frame
//     predicted       order (6 bits, 0..32); for order > 0 the coefficient precision minus 1
//                     (4 bits), the shift (5 bits) and the coefficients (precision bits, signed);
//                     then the initial Rice parameter (5 bits) and one residual per frame
//
// A residual is folded to unsigned (Fold) and split at k = Split(the current Rice parameter) into
// a quotient and k low bits. The quotient is range-coded in unary: a 1 for each step it continues,
// a 0 where it stops. Its top low bit is range-coded too, and the k - 1 below it are raw. A
// quotient reaching kEscape stops without the 0, and the whole 32-bit value follows raw. Then k
// adapts (Rice). The decisions' models (Models), shared by the channels, start afresh with each
// block. (Separate streams and models per channel, decoded side by side, measured slower and larger.)
//
// Prediction reaches back across frames into the block's earlier samples (zeros before the block's
// start), restated in the frame's own stereo mode and wasted bits, so no frame stores warm-up
// samples and only a block boundary costs any. The coded residual is what the cascade leaves of
// the prediction's residual.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define CORD_SSE2 1
#endif

// The AVX2 translation unit compiles these helpers too. Forced inline, they leave no out-of-line
// copy for the linker to share between the units, so an AVX2 build of one (with BMI instructions)
// can never stand in for the baseline's.
#if defined(_MSC_VER)
#define CORD_INLINE __forceinline
#else
#define CORD_INLINE inline __attribute__((always_inline))
#endif

namespace cord {

enum Stereo : unsigned { kLeftRight, kLeftSide, kSideRight, kMidSide };
enum Coding : unsigned { kConstant, kVerbatim, kPredicted };
constexpr unsigned kMaxOrder = 32;     // the longest prediction, and the history kept across frames
constexpr unsigned kEscape = 24;       // a quotient this long is followed by the raw 32-bit value
constexpr unsigned kRiceWindow = 64;   // the adaptive Rice estimate halves every this many samples
// Binary range coding: 11-bit probabilities of a 0, each moved 1/32 of the way toward
// the decision coded, and a 32-bit range kept above 2^24.
constexpr unsigned kProbabilityBits = 11, kAdaptation = 5;
constexpr uint32_t kRangeTop = 1u << 24;

// The width of coded channel c in a stereo mode: a side channel needs one more bit.
inline unsigned CodedBits(unsigned bits, unsigned mode, unsigned c) {
    return bits + ((mode == kLeftSide && c == 1) || (mode == kSideRight && c == 0) || (mode == kMidSide && c == 1));
}

// One left/right pair as the mode codes it, and back.
inline void Decorrelate(unsigned mode, int64_t l, int64_t r, int64_t& a, int64_t& b) {
    switch (mode) {
    case kLeftSide: a = l; b = l - r; break;
    case kSideRight: a = l - r; b = r; break;
    case kMidSide: a = (l + r) >> 1; b = l - r; break;
    default: a = l; b = r; break;
    }
}
inline void Correlate(unsigned mode, int64_t a, int64_t b, int64_t& l, int64_t& r) {
    switch (mode) {
    case kLeftSide: l = a; r = a - b; break;
    case kSideRight: l = a + b; r = b; break;
    case kMidSide: {
        const int64_t mid = a * 2 + (b & 1);
        l = (mid + b) >> 1;
        r = (mid - b) >> 1;
        break;
    }
    default: l = a; r = b; break;
    }
}

// The prediction for x[0] from x[-1] .. x[-order]. Coefficients have at most 16 bits and samples
// 32, so every product fits 46 bits and the sum 51: exact in 64-bit integers, and in doubles too,
// which is what lets the AVX2 path use fused multiply-adds.
// The newest sample's term is added last: it is the one still being decoded, and adding it first
// would put every other term on the path from one sample to the next.
CORD_INLINE int64_t Predict(const int32_t* x, const int32_t* coefficients, unsigned order, unsigned shift) {
    int64_t sum = 0;
    for (unsigned j = order; j-- > 0;) sum += (int64_t)coefficients[j] * x[-1 - (int)j];
    return sum >> shift;
}

// Where a residual splits into quotient and low bits: one bit above the Rice parameter, so the
// quotient is mostly 0 and costs about one range-coded decision. Splitting at the parameter itself
// is 0.35% smaller but decodes a fifth slower (docs/cord-format.md, milestone 3).
CORD_INLINE unsigned Split(unsigned k) { return std::min(k + 1, 31u); }

// Residuals travel folded to unsigned: 0, -1, 1, -2 ... become 0, 1, 2, 3 ...
CORD_INLINE uint32_t Fold(int64_t e) { return (uint32_t)(e < 0 ? -2 * e - 1 : 2 * e); }
CORD_INLINE int64_t Unfold(uint32_t u) { return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); }

// The adaptive Rice parameter, kept as JPEG-LS does: the smallest k with count * 2^k at least
// the sum of recent residual magnitudes. Each frame seeds it from its stored initial parameter.
struct Rice {
    uint64_t sum = 0;
    uint32_t count = 0;
    unsigned k = 0;
    CORD_INLINE void Seed(unsigned k0) {
        count = 16;
        sum = (uint64_t)16 << k0;
        k = k0;
    }
    CORD_INLINE void Update(uint32_t folded) {
        sum += ((uint64_t)folded + 1) >> 1;  // the residual's magnitude
        if (++count == kRiceWindow) {
            sum >>= 1;
            count >>= 1;
        }
        while (k < 31 && ((uint64_t)count << k) < sum) ++k;
        while (k && ((uint64_t)count << (k - 1)) >= sum) --k;
    }
};

// The decisions' probabilities, by Rice parameter: the unary quotient's first 16 steps (later
// steps share the last), then the top low bit after a zero quotient and after a nonzero one.
struct Models {
    uint16_t p[24][18];
    CORD_INLINE void Reset() { std::fill(&p[0][0], &p[0][0] + 24 * 18, (uint16_t)(1u << (kProbabilityBits - 1))); }
    CORD_INLINE uint16_t* For(unsigned k) { return p[std::min(k, 23u)]; }
};

// MSB-first reader over a block's raw stream. Unread bits sit at the top of cache_; a read past
// the end fails the block rather than inventing zeros.
class BitReader {
public:
    BitReader() = default;
    BitReader(const uint8_t* p, size_t n) : begin_(p), p_(p), end_(p + n) {}
    CORD_INLINE uint32_t Get(unsigned n) {
        if (!n) return 0;
        if (available_ < n) Fill();
        if (available_ < n) { good_ = false; return 0; }
        const uint32_t v = (uint32_t)(cache_ >> (64 - n));
        cache_ <<= n; available_ -= n;
        return v;
    }
    CORD_INLINE int32_t Signed(unsigned n) { return (int32_t)(Get(n) << (32 - n)) >> (32 - n); }
    CORD_INLINE size_t Position() const { return (size_t)(p_ - begin_) * 8 - available_; }
    CORD_INLINE bool Good() const { return good_; }
private:
    CORD_INLINE void Fill() {
        if (end_ - p_ >= 4 && available_ <= 32) {
            cache_ |= (uint64_t)((uint32_t)p_[0] << 24 | (uint32_t)p_[1] << 16 | (uint32_t)p_[2] << 8 | p_[3]) << (32 - available_);
            p_ += 4; available_ += 32;
        }
        while (p_ < end_ && available_ <= 56) { cache_ |= (uint64_t)*p_++ << (56 - available_); available_ += 8; }
    }
    const uint8_t *begin_ = nullptr, *p_ = nullptr, *end_ = nullptr;
    uint64_t cache_ = 0;
    unsigned available_ = 0;
    bool good_ = true;
};

// The range decoder for a block's decisions. It reads exactly the bytes the encoder wrote, so a
// block whose decisions end anywhere else is damaged.
class RangeDecoder {
public:
    RangeDecoder() = default;
    RangeDecoder(const uint8_t* p, size_t n) : p_(p), end_(p + n) {
        for (int i = 0; i < 5; ++i) code_ = code_ << 8 | Next();
    }
    // Selects rather than branches on the decision: a coin-flip branch would mispredict half the time.
    CORD_INLINE unsigned Bit(uint16_t& probability) {
        const uint32_t bound = (range_ >> kProbabilityBits) * probability, p = probability;
        const unsigned bit = code_ >= bound;
        const uint32_t mask = 0u - bit;
        code_ -= bound & mask;
        range_ = (bound & ~mask) | ((range_ - bound) & mask);
        probability = (uint16_t)(((p + (((1u << kProbabilityBits) - p) >> kAdaptation)) & ~mask) | ((p - (p >> kAdaptation)) & mask));
        if (range_ < kRangeTop) {
            range_ <<= 8;
            code_ = code_ << 8 | Next();
        }
        return bit;
    }
    CORD_INLINE bool Good() const { return good_; }
    CORD_INLINE bool Finished() const { return good_ && p_ == end_; }
private:
    CORD_INLINE uint8_t Next() {
        if (p_ < end_) return *p_++;
        good_ = false;
        return 0;
    }
    const uint8_t *p_ = nullptr, *end_ = nullptr;
    uint32_t code_ = 0, range_ = 0xFFFFFFFF;
    bool good_ = true;
};

// The cascade: after the LPC, a predicted channel's residual passes through sign-sign LMS filters,
// Monkey's Audio's NN filters with the steps of its version 3.98 (audio_ape.cpp). In a stereo pair
// the second coded channel first meets a cross filter over the first channel's LPC residuals at
// the same and earlier positions; then each channel runs up to three filters of its own, longest
// first. Filters adapt every sample and start from zero with each block. A channel coded constant
// or verbatim leaves its filters as they were and gives the cross filter zeros.
constexpr unsigned kMaxStages = 3, kMaxTaps = 2048, kMaxCrossTaps = 48;

// A block's cascade, the first fields of its raw stream:
//
//   stages        2 bits: filters per channel, 0..3, in the order the encoder applies them
//     taps        7 bits: taps / 16 - 1 (16..2048)
//     shift       4 bits: 1..15, the weights' fraction bits
//   cross         2 bits, two channels only: the cross filter's taps / 16, 0 for none
//     shift       4 bits, with cross taps
//   scale         4 bits, with any filter: inputs enter the filters' 16-bit history shifted right
//                 this much, so wide samples need not saturate it
struct CascadeSpec {
    unsigned stages = 0, taps[kMaxStages]{}, shifts[kMaxStages]{}, crossTaps = 0, crossShift = 0, scale = 0;
    bool Any() const { return stages || crossTaps; }
};

inline bool ReadCascade(BitReader& raw, unsigned channels, CascadeSpec& spec) {
    spec = CascadeSpec{};
    spec.stages = raw.Get(2);
    for (unsigned s = 0; s < spec.stages; ++s) {
        spec.taps[s] = (raw.Get(7) + 1) * 16;
        spec.shifts[s] = raw.Get(4);
        if (!spec.shifts[s]) return false;
    }
    if (channels == 2 && (spec.crossTaps = raw.Get(2) * 16) && !(spec.crossShift = raw.Get(4))) return false;
    if (spec.Any()) spec.scale = raw.Get(4);
    return raw.Good();
}

// Weights times a window of history, wrapping at 32 bits as the SIMD sums do; then each weight moves
// by its step in the direction that shrinks the error the filter left (not at all for 0). These are
// the baseline kernels; the AVX2 unit has its own (Kernel below).
CORD_INLINE int32_t Dot(const int16_t* w, const int16_t* h, unsigned taps) {
#if CORD_SSE2
    __m128i sum = _mm_setzero_si128();
    for (unsigned j = 0; j < taps; j += 8)
        sum = _mm_add_epi32(sum, _mm_madd_epi16(_mm_load_si128((const __m128i*)(w + j)), _mm_loadu_si128((const __m128i*)(h + j))));
    sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0x4E));
    return _mm_cvtsi128_si32(_mm_add_epi32(sum, _mm_shuffle_epi32(sum, 0xB1)));
#else
    uint32_t sum = 0;
    for (unsigned j = 0; j < taps; ++j) sum += (uint32_t)((int32_t)w[j] * h[j]);
    return (int32_t)sum;
#endif
}
// A step shrinks as its input ages: whole for the newest, a half one sample on, a quarter from
// three to eight, an eighth after that. Steps are 8, 16 or 32 and are stored divided by 8 (exact),
// so a step at its age is the stored one times its lag's factor below. (Monkey's Audio halves the
// stored steps in place; the steps it uses are the same.)
constexpr int16_t kStepFactor[16] = {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 4, 8};  // lags 16 .. 1
CORD_INLINE void Adapt(int16_t* w, const int16_t* a, unsigned taps, int64_t error) {
    const int16_t sign = (int16_t)((error > 0) - (error < 0));
#if CORD_SSE2
    const __m128i s = _mm_set1_epi16(sign), newest = _mm_loadu_si128((const __m128i*)(kStepFactor + 8));
    for (unsigned j = 0; j < taps; j += 8) {
        __m128i step = _mm_loadu_si128((const __m128i*)(a + j));
        if (j + 8 == taps) step = _mm_mullo_epi16(step, newest);
        const __m128i weights = _mm_load_si128((const __m128i*)(w + j));
        _mm_store_si128((__m128i*)(w + j), _mm_add_epi16(weights, _mm_mullo_epi16(step, s)));
    }
#else
    for (unsigned j = 0; j < taps; ++j) {
        const unsigned lag = taps - j;
        w[j] = (int16_t)(w[j] + a[j] * (lag <= 16 ? kStepFactor[16 - lag] : 1) * sign);
    }
#endif
}
// One filter: 16-bit weights over a 16-bit history of its inputs and the steps their signs give,
// both in mirrored rings, so the window ending at the newest input is contiguous at every cursor.
class Lms {
public:
    void Configure(unsigned taps, unsigned shift, unsigned scale) {
        taps_ = taps;
        shift_ = shift;
        scale_ = scale;
        storage_.assign(5 * (size_t)taps + 16, 0);
        w_ = (int16_t*)(((uintptr_t)storage_.data() + 31) & ~(uintptr_t)31);
        h_ = w_ + taps;
        a_ = h_ + 2 * (size_t)taps;
        cursor_ = 0;
        average_ = 0;
    }
    // Encoder: what the filter leaves of e, then e joins its history. The cross filter's newest
    // input is the other channel's instead, taken before predicting.
    CORD_INLINE int64_t Filter(int64_t e) {
        const int64_t out = e - Prediction(Dot(w_, h_ + cursor_, taps_));
        Adapt(w_, a_ + cursor_, taps_, out);
        Push(e);
        return out;
    }
    CORD_INLINE int64_t FilterCross(int64_t e, int64_t other) {
        Push(other);
        const int64_t out = e - Prediction(Dot(w_, h_ + cursor_, taps_));
        Adapt(w_, a_ + cursor_, taps_, out);
        return out;
    }
    // Decoder, in two halves: the dot product, which needs only earlier samples, then the input
    // that left r, which adapts the weights and (but for the cross filter) joins the history.
    template<class Kernel> CORD_INLINE int32_t Window(const Kernel& kernel) const { return kernel.Dot(w_, h_ + cursor_, taps_); }
    template<class Kernel> CORD_INLINE int64_t Restore(int64_t r, int32_t dot, const Kernel& kernel, bool cross) {
        kernel.Adapt(w_, a_ + cursor_, taps_, r);
        const int64_t in = r + Prediction(dot);
        if (!cross) Push(in);
        return in;
    }
private:
    CORD_INLINE int64_t Prediction(int32_t dot) const {
        return ((int64_t)dot * ((int64_t)1 << scale_) + ((int64_t)1 << (shift_ - 1))) >> shift_;
    }
public:
    // The step is 8, 16 or 32 as the input's magnitude stands to the running average, stored
    // divided by 8; Adapt scales it by the input's age.
    CORD_INLINE void Push(int64_t input) {
        const uint64_t m = (uint64_t)(input < 0 ? -input : input);
        const uint32_t magnitude = m > UINT32_MAX ? UINT32_MAX : (uint32_t)m;
        const int16_t step =
            (int16_t)(1 << ((magnitude > (uint64_t)average_ * 3) + (magnitude > (uint64_t)average_ + average_ / 3)));
        average_ = (uint32_t)((int64_t)average_ + ((int64_t)magnitude - (int64_t)average_) / 16);
        const int64_t scaled = input >> scale_;
        h_[cursor_] = h_[cursor_ + taps_] = (int16_t)(scaled < -32768 ? -32768 : scaled > 32767 ? 32767 : scaled);
        a_[cursor_] = a_[cursor_ + taps_] = input > 0 ? step : input < 0 ? (int16_t)-step : (int16_t)0;
        if (++cursor_ == taps_) cursor_ = 0;
    }
    // The state as it is stored, for a decoding loop that keeps its newest part in registers
    // (audio_avx.cpp) and hands the cursor and average back when it is done.
    struct Raw {
        int16_t *w, *h, *a;
        unsigned taps, shift, scale, cursor;
        uint32_t average;
    };
    Raw Expose() const { return {w_, h_, a_, taps_, shift_, scale_, cursor_, average_}; }
    void Settle(unsigned cursor, uint32_t average) { cursor_ = cursor; average_ = average; }
private:
    std::vector<int16_t> storage_;
    int16_t *w_ = nullptr, *h_ = nullptr, *a_ = nullptr;
    unsigned taps_ = 0, shift_ = 1, scale_ = 0, cursor_ = 0;
    uint32_t average_ = 0;
};

// One coded channel's filters for a block; `second` is the second channel of a stereo pair.
class Cascade {
public:
    void Configure(const CascadeSpec& spec, bool second) {
        count_ = spec.stages;
        for (unsigned s = 0; s < count_; ++s) stage_[s].Configure(spec.taps[s], spec.shifts[s], spec.scale);
        cross_ = second && spec.crossTaps;
        if (cross_) crossStage_.Configure(spec.crossTaps, spec.crossShift, spec.scale);
    }
    // Encoder: the residual coded for the LPC residual e, given the first channel's at this position.
    CORD_INLINE int64_t Filter(int64_t e, int32_t other) {
        if (cross_) e = crossStage_.FilterCross(e, other);
        for (unsigned s = 0; s < count_; ++s) e = stage_[s].Filter(e);
        return e;
    }
    // Decoder: the LPC residual for a coded one, the filters in reverse order. Every filter's dot
    // product needs only earlier samples, so all are taken before the first prediction is needed.
    template<class Kernel> CORD_INLINE int64_t Restore(int64_t r, int32_t other, const Kernel& kernel) {
        int32_t dot[kMaxStages], crossDot = 0;
        if (cross_) {
            crossStage_.Push(other);
            crossDot = crossStage_.Window(kernel);
        }
        for (unsigned s = 0; s < count_; ++s) dot[s] = stage_[s].Window(kernel);
        for (unsigned s = count_; s-- > 0;) r = stage_[s].Restore(r, dot[s], kernel, false);
        return cross_ ? crossStage_.Restore(r, crossDot, kernel, true) : r;
    }
    unsigned Stages() const { return count_; }
    Lms& Stage(unsigned s) { return stage_[s]; }
    Lms* Cross() { return cross_ ? &crossStage_ : nullptr; }
private:
    Lms stage_[kMaxStages], crossStage_;
    unsigned count_ = 0;
    bool cross_ = false;
};

// A predicted channel's way back from coded residuals to LPC residuals: its cascade (null when
// the block has none), where the first channel of a stereo pair records its LPC residuals for the
// cross filter, and where the second reads them.
struct CascadeLink {
    Cascade* cascade = nullptr;
    int32_t* record = nullptr;
    const int32_t* other = nullptr;
};
// Kernel supplies Dot and Adapt with the instruction set of the caller's translation unit.
template<class Kernel> struct Restorer {
    CascadeLink link;
    Kernel kernel;
    CORD_INLINE int64_t operator()(int64_t r, size_t i) {
        const int64_t e = link.cascade->Restore(r, link.other ? link.other[i] : 0, kernel);
        if (link.record) link.record[i] = (int32_t)e;
        return e;
    }
};
struct Unfiltered {
    CORD_INLINE int64_t operator()(int64_t r, size_t) const { return r; }
};

// One predicted channel's n folded residuals. They depend only on the two streams, never on the
// samples, so they are read in a pass of their own: the range decoder's chain then runs without
// the reconstruction's in the same loop, and each loop overlaps its own iterations.
CORD_INLINE bool DecodeFolded(BitReader& raw, RangeDecoder& rc, Models& models, uint32_t* folded, size_t n, unsigned k0) {
    Rice rice;
    rice.Seed(k0);
    for (size_t i = 0; i < n; ++i) {
        const unsigned k = Split(rice.k);
        uint16_t* m = models.For(k);
        unsigned q = 0;
        while (q < kEscape && rc.Bit(m[std::min(q, 15u)])) ++q;
        uint32_t u;
        if (q == kEscape) {
            u = raw.Get(32);
        } else {
            u = q << k;
            if (k) u |= (uint32_t)rc.Bit(m[16 + (q > 0)]) << (k - 1) | raw.Get(k - 1);
        }
        folded[i] = u;
        rice.Update(u);
    }
    return raw.Good() && rc.Good();
}

// One predicted channel's n samples into x (its history before it) from its folded residuals,
// between low and high. The predictor gives x[i]'s prediction and hears each value decoded, so a
// vector path can keep its own copy of the samples; `restore` turns each coded residual back into
// the prediction's.
template<class Predictor, class Restore>
bool Reconstruct(const uint32_t* folded, int32_t* x, size_t n, int64_t low, int64_t high, Predictor& predictor, Restore& restore) {
    for (size_t i = 0; i < n; ++i) {
        const int64_t v = predictor.Predict(i) + restore(Unfold(folded[i]), i);
        if (v < low || v > high) return false;
        x[i] = (int32_t)v;
        predictor.Decoded(i, (int32_t)v);
    }
    return true;
}

// A predicted channel of one frame, read and waiting to be rebuilt: its folded residuals, its
// samples (x, with kMaxOrder of history before it), its predictor, range and cascade.
struct PredictedChannel {
    const uint32_t* folded;
    int32_t* x;
    double* scratch;  // kMaxOrder + n doubles, for the AVX2 predictor
    size_t n;
    int32_t coefficients[kMaxOrder];
    unsigned order, shift;
    int64_t low, high;
    CascadeLink link;
};

// Reconstruct built in the AVX2 translation unit, with a fused multiply-add predictor for long
// orders and 256-bit cascade filters; call these only when nativeaudio::FastAudioKernels() holds.
// The pair form rebuilds a stereo frame's two channels sample by sample in one loop: each
// sample's filters form a chain too long for the processor to overlap one sample with the next,
// so a second, independent chain beside it nearly doubles the work done in the same time. The
// second channel's cross filter reads the first's value at each position, already rebuilt.
bool ReconstructAvx2(const PredictedChannel& c);
bool ReconstructPairAvx2(const PredictedChannel& first, const PredictedChannel& second);

// The encoder's cascade over one predicted channel's folded LPC residuals, into the folded
// residuals it leaves, in the AVX2 translation unit: Cascade::Filter for each sample, recording the
// inputs for the cross filter (record) or reading the first channel's (other). Only the encoder's
// own preset is handled there; false means the caller filters it itself. `overflow` is set when a
// residual left needs more than 32 bits.
bool FilterAvx2(Cascade& cascade, const uint32_t* source, size_t n, int32_t* record, const int32_t* other, uint32_t* out,
                bool& overflow);

// The encoder's correlations: out[lag] = the sum of x[i] * x[i - lag] for i from `from` (or from
// lag, when x has no history before it) to n - 1, for every lag up to maxLag. The sums run in eight
// lanes by i mod 8, joined in a fixed order, so the baseline (cord_encode.cpp) and the AVX2 form
// give the same doubles and every build writes the same file.
void CorrelateAvx2(const double* x, size_t n, ptrdiff_t from, bool history, unsigned maxLag, double* out);

// The encoder's folded residuals of one predictor over a frame, sixteen samples at a time in the
// AVX2 translation unit; the same rule only. `y` holds the samples as doubles, kMaxOrder of history
// before them. False when a residual needs more than 32 bits, as the integer path would find.
bool FoldResidualsAvx2(const double* y, size_t n, const int32_t* coefficients, unsigned order, unsigned shift,
                       uint32_t* folded);

// CRC-32 (the zlib polynomial, as Crc32 in inflate.h) over a block payload, eight bytes a step.
uint32_t Crc32(const uint8_t* p, size_t n);

}  // namespace cord
