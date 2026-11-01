// cordinal AAC-LC encoder: MDCT, band quantization, codebook selection,
// Huffman emission and ADTS framing.
#include "aac_lc_encoder.h"
#include "bitwriter.h"

#include "../audio_aac_tables.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>

namespace cordinal::encode {
namespace {

using namespace nativeaudio::aacdata;

constexpr double kPi = 3.14159265358979323846;
constexpr int kFrameLen = 1024;
constexpr int kMaxBands = 64;

const uint16_t* const kCodeTables[12] = {
    codes0, codes1, codes2, codes3, codes4, codes5,
    codes6, codes7, codes8, codes9, codes10, codes11,
};
const size_t kCodeCounts[12] = {
    std::size(codes0), std::size(codes1), std::size(codes2), std::size(codes3),
    std::size(codes4), std::size(codes5), std::size(codes6), std::size(codes7),
    std::size(codes8), std::size(codes9), std::size(codes10), std::size(codes11),
};

struct Code {
    uint32_t bits = 0;
    uint8_t length = 0;
};

// Codebook shape: dimension, digit base, unsigned offset, largest magnitude and
// whether the codebook carries sign bits.
struct BookShape {
    int dim;
    int base;
    int offset;
    int max_value;
    bool signs;
};

int MaxValue(int book) {
    if (book <= 2) return 1;
    if (book <= 4) return 2;
    if (book <= 6) return 4;
    if (book <= 8) return 7;
    if (book <= 10) return 12;
    return 8190;
}

BookShape ShapeOf(int book) {
    BookShape s{};
    s.dim = book < 5 ? 4 : 2;
    if (book < 5) s.base = 3;
    else if (book < 7) s.base = 9;
    else if (book < 9) s.base = 8;
    else if (book < 11) s.base = 13;
    else s.base = 17;
    s.offset = (book == 1 || book == 2) ? 1 : ((book == 5 || book == 6) ? 4 : 0);
    s.max_value = MaxValue(book);
    s.signs = !(book == 1 || book == 2 || book == 5 || book == 6);
    return s;
}

std::span<const uint16_t> LongBands(int sr_index) {
    if (sr_index < 2) return bands140;
    if (sr_index == 2) return bands138;
    if (sr_index < 5) return bands129;
    if (sr_index == 5) return bands131;
    if (sr_index < 8) return bands136;
    if (sr_index < 11) return bands134;
    return bands132;
}

inline double Pow34(double x) {
    if (x <= 0.0) return 0.0;
    if (x > 1.0e12) return 1.0e9;
    return std::sqrt(x * std::sqrt(x));
}

}  // namespace

struct AacLcEncoder::Impl {
    int sample_rate = 0;
    int channels = 0;
    int bitrate = 0;
    int sr_index = 0;
    bool ready = false;
    bool flushed = false;
    uint64_t received = 0;

    std::array<std::array<Code, 289>, 12> codes{};
    float taper[kFrameLen]{};
    std::vector<float> cos_table;  // [k][n], 1024 x 2048
    float history[2][3][kFrameLen]{};

    void InitTables() {
        for (int book = 0; book < 12; ++book) {
            uint32_t position = 0;
            const uint16_t* leaves = kCodeTables[book];
            for (size_t i = 0; i < kCodeCounts[book]; ++i) {
                const unsigned n = leaves[i] & 31u;
                const unsigned symbol = leaves[i] >> 5;
                const uint32_t code = n ? position >> (32 - n) : 0;
                position += n ? (1u << (32 - n)) : 0;
                if (symbol < 289 && n) codes[book][symbol] = {code, (uint8_t)n};
            }
        }
        // The low-overlap sine window is signalled with window_shape 0, so the
        // filterbank and decoder use the same taper without a KBD table.
        for (int i = 0; i < kFrameLen; ++i)
            taper[i] = (float)std::sin(kPi * (i + 0.5) / (2.0 * kFrameLen));

        cos_table.resize((size_t)kFrameLen * 2 * kFrameLen);
        for (int k = 0; k < kFrameLen; ++k)
            for (int n = 0; n < 2 * kFrameLen; ++n)
                cos_table[(size_t)k * 2 * kFrameLen + n] =
                    (float)std::cos(kPi / kFrameLen * (n + 0.5 + kFrameLen * 0.5) * (k + 0.5));
    }

