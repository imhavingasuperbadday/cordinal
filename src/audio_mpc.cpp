// Cordinal Musepack decoder: SV7 (MP+) and SV8 (MPCK) streams, mono or stereo.
//
// A frame is 36 time slots of 32 subbands. Its side information picks a quantizer per subband
// ("resolution") and three scale factors, one per 12 slots; the samples are prefix coded (SV7, SV8),
// enumerated (SV8's two-level quantizer) or raw. Dequantized subbands go through the MPEG-1 synthesis
// filterbank, shared with audio_mpeg.cpp and its AVX2 kernels, 18 slots per call.
//
// Seeking is exact: a frame depends on the one before only through its scale factors, the noise
// generator and the last 16 slots of synthesis history. Small snapshots of the first two, taken every
// 256 frames as the stream is read, let a seek parse forward from the nearest one without synthesis
// (an SV7 seek reads only each frame's side information), then synthesize the frame before the
// target to settle the filterbank. An SV8 seek far past what has been read jumps to its packet.
#include "native_audio.h"
#include "audio_file.h"
#include "audio_huffman.h"
#include "audio_kernels.h"
#include "audio_mpc_tables.h"
#include "audio_polyphase.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace {
using namespace nativeaudio;
using namespace nativeaudio::mpc;

constexpr unsigned kSlots = 36, kFrame = kSlots * 32, kDelay = 481, kSnapshotEvery = 256;
constexpr unsigned kRates[4] = {44100, 48000, 37800, 32000};

// The codebooks, built once. A book's symbols carry the offset its table header names.
struct Books {
    PrefixBook hdr{sv7Hdr}, scfi{sv7Scfi}, dscf{sv7Dscf};
    PrefixBook q7[7][2]{{PrefixBook{sv7Q1a}, PrefixBook{sv7Q1b}}, {PrefixBook{sv7Q2a}, PrefixBook{sv7Q2b}},
                        {PrefixBook{sv7Q3a}, PrefixBook{sv7Q3b}}, {PrefixBook{sv7Q4a}, PrefixBook{sv7Q4b}},
                        {PrefixBook{sv7Q5a}, PrefixBook{sv7Q5b}}, {PrefixBook{sv7Q6a}, PrefixBook{sv7Q6b}},
                        {PrefixBook{sv7Q7a}, PrefixBook{sv7Q7b}}};
    PrefixBook bands{sv8Bands}, scfi8[2]{PrefixBook{sv8Scfi1}, PrefixBook{sv8Scfi2}};
    PrefixBook dscf8[2]{PrefixBook{sv8Dscf1}, PrefixBook{sv8Dscf2}}, res8[2]{PrefixBook{sv8Res1}, PrefixBook{sv8Res2}};
    PrefixBook q1{sv8Q1}, q2[2]{PrefixBook{sv8Q2a}, PrefixBook{sv8Q2b}}, q34[2]{PrefixBook{sv8Q3}, PrefixBook{sv8Q4}};
    PrefixBook q58[4][2]{{PrefixBook{sv8Q5a}, PrefixBook{sv8Q5b}}, {PrefixBook{sv8Q6a}, PrefixBook{sv8Q6b}},
                         {PrefixBook{sv8Q7a}, PrefixBook{sv8Q7b}}, {PrefixBook{sv8Q8a}, PrefixBook{sv8Q8b}}};
    PrefixBook q9{sv8Q9};
    // Binomial coefficients C(n, k) for SV8's enumerated codes (n <= 32, k <= 16).
    uint32_t choose[33][17]{};
    // Dequantizer per resolution -1..17: the step of a (2D+1)-level quantizer, and D.
    float step[19]{};
    int offset[19]{};
    // 1.58 dB scale-factor steps; an index is taken modulo 256 with 1 at full scale, as the format wraps.
    float scale[256]{};
    Books() {
        for (unsigned n = 0; n <= 32; ++n)
            for (unsigned k = 0; k <= 16 && k <= n; ++k)
                choose[n][k] = k == 0 || k == n ? 1 : choose[n - 1][k - 1] + choose[n - 1][k];
        for (int r = 1; r <= 17; ++r) {
            offset[r + 1] = r <= 4 ? r : (1 << (r - 2)) - 1;
            step[r + 1] = float(65536.0 / (2 * offset[r + 1] + 1));
        }
        offset[0] = 2;
        step[0] = float(32768.0 / 2 / 255 * std::sqrt(3.0));  // the noise's uniform-sum spread
        for (int i = 0; i < 256; ++i)
            scale[i] = float(std::pow(0.83298066476582673961, (int8_t)(uint8_t)(i - 1)) / 32768.0);
    }
};
const Books& GetBooks() {
    static const Books books;
    return books;
}

