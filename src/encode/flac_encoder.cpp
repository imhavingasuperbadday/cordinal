// cordinal FLAC encoder (RFC 9639). Choices use estimated Rice costs, which never undercount:
// the sum of each residual's quotient is at most the partition's sum shifted once. The bits written
// are exact, and every predictor's residuals are checked to fit FLAC's signed 32-bit range.
#include "flac_encoder.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace cordinal::encode {

constexpr unsigned kMaxLpcOrder = 12, kMaxPartitionOrder = 8, kLpcTrials = 3;

struct FlacEncoder::Plan {
    enum Kind : uint8_t { kConstant, kVerbatim, kFixed, kLpc } kind = kVerbatim;
    unsigned wasted = 0, order = 0, precision = 0, shift = 0, partitionOrder = 0;
    bool wide = false;  // 5-bit Rice parameters (method 1), needed above 14
    uint64_t bits = 0;
    int32_t q[kMaxLpcOrder]{};
    uint8_t rice[1u << kMaxPartitionOrder]{};
    std::vector<int32_t> samples;    // the channel without its wasted bits
    std::vector<uint32_t> residual;  // folded residuals from `order` on
};

namespace {

struct CrcTables {
    uint8_t crc8[256];
    uint16_t crc16[256];
    CrcTables() {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned a = i, b = i << 8;
            for (int j = 0; j < 8; ++j) {
                a = (a << 1) ^ (a & 0x80 ? 0x07 : 0);
                b = (b << 1) ^ (b & 0x8000 ? 0x8005 : 0);
            }
            crc8[i] = (uint8_t)a;
            crc16[i] = (uint16_t)b;
        }
    }
};

const CrcTables& Crc() {
    static const CrcTables tables;
    return tables;
}

constexpr int32_t kFixed[5][4] = {{}, {1}, {2, -1}, {3, -3, 1}, {4, -6, 4, -1}};

// Folded residuals of y[order..n) under the prediction sum(q[j] * y[i-1-j]) >> shift, which
// covers the fixed predictors too (shift 0). False when one doesn't fit FLAC's 32-bit residual.
// Each order is its own loop, unrolled by the compiler, and the range is checked once at the end.
template <unsigned Order>
bool ResidualOf(const int32_t* y, size_t n, const int32_t* q, unsigned shift, uint32_t* u) {
    int64_t c[Order ? Order : 1]{};
    for (unsigned j = 0; j < Order; ++j) c[j] = q[j];
    uint64_t outside = 0;
    for (size_t i = Order; i < n; ++i) {
        int64_t p = 0;
        for (unsigned j = 0; j < Order; ++j) p += c[j] * y[i - 1 - j];
        const int64_t r = y[i] - (p >> shift);
        outside |= (uint64_t)(r + INT32_MAX) > 2ull * INT32_MAX;
        u[i] = (uint32_t)((r << 1) ^ (r >> 63));
    }
    return !outside;
}

template <size_t... Orders>
constexpr auto ResidualTable(std::index_sequence<Orders...>) {
    return std::array{&ResidualOf<Orders>...};
}
constexpr auto kResidual = ResidualTable(std::make_index_sequence<kMaxLpcOrder + 1>{});

bool Residual(const int32_t* y, size_t n, unsigned order, const int32_t* q, unsigned shift, uint32_t* u) {
    return kResidual[order](y, n, q, shift, u);
}

// The best Rice parameter for a partition holding `count` residuals summing to `sum`, and its cost.
unsigned Parameter(uint64_t sum, uint64_t count, uint64_t& cost) {
    if (!count) { cost = 0; return 0; }
    const uint64_t mean = sum / count;
    const unsigned guess = mean ? (unsigned)std::bit_width(mean) - 1 : 0;
    unsigned best = 0;
    cost = UINT64_MAX;
    for (unsigned k = guess ? guess - 1 : 0; k <= std::min(30u, guess + 1); ++k) {
        const uint64_t bits = count * (k + 1) + (sum >> k);
        if (bits < cost) { cost = bits; best = k; }
    }
    return best;
}

