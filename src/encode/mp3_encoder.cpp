// cordinal MP3 encoder: polyphase analysis, alias-reduced MDCT, iterative
// global-gain allocation and an own MPEG-1 Layer III frame writer.
#include "mp3_encoder.h"
#include "bitwriter.h"

#include "../audio_mpeg_tables.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace cordinal::encode {
namespace {

using nativeaudio::mpeg::HuffNode;
using nativeaudio::mpeg::huffman;
using nativeaudio::mpeg::linbits;
using nativeaudio::mpeg::synthesisWindow;

constexpr double kPi = 3.14159265358979323846;
constexpr int kSubbands = 32;
constexpr int kSlots = 18;
constexpr int kGranuleSamples = 576;

constexpr int kBitrates[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
constexpr int kLongBands[3][23] = {
    {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576},
    {0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576},
    {0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576},
};

constexpr float kAliasCs[8] = {0.8574929257f, 0.8817419973f, 0.9496286491f, 0.9833145925f,
                               0.9955178161f, 0.9991605582f, 0.9998991952f, 0.9999931551f};
constexpr float kAliasCa[8] = {-0.5144957554f, -0.4717319686f, -0.3133774542f, -0.1819131996f,
                               -0.0945741925f, -0.0409655829f, -0.0141985686f, -0.0036999747f};

struct HuffCode {
    uint32_t bits = 0;
    uint8_t length = 0;
};

void WalkTree(const HuffNode* tree, int node, uint32_t code, unsigned length, HuffCode* out) {
    if (node < 0) {
        const int symbol = -node - 1;
        if (symbol >= 0 && symbol < 256) out[symbol] = {code, (uint8_t)length};
        return;
    }
    WalkTree(tree, tree[node].child[0], code << 1, length + 1, out);
    WalkTree(tree, tree[node].child[1], (code << 1) | 1u, length + 1, out);
}

int TableForPeak(int peak) {
    if (peak <= 15) return 15;
    if (peak <= 30) return 24;
    return 31;
}

inline double QuantizeMagnitude(double magnitude, double gain) {
    const double v = magnitude * gain;
    if (v <= 0.0) return 0.0;
    if (v > 1.0e12) return 8206.0;
    return std::sqrt(v * std::sqrt(v));
}

}  // namespace

struct Mp3Encoder::Impl {
    int sample_rate = 0;
    int channels = 0;
    int bitrate = 0;
    int br_index = 0;
    int sr_index = 0;
    bool ready = false;

    double window[512]{};
    double matrix[kSubbands][64]{};
    double mdct_win[36]{};
    double mdct_cos[18][36]{};
    std::array<HuffCode, 256> codes[34]{};

    double history[2][512]{};
    int offset[2]{};
    double prev[2][kSubbands][kSlots]{};
    uint64_t pad_acc = 0;

    struct Plan {
        int gain = 255;
        int table = 15;
        int big_values = 0;
        int bits = 0;
    };
    Plan plans[2][2];
    int16_t quant[2][2][kGranuleSamples]{};

    void InitTables() {
        for (int i = 0; i < 512; ++i) window[i] = (double)synthesisWindow[i];
        for (int sb = 0; sb < kSubbands; ++sb)
            for (int k = 0; k < 64; ++k)
                matrix[sb][k] = std::cos((2.0 * sb + 1.0) * (k - 16.0) * kPi / 64.0);
        for (int i = 0; i < 36; ++i) {
            mdct_win[i] = std::sin(kPi / 36.0 * (i + 0.5));
            for (int k = 0; k < 18; ++k)
                mdct_cos[k][i] = std::cos(kPi / 72.0 * (2.0 * i + 19.0) * (2.0 * k + 1.0)) / 288.0;
        }
        for (int table = 1; table < 34; ++table) {
            if (!huffman[table]) continue;
            WalkTree(huffman[table], 0, 0, 0, codes[table].data());
        }
    }