// What one frame leaves for the next: scale factors (deltas run across frames), whether a band's
// next SV8 scale factor is absolute, the noise generator and SV8's band count.
struct Carry {
    int16_t scf[2][32][3]{};  // only index & 255 is used; an SV7 overflow is kept as 0x480
    bool absolute[2][32]{};
    uint32_t noise1 = 1, noise2 = 1;
    int lastBands = 0;
};

class MpcDecoder final : public Decoder {
public:
    bool Open(const std::wstring& path);
    size_t Read(float* out, size_t frames) override;
    bool Seek(double seconds) override;
    bool FastSeek() const override { return true; }
    double Duration() const override { return (double)total_ / rate_; }
    unsigned Rate() const override { return rate_; }
    bool Failed() const override { return failed_; }

private:
    bool OpenSv7(uint64_t at);
    bool OpenSv8(uint64_t at);
    // Parses frame `frame_` and advances. With `synth`, its PCM lands in pcm_.
    bool Frame(bool synth);
    bool Sv7(Bits& b, int res[2][32], bool ms[32]);
    bool Sv8(Bits& b, bool key, int res[2][32], bool ms[32]);
    void Samples(Bits& b, int res, int16_t* q, bool sv8);
    uint32_t Noise() {
        c_.noise1 = (c_.noise1 >> 1) | uint32_t(std::popcount(c_.noise1 & 0xF5) & 1) << 31;
        c_.noise2 = (c_.noise2 << 1) | uint32_t(std::popcount((c_.noise2 >> 25) & 0x63) & 1);
        return c_.noise1 ^ c_.noise2;
    }
    // SV8's enumerated codes: a value below `count`, then which k of n positions are set.
    static uint32_t Bounded(Bits& b, unsigned count);
    uint32_t Choose(Bits& b, unsigned k, unsigned n) const;
    void Synthesize(bool silent);
    // Moves the parser to the start of `frame` from the nearest snapshot, without synthesis.
    bool Reach(uint64_t frame);

    struct Snapshot { Carry carry; size_t packet = 0; uint64_t bit = 0; };
    File file_;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    const Books& books_ = GetBooks();
    unsigned version_ = 0, rate_ = 44100, channels_ = 2, maxBand_ = 0, perPacket_ = 1;
    bool ms_ = false, failed_ = false;
    bool exact_ = true;  // false after an SV8 jump: the noise generator's state is then a guess
    uint64_t frames_ = 0, total_ = 0, skip_ = kDelay, position_ = 0;
    // SV7: each frame's bits in the word-swapped stream. SV8: the audio packets.
    struct Span { uint64_t start, end; };
    std::vector<Span> sv7Frames_;  // each frame's bits, counted from sv7Base_, where the words start
    uint64_t sv7Base_ = 0;
    struct Packet { uint64_t at, bytes; };
    std::vector<Packet> packets_;
    std::vector<Snapshot> snapshots_;  // snapshots_[i] is frame i * kSnapshotEvery
    std::vector<uint8_t> swapped_;
    Carry c_;
    uint64_t frame_ = 0;                // the next frame to parse
    size_t packet_ = 0;                 // SV8: the packet it is in, and where in it
    uint64_t packetBit_ = 0;
    Polyphase synthesis_;
    float sub_[2][kSlots][32]{};
    float out_[2][kSlots * 32]{};
    std::vector<float> pcm_ = std::vector<float>(kFrame * 2);
    uint64_t pcmFrame_ = UINT64_MAX;     // the frame pcm_ holds
};