    void Mdct(const float* previous, const float* current, float* out) const {
        float x[2 * kFrameLen];
        for (int i = 0; i < kFrameLen; ++i) {
            x[i] = previous[i] * taper[i];
            x[kFrameLen + i] = current[i] * taper[kFrameLen - 1 - i];
        }
        for (int k = 0; k < kFrameLen; ++k) {
            const float* row = &cos_table[(size_t)k * 2 * kFrameLen];
            double sum = 0.0;
            for (int n = 0; n < 2 * kFrameLen; ++n) sum += (double)x[n] * row[n];
            out[k] = (float)(2.0 * sum);
        }
    }

    int MaxSfb(const std::span<const uint16_t>& bands, const float* spec) const {
        const int count = (int)bands.size() - 1;
        float peak = 0.0f;
        for (int i = 0; i < kFrameLen; ++i) peak = std::max(peak, std::fabs(spec[i]));
        if (peak < 1.0e-7f) return 0;
        const float floor = peak * 1.0e-5f;
        int last = 0;
        for (int b = 0; b < count; ++b)
            for (int i = bands[b]; i < bands[b + 1]; ++i)
                if (std::fabs(spec[i]) > floor) {
                    last = b + 1;
                    break;
                }
        return std::min(last, count);
    }

    void Quantize(const float* spec, int gain, int16_t* q) const {
        const double step = std::exp2((gain - 100) * 0.25);
        for (int i = 0; i < kFrameLen; ++i) {
            // The decoder's dequantizer divides by 32768, so the encoder works in
            // the same int16 amplitude scale even though the filterbank input is
            // normalized float.
            const double magnitude = std::fabs((double)spec[i]) * 32768.0 / step;
            int value = (int)(Pow34(magnitude) + 0.4054);
            if (value > 8190) value = 8190;
            q[i] = (int16_t)(spec[i] < 0.0f ? -value : value);
        }
    }

    int BandCost(const int16_t* q, int start, int end, int book) const {
        const BookShape s = ShapeOf(book);
        if ((end - start) % s.dim) return 0x3FFFFFFF;
        int bits = 0;
        for (int at = start; at < end; at += s.dim) {
            int symbol = 0;
            int values[4]{};
            for (int j = 0; j < s.dim; ++j) {
                int v = q[at + j];
                if (std::abs(v) > s.max_value && book != 11) return 0x3FFFFFFF;
                values[j] = v;
                int digit = s.signs ? std::abs(v) : v + s.offset;
                if (book == 11 && digit > 16) digit = 16;
                symbol = symbol * s.base + digit;
            }
            if (symbol < 0 || symbol >= 289) return 0x3FFFFFFF;
            bits += codes[book][symbol].length;
            if (s.signs) {
                for (int j = 0; j < s.dim; ++j) if (values[j]) ++bits;
            }
            if (book == 11) {
                for (int j = 0; j < s.dim; ++j) {
                    const int m = std::abs(values[j]);
                    if (m >= 16) {
                        int n = 4;
                        while (n < 12 && m >= (1 << (n + 1))) ++n;
                        bits += (n - 4) + 1 + n;
                    }
                }
            }
        }
        return bits;
    }

    int ChooseBook(const int16_t* q, int start, int end) const {
        int peak = 0;
        for (int i = start; i < end; ++i) peak = std::max(peak, (int)std::abs(q[i]));
        if (!peak) return 0;
        int best = 11, best_cost = 0x3FFFFFFF;
        for (int book = 1; book <= 11; ++book) {
            const BookShape s = ShapeOf(book);
            if ((end - start) % s.dim) continue;
            if (book != 11 && peak > s.max_value) continue;
            const int cost = BandCost(q, start, end, book);
            if (cost < best_cost) {
                best_cost = cost;
                best = book;
            }
        }
        return best;
    }