    void Analyze(const float* pcm, int ch, double out[kSubbands][36]) {
        for (int ts = 0; ts < 36; ++ts) {
            offset[ch] = (offset[ch] - 32) & 511;
            for (int i = 0; i < 32; ++i)
                history[ch][(offset[ch] + 31 - i) & 511] = (double)pcm[ts * 32 + i];
            double z[64];
            for (int j = 0; j < 64; ++j) {
                double sum = 0.0;
                for (int p = 0; p < 8; ++p)
                    sum += history[ch][(offset[ch] + j + 64 * p) & 511] * window[j + 64 * p];
                z[j] = sum;
            }
            for (int sb = 0; sb < kSubbands; ++sb) {
                double sum = 0.0;
                for (int k = 0; k < 64; ++k) sum += z[k] * matrix[sb][k];
                out[sb][ts] = sum;
            }
        }
    }

    void Mdct(const double sub[kSubbands][2 * kSlots], int granule, int ch, double* out) {
        for (int sb = 0; sb < kSubbands; ++sb) {
            double x[36];
            for (int i = 0; i < 18; ++i) x[i] = prev[ch][sb][i];
            for (int i = 0; i < 18; ++i) {
                double v = sub[sb][granule * 18 + i];
                if ((sb & 1) && (i & 1)) v = -v;
                x[18 + i] = v;
            }
            for (int k = 0; k < 18; ++k) {
                double sum = 0.0;
                for (int n = 0; n < 36; ++n) sum += x[n] * mdct_win[n] * mdct_cos[k][n];
                out[sb * 18 + k] = sum;
            }
            for (int i = 0; i < 18; ++i) prev[ch][sb][i] = sub[sb][granule * 18 + i];
        }
        for (int sb = 0; sb < 31; ++sb) {
            for (int i = 0; i < 8; ++i) {
                const int lo = sb * 18 + 17 - i, hi = (sb + 1) * 18 + i;
                const double a = out[lo], b = out[hi];
                out[lo] = a * kAliasCs[i] + b * kAliasCa[i];
                out[hi] = b * kAliasCs[i] - a * kAliasCa[i];
            }
        }
    }

    void Lowpass(double* spec) const {
        const int per_channel = bitrate / std::max(1, channels);
        double hz;
        if (per_channel <= 64) hz = 11000.0;
        else if (per_channel <= 96) hz = 14000.0;
        else if (per_channel <= 128) hz = 16000.0;
        else if (per_channel <= 192) hz = 18500.0;
        else hz = 20000.0;
        const int first = std::min(kGranuleSamples, (int)(hz * 1152.0 / sample_rate));
        std::fill(spec + first, spec + kGranuleSamples, 0.0);
    }

    int Quantize(const double* spec, int gain, int16_t* q) const {
        // 2^((210-gain)/4): the decoder's gain applied to |q|^(4/3).
        const double scale = std::exp2((210 - gain) * 0.25);
        int peak = 0;
        for (int i = 0; i < kGranuleSamples; ++i) {
            const double v = std::fabs(spec[i]);
            int value = (int)QuantizeMagnitude(v, scale);
            if (value > 8206) value = 8206;
            q[i] = (int16_t)(spec[i] < 0.0 ? -value : value);
            peak = std::max(peak, value);
        }
        return peak;
    }

    int CountBits(const int16_t* q, int table, int& big_values) const {
        int last = kGranuleSamples - 1;
        while (last >= 0 && q[last] == 0) --last;
        big_values = last < 0 ? 0 : (last + 2) / 2;
        int bits = 0;
        for (int i = 0; i < big_values; ++i) {
            const int x = q[i * 2], y = q[i * 2 + 1];
            const int sx = std::min(15, std::abs(x)), sy = std::min(15, std::abs(y));
            const auto& code = codes[table][(sx << 4) | sy];
            bits += code.length;
            if (sx == 15 && linbits[table]) bits += linbits[table];
            bits += x != 0;
            if (sy == 15 && linbits[table]) bits += linbits[table];
            bits += y != 0;
            if (bits > 4095) return bits;
        }
        return bits;
    }