uint32_t MpcDecoder::Bounded(Bits& b, unsigned count) {
    if (count <= 1) return 0;
    unsigned bits = std::bit_width(count - 1), lost = (1u << bits) - count;
    uint32_t v = bits > 1 ? (uint32_t)b.Get(bits - 1) : 0;
    if (v >= lost) v = ((v << 1) | (uint32_t)b.Get(1)) - lost;
    return v;
}

uint32_t MpcDecoder::Choose(Bits& b, unsigned k, unsigned n) const {
    uint32_t code = Bounded(b, books_.choose[n][k]), bits = 0;
    while (k > 0 && n > 0) {
        --n;
        if (code >= books_.choose[n][k]) {
            bits |= 1u << n;
            code -= books_.choose[n][k];
            --k;
        }
    }
    return bits;
}

bool MpcDecoder::Open(const std::wstring& path) {
    if (!file_.Open(path) || file_.Size() < 32 || file_.Size() > SIZE_MAX / 2) return false;
    size_ = (size_t)file_.Size();
    data_ = file_.View(0, size_);
    if (!data_) return false;
    uint64_t at = 0;
    // An ID3v2 tag some taggers put in front.
    if (!memcmp(data_, "ID3", 3) && size_ > 10) {
        at = 10 + (uint64_t(data_[6] & 127) << 21 | uint64_t(data_[7] & 127) << 14 | uint64_t(data_[8] & 127) << 7 |
                   (data_[9] & 127));
        if (data_[5] & 0x10) at += 10;
        if (at + 32 > size_) return false;
    }
    bool ok = !memcmp(data_ + at, "MPCK", 4) ? OpenSv8(at + 4) : !memcmp(data_ + at, "MP+", 3) ? OpenSv7(at) : false;
    if (!ok || !frames_ || !total_) return false;
    snapshots_.push_back({c_, packet_, packetBit_});
    return true;
}

// SV7: a 25-byte header, then frames, each a 20-bit length and its bits. The whole stream is
// stored as little-endian 32-bit words read most significant bit first.
bool MpcDecoder::OpenSv7(uint64_t at) {
    if ((data_[at + 3] & 15) != 7 || size_ - at < 28) return false;
    const uint8_t* h = data_ + at;
    frames_ = U32(h + 4);
    uint32_t flags = U32(h + 8), gapless = U32(h + 20);
    if (flags >> 31) return false;  // intensity stereo was never used and isn't decoded
    version_ = 7;
    ms_ = (flags >> 30) & 1;
    maxBand_ = (flags >> 24) & 63;
    rate_ = kRates[(flags >> 16) & 3];
    if (maxBand_ > 31 || !frames_ || frames_ > (size_ * 8) / 20) return false;
    uint64_t last = (gapless >> 20) & 2047;
    total_ = (gapless >> 31) && last ? (frames_ - 1) * kFrame + last : frames_ * kFrame;
    // Index the frames by their lengths; a stream cut short keeps the frames that are whole.
    const uint64_t words = (size_ - at) / 4, limit = words * 32;
    // Up to 32 bits at any position fit in the two words that hold them.
    auto bits = [&](uint64_t bit, unsigned n) {
        const uint64_t word = bit / 32;
        uint64_t v = word < words ? uint64_t(U32(data_ + at + word * 4)) << 32 : 0;
        if (word + 1 < words) v |= U32(data_ + at + word * 4 + 4);
        return uint32_t((v << (bit & 31)) >> (64 - n));
    };
    sv7Frames_.reserve((size_t)frames_);
    uint64_t bit = 200;
    for (uint64_t f = 0; f < frames_ && bit + 20 <= limit; ++f) {
        uint64_t length = bits(bit, 20);
        if (bit + 20 + length > limit) break;
        sv7Frames_.push_back({bit + 20, bit + 20 + length});
        bit += 20 + length;
    }
    if (sv7Frames_.empty()) return false;
    if (sv7Frames_.size() < frames_) {
        frames_ = sv7Frames_.size();
        total_ = std::min(total_, frames_ * kFrame);
    }
    sv7Base_ = at;
    // After the counted frames come the last frame's 11-bit length (gapless streams) and one more
    // frame, which carries the end of the audio through the filterbank's delay.
    const uint64_t flush = bit + ((gapless >> 31) ? 11 : 0);
    if (sv7Frames_.size() == frames_ && flush + 20 <= limit) {
        const uint64_t length = bits(flush, 20);
        if (flush + 20 + length <= limit) {
            sv7Frames_.push_back({flush + 20, flush + 20 + length});
            ++frames_;
        }
    }
    return true;
}

