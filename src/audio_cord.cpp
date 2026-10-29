// Decoder for cordinal's own format (docs/cord-format.md): the container in cord.h and the CL03
// lossless codec in cord_codec.h. A block is checked against its CRC-32 before any of its samples
// are used, decoded whole into planar integers, and then read as float for playback or as exact
// integers for conversion (PcmReader). Seeking decodes at most one block.
#include "native_audio.h"
#include "audio_file.h"
#include "audio_kernels.h"
#include "cord.h"
#include "cord_codec.h"

#include <array>
#include <cmath>

namespace {
using namespace nativeaudio;
using namespace cord;

// Slice-by-8 tables for the zlib CRC-32, derived on first use.
std::array<std::array<uint32_t, 256>, 8> crcTable;
void InitCrc() {
    static const bool ready = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
            crcTable[0][i] = c;
        }
        for (int t = 1; t < 8; ++t)
            for (uint32_t i = 0; i < 256; ++i) crcTable[t][i] = (crcTable[t - 1][i] >> 8) ^ crcTable[0][crcTable[t - 1][i] & 255];
        return true;
    }();
    (void)ready;
}

// The baseline predictor for Reconstruct: 64-bit integer sums over the samples themselves.
struct ScalarPredictor {
    const int32_t* x;
    const int32_t* coefficients;
    unsigned order, shift;
    int64_t Predict(size_t i) const { return cord::Predict(x + i, coefficients, order, shift); }
    void Decoded(size_t, int32_t) const {}
};
struct BaselineKernel {
    int32_t Dot(const int16_t* w, const int16_t* h, unsigned taps) const { return cord::Dot(w, h, taps); }
    void Adapt(int16_t* w, const int16_t* a, unsigned taps, int64_t error) const { cord::Adapt(w, a, taps, error); }
};

class CordDecoder final : public Decoder {
public:
    bool Open(File file);
    size_t Read(float* out, size_t frames) override;
    size_t ReadExact(int32_t* out, size_t frames);
    bool Seek(double seconds) override;
    double Duration() const override { return (double)info_.frames / info_.rate; }
    unsigned Rate() const override { return info_.rate; }
    bool Failed() const override { return failed_; }
    PcmFormat ExactFormat() const {
        PcmFormat f;
        f.rate = info_.rate; f.channels = info_.channels; f.bits = info_.bits; f.channelMask = info_.channelMask;
        f.frames = info_.frames; f.md5 = info_.md5;
        return f;
    }
private:
    bool Load(uint64_t block);
    bool DecodeFrame(BitReader& raw, RangeDecoder& rc, size_t offset, size_t n);
    bool ReadChannel(BitReader& raw, RangeDecoder& rc, int32_t* x, size_t n, unsigned width, PredictedChannel& plan,
                     unsigned& wasted, bool& predicted);
    bool Rebuild(const PredictedChannel& plan);
    bool Fail() { failed_ = true; block_ = UINT64_MAX; buffered_ = cursor_ = 0; return false; }
    // Pulls blocks and hands `emit` (frames done, position in the block, count) each run of
    // frames until the request is filled or the stream ends.
    template<class Emit> size_t Copy(size_t frames, Emit emit);
    File file_;
    Info info_;
    std::vector<uint64_t> offsets_;  // where each block starts in the file, then where AUDI ends
    std::vector<int32_t> samples_;   // the loaded block, planar: channel c at c * blockLength
    std::vector<int32_t> coded_;     // one frame's coded channels, each kMaxOrder of history first
    std::vector<double> scratch_;    // the AVX2 predictor's copy of each coded channel
    std::vector<uint32_t> folded_;   // each coded channel's folded residuals, read before it is rebuilt
    std::vector<int32_t> crossInput_;  // the first coded channel's LPC residuals in this frame
    Models models_;
    CascadeSpec spec_;
    Cascade cascades_[kMaxChannels];
    uint64_t block_ = UINT64_MAX;    // the block in samples_
    size_t buffered_ = 0, cursor_ = 0;
    bool failed_ = false, fast_ = FastAudioKernels();
};