    Plan ChoosePlan(const double* spec, int budget, int16_t* q) {
        // Binary search for the finest (lowest) global gain whose Huffman
        // payload still fits. A gain of 255 zeroes the granule, so a fit always
        // exists unless the caller's budget is smaller than a single code.
        Plan best;
        double peak = 0.0;
        for (int i = 0; i < kGranuleSamples; ++i) peak = std::max(peak, std::fabs(spec[i]));
        int lo = 0;
        if (peak > 0.0) {
            const double limit = std::pow(8206.0, 4.0 / 3.0);
            lo = std::clamp((int)std::ceil(210.0 - 4.0 * std::log2(limit / peak)), 0, 255);
        }
        int hi = 255;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            const int quantizedPeak = Quantize(spec, mid, q);
            Plan candidate;
            candidate.gain = mid;
            candidate.table = TableForPeak(quantizedPeak);
            candidate.bits = CountBits(q, candidate.table, candidate.big_values);
            if (candidate.bits <= budget) {
                best = candidate;
                hi = mid - 1;
            } else {
                lo = mid + 1;
            }
        }
        if (best.bits > budget) {
            best.gain = 255;
            best.table = 15;
            Quantize(spec, best.gain, q);
            best.bits = CountBits(q, best.table, best.big_values);
        }
        // Re-quantize into the caller's buffer for the selected plan; the last
        // candidate tried by the search is not necessarily the one chosen.
        Quantize(spec, best.gain, q);
        best.bits = CountBits(q, best.table, best.big_values);
        return best;
    }

    void WriteHuffman(BitWriter& bits, const int16_t* q, int table, int big_values) const {
        for (int i = 0; i < big_values; ++i) {
            const int x = q[i * 2], y = q[i * 2 + 1];
            const int sx = std::min(15, std::abs(x)), sy = std::min(15, std::abs(y));
            const auto& code = codes[table][(sx << 4) | sy];
            bits.Put(code.bits, code.length);
            if (sx == 15 && linbits[table]) bits.Put((uint32_t)(std::abs(x) - 15), linbits[table]);
            if (x) bits.Put(x < 0 ? 1u : 0u, 1);
            if (sy == 15 && linbits[table]) bits.Put((uint32_t)(std::abs(y) - 15), linbits[table]);
            if (y) bits.Put(y < 0 ? 1u : 0u, 1);
        }
    }