// SV8: packets of a two-letter key and a size; the stream header, then audio packets of
// perPacket_ frames each (the last may hold fewer), until the stream end packet.
bool MpcDecoder::OpenSv8(uint64_t at) {
    bool header = false;
    uint64_t samples = 0, silence = 0;
    while (at + 3 <= size_) {
        const uint8_t* p = data_ + at;
        uint64_t length = 0, used = 2;
        while (used < 12 && at + used < size_) {
            uint8_t byte = p[used++];
            length = (length << 7) | (byte & 127);
            if (!(byte & 128)) break;
        }
        if (length < used || length > size_ - at) break;
        Bits b(p + used, size_t(length - used));
        if (p[0] == 'S' && p[1] == 'H') {
            b.Skip(32);  // CRC
            if (b.Get(8) != 8) return false;
            auto number = [&] {
                uint64_t v = 0;
                for (int i = 0; i < 9; ++i) {
                    uint64_t byte = b.Get(8);
                    v = (v << 7) | (byte & 127);
                    if (!(byte & 128)) break;
                }
                return v;
            };
            samples = number();
            silence = number();
            unsigned rate = (unsigned)b.Get(3);
            maxBand_ = (unsigned)b.Get(5) + 1;
            channels_ = (unsigned)b.Get(4) + 1;
            ms_ = b.Get(1) != 0;
            perPacket_ = 1u << (2 * b.Get(3));
            if (!b.Good() || rate > 3 || channels_ > 2 || maxBand_ > 32) return false;
            rate_ = kRates[rate];
            header = true;
        } else if (p[0] == 'A' && p[1] == 'P') {
            packets_.push_back({at + used, length - used});
        } else if (p[0] == 'S' && p[1] == 'E') {
            break;
        }
        at += length;
    }
    if (!header || packets_.empty() || samples <= silence) return false;
    version_ = 8;
    // The encoder adds frames to cover the filterbank delay, so the last output sample is coded.
    frames_ = std::min<uint64_t>((samples + kDelay + kFrame - 1) / kFrame, packets_.size() * uint64_t(perPacket_));
    skip_ = kDelay + silence;
    total_ = std::min<uint64_t>(samples, frames_ * kFrame) - silence;
    packetBit_ = 0;
    return true;
}