// The cheapest partition order for u[order..n), with each partition's parameter: the residual
// section's estimated bits, method and partition order fields included.
uint64_t Partition(const uint32_t* u, size_t n, unsigned order, unsigned& po, bool& wide, uint8_t* params) {
    unsigned top = 0;
    while (top < kMaxPartitionOrder && !(n & (((size_t)1 << (top + 1)) - 1)) && (n >> (top + 1)) >= order) ++top;
    uint64_t sums[1u << kMaxPartitionOrder];
    const size_t length = n >> top;
    for (size_t part = 0, i = order; part < ((size_t)1 << top); ++part) {
        uint64_t sum = 0;
        for (const size_t end = (part + 1) * length; i < end; ++i) sum += u[i];
        sums[part] = sum;
    }
    uint64_t best = UINT64_MAX;
    for (int level = (int)top;; --level) {
        const size_t parts = (size_t)1 << level, size = n >> level;
        uint64_t total = 6;
        unsigned high = 0;
        uint8_t chosen[1u << kMaxPartitionOrder];
        for (size_t part = 0; part < parts; ++part) {
            uint64_t cost;
            chosen[part] = (uint8_t)Parameter(sums[part], size - (part ? 0 : order), cost);
            high = std::max<unsigned>(high, chosen[part]);
            total += cost;
        }
        total += parts * (high > 14 ? 5 : 4);
        if (total < best) {
            best = total;
            po = (unsigned)level;
            wide = high > 14;
            std::memcpy(params, chosen, parts);
        }
        if (!level) break;
        for (size_t part = 0; part < parts / 2; ++part) sums[part] = sums[2 * part] + sums[2 * part + 1];
    }
    return best;
}

// Quantizes predictor coefficients to `precision` signed bits with the largest shift (0..15) that
// holds them, carrying each rounding error into the next. False when no shift can hold them.
bool Quantize(const double* lp, unsigned order, unsigned precision, int32_t* q, unsigned& shift) {
    double peak = 0;
    for (unsigned i = 0; i < order; ++i) peak = std::max(peak, std::fabs(lp[i]));
    if (!(peak > 0) || !std::isfinite(peak)) return false;
    int exponent;
    std::frexp(peak, &exponent);
    const int s = std::min(15, (int)precision - 1 - exponent);
    if (s < 0) return false;
    const int32_t high = (1 << (precision - 1)) - 1, low = -(1 << (precision - 1));
    double carry = 0;
    for (unsigned i = 0; i < order; ++i) {
        const double v = std::ldexp(lp[i], s) + carry;
        const int32_t r = (int32_t)std::clamp<long long>(std::llround(v), low, high);
        q[i] = r;
        carry = v - r;
    }
    shift = (unsigned)s;
    return true;
}

}  // namespace

bool FlacEncoder::Supports(unsigned rate, unsigned channels, unsigned bits, uint64_t frames) {
    return bits >= 4 && bits <= 32 && channels >= 1 && channels <= 8 && rate >= 1 && rate < (1u << 20) &&
           frames < (1ull << 36);
}

FlacEncoder::FlacEncoder(unsigned rate, unsigned channels, unsigned bits)
    : rate_(rate), channels_(channels), bits_(bits), channel_(std::max(channels, 4u)), plans_(std::max(channels, 4u)) {}

FlacEncoder::~FlacEncoder() = default;