// Walks the chunks: INFO must come first, INDX before AUDI, and the block sizes INDX lists must
// add up to AUDI exactly.
bool CordDecoder::Open(File file) {
    InitCrc();
    file_ = std::move(file);
    file_.Sequential();
    const uint64_t size = file_.Size();
    const uint8_t* p = file_.View(0, kHeaderBytes);
    if (!p || !IsHeader(p)) return false;
    uint64_t at = kHeaderBytes, index = 0, indexBytes = 0, audio = 0, audioBytes = 0;
    bool info = false;
    while (at < size) {
        if (size - at < kChunkHeaderBytes || !(p = file_.View(at, kChunkHeaderBytes))) return false;
        const uint32_t id = Le32(p);
        const uint64_t bytes = Le64(p + 4), body = at + kChunkHeaderBytes;
        if (bytes > size - body) return false;
        if (!info) {
            const size_t n = (size_t)std::min<uint64_t>(bytes, 256);
            if (id != kInfo || !(p = file_.View(body, n)) || !ParseInfo(p, n, info_)) return false;
            info = true;
        } else if (id == kIndex && !index) {
            index = body; indexBytes = bytes;
        } else if (id == kAudio && index && !audio) {
            audio = body; audioBytes = bytes;
        }
        at = body + bytes;
    }
    if (!info || !audio) return false;
    const uint64_t blocks = info_.Blocks();
    if (indexBytes != blocks * 4) return false;
    offsets_.assign(1, audio);
    if (blocks && !(p = file_.View(index, (size_t)indexBytes))) return false;
    for (uint64_t i = 0; i < blocks; ++i) {
        const uint32_t n = Le32(p + i * 4);
        if (n < 8) return false;  // a CRC and the decisions' length at least
        offsets_.push_back(offsets_.back() + n);
    }
    if (offsets_.back() != audio + audioBytes) return false;
    samples_.resize((size_t)info_.blockLength * info_.channels);
    coded_.resize((size_t)(kMaxOrder + info_.frameLength) * info_.channels);
    scratch_.resize((size_t)(kMaxOrder + info_.frameLength) * info_.channels);
    folded_.resize((size_t)info_.frameLength * info_.channels);
    crossInput_.resize(info_.frameLength);
    return true;
}

bool CordDecoder::Load(uint64_t block) {
    const uint64_t at = offsets_[(size_t)block], bytes = offsets_[(size_t)block + 1] - at;
    const size_t frames = (size_t)std::min<uint64_t>(info_.blockLength, info_.frames - block * info_.blockLength);
    const uint8_t* p = file_.View(at, (size_t)bytes);
    if (!p || Le32(p) != Crc32(p + 4, (size_t)bytes - 4)) return Fail();
    // The payload: the decisions' length and bytes, then the raw stream to the block's end.
    const size_t decisions = Le32(p + 4), payload = (size_t)bytes - 8;
    if (decisions > payload) return Fail();
    RangeDecoder rc(p + 8, decisions);
    BitReader raw(p + 8 + decisions, payload - decisions);
    models_.Reset();
    if (!ReadCascade(raw, info_.channels, spec_)) return Fail();
    for (unsigned c = 0; c < info_.channels; ++c) cascades_[c].Configure(spec_, info_.channels == 2 && c == 1);
    for (size_t o = 0; o < frames; o += info_.frameLength)
        if (!DecodeFrame(raw, rc, o, std::min<size_t>(info_.frameLength, frames - o))) return Fail();
    // Both streams end exactly: the decisions at their last byte, the raw bits at a zero padding.
    const unsigned pad = (unsigned)((8 - raw.Position() % 8) % 8);
    if (!rc.Finished() || raw.Get(pad) || !raw.Good() || raw.Position() != (payload - decisions) * 8) return Fail();
    block_ = block;
    buffered_ = frames;
    return true;
}