bool MpcDecoder::Sv7(Bits& b, int res[2][32], bool ms[32]) {
    const Books& k = books_;
    int used = 0;
    for (unsigned n = 0; n <= maxBand_; ++n) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            if (!n) res[ch][0] = (int)b.Get(4);
            else {
                int delta = k.hdr.Read(b) - 5;
                res[ch][n] = delta == 4 ? (int)b.Get(4) : res[ch][n - 1] + delta;
            }
            if (res[ch][n] < -1 || res[ch][n] > 17) return false;
        }
        ms[n] = false;
        if (res[0][n] || res[1][n]) {
            if (ms_) ms[n] = b.Get(1) != 0;
            used = n + 1;
        }
    }
    for (unsigned n = (unsigned)used; n < 32; ++n) res[0][n] = res[1][n] = 0;
    int scfi[2][32]{};
    for (int n = 0; n < used; ++n)
        for (unsigned ch = 0; ch < 2; ++ch)
            if (res[ch][n]) scfi[ch][n] = k.scfi.Read(b);
    for (int n = 0; n < used; ++n)
        for (unsigned ch = 0; ch < 2; ++ch) {
            if (!res[ch][n]) continue;
            int16_t* scf = c_.scf[ch][n];
            auto next = [&](int previous) {
                int delta = k.dscf.Read(b) - 7;
                int v = delta == 8 ? (int)b.Get(6) : previous + delta;
                return int16_t(v > 1024 ? 0x480 : v);  // the format's out-of-range value; index 128
            };
            // Which of the three are sent: 0 all, 1 the first two, 2 the first and last, 3 one for all.
            switch (scfi[ch][n]) {
                case 0: scf[0] = next(scf[2]); scf[1] = next(scf[0]); scf[2] = next(scf[1]); break;
                case 1: scf[0] = next(scf[2]); scf[1] = scf[2] = next(scf[0]); break;
                case 2: scf[0] = scf[1] = next(scf[2]); scf[2] = next(scf[1]); break;
                default: scf[0] = scf[1] = scf[2] = next(scf[2]); break;
            }
        }
    return b.Good();
}

bool MpcDecoder::Sv8(Bits& b, bool key, int res[2][32], bool ms[32]) {
    const Books& k = books_;
    int used = key ? (int)Bounded(b, maxBand_ + 2) : c_.lastBands + k.bands.Read(b);
    if (used > 32) used -= 33;
    if (used < 0 || used > (int)maxBand_ + 1 || used > 32) return false;
    c_.lastBands = used;
    for (int n = 0; n < 32; ++n) res[0][n] = res[1][n] = 0, ms[n] = false;
    if (used) {
        // The top band's two resolutions, then each band below as a step from the one above it.
        for (unsigned ch = 0; ch < 2; ++ch) {
            int r = k.res8[0].Read(b);
            res[ch][used - 1] = r > 15 ? r - 17 : r;
        }
        for (int n = used - 2; n >= 0; --n)
            for (unsigned ch = 0; ch < 2; ++ch) {
                int r = k.res8[res[ch][n + 1] > 2].Read(b) + res[ch][n + 1];
                res[ch][n] = r > 15 ? r - 17 : r;
            }
        if (ms_) {
            unsigned total = 0;
            for (int n = 0; n < used; ++n) total += res[0][n] || res[1][n];
            unsigned count = Bounded(b, total + 1);
            uint32_t flags = 0;
            if (count && count != total) flags = Choose(b, std::min(count, total - count), total);
            if (count * 2 > total) flags = ~flags;
            for (int n = used - 1; n >= 0; --n)
                if (res[0][n] || res[1][n]) { ms[n] = flags & 1; flags >>= 1; }
        }
    }
    if (key)
        for (auto& channel : c_.absolute) std::fill(std::begin(channel), std::end(channel), true);
    int scfi[2][32]{};
    for (int n = 0; n < used; ++n) {
        int count = (res[0][n] != 0) + (res[1][n] != 0);
        if (!count) continue;
        int v = k.scfi8[count - 1].Read(b);
        if (res[0][n]) scfi[0][n] = v >> (2 * (count - 1));
        if (res[1][n]) scfi[1][n] = v & 3;
    }
    for (int n = 0; n < used; ++n)
        for (unsigned ch = 0; ch < 2; ++ch) {
            if (!res[ch][n]) continue;
            int16_t* scf = c_.scf[ch][n];
            if (c_.absolute[ch][n]) {
                scf[0] = int16_t((int)b.Get(7) - 6);
                c_.absolute[ch][n] = false;
            } else {
                int delta = k.dscf8[1].Read(b);
                if (delta == 64) delta += (int)b.Get(6);
                scf[0] = int16_t(((scf[2] - 25 + delta) & 127) - 6);
            }
            for (int m = 0; m < 2; ++m) {
                if ((scfi[ch][n] << m) & 2) { scf[m + 1] = scf[m]; continue; }
                int delta = k.dscf8[0].Read(b);
                if (delta == 31) delta = 64 + (int)b.Get(6);
                scf[m + 1] = int16_t(((scf[m] - 25 + delta) & 127) - 6);
            }
        }
    return b.Good();
}