    void AnalyzeBands(const int16_t* q, const std::span<const uint16_t>& bands, int max_sfb,
                      uint8_t* books) const {
        for (int b = 0; b < max_sfb; ++b)
            books[b] = (uint8_t)ChooseBook(q, bands[b], bands[b + 1]);
    }

    void WriteSections(BitWriter& bits, const uint8_t* books, int max_sfb) const {
        for (int b = 0; b < max_sfb;) {
            const int book = books[b];
            int run = 1;
            while (b + run < max_sfb && books[b + run] == book) ++run;
            bits.Put((uint32_t)book, 4);
            int left = run;
            while (left >= 31) {
                bits.Put(31, 5);
                left -= 31;
            }
            bits.Put((uint32_t)left, 5);
            b += run;
        }
    }

    void WriteScaleFactors(BitWriter& bits, const uint8_t* books, int max_sfb) const {
        // All bands share global_gain, so every transmitted delta is zero; the
        // scalefactor codebook maps zero to symbol 60.
        const Code zero = codes[0][60];
        for (int b = 0; b < max_sfb; ++b)
            if (books[b]) bits.Put(zero.bits, zero.length);
    }

    void WriteSpectral(BitWriter& bits, const int16_t* q, const std::span<const uint16_t>& bands,
                       int max_sfb, const uint8_t* books) const {
        for (int b = 0; b < max_sfb; ++b) {
            const int book = books[b];
            if (!book) continue;
            const BookShape s = ShapeOf(book);
            for (int at = bands[b]; at < bands[b + 1]; at += s.dim) {
                int symbol = 0;
                int values[4]{};
                for (int j = 0; j < s.dim; ++j) {
                    const int v = q[at + j];
                    values[j] = v;
                    int digit = s.signs ? std::abs(v) : v + s.offset;
                    if (book == 11 && digit > 16) digit = 16;
                    symbol = symbol * s.base + digit;
                }
                const Code code = codes[book][symbol];
                bits.Put(code.bits, code.length);
                if (s.signs) {
                    for (int j = 0; j < s.dim; ++j)
                        if (values[j]) bits.Put(values[j] < 0 ? 1u : 0u, 1);
                }
                if (book == 11) {
                    for (int j = 0; j < s.dim; ++j) {
                        const int m = std::abs(values[j]);
                        if (m >= 16) {
                            int n = 4;
                            while (n < 12 && m >= (1 << (n + 1))) ++n;
                            for (int u = 0; u < n - 4; ++u) bits.Put(1, 1);
                            bits.Put(0, 1);
                            bits.Put((uint32_t)(m - (1 << n)), (unsigned)n);
                        }
                    }
                }
            }
        }
    }

    void WriteIcs(BitWriter& bits, int max_sfb) const {
        bits.Put(0, 1);              // ics_reserved_bit
        bits.Put(0, 2);              // window_sequence: only long blocks
        bits.Put(0, 1);              // window_shape: sine (matches the filterbank)
        bits.Put((uint32_t)max_sfb, 6);
        bits.Put(0, 1);              // predictor_data_present
    }

    void WriteChannel(BitWriter& bits, int gain, const int16_t* q,
                      const std::span<const uint16_t>& bands, int max_sfb,
                      const uint8_t* books) const {
        bits.Put((uint32_t)gain, 8);
        WriteSections(bits, books, max_sfb);
        WriteScaleFactors(bits, books, max_sfb);
        bits.Put(0, 1);              // pulse_data_present
        bits.Put(0, 1);              // tns_data_present
        bits.Put(0, 1);              // gain_control_data_present (SSR only)
        WriteSpectral(bits, q, bands, max_sfb, books);
    }