void FlacEncoder::Choose(const int32_t* x, size_t n, unsigned bits, Plan& plan) {
    plan.wasted = plan.order = 0;
    if (std::all_of(x, x + n, [&](int32_t s) { return s == x[0]; })) {
        plan.kind = Plan::kConstant;
        plan.samples.assign(x, x + 1);
        plan.bits = 8 + bits;
        return;
    }
    uint32_t any = 0;
    for (size_t i = 0; i < n; ++i) any |= (uint32_t)x[i];
    const unsigned wasted = (unsigned)std::countr_zero(any), w = bits - wasted;
    plan.wasted = wasted;
    plan.samples.resize(n);
    for (size_t i = 0; i < n; ++i) plan.samples[i] = x[i] >> wasted;
    const int32_t* y = plan.samples.data();
    plan.kind = Plan::kVerbatim;
    plan.bits = 8 + wasted + (uint64_t)n * w;
    plan.residual.resize(n);
    trial_.resize(n);
    auto consider = [&](Plan::Kind kind, unsigned order, const int32_t* q, unsigned shift, unsigned precision) {
        if (!Residual(y, n, order, q, shift, trial_.data())) return;
        unsigned po = 0;
        bool wide = false;
        uint8_t params[1u << kMaxPartitionOrder];
        const uint64_t cost = 8 + wasted + (uint64_t)order * w + (kind == Plan::kLpc ? 9 + order * precision : 0) +
                              Partition(trial_.data(), n, order, po, wide, params);
        if (cost >= plan.bits) return;
        plan.kind = kind;
        plan.bits = cost;
        plan.order = order;
        plan.precision = precision;
        plan.shift = shift;
        plan.partitionOrder = po;
        plan.wide = wide;
        std::copy_n(q, order, plan.q);
        std::memcpy(plan.rice, params, (size_t)1 << po);
        std::swap(plan.residual, trial_);
    };
    for (unsigned order = 0; order <= 4 && order < n; ++order) consider(Plan::kFixed, order, kFixed[order], 0, 0);
    if (n <= 2 * kMaxLpcOrder) return;

    // Tukey(0.5)-windowed autocorrelation and Levinson-Durbin give a predictor of every order.
    if (window_.size() != n) {
        window_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const double t = (double)i / (double)(n - 1), edge = std::min(t, 1 - t);
            window_[i] = edge < 0.25 ? 0.5 * (1 - std::cos(2 * 3.14159265358979323846 * edge / 0.5)) : 1.0;
        }
        windowed_.resize(n);
    }
    for (size_t i = 0; i < n; ++i) windowed_[i] = y[i] * window_[i];
    double r[kMaxLpcOrder + 1];
    for (unsigned lag = 0; lag <= kMaxLpcOrder; ++lag) {
        double sum = 0;
        for (size_t i = lag; i < n; ++i) sum += windowed_[i] * windowed_[i - lag];
        r[lag] = sum;
    }
    if (!(r[0] > 0)) return;
    r[0] *= 1 + 1e-10;  // a little white noise keeps the recursion stable on pure tones
    double lpc[kMaxLpcOrder + 1][kMaxLpcOrder]{}, a[kMaxLpcOrder]{}, error = r[0], estimate[kMaxLpcOrder + 1];
    unsigned top = 0;
    // Coefficients take 12 bits at FLAC's usual block size; quieter, narrower audio needs fewer.
    const unsigned precision = std::clamp(w / 2 + 4, 5u, 12u);
    const double scale = 0.5 * 0.693147180559945309 * 0.693147180559945309 / (double)n;
    for (unsigned p = 1; p <= kMaxLpcOrder; ++p) {
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
        std::copy_n(a, p, lpc[p]);
        const double perSample = error > 0 ? std::max(0.0, 0.5 * std::log2(error * scale)) : 0;
        estimate[p] = perSample * (double)(n - p) + p * (double)(w + precision);
        top = p;
        if (!(error > 0)) break;
    }
    bool tried[kMaxLpcOrder + 1]{};
    for (unsigned trial = 0; trial < kLpcTrials && trial < top; ++trial) {
        unsigned order = 0;
        for (unsigned p = 1; p <= top; ++p)
            if (!tried[p] && (!order || estimate[p] < estimate[order])) order = p;
        tried[order] = true;
        int32_t q[kMaxLpcOrder];
        unsigned shift;
        if (Quantize(lpc[order], order, precision, q, shift)) consider(Plan::kLpc, order, q, shift, precision);
    }
}