void MpcDecoder::Samples(Bits& b, int res, int16_t* q, bool sv8) {
    const Books& k = books_;
    if (res == -1) {
        for (unsigned i = 0; i < kSlots; ++i) {
            uint32_t v = Noise();
            q[i] = int16_t((v >> 24) + ((v >> 16) & 255) + ((v >> 8) & 255) + (v & 255) - 510);
        }
        return;
    }
    if (!sv8) {
        if (res == 1) {
            const PrefixBook& book = k.q7[0][b.Get(1)];
            for (unsigned i = 0; i < kSlots; i += 3) {
                int v = book.Read(b);
                q[i] = int16_t(v % 3 - 1); q[i + 1] = int16_t(v / 3 % 3 - 1); q[i + 2] = int16_t(v / 9 - 1);
            }
        } else if (res == 2) {
            const PrefixBook& book = k.q7[1][b.Get(1)];
            for (unsigned i = 0; i < kSlots; i += 2) {
                int v = book.Read(b);
                q[i] = int16_t(v % 5 - 2); q[i + 1] = int16_t(v / 5 - 2);
            }
        } else if (res <= 7) {
            const PrefixBook& book = k.q7[res - 1][b.Get(1)];
            static constexpr int offsets[8] = {0, 0, 0, 3, 4, 7, 15, 31};  // the books' symbol offsets
            const int offset = offsets[res];
            for (unsigned i = 0; i < kSlots; ++i) q[i] = int16_t(book.Read(b) - offset);
        } else {
            const int offset = k.offset[res + 1];
            for (unsigned i = 0; i < kSlots; ++i) q[i] = int16_t((int)b.Get(res - 1) - offset);
        }
        return;
    }
    if (res == 1) {
        // Two halves of 18: how many are nonzero, which ones, then their signs.
        for (unsigned half = 0; half < kSlots; half += 18) {
            unsigned count = (unsigned)k.q1.Read(b);
            uint32_t set = 0;
            if (count > 0 && count < 18) set = Choose(b, count <= 9 ? count : 18 - count, 18);
            if (count > 9) set = ~set;
            for (unsigned i = 0; i < 18; ++i)
                q[half + i] = (set >> (17 - i)) & 1 ? int16_t(b.Get(1) ? 1 : -1) : int16_t(0);
        }
    } else if (res == 2) {
        // Three values per code; how loud the last codes were picks the next book.
        unsigned context = 6;
        for (unsigned i = 0; i < kSlots; i += 3) {
            int v = k.q2[context > 3].Read(b);
            int a = v % 5 - 2, c = v / 5 % 5 - 2, d = v / 25 - 2;
            q[i] = int16_t(a); q[i + 1] = int16_t(c); q[i + 2] = int16_t(d);
            context = (context >> 1) + std::abs(a) + std::abs(c) + std::abs(d);
        }
    } else if (res <= 4) {
        // Two signed four-bit values per code.
        const PrefixBook& book = k.q34[res - 3];
        for (unsigned i = 0; i < kSlots; i += 2) {
            int v = book.Read(b);
            q[i] = int16_t(int8_t(uint8_t(v << 4)) >> 4);
            q[i + 1] = int16_t(int8_t(uint8_t(v)) >> 4);
        }
    } else if (res <= 8) {
        static constexpr unsigned threshold[4] = {1, 3, 4, 8};
        const unsigned limit = threshold[res - 5];
        const int offset = (1 << (res - 2)) - 1;
        unsigned context = 2 * limit;
        for (unsigned i = 0; i < kSlots; ++i) {
            int v = k.q58[res - 5][context > limit].Read(b) - offset;
            q[i] = int16_t(v);
            context = (context >> 1) + (unsigned)std::abs(v);
        }
    } else {
        const int offset = k.offset[res + 1];
        const unsigned extra = (unsigned)res - 9;
        for (unsigned i = 0; i < kSlots; ++i) {
            int v = k.q9.Read(b);
            if (extra) v = (v << extra) | (int)b.Get(extra);
            q[i] = int16_t(v - offset);
        }
    }
}