    void BuildElements(BitWriter& bits, const float spec[2][kFrameLen], int max_sfb, int gain,
                       int16_t q[2][kFrameLen], uint8_t books[2][kMaxBands]) const {
        const auto bands = LongBands(sr_index);
        for (int ch = 0; ch < channels; ++ch) {
            Quantize(spec[ch], gain, q[ch]);
            AnalyzeBands(q[ch], bands, max_sfb, books[ch]);
        }
        if (channels == 1) {
            bits.Put(0, 3);          // ID_SCE
            bits.Put(0, 4);          // element_instance_tag
            WriteIcs(bits, max_sfb);
            WriteChannel(bits, gain, q[0], bands, max_sfb, books[0]);
        } else {
            bits.Put(1, 3);          // ID_CPE
            bits.Put(0, 4);
            bits.Put(1, 1);          // common_window
            WriteIcs(bits, max_sfb);
            bits.Put(0, 2);          // ms_mask_present: no M/S
            WriteChannel(bits, gain, q[0], bands, max_sfb, books[0]);
            WriteChannel(bits, gain, q[1], bands, max_sfb, books[1]);
        }
    }

    size_t RawBytes(BitWriter& bits) const {
        return (bits.Bits() + 3 + 7) / 8;  // ID_END and byte alignment
    }

    // Smallest global gain that does not clamp the largest coded line. A lower
    // gain looks cheaper only because every oversized value stops growing at 8190,
    // so the rate search must not choose one.
    int LowestGain(const float spec[2][kFrameLen], const std::span<const uint16_t>& bands, int max_sfb) const {
        float peak = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            for (int b = 0; b < max_sfb; ++b)
                for (int i = bands[b]; i < bands[b + 1]; ++i) peak = std::max(peak, std::fabs(spec[ch][i]));
        if (!(peak > 0.0f)) return 0;
        const double limit = std::pow(8190.0, 4.0 / 3.0);
        const double step = (double)peak * 32768.0 / limit;
        return std::clamp((int)std::ceil(100.0 + 4.0 * std::log2(std::max(step, 1.0e-9))), 0, 255);
    }

    bool EncodeBlock(const float spec[2][kFrameLen], std::vector<uint8_t>& frame) {
        const auto bands = LongBands(sr_index);
        int max_sfb = 0;
        for (int ch = 0; ch < channels; ++ch) max_sfb = std::max(max_sfb, MaxSfb(bands, spec[ch]));

        const int target_bytes =
            (int)(((uint64_t)bitrate * 1000 * kFrameLen + 8ull * sample_rate - 1) / (8ull * sample_rate));
        if (target_bytes <= 8) return false;
        const int target_raw = target_bytes - 7;

        // Drop high scalefactor bands only if the requested rate cannot fit the
        // full band at maximum gain.
        int chosen_gain = -1, chosen_sfb = max_sfb;
        for (int sfb = max_sfb; sfb >= 0 && chosen_gain < 0; --sfb) {
            int lo = LowestGain(spec, bands, sfb), hi = 255, best = -1;
            while (lo <= hi) {
                const int mid = (lo + hi) / 2;
                int16_t q[2][kFrameLen];
                uint8_t books[2][kMaxBands]{};
                BitWriter probe;
                BuildElements(probe, spec, sfb, mid, q, books);
                if ((int)RawBytes(probe) <= target_raw) {
                    best = mid;
                    hi = mid - 1;
                } else {
                    lo = mid + 1;
                }
            }
            if (best >= 0) {
                chosen_gain = best;
                chosen_sfb = sfb;
            }
        }
        if (chosen_gain < 0) return false;
        int16_t q[2][kFrameLen];
        uint8_t books[2][kMaxBands]{};
        BitWriter bits;
        BuildElements(bits, spec, chosen_sfb, chosen_gain, q, books);

        // Choose the largest fill payload that still leaves the ADTS frame at
        // or below the target size. A fill element is legal before ID_END and
        // keeps the stream close to the requested constant rate.
        int best_fill = 0;
        for (int payload = 0; payload <= 269; ++payload) {
            const int header = payload < 15 ? 7 : 15;
            const size_t total = bits.Bits() + header + (size_t)payload * 8 + 3;
            if ((int)((total + 7) / 8) <= target_raw) best_fill = payload;
        }
        if (best_fill) {
            bits.Put(6, 3);          // ID_FIL
            if (best_fill < 15) {
                bits.Put((uint32_t)best_fill, 4);
            } else {
                bits.Put(15, 4);
                bits.Put((uint32_t)(best_fill - 14), 8);
            }
            for (int i = 0; i < best_fill; ++i) bits.Put(0, 8);
        }
        bits.Put(7, 3);              // ID_END
        bits.Align();
        const std::vector<uint8_t>& raw = bits.BytesVector();

        const size_t total = 7 + raw.size();
        if (total > (1u << 13)) return false;
        frame.assign(total, 0);
        frame[0] = 0xFF;
        frame[1] = 0xF1;             // MPEG-4, Layer 0, no CRC
        frame[2] = (uint8_t)((1 << 6) | (sr_index << 2) | ((channels >> 2) & 1));
        frame[3] = (uint8_t)(((channels & 3) << 6) | ((total >> 11) & 3));
        frame[4] = (uint8_t)((total >> 3) & 0xFF);
        frame[5] = (uint8_t)(((total & 7) << 5) | 0x1F);
        frame[6] = 0xFC;
        std::copy(raw.begin(), raw.end(), frame.begin() + 7);
        return true;
    }

