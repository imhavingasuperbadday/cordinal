// Cordinal Creative Voice File (.voc) decoder, written from Creative Labs' published VOC block
// layout. A VOC file is a chain of typed blocks: sound data (type 1, or type 9 with a full format),
// continuations of it (2), runs of silence (3), text (5), repeat marks (6, 7, played once) and an
// extended format for stereo (8). Unsigned 8-bit, signed 16-bit and G.711 sound plays; Creative's
// 4-, 2.6- and 2-bit ADPCM does not.
#include "native_audio.h"
#include "audio_file.h"
#include "tags.h"
#include <cmath>

namespace {
using namespace nativeaudio;

uint32_t U24(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }

class VocDecoder final : public Decoder {
public:
    bool Open(const std::wstring& path);
    size_t Read(float* out, size_t frames) override;
    bool Seek(double seconds) override;
    bool FastSeek() const override { return true; }
    double Duration() const override { return (double)total_ / rate_; }
    unsigned Rate() const override { return rate_; }
    bool Failed() const override { return failed_; }
    unsigned Channels() const { return channels_; }
    unsigned Bits() const { return codec_ == 4 ? 16 : 8; }
    const std::string& Text() const { return text_; }

private:
    // A run of frames: sound read from the file at `offset`, or silence when `offset` is 0.
    struct Part { uint64_t first, frames, offset; };
    float Sample(const uint8_t* p) const {
        if (codec_ == 4) return (int16_t)U16(p) * (1.0f / 32768);
        if (codec_ == 0) return ((int)*p - 128) * (1.0f / 128);
        return G711(*p, codec_ == 7);
    }
    File file_;
    std::vector<Part> parts_;
    std::string text_;
    unsigned rate_ = 0, channels_ = 0, codec_ = 0, align_ = 0;
    uint64_t total_ = 0, pos_ = 0;
    size_t part_ = 0;  // the part holding pos_
    bool failed_ = false;
};

// The header is the 20-byte signature, the offset of the first block, the version and its
// checksum (~version + 0x1234). The chain ends at a type-0 block or at the end of the file. A
// sound block whose format differs from the first one ends the playable stream there.
bool VocDecoder::Open(const std::wstring& path) {
    if (!file_.Open(path) || file_.Size() < 26) return false;
    const uint8_t* p = file_.View(0, 26);
    if (!p || memcmp(p, "Creative Voice File\x1A", 20)) return false;
    const unsigned version = U16(p + 22);
    if (U16(p + 24) != (uint16_t)(~version + 0x1234)) return false;
    const uint64_t size = file_.Size();
    // Type 8 sets the rate and channels of the type-1 block that follows it.
    unsigned extendedRate = 0, extendedChannels = 0;
    bool haveFormat = false, ended = false;
    struct Silence { uint64_t frames; unsigned rate; size_t before; };
    std::vector<Silence> silences;  // their source rate may differ, so they are scaled at the end
    auto addSound = [&](uint64_t offset, uint64_t bytes) {
        const uint64_t frames = bytes / align_;
        if (frames) parts_.push_back({total_, frames, offset});
        total_ += frames;
    };
    auto format = [&](unsigned rate, unsigned channels, unsigned codec) {
        const unsigned bits = codec == 4 ? 16 : 8;
        if (codec != 0 && codec != 4 && codec != 6 && codec != 7) return false;
        if (!rate || rate > 1048575 || !channels || channels > 8) return false;
        if (!haveFormat) {
            rate_ = rate; channels_ = channels; codec_ = codec; align_ = channels * bits / 8;
            haveFormat = true;
            return true;
        }
        return rate == rate_ && channels == channels_ && codec == codec_;
    };
    for (uint64_t at = U16(p + 20); !ended && at < size;) {
        if (!(p = file_.View(at, 1))) return false;
        const unsigned type = *p;
        if (!type || at + 4 > size) break;
        if (!(p = file_.View(at, 4))) return false;
        const uint64_t body = at + 4, n = std::min<uint64_t>(U24(p + 1), size - body);  // a cut-off last block plays what is there
        const uint8_t* b = file_.View(body, (size_t)std::min<uint64_t>(n, 12));
        if (!b) return false;
        switch (type) {
        case 1:
            if (n >= 2) {
                // The time constant is 256 - 1,000,000 / rate; an extended block's is 65536 - 256,000,000 / (rate * channels).
                const unsigned rate = extendedRate ? extendedRate : (unsigned)std::llround(1000000.0 / (256 - b[0]));
                if (!format(rate, extendedRate ? extendedChannels : 1, b[1])) { ended = true; break; }
                addSound(body + 2, n - 2);
            }
            extendedRate = 0;
            break;
        case 2:
            if (haveFormat) addSound(body, n);
            break;
        case 3:
            if (n >= 3) silences.push_back({(uint64_t)U16(b) + 1, (unsigned)std::llround(1000000.0 / (256 - b[2])), parts_.size()});
            break;
        case 5:
            if (text_.empty()) {
                const uint8_t* t = file_.View(body, (size_t)std::min<uint64_t>(n, 4096));
                if (!t) return false;
                text_.assign((const char*)t, strnlen((const char*)t, (size_t)std::min<uint64_t>(n, 4096)));
            }
            break;
        case 8:
            if (n >= 4) {
                extendedChannels = b[3] + 1u;
                const unsigned constant = U16(b);
                extendedRate = constant < 65536 ? (unsigned)std::llround(256000000.0 / (extendedChannels * (65536.0 - constant))) : 0;
            }
            break;
        case 9:
            if (n >= 12) {
                const unsigned bits = b[4], codec = U16(b + 6);
                if ((codec == 0 && bits != 8) || (codec == 4 && bits != 16) || !format(U32(b), b[5], codec)) { ended = true; break; }
                addSound(body + 12, n - 12);
            }
            break;
        default:  // markers, repeats and anything newer carry no sound
            break;
        }
        at = body + n;
    }
    if (!haveFormat) return false;
    // Fold the silences in, in order, scaled to the stream's rate.
    for (size_t i = silences.size(); i-- > 0;) {
        const Silence& s = silences[i];
        const uint64_t frames = (uint64_t)std::llround((double)s.frames * rate_ / s.rate);
        if (!frames) continue;
        parts_.insert(parts_.begin() + (ptrdiff_t)s.before, Part{0, frames, 0});
    }
    total_ = 0;
    for (Part& part : parts_) { part.first = total_; total_ += part.frames; }
    if (!total_) return false;
    file_.Sequential();
    return true;
}

size_t VocDecoder::Read(float* out, size_t frames) {
    if (failed_) return 0;
    frames = (size_t)std::min<uint64_t>(frames, total_ - pos_);
    size_t done = 0;
    while (done < frames) {
        while (part_ + 1 < parts_.size() && pos_ >= parts_[part_].first + parts_[part_].frames) ++part_;
        const Part& part = parts_[part_];
        const uint64_t within = pos_ - part.first;
        const size_t n = (size_t)std::min<uint64_t>({frames - done, part.frames - within, 16384});
        float* dst = out + done * 2;
        if (!part.offset) std::fill_n(dst, n * 2, 0.0f);
        else {
            const uint8_t* p = file_.View(part.offset + within * align_, n * align_);
            if (!p) { failed_ = true; break; }
            const unsigned width = align_ / channels_;
            for (size_t i = 0; i < n; ++i, p += align_) {
                float samples[8];
                for (unsigned c = 0; c < channels_; ++c) samples[c] = Sample(p + c * width);
                Mix(samples, channels_, dst + i * 2);
            }
        }
        pos_ += n;
        done += n;
    }
    return done;
}

bool VocDecoder::Seek(double seconds) {
    if (!std::isfinite(seconds)) return false;
    pos_ = seconds <= 0 ? 0 : seconds >= Duration() ? total_ : std::min<uint64_t>(total_, (uint64_t)(seconds * rate_));
    auto at = std::upper_bound(parts_.begin(), parts_.end(), pos_, [](uint64_t v, const Part& p) { return v < p.first; });
    part_ = at == parts_.begin() ? 0 : (size_t)(at - parts_.begin() - 1);
    failed_ = false;
    return true;
}
}  // namespace

std::unique_ptr<Decoder> OpenVocDecoder(const std::wstring& path) {
    auto d = std::make_unique<VocDecoder>();
    return d->Open(path) ? std::move(d) : nullptr;
}

bool ReadVocTags(const std::wstring& path, TagData& out) {
    VocDecoder d;
    if (!d.Open(path)) return false;
    out.codec = Codec::PCM;
    out.duration = d.Duration();
    out.sampleRate = d.Rate();
    out.channels = d.Channels();
    out.bitrate = d.Rate() * d.Channels() * d.Bits() / 1000;
    // The text block is plain ASCII by the format's definition; keep only what is printable.
    for (char c : d.Text())
        if ((unsigned char)c >= 32 && (unsigned char)c < 127) out.comment += c;
    return true;
}