void FlacEncoder::Write(const Plan& plan, size_t n, unsigned bits) {
    BitWriter& out = writer_;
    const unsigned w = bits - plan.wasted;
    const unsigned type = plan.kind == Plan::kConstant ? 0 : plan.kind == Plan::kVerbatim ? 1
                        : plan.kind == Plan::kFixed    ? 8 + plan.order : 31 + plan.order;
    out.Put(type << 1 | (plan.wasted ? 1 : 0), 8);
    if (plan.wasted) out.Put(1, plan.wasted);  // unary: wasted - 1 zeros, then a one
    if (plan.kind == Plan::kConstant) { out.Put((uint32_t)plan.samples[0], w); return; }
    if (plan.kind == Plan::kVerbatim) {
        for (size_t i = 0; i < n; ++i) out.Put((uint32_t)plan.samples[i], w);
        return;
    }
    for (unsigned i = 0; i < plan.order; ++i) out.Put((uint32_t)plan.samples[i], w);
    if (plan.kind == Plan::kLpc) {
        out.Put(plan.precision - 1, 4);
        out.Put(plan.shift, 5);
        for (unsigned i = 0; i < plan.order; ++i) out.Put((uint32_t)plan.q[i], plan.precision);
    }
    out.Put(plan.wide ? 1 : 0, 2);
    out.Put(plan.partitionOrder, 4);
    const size_t length = n >> plan.partitionOrder;
    for (size_t part = 0, i = plan.order; part < ((size_t)1 << plan.partitionOrder); ++part) {
        const unsigned k = plan.rice[part];
        out.Put(k, plan.wide ? 5 : 4);
        for (const size_t end = (part + 1) * length; i < end; ++i) {
            const uint32_t u = plan.residual[i];
            uint32_t zeros = u >> k;
            while (zeros >= 32) { out.Put(0, 32); zeros -= 32; }
            out.Put(1, zeros + 1);
            out.Put(u, k);
        }
    }
}