    bool Encode(const float* const* pcm, std::vector<uint8_t>& frame) {
        float spec[2][kFrameLen];
        for (int ch = 0; ch < channels; ++ch)
            Mdct(history[ch][0], history[ch][1], spec[ch]);
        for (int ch = 0; ch < channels; ++ch) {
            std::memcpy(history[ch][0], history[ch][1], sizeof history[ch][0]);
            std::memcpy(history[ch][1], history[ch][2], sizeof history[ch][1]);
            for (int i = 0; i < kFrameLen; ++i) history[ch][2][i] = pcm[ch][i];
        }
        ++received;
        return EncodeBlock(spec, frame);
    }

    void Flush(std::vector<std::vector<uint8_t>>& frames) {
        if (flushed) return;
        flushed = true;
        if (!received) return;
        // The three held blocks are B[N-2], B[N-1] and the final input block.
        // Three tail frames finish the transform and leave enough media for the
        // M4A edit list to trim both the priming and the final partial frame.
        float zero[kFrameLen]{};
        for (int block = 0; block < 3; ++block) {
            float spec[2][kFrameLen];
            for (int ch = 0; ch < channels; ++ch)
                Mdct(history[ch][block], block < 2 ? history[ch][block + 1] : zero, spec[ch]);
            std::vector<uint8_t> frame;
            if (EncodeBlock(spec, frame) && !frame.empty()) frames.push_back(std::move(frame));
        }
    }
};

AacLcEncoder::AacLcEncoder() : impl_(new Impl()) {}

AacLcEncoder::~AacLcEncoder() { delete impl_; }

bool AacLcEncoder::Open(int sample_rate, int channels, int bitrate) {
    ready_ = false;
    if (!impl_) impl_ = new Impl();
    if (sample_rate == 48000) impl_->sr_index = 3;
    else if (sample_rate == 44100) impl_->sr_index = 4;
    else if (sample_rate == 32000) impl_->sr_index = 5;
    else return false;
    if (channels < 1 || channels > 2) return false;
    if (bitrate < 16 || bitrate > 512) return false;
    impl_->sample_rate = sample_rate;
    impl_->channels = channels;
    impl_->bitrate = bitrate;
    impl_->flushed = false;
    impl_->received = 0;
    impl_->InitTables();
    std::memset(impl_->history, 0, sizeof impl_->history);
    sample_rate_ = sample_rate;
    channels_ = channels;
    bitrate_ = bitrate;
    ready_ = true;
    return true;
}

bool AacLcEncoder::Encode(const float* const* channel_data, std::vector<uint8_t>& frame) {
    if (!ready_ || !impl_ || !channel_data || !channel_data[0]) return false;
    if (impl_->channels == 2 && !channel_data[1]) return false;
    return impl_->Encode(channel_data, frame);
}

void AacLcEncoder::Flush(std::vector<std::vector<uint8_t>>& frames) {
    if (ready_ && impl_) impl_->Flush(frames);
}

}  // namespace cordinal::encode
