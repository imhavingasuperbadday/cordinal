// Cordinal Amiga IFF 8SVX decoder, written from Electronic Arts' "8SVX" IFF standard (1985),
// including its appendix on Fibonacci-delta compression. Also reads the exponential-delta
// variant and 16SV (16-bit big-endian samples). VHDR gives the rate and the one-shot and repeat
// lengths of the first octave; CHAN marks stereo, stored as all of the left channel then all of
// the right. The sample plays once through the one-shot and repeat parts: a looping instrument
// is not looped, and the higher octaves of a multi-octave instrument are not played.
#include "native_audio.h"
#include "audio_file.h"
#include "tags.h"
#include <cmath>

namespace {
using namespace nativeaudio;

const int8_t kFibonacci[16] = {-34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21};
const int8_t kExponential[16] = {-128, -64, -32, -16, -8, -4, -2, -1, 0, 1, 2, 4, 8, 16, 32, 64};

class SvxDecoder final : public Decoder {
public:
    bool Open(const std::wstring& path);
    size_t Read(float* out, size_t frames) override;
    bool Seek(double seconds) override;
    // Delta-coded samples are decoded from the start, though at a billion nibbles a second.
    bool FastSeek() const override { return !table_; }
    double Duration() const override { return (double)total_ / rate_; }
    unsigned Rate() const override { return rate_; }
    bool Failed() const override { return failed_; }
    unsigned Channels() const { return channels_; }
    unsigned Bits() const { return wide_ ? 16 : 8; }
    std::string name, author, annotation, copyright;

private:
    File file_;
    // Each channel's data starts at body_ + c * half_. A compressed channel starts with a pad
    // byte and its initial value, then two samples a byte, high nibble first.
    uint64_t body_ = 0, half_ = 0, total_ = 0, pos_ = 0;
    unsigned rate_ = 0, channels_ = 1;
    const int8_t* table_ = nullptr;
    bool wide_ = false, failed_ = false;
    int8_t value_[2]{};  // compressed: each channel's sample before pos_
};

bool SvxDecoder::Open(const std::wstring& path) {
    if (!file_.Open(path) || file_.Size() < 12) return false;
    const uint8_t* p = file_.View(0, 12);
    if (!p || !Is(p, "FORM") || (!Is(p + 8, "8SVX") && !Is(p + 8, "16SV"))) return false;
    wide_ = Is(p + 8, "16SV");
    const uint64_t limit = std::min<uint64_t>(file_.Size(), (uint64_t)U32(p + 4, true) + 8);
    uint64_t oneShot = 0, repeat = 0, bodyBytes = 0;
    unsigned compression = 0;
    bool vhdr = false, body = false;
    for (uint64_t at = 12; at + 8 <= limit;) {
        if (!(p = file_.View(at, 8))) return false;
        uint64_t n = U32(p + 4, true);
        const uint64_t start = at + 8;
        const bool isBody = Is(p, "BODY");
        if (n > limit - start) {
            if (!isBody) return false;
            n = limit - start;  // a cut-off BODY plays what is there
        }
        auto text = [&](std::string& out) {
            const uint8_t* t = file_.View(start, (size_t)std::min<uint64_t>(n, 4096));
            if (t) out.assign((const char*)t, strnlen((const char*)t, (size_t)std::min<uint64_t>(n, 4096)));
        };
        if (Is(p, "VHDR") && !vhdr) {
            if (n < 20 || !(p = file_.View(start, 20))) return false;
            oneShot = U32(p, true);
            repeat = U32(p + 4, true);
            rate_ = U16(p + 12, true);
            compression = p[15];
            vhdr = true;
        } else if (Is(p, "CHAN") && n >= 4) {
            if (!(p = file_.View(start, 4))) return false;
            channels_ = U32(p, true) == 6 ? 2 : 1;  // 2 and 4 are one side alone
        } else if (isBody && !body) {
            body_ = start;
            bodyBytes = n;
            body = true;
        } else if (Is(p, "NAME")) text(name);
        else if (Is(p, "AUTH")) text(author);
        else if (Is(p, "ANNO")) text(annotation);
        else if (Is(p, "(c) ")) text(copyright);
        at = start + n + (n & 1);
    }
    if (!vhdr || !body || !rate_ || compression > 2 || (wide_ && compression)) return false;
    if (compression) table_ = compression == 1 ? kFibonacci : kExponential;
    half_ = bodyBytes / channels_;
    // Samples each channel's data holds; the header's lengths count the first octave in samples.
    const uint64_t stored = table_ ? (half_ >= 2 ? (half_ - 2) * 2 : 0) : half_ / (wide_ ? 2 : 1);
    total_ = oneShot + repeat ? std::min(stored, oneShot + repeat) : stored;
    if (!total_) return false;
    file_.Sequential();
    return Seek(0);
}

size_t SvxDecoder::Read(float* out, size_t frames) {
    if (failed_) return 0;
    frames = (size_t)std::min<uint64_t>(frames, total_ - pos_);
    size_t done = 0;
    while (done < frames) {
        const size_t n = std::min<size_t>(frames - done, 4096);
        float* dst = out + done * 2;
        for (unsigned c = 0; c < channels_; ++c) {
            const uint64_t base = body_ + c * half_;
            if (table_) {
                // Nibbles pos_ .. pos_ + n - 1, after the pad and initial bytes.
                const uint64_t from = 2 + pos_ / 2, to = 2 + (pos_ + n + 1) / 2;
                const uint8_t* p = file_.View(base + from, (size_t)(to - from));
                if (!p) { failed_ = true; return done; }
                int8_t v = value_[c];
                for (size_t i = 0; i < n; ++i) {
                    const uint64_t nibble = pos_ + i;
                    const uint8_t d = p[nibble / 2 - (from - 2)];
                    v = (int8_t)(uint8_t)(v + table_[(nibble & 1) ? d & 15 : d >> 4]);  // the format's byte arithmetic wraps
                    dst[i * 2 + c] = v * (1.0f / 128);
                }
                value_[c] = v;
            } else {
                const unsigned width = wide_ ? 2 : 1;
                const uint8_t* p = file_.View(base + pos_ * width, n * width);
                if (!p) { failed_ = true; return done; }
                for (size_t i = 0; i < n; ++i)
                    dst[i * 2 + c] = wide_ ? (int16_t)U16(p + i * 2, true) * (1.0f / 32768) : (int8_t)p[i] * (1.0f / 128);
            }
        }
        if (channels_ == 1)
            for (size_t i = 0; i < n; ++i) dst[i * 2 + 1] = dst[i * 2];
        pos_ += n;
        done += n;
    }
    return done;
}

bool SvxDecoder::Seek(double seconds) {
    if (!std::isfinite(seconds)) return false;
    const uint64_t target = seconds <= 0 ? 0 : seconds >= Duration() ? total_ : std::min<uint64_t>(total_, (uint64_t)(seconds * rate_));
    failed_ = false;
    if (!table_) { pos_ = target; return true; }
    // Delta coding needs every earlier sample: restart from the initial values unless the
    // target lies ahead, then run the deltas up to it.
    if (target < pos_ || seconds <= 0) {
        pos_ = 0;
        for (unsigned c = 0; c < channels_; ++c) {
            const uint8_t* p = file_.View(body_ + c * half_ + 1, 1);
            if (!p) { failed_ = true; return false; }
            value_[c] = (int8_t)*p;
        }
    }
    float scratch[4096 * 2];
    while (pos_ < target) {
        if (!Read(scratch, (size_t)std::min<uint64_t>(target - pos_, 4096))) { failed_ = true; return false; }
    }
    return true;
}
}  // namespace

std::unique_ptr<Decoder> OpenSvxDecoder(const std::wstring& path) {
    auto d = std::make_unique<SvxDecoder>();
    return d->Open(path) ? std::move(d) : nullptr;
}

bool ReadSvxTags(const std::wstring& path, TagData& out) {
    SvxDecoder d;
    if (!d.Open(path)) return false;
    out.codec = Codec::PCM;
    out.duration = d.Duration();
    out.sampleRate = d.Rate();
    out.channels = d.Channels();
    out.bitrate = d.Rate() * d.Channels() * d.Bits() / 1000;
    // IFF text chunks are plain ASCII.
    auto ascii = [](const std::string& s) {
        std::string clean;
        for (char c : s)
            if ((unsigned char)c >= 32 && (unsigned char)c < 127) clean += c;
        return clean;
    };
    out.title = ascii(d.name);
    out.artist = ascii(d.author);
    out.comment = ascii(d.annotation);
    if (!d.copyright.empty()) out.comment += (out.comment.empty() ? "" : "\n") + std::string("(c) ") + ascii(d.copyright);
    return true;
}