void FlacEncoder::Frame(const int32_t* pcm, size_t n, std::string& out) {
    if (!n || n > kBlockFrames) return;
    for (unsigned c = 0; c < channels_; ++c) {
        auto& x = channel_[c];
        x.resize(n);
        for (size_t i = 0; i < n; ++i) x[i] = pcm[i * channels_ + c];
    }
    // Side needs one bit more than the samples, which 32-bit samples can't spare.
    const bool stereo = channels_ == 2 && bits_ < 32;
    unsigned assignment = channels_ - 1;
    const Plan* order[8];
    if (stereo) {
        auto &l = channel_[0], &r = channel_[1], &side = channel_[2], &mid = channel_[3];
        side.resize(n);
        mid.resize(n);
        for (size_t i = 0; i < n; ++i) {
            side[i] = l[i] - r[i];
            mid[i] = (int32_t)(((int64_t)l[i] + r[i]) >> 1);
        }
        Choose(l.data(), n, bits_, plans_[0]);
        Choose(r.data(), n, bits_, plans_[1]);
        Choose(side.data(), n, bits_ + 1, plans_[2]);
        Choose(mid.data(), n, bits_, plans_[3]);
        const uint64_t lb = plans_[0].bits, rb = plans_[1].bits, sb = plans_[2].bits, mb = plans_[3].bits;
        const uint64_t costs[4] = {lb + rb, lb + sb, sb + rb, mb + sb};
        const unsigned mode = (unsigned)(std::min_element(costs, costs + 4) - costs);
        static constexpr unsigned pairs[4][2] = {{0, 1}, {0, 2}, {2, 1}, {3, 2}};
        assignment = mode ? 7 + mode : 1;
        order[0] = &plans_[pairs[mode][0]];
        order[1] = &plans_[pairs[mode][1]];
    } else {
        for (unsigned c = 0; c < channels_; ++c) {
            Choose(channel_[c].data(), n, bits_, plans_[c]);
            order[c] = &plans_[c];
        }
    }

    // The frame header states common rates and depths too, for players that start mid-stream.
    constexpr unsigned rates[] = {0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
    constexpr unsigned depths[] = {0, 8, 12, 0, 16, 20, 24, 32};
    unsigned rate = 0, depth = 0;
    for (unsigned i = 1; i < std::size(rates); ++i) if (rates[i] == rate_) rate = i;
    for (unsigned i = 1; i < std::size(depths); ++i) if (depths[i] == bits_) depth = i;
    if (!rate && rate_ <= 65535) rate = 13;
    else if (!rate && rate_ % 10 == 0 && rate_ <= 655350) rate = 14;
    else if (!rate && rate_ % 1000 == 0 && rate_ <= 255000) rate = 12;
    const unsigned block = n == kBlockFrames ? 12 : n <= 256 ? 6 : 7;
    BitWriter& bits = writer_;
    bits.Clear();
    bits.Put(0xFFF8, 16);
    bits.Put(block << 4 | rate, 8);
    bits.Put(assignment << 4 | depth << 1, 8);
    const uint32_t number = number_++;
    if (number < 128) bits.Put(number, 8);
    else {
        unsigned bytes = 2;
        while (number >= (1u << (5 * bytes + 1))) ++bytes;
        bits.Put((0xFFu << (8 - bytes) | number >> ((bytes - 1) * 6)) & 0xFF, 8);
        while (--bytes) bits.Put(0x80 | ((number >> ((bytes - 1) * 6)) & 63), 8);
    }
    if (block == 6) bits.Put((uint32_t)(n - 1), 8);
    if (block == 7) bits.Put((uint32_t)(n - 1), 16);
    if (rate == 12) bits.Put(rate_ / 1000, 8);
    if (rate == 13) bits.Put(rate_, 16);
    if (rate == 14) bits.Put(rate_ / 10, 16);
    const CrcTables& crc = Crc();
    uint8_t c8 = 0;
    for (uint8_t b : bits.BytesVector()) c8 = crc.crc8[c8 ^ b];
    bits.Put(c8, 8);
    for (unsigned c = 0; c < channels_; ++c) {
        // In the side modes, the side channel is the one coded with an extra bit.
        const bool side = stereo && order[c] == &plans_[2];
        Write(*order[c], n, bits_ + (side ? 1 : 0));
    }
    bits.Align();
    uint16_t c16 = 0;
    for (uint8_t b : bits.BytesVector()) c16 = (uint16_t)(c16 << 8 ^ crc.crc16[(c16 >> 8) ^ b]);
    bits.Put(c16, 16);
    const auto& bytes = bits.BytesVector();
    out.append((const char*)bytes.data(), bytes.size());
    const uint32_t size = (uint32_t)bytes.size();
    minFrame_ = minFrame_ ? std::min(minFrame_, size) : size;
    maxFrame_ = std::max(maxFrame_, size);
}

std::string FlacStreamInfo(unsigned rate, unsigned channels, unsigned bits, uint64_t frames, uint32_t minFrame,
                           uint32_t maxFrame, const std::array<uint8_t, 16>& md5) {
    std::string info;
    auto put = [&](uint64_t value, unsigned bytes) { while (bytes) info += (char)(value >> (--bytes * 8)); };
    put(FlacEncoder::kBlockFrames, 2);
    put(FlacEncoder::kBlockFrames, 2);  // the final block may be shorter, even shorter than 16 frames
    put(minFrame, 3);
    put(maxFrame, 3);
    put((uint64_t)rate << 44 | (uint64_t)(channels - 1) << 41 | (uint64_t)(bits - 1) << 36 | frames, 8);
    info.append((const char*)md5.data(), md5.size());
    return info;
}

}  // namespace cordinal::encode