    bool EncodeFrame(const float* const* pcm, std::vector<uint8_t>& frame) {
        // CBR frame size, with the standard fractional padding accumulator.
        const int base = 144000 * bitrate / sample_rate;
        pad_acc += (uint64_t)(144000 * bitrate % sample_rate);
        int padding = 0;
        if (pad_acc >= (uint64_t)sample_rate) {
            padding = 1;
            pad_acc -= (uint64_t)sample_rate;
        }
        const int frame_bytes = base + padding;

        double sub[2][kSubbands][36]{};
        for (int ch = 0; ch < channels; ++ch) Analyze(pcm[ch], ch, sub[ch]);

        double spec[2][2][kGranuleSamples];
        for (int gr = 0; gr < 2; ++gr) {
            for (int ch = 0; ch < channels; ++ch) {
                Mdct(sub[ch], gr, ch, spec[gr][ch]);
                Lowpass(spec[gr][ch]);
            }
        }

        const int side_bytes = channels == 1 ? 17 : 32;
        const int main_bytes = frame_bytes - 4 - side_bytes;
        if (main_bytes <= 2) return false;
        // Leave two bytes of slack so that rounding each fit up to a byte can
        // never spill into the ancillary area.
        const int main_bits = (main_bytes - 2) * 8;
        const int per_granule = std::min(4095, main_bits / (2 * channels));

        for (int gr = 0; gr < 2; ++gr)
            for (int ch = 0; ch < channels; ++ch)
                plans[gr][ch] = ChoosePlan(spec[gr][ch], per_granule, quant[gr][ch]);

        BitWriter header;
        header.Put(0x7FF, 11);       // sync
        header.Put(3, 2);            // MPEG-1
        header.Put(1, 2);            // Layer III
        header.Put(1, 1);            // no CRC
        header.Put((uint32_t)br_index, 4);
        header.Put((uint32_t)sr_index, 2);
        header.Put((uint32_t)padding, 1);
        header.Put(0, 1);            // private
        header.Put(channels == 1 ? 3u : 0u, 2);  // mono / stereo
        header.Put(0, 2);            // mode extension
        header.Put(0, 1);            // copyright
        header.Put(1, 1);            // original
        header.Put(0, 2);            // emphasis
        header.Align();

        BitWriter side;
        side.Put(0, 9);              // main_data_begin: no reservoir
        side.Put(0, channels == 1 ? 5 : 3);
        if (channels == 2) {
            side.Put(0, 4);          // scfsi[0]
            side.Put(0, 4);          // scfsi[1]
        }
        for (int gr = 0; gr < 2; ++gr) {
            for (int ch = 0; ch < channels; ++ch) {
                const Plan& p = plans[gr][ch];
                side.Put((uint32_t)p.bits, 12);
                side.Put((uint32_t)p.big_values, 9);
                side.Put((uint32_t)p.gain, 8);
                side.Put(0, 4);      // scalefac_compress: all scalefactors zero
                side.Put(0, 1);      // window_switching_flag: long blocks
                side.Put((uint32_t)p.table, 5);
                side.Put((uint32_t)p.table, 5);
                side.Put((uint32_t)p.table, 5);
                side.Put(0, 4);      // region0_count
                side.Put(0, 3);      // region1_count
                side.Put(0, 1);      // preflag
                side.Put(0, 1);      // scalefac_scale
                side.Put(0, 1);      // count1table_select
            }
        }
        std::vector<uint8_t> side_data = side.Finish();
        if ((int)side_data.size() != side_bytes) return false;

        BitWriter main;
        for (int gr = 0; gr < 2; ++gr)
            for (int ch = 0; ch < channels; ++ch)
                WriteHuffman(main, quant[gr][ch], plans[gr][ch].table, plans[gr][ch].big_values);
        std::vector<uint8_t> main_data = main.Finish();
        if ((int)main_data.size() > main_bytes) return false;

        frame.assign((size_t)frame_bytes, 0);
        std::copy_n(header.BytesVector().begin(), 4, frame.begin());
        std::copy(side_data.begin(), side_data.end(), frame.begin() + 4);
        std::copy(main_data.begin(), main_data.end(), frame.begin() + 4 + side_bytes);
        return true;
    }
};

Mp3Encoder::Mp3Encoder() : impl_(new Impl()) {}

Mp3Encoder::~Mp3Encoder() { delete impl_; }

bool Mp3Encoder::Open(int sample_rate, int channels, int bitrate) {
    ready_ = false;
    if (!impl_) impl_ = new Impl();
    if (sample_rate == 48000) impl_->sr_index = 1;
    else if (sample_rate == 44100) impl_->sr_index = 0;
    else if (sample_rate == 32000) impl_->sr_index = 2;
    else return false;
    if (channels < 1 || channels > 2) return false;
    int br_index = 0;
    for (int i = 1; i < 15; ++i)
        if (kBitrates[i] == bitrate) br_index = i;
    if (!br_index) return false;
    if (channels == 1 && bitrate > 160) return false;
    impl_->sample_rate = sample_rate;
    impl_->channels = channels;
    impl_->bitrate = bitrate;
    impl_->br_index = br_index;
    impl_->ready = true;
    impl_->InitTables();
    impl_->pad_acc = 0;
    std::memset(impl_->history, 0, sizeof impl_->history);
    std::memset(impl_->prev, 0, sizeof impl_->prev);
    impl_->offset[0] = impl_->offset[1] = 0;
    sample_rate_ = sample_rate;
    channels_ = channels;
    bitrate_ = bitrate;
    ready_ = true;
    return true;
}

bool Mp3Encoder::Encode(const float* const* channel_data, std::vector<uint8_t>& frame) {
    if (!ready_ || !impl_ || !channel_data || !channel_data[0]) return false;
    return impl_->EncodeFrame(channel_data, frame);
}

}  // namespace cordinal::encode