bool MpcDecoder::Frame(bool synth) {
    if (frame_ >= frames_) return false;
    int res[2][32];
    bool ms[32];
    int16_t q[2][kSlots];
    bool ok;
    auto decode = [&](Bits& b, bool key) {
        ok = version_ == 8 ? Sv8(b, key, res, ms) : Sv7(b, res, ms);
        if (!ok) return;
        // Both channels are coded; a mono stream plays its first on both sides.
        for (unsigned n = 0; n < 32; ++n) {
            for (unsigned ch = 0; ch < 2; ++ch) {
                if (!res[ch][n]) continue;
                // An SV7 frame's length is known, so a seek reads only its side information; the
                // noise generator still advances as the skipped samples would have moved it.
                if (synth || version_ == 8) Samples(b, res[ch][n], q[ch], version_ == 8);
                else if (res[ch][n] == -1) for (unsigned i = 0; i < kSlots; ++i) Noise();
            }
            if (!synth) continue;
            // Dequantize a band at a time (each 12 slots share a scale factor), turn mid/side into
            // left/right, then store the band's column of the slot-major synthesis input.
            float band[2][kSlots];
            for (unsigned ch = 0; ch < 2; ++ch) {
                if (!res[ch][n]) { std::fill(std::begin(band[ch]), std::end(band[ch]), 0.0f); continue; }
                const float step = books_.step[res[ch][n] + 1];
                for (unsigned part = 0; part < 3; ++part) {
                    const float f = step * books_.scale[c_.scf[ch][n][part] & 255];
                    for (unsigned s = part * 12; s < part * 12 + 12; ++s) band[ch][s] = f * q[ch][s];
                }
            }
            if (ms[n])
                for (unsigned s = 0; s < kSlots; ++s) {
                    const float m = band[0][s], side = band[1][s];
                    band[0][s] = m + side;
                    band[1][s] = m - side;
                }
            for (unsigned s = 0; s < kSlots; ++s) sub_[0][s][n] = band[0][s], sub_[1][s][n] = band[1][s];
        }
        ok = b.Good();
    };
    if (version_ == 7) {
        const uint64_t start = sv7Frames_[frame_].start, end = sv7Frames_[frame_].end;
        // Swap the frame's words into reading order once; it is a few hundred bytes.
        // A seek reads only the side information, which nearly always fits in the first 384 bytes.
        const uint64_t first = (start - (start & 31)) / 8, all = (end + 31) / 32 - first / 4;
        for (uint64_t words = synth ? all : std::min<uint64_t>(all, 96);; words = all) {
            swapped_.resize((size_t)words * 4);
            for (uint64_t i = 0; i < words; ++i) {
                const uint64_t at = sv7Base_ + first + i * 4;
                const uint8_t* w = data_ + at;
                uint8_t* to = swapped_.data() + i * 4;
                if (at + 4 <= size_) to[0] = w[3], to[1] = w[2], to[2] = w[1], to[3] = w[0];
                else memset(to, 0, 4);
            }
            const Carry before = c_;
            Bits b(swapped_.data(), swapped_.size(), (size_t)(end - first * 8));
            b.Skip(start & 31);
            decode(b, false);
            if (ok || words == all) break;
            c_ = before;  // ran past the part swapped: once more with the whole frame
        }
    } else {
        if (packet_ >= packets_.size()) return false;
        const Packet& p = packets_[packet_];
        Bits b(data_ + p.at, (size_t)p.bytes);
        b.Skip((size_t)packetBit_);
        decode(b, packetBit_ == 0);
        packetBit_ = b.Position();
        if ((frame_ + 1) % perPacket_ == 0) packet_++, packetBit_ = 0;
    }
    ++frame_;
    if (exact_ && frame_ % kSnapshotEvery == 0 && frame_ / kSnapshotEvery == snapshots_.size())
        snapshots_.push_back({c_, packet_, packetBit_});
    if (!ok) return false;
    if (synth) Synthesize(false);
    return true;
}