bool CordDecoder::DecodeFrame(BitReader& raw, RangeDecoder& rc, size_t o, size_t n) {
    const unsigned channels = info_.channels, mode = channels == 2 ? raw.Get(2) : kLeftRight;
    if (mode != kLeftRight && info_.bits > 31) return false;
    const size_t stride = kMaxOrder + info_.frameLength, length = info_.blockLength;
    // History: the block's previous kMaxOrder frames restated in this frame's stereo mode.
    for (unsigned j = 0; j < kMaxOrder; ++j) {
        const ptrdiff_t at = (ptrdiff_t)o - (ptrdiff_t)kMaxOrder + (ptrdiff_t)j;
        if (channels == 2) {
            int64_t a = 0, c = 0;
            if (at >= 0) Decorrelate(mode, samples_[(size_t)at], samples_[length + (size_t)at], a, c);
            coded_[j] = (int32_t)a;
            coded_[stride + j] = (int32_t)c;
        } else {
            for (unsigned c = 0; c < channels; ++c) coded_[c * stride + j] = at >= 0 ? samples_[c * length + (size_t)at] : 0;
        }
    }
    // Every channel is read before any is rebuilt; a stereo frame whose channels are both predicted
    // is rebuilt as a pair (cord_codec.h, ReconstructPairAvx2).
    PredictedChannel plans[kMaxChannels];
    unsigned wasted[kMaxChannels];
    bool predicted[kMaxChannels];
    for (unsigned c = 0; c < channels; ++c) {
        PredictedChannel& plan = plans[c];
        plan.link = CascadeLink{};
        if (spec_.Any()) plan.link.cascade = &cascades_[c];
        if (channels == 2 && spec_.crossTaps) {
            if (c) plan.link.other = crossInput_.data();
            else plan.link.record = crossInput_.data();
        }
        plan.folded = folded_.data() + (size_t)c * info_.frameLength;
        plan.scratch = scratch_.data() + (size_t)c * stride;
        if (!ReadChannel(raw, rc, coded_.data() + c * stride + kMaxOrder, n, CodedBits(info_.bits, mode, c), plan, wasted[c],
                         predicted[c]))
            return false;
    }
    if (fast_ && channels == 2 && predicted[0] && predicted[1] && spec_.Any()) {
        if (!ReconstructPairAvx2(plans[0], plans[1])) return false;
    } else {
        for (unsigned c = 0; c < channels; ++c)
            if (predicted[c] && !Rebuild(plans[c])) return false;
    }
    for (unsigned c = 0; c < channels; ++c)
        if (wasted[c]) {
            int32_t* x = coded_.data() + c * stride + kMaxOrder;
            for (size_t i = 0; i < n; ++i) x[i] = (int32_t)((uint32_t)x[i] << wasted[c]);
        }
    if (channels == 2) {
        const int32_t *a = coded_.data() + kMaxOrder, *c = a + stride;
        const int64_t bound = int64_t(1) << (info_.bits - 1);
        for (size_t i = 0; i < n; ++i) {
            int64_t l, r;
            Correlate(mode, a[i], c[i], l, r);
            if (l < -bound || l >= bound || r < -bound || r >= bound) return false;
            samples_[o + i] = (int32_t)l;
            samples_[length + o + i] = (int32_t)r;
        }
    } else {
        for (unsigned c = 0; c < channels; ++c)
            std::copy_n(coded_.data() + c * stride + kMaxOrder, n, samples_.data() + c * length + o);
    }
    return true;
}

// x[-kMaxOrder .. -1] holds the channel's history; x[0 .. n) receives its samples. A constant or
// verbatim channel is decoded here; a predicted one is read into `plan` for Rebuild. Its samples
// are left shifted down by `wasted` bits, which the caller restores.
bool CordDecoder::ReadChannel(BitReader& raw, RangeDecoder& rc, int32_t* x, size_t n, unsigned width, PredictedChannel& plan,
                              unsigned& wasted, bool& predicted) {
    const unsigned coding = raw.Get(2);
    wasted = raw.Get(1) ? raw.Get(5) + 1 : 0;
    predicted = coding == kPredicted;
    if (coding > kPredicted || wasted >= width) return false;
    const unsigned w = width - wasted;
    if (wasted)
        for (int j = -(int)kMaxOrder; j < 0; ++j) x[j] >>= wasted;
    if (!predicted && plan.link.record) std::fill_n(plan.link.record, n, 0);
    if (coding == kConstant) {
        std::fill_n(x, n, raw.Signed(w));
    } else if (coding == kVerbatim) {
        for (size_t i = 0; i < n; ++i) x[i] = raw.Signed(w);
    } else {
        plan.order = raw.Get(6);
        if (plan.order > kMaxOrder) return false;
        plan.shift = 0;
        if (plan.order) {
            const unsigned precision = raw.Get(4) + 1;
            plan.shift = raw.Get(5);
            for (unsigned j = 0; j < plan.order; ++j) plan.coefficients[j] = raw.Signed(precision);
        }
        const unsigned k0 = raw.Get(5);
        if (!DecodeFolded(raw, rc, models_, (uint32_t*)plan.folded, n, k0)) return false;
        plan.x = x;
        plan.n = n;
        plan.low = -(int64_t(1) << (w - 1));
        plan.high = (int64_t(1) << (w - 1)) - 1;
    }
    return raw.Good();
}

bool CordDecoder::Rebuild(const PredictedChannel& plan) {
    if (fast_) return ReconstructAvx2(plan);
    ScalarPredictor predictor{plan.x, plan.coefficients, plan.order, plan.shift};
    if (plan.link.cascade) {
        Restorer<BaselineKernel> restore{plan.link, {}};
        return Reconstruct(plan.folded, plan.x, plan.n, plan.low, plan.high, predictor, restore);
    }
    Unfiltered restore;
    return Reconstruct(plan.folded, plan.x, plan.n, plan.low, plan.high, predictor, restore);
}