void MpcDecoder::Synthesize(bool silent) {
    if (silent) memset(sub_, 0, sizeof sub_);
    const unsigned channels = channels_ == 1 ? 1 : 2;
    for (unsigned ch = 0; ch < channels; ++ch)
        for (unsigned half = 0; half < 2; ++half)
            synthesis_.Synthesize18(sub_[ch][half * 18], ch, out_[ch] + half * 18 * 32);
    const float* right = out_[channels - 1];
    float* pcm = pcm_.data();
    for (unsigned i = 0; i < kFrame; ++i) pcm[i * 2] = out_[0][i], pcm[i * 2 + 1] = right[i];
}

bool MpcDecoder::Reach(uint64_t frame) {
    size_t at = std::min<size_t>((size_t)(frame / kSnapshotEvery), snapshots_.size() - 1);
    const uint64_t snapshot = uint64_t(at) * kSnapshotEvery;
    const uint64_t from = frame_ <= frame && frame_ >= snapshot ? frame_ : snapshot;
    if (version_ == 8 && frame - from > 4 * uint64_t(perPacket_) + kSnapshotEvery) {
        // Far past what has been read: SV8 packets start afresh, all but the noise generator, which
        // runs through the whole stream. The reference player's seeks jump the same way; only
        // noise-substituted bands (the lowest presets) can differ from a straight decode.
        packet_ = (size_t)(frame / perPacket_);
        packetBit_ = 0;
        frame_ = uint64_t(packet_) * perPacket_;
        c_.noise1 = 1 + uint32_t(packet_) * 2654435761u;
        c_.noise2 = 1 | uint32_t(packet_) * 40503u;
        exact_ = false;
    } else if (from != frame_) {
        const Snapshot& s = snapshots_[at];
        c_ = s.carry;
        packet_ = s.packet;
        packetBit_ = s.bit;
        frame_ = uint64_t(at) * kSnapshotEvery;
        exact_ = true;
    }
    while (frame_ < frame)
        if (!Frame(false)) return false;
    return true;
}

size_t MpcDecoder::Read(float* out, size_t frames) {
    size_t done = 0;
    while (done < frames && position_ < total_ && !failed_) {
        const uint64_t stream = position_ + skip_, frame = stream / kFrame;
        if (pcmFrame_ != frame) {
            if (frame != frame_ && !Reach(std::min(frame, frames_))) { failed_ = true; break; }
            if (frame < frames_) {
                if (!Frame(true)) { failed_ = true; break; }
            } else {
                Synthesize(true);  // the filterbank's tail after the last frame
                ++frame_;
            }
            pcmFrame_ = frame;
        }
        const size_t within = (size_t)(stream % kFrame);
        const size_t take = (size_t)std::min<uint64_t>({frames - done, kFrame - within, total_ - position_});
        memcpy(out + done * 2, pcm_.data() + within * 2, take * 2 * sizeof(float));
        done += take;
        position_ += take;
    }
    return done;
}

bool MpcDecoder::Seek(double seconds) {
    if (!std::isfinite(seconds)) return false;
    const uint64_t target = (uint64_t)std::clamp(std::floor(seconds * rate_), 0.0, (double)total_);
    const uint64_t frame = (target + skip_) / kFrame;
    failed_ = false;
    synthesis_.Reset();
    pcmFrame_ = UINT64_MAX;
    // One frame before the target fills the filterbank's 16-slot history exactly.
    // The output ends within the frame after the last, so frame - 1 is always a real frame.
    if (frame > 0) {
        if (!Reach(frame - 1) || !Frame(true)) { failed_ = true; return false; }
    } else if (!Reach(0)) {
        failed_ = true;
        return false;
    }
    position_ = target;
    return true;
}
}  // namespace

std::unique_ptr<Decoder> OpenMusepackDecoder(const std::wstring& path) {
    auto decoder = std::make_unique<MpcDecoder>();
    return decoder->Open(path) ? std::move(decoder) : nullptr;
}