template<class Emit> size_t CordDecoder::Copy(size_t frames, Emit emit) {
    size_t done = 0;
    while (done < frames) {
        if (cursor_ == buffered_) {
            const uint64_t next = block_ == UINT64_MAX ? 0 : block_ + 1;
            if (failed_ || next + 1 >= offsets_.size() || !Load(next)) break;
            cursor_ = 0;
        }
        const size_t n = std::min(frames - done, buffered_ - cursor_);
        emit(done, cursor_, n);
        cursor_ += n;
        done += n;
    }
    return done;
}

size_t CordDecoder::Read(float* out, size_t frames) {
    const float scale = std::ldexp(1.0f, 1 - (int)info_.bits);
    const unsigned channels = info_.channels;
    const size_t length = info_.blockLength;
    return Copy(frames, [&](size_t done, size_t at, size_t n) {
        const int32_t* l = samples_.data() + at;
        float* o = out + done * 2;
        if (channels <= 2) {
            const int32_t* r = channels == 2 ? l + length : l;
            for (size_t i = 0; i < n; ++i) { o[i * 2] = (float)l[i] * scale; o[i * 2 + 1] = (float)r[i] * scale; }
        } else {
            for (size_t i = 0; i < n; ++i) {
                float mix[kMaxChannels];
                for (unsigned c = 0; c < channels; ++c) mix[c] = (float)l[c * length + i] * scale;
                Mix(mix, channels, o + i * 2);
            }
        }
    });
}

size_t CordDecoder::ReadExact(int32_t* out, size_t frames) {
    const unsigned channels = info_.channels;
    const size_t length = info_.blockLength;
    return Copy(frames, [&](size_t done, size_t at, size_t n) {
        for (unsigned c = 0; c < channels; ++c) {
            const int32_t* s = samples_.data() + c * length + at;
            int32_t* o = out + done * channels + c;
            for (size_t i = 0; i < n; ++i) o[i * channels] = s[i];
        }
    });
}

// Seeks to floor(seconds * rate), decoding the block that holds it.
bool CordDecoder::Seek(double seconds) {
    if (!std::isfinite(seconds)) return false;
    const uint64_t target = seconds <= 0 ? 0 : seconds >= Duration() ? info_.frames : (uint64_t)(seconds * info_.rate);
    failed_ = false;
    if (target >= info_.frames) {
        block_ = offsets_.size() - 1;  // past the last block: reads return nothing
        buffered_ = cursor_ = 0;
        return true;
    }
    const uint64_t block = target / info_.blockLength;
    if (block != block_ && !Load(block)) return false;
    cursor_ = (size_t)(target - block * info_.blockLength);
    return true;
}

// Exact reading through the same decoder; PcmReader checks the length and INFO's MD5 at the end.
class CordExact final : public PcmReader {
public:
    bool Open(File file) {
        if (!decoder_.Open(std::move(file))) return false;
        format_ = decoder_.ExactFormat();
        return true;
    }
private:
    size_t Next(int32_t* out, size_t frames) override {
        const size_t n = decoder_.ReadExact(out, frames);
        damaged_ = decoder_.Failed();
        return n;
    }
    CordDecoder decoder_;
};
} // namespace

uint32_t cord::Crc32(const uint8_t* p, size_t n) {
    InitCrc();
    uint32_t c = ~0u;
    for (; n >= 8; p += 8, n -= 8) {
        const uint32_t a = c ^ Le32(p), b = Le32(p + 4);
        c = crcTable[7][a & 255] ^ crcTable[6][(a >> 8) & 255] ^ crcTable[5][(a >> 16) & 255] ^ crcTable[4][a >> 24] ^
            crcTable[3][b & 255] ^ crcTable[2][(b >> 8) & 255] ^ crcTable[1][(b >> 16) & 255] ^ crcTable[0][b >> 24];
    }
    while (n--) c = crcTable[0][(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}

std::unique_ptr<Decoder> OpenCordDecoder(const std::wstring& path) {
    File file;
    auto d = std::make_unique<CordDecoder>();
    if (!file.Open(path) || !d->Open(std::move(file))) return nullptr;
    return d;
}

std::unique_ptr<PcmReader> OpenCordExact(const std::wstring& path) {
    File file;
    auto r = std::make_unique<CordExact>();
    if (!file.Open(path) || !r->Open(std::move(file))) return nullptr;
    return r;
}
