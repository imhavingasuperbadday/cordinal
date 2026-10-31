// Cordinal FLAC decoder, implemented from RFC 9639. Native FLAC, 1..8 channels,
// 4..32-bit PCM, fixed/LPC/verbatim/constant subframes, both Rice methods, wasted
// bits and every stereo assignment. Frames are authenticated before being emitted.
#include "native_audio.h"
#include "audio_file.h"
#include "audio_packets.h"
#include "audio_kernels.h"
#include <array>
#include <bit>
#include <cmath>
#include <emmintrin.h>

namespace {
using namespace nativeaudio;

// CRC8 and CRC16 are the same shift-register recurrence; the constant tables
// below (and the slice-by-8 table derived at first open) expand it, so no
// bit-at-a-time loop remains on the hot path.
template<unsigned Polynomial, unsigned Width> constexpr auto CrcTable() {
    std::array<uint16_t, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned v = i << (Width - 8);
        for (int j = 0; j < 8; ++j) v = (v << 1) ^ ((v & (1u << (Width - 1))) ? Polynomial : 0);
        table[i] = (uint16_t)(v & ((1u << Width) - 1));
    }
    return table;
}
constexpr auto crc8Table = CrcTable<7, 8>();
constexpr auto crc16Table = CrcTable<0x8005, 16>();
// Slice-by-8 CRC16 state: eight 256-entry tables let Crc16 fold eight bytes per
// iteration instead of one. Derived on first use into .bss instead of being folded
// into the image.
using CrcSlices = std::array<std::array<uint16_t, 256>, 8>;
CrcSlices crcSlices;
// Two short Rice residuals share one byte lookup. Each entry holds the consumed
// length and two unsigned nibbles; all three tables fit comfortably in L1.
std::array<std::array<uint16_t,256>,3> riceGroups;
void InitRiceGroups() {
    static const bool ready=[] {
        for (unsigned k=0;k<3;++k) for (unsigned prefix=0;prefix<256;++prefix) {
            unsigned at=0,count=0,packed=0,consumed=0;
            while (count<2 && at<8) {
                unsigned q=0;
                while (at<8 && !((prefix>>(7-at))&1)) { ++q;++at; }
                if (at==8 || at+1+k>8) break;
                ++at;
                unsigned v=(q<<k)|((prefix>>(8-at-k))&((1u<<k)-1));
                if (v>15) break;
                at+=k;packed|=v<<(count*4);++count;consumed=at;
            }
            riceGroups[k][prefix]=count==2?(uint16_t)((packed<<4)|consumed):0;
        }
        return true;
    }();
    (void)ready;
}
uint8_t Crc8(const uint8_t* p, size_t n) {
    uint8_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (uint8_t)crc8Table[v ^ p[i]];
    return v;
}
}

// Declared in audio_bits.h, whose Bits would clash with the frame reader below.
namespace nativeaudio {
uint16_t Crc16(const uint8_t* p, size_t n) {
    static const bool ready=[] {
        crcSlices[0] = crc16Table;
        for (unsigned row = 1; row < 8; ++row)
            for (unsigned i = 0; i < 256; ++i) {
                unsigned v = crcSlices[row - 1][i];
                crcSlices[row][i] = (uint16_t)((v << 8) ^ crcSlices[0][v >> 8]);
            }
        return true;
    }();
    (void)ready;
    // Continues a table CRC v over n more bytes.
    auto table = [](uint16_t v, const uint8_t* p, size_t n) {
        while (n >= 8) {
            v = crcSlices[7][p[0] ^ (v >> 8)] ^ crcSlices[6][p[1] ^ (v & 255)] ^
                crcSlices[5][p[2]] ^ crcSlices[4][p[3]] ^ crcSlices[3][p[4]] ^
                crcSlices[2][p[5]] ^ crcSlices[1][p[6]] ^ crcSlices[0][p[7]];
            p += 8; n -= 8;
        }
        while (n--) v = (uint16_t)((v << 8) ^ crc16Table[(v >> 8) ^ *p++]);
        return v;
    };
    // Long runs fold by carry-less multiplication; the folded 16 bytes then start the table
    // CRC that finishes the remainder.
    if (n >= 64 && FastCrcKernel()) {
        uint8_t folded[16];
        size_t done = AudioCrc16FoldAvx2(p, n, folded);
        return table(table(0, folded, 16), p + done, n - done);
    }
    return table(0, p, n);
}
}

namespace {

// MSB-first bit reader over one frame body. Unread bits live in the high lanes
// of cache_, so consuming is a shift; a short read sets good_ and fails the
// frame rather than silently decoding zeros.
class Bits {
public:
    Bits(const uint8_t* p, size_t size) : begin_(p), p_(p), end_(p + size) {}
    uint32_t Get(unsigned n) {
        if (!n) return 0;
        if (available_ < n) Fill();
        if (available_ < n) { good_ = false; return 0; }
        uint32_t v = (uint32_t)(cache_ >> (64 - n));
        cache_ <<= n; available_ -= n;
        return v;
    }
    int64_t Signed(unsigned n) {
        if (!n) return 0;
        uint64_t v = n <= 32 ? Get(n) : ((uint64_t)Get(n - 32) << 32) | Get(32);
        return (int64_t)(v ^ (uint64_t(1) << (n - 1))) - (int64_t)(uint64_t(1) << (n - 1));
    }
    uint32_t Unary(uint32_t limit) {
        uint64_t zeros = 0;
        for (;;) {
            if (!available_) Fill();
            if (!available_) { good_ = false; return 0; }
            unsigned run = std::min<unsigned>(std::countl_zero(cache_), available_);
            zeros += run;
            if (zeros > limit) { good_ = false; return 0; }
            if (run < available_) {
                cache_ <<= run + 1; available_ -= run + 1;
                return (uint32_t)zeros;
            }
            cache_ = 0; available_ = 0;
        }
    }
    uint32_t Rice(unsigned k,uint32_t limit) {
        if (available_<32) Fill();
        unsigned q=std::countl_zero(cache_);
        if (q<=limit && q+1+k<64 && q+1+k<=available_) {
            unsigned consumed=q+1+k;
            uint32_t v=(q<<k)|((uint32_t)(cache_>>(64-consumed))&((1u<<k)-1));
            cache_<<=consumed;available_-=consumed;
            return v;
        }
        uint32_t quotient=Unary(limit);
        return (quotient<<k)|Get(k);
    }
    template<class T> __forceinline unsigned SmallRice(T* out,unsigned k) {
        if (available_<8) Fill();
        if (available_<8) return 0;
        unsigned entry=riceGroups[k][cache_>>56];
        if (!entry) return 0;
        unsigned used=entry&15;cache_<<=used;available_-=used;
        unsigned a=(entry>>4)&15,b=(entry>>8)&15;
        out[0]=(T)((int)(a>>1)^-(int)(a&1));out[1]=(T)((int)(b>>1)^-(int)(b&1));
        return 2;
    }
    size_t Position() const { return (size_t)(p_ - begin_) * 8 - available_; }
    bool Good() const { return good_; }
private:
    void Fill() {
        if (end_ - p_ >= 4 && available_ <= 32) {
            cache_ |= (uint64_t)U32(p_, true) << (32 - available_);
            p_ += 4; available_ += 32;
        } else while (p_ < end_ && available_ <= 56) {
            cache_ |= (uint64_t)*p_++ << (56 - available_); available_ += 8;
        }
    }
    const uint8_t *begin_, *p_, *end_;
    uint64_t cache_ = 0;
    unsigned available_ = 0;
    bool good_ = true;
};

// Frame header fields. `number` is the frame index in fixed-blocksize streams
// and the first sample number in variable-blocksize ones.
struct Header {
    unsigned frames = 0, channels = 0, assignment = 0, bits = 0;
    uint64_t number = 0;
    size_t bytes = 0;
    bool variable = false;
};

// Parse and validate a frame header: sync, block/sample-rate/sample-depth
// codewords, channel assignment, the UTF-8-like frame number and the trailing
// CRC8. h.bytes is the offset of the subframe body.
bool ParseHeader(const uint8_t* p, size_t n, unsigned rate, unsigned bits, unsigned channels, Header& h) {
    if (n < 6 || p[0] != 255 || (p[1] & 0xFE) != 0xF8 || (p[3] & 1)) return false;
    unsigned block = p[2] >> 4, sr = p[2] & 15, depth = (p[3] >> 1) & 7;
    h.assignment = p[3] >> 4;
    h.channels = h.assignment < 8 ? h.assignment + 1 : 2;
    if (h.assignment > 10 || h.channels != channels || !block || sr == 15 || depth == 3) return false;
    constexpr unsigned depths[] = {0,8,12,0,16,20,24,32};
    h.bits = depth ? depths[depth] : bits;
    if (h.bits != bits) return false;
    h.variable = (p[1] & 1) != 0;
    size_t at = 4;
    uint8_t first = p[at++];
    if (first < 128) h.number = first;
    else {
        unsigned count = std::countl_one(first);
        if (count < 2 || count > (h.variable ? 7u : 6u) || n - at < count) return false;
        h.number = first & ((1u << (7 - count)) - 1);
        for (unsigned j = 1; j < count; ++j) {
            if ((p[at] & 0xC0) != 0x80) return false;
            h.number = (h.number << 6) | (p[at++] & 63);
        }
        constexpr uint64_t minimum[] = {0,0,128,2048,65536,2097152,67108864,2147483648ull};
        if (h.number < minimum[count]) return false;
    }
    size_t extra = (block == 6 ? 1u : block == 7 ? 2u : 0u) + (sr == 12 ? 1u : sr >= 13 ? 2u : 0u);
    if (n - at < extra + 1) return false;
    if (block == 6) h.frames = (unsigned)p[at++] + 1;
    else if (block == 7) { h.frames = (unsigned)U16(p + at, true) + 1; at += 2; }
    else h.frames = block == 1 ? 192 : block < 6 ? (576u << (block - 2)) : (256u << (block - 8));
    constexpr unsigned rates[] = {0,88200,176400,192000,8000,16000,22050,24000,32000,44100,48000,96000};
    unsigned frameRate = rate;
    if (sr < 12) { if (sr) frameRate = rates[sr]; }
    else if (sr == 12) frameRate = (unsigned)p[at++] * 1000;
    else { frameRate = U16(p + at, true) * (sr == 14 ? 10u : 1u); at += 2; }
    if (frameRate != rate || Crc8(p, at) != p[at]) return false;
    h.bytes = at + 1;
    return true;
}

// Decode one subframe's residual via Rice partitioning. The 2^partitionCount
// must divide the block and leave `order` warmup samples; the escape code
// (k == all-ones) falls back to a fixed-width raw sample.
template<class T> bool Residual(Bits& b, T* samples, unsigned n, unsigned order) {
    unsigned method = b.Get(2), partitions = b.Get(4);
    if (method > 1 || (n & ((1u << partitions) - 1)) || (n >> partitions) < order) return false;
    unsigned width = 4 + method, end = 0;
    for (unsigned part = 0; part < (1u << partitions); ++part) {
        unsigned k = b.Get(width), start = part ? end : order;
        end += n >> partitions;
        if (k == (1u << width) - 1) {
            unsigned raw = b.Get(5);
            for (unsigned i = start; i < end; ++i) samples[i] = (T)b.Signed(raw);
        } else {
            const uint32_t maxUnary = (UINT32_MAX - 1) >> k;
            unsigned i=start;
            if (k<3) for (;i+2<=end;) {
                if (unsigned count=b.SmallRice(samples+i,k)) { i+=count;continue; }
                uint32_t v=b.Rice(k,maxUnary);
                if (!b.Good() || v==UINT32_MAX) return false;
                samples[i++]=(T)((int64_t)(v>>1)^-(int64_t)(v&1));
            }
            for (; i < end; ++i) {
                uint32_t v = b.Rice(k,maxUnary);
                if (!b.Good() || v == UINT32_MAX) return false;
                samples[i] = (T)((int64_t)(v >> 1) ^ -(int64_t)(v & 1));
            }
        }
        if (!b.Good()) return false;
    }
    return true;
}

// Dot product for the tail case of LPC restoration; the recursion unrolls per
// predictor order at compile time.
template<unsigned Order,class Sum,class T> __forceinline Sum LpcDot(const int* coefficients,const T* sample) {
    if constexpr (!Order) return 0;
    else return LpcDot<Order-1,Sum>(coefficients,sample)+(Sum)coefficients[Order-1]*(Sum)sample[-(int)Order];
}
// Reconstruct an LPC subframe. Orders 1..8 keep a sliding window of the last
// eight samples; wider orders recompute the dot product directly. Any sample
// outside the declared bit depth fails the frame.
template<unsigned Order,class Sum,class T> bool RestoreLpc(T* s,unsigned n,const int* coefficients,unsigned shift,int64_t low,int64_t high) {
    if constexpr (Order<=8) {
        Sum a=(Sum)s[Order-1],b=Order>1?(Sum)s[Order-2]:0,c=Order>2?(Sum)s[Order-3]:0,d=Order>3?(Sum)s[Order-4]:0;
        Sum e=Order>4?(Sum)s[Order-5]:0,f=Order>5?(Sum)s[Order-6]:0,g=Order>6?(Sum)s[Order-7]:0,h=Order>7?(Sum)s[Order-8]:0;
        for (unsigned i=Order;i<n;++i) {
            Sum sum=(Sum)coefficients[0]*a;
            if constexpr (Order>1) sum+=(Sum)coefficients[1]*b;
            if constexpr (Order>2) sum+=(Sum)coefficients[2]*c;
            if constexpr (Order>3) sum+=(Sum)coefficients[3]*d;
            if constexpr (Order>4) sum+=(Sum)coefficients[4]*e;
            if constexpr (Order>5) sum+=(Sum)coefficients[5]*f;
            if constexpr (Order>6) sum+=(Sum)coefficients[6]*g;
            if constexpr (Order>7) sum+=(Sum)coefficients[7]*h;
            int64_t v=(int64_t)(sum>>shift)+s[i];
            if (v<low || v>high) return false;
            s[i]=(T)v;h=g;g=f;f=e;e=d;d=c;c=b;b=a;a=(Sum)v;
        }
    } else for (unsigned i=Order;i<n;++i) {
        int64_t v=(int64_t)(LpcDot<Order,Sum>(coefficients,s+i)>>shift)+s[i];
        if (v<low || v>high) return false;
        s[i]=(T)v;
    }
    return true;
}
// Integrating the finite differences restores a fixed predictor with only adds.
// The differences use 64 bits even for 32-bit samples, including 33-bit side channels.
template<unsigned Order,class T> bool RestoreFixed(T* s,unsigned n,int64_t low,int64_t high) {
    int64_t a=0,b=0,c=0,d=0;
    if constexpr (Order>0) a=s[Order-1];
    if constexpr (Order>1) b=a-s[Order-2];
    if constexpr (Order>2) c=b-(int64_t)s[Order-2]+s[Order-3];
    if constexpr (Order>3) d=c-(int64_t)s[Order-2]+2*(int64_t)s[Order-3]-s[Order-4];
    for (unsigned i=Order;i<n;++i) {
        int64_t v=s[i];
        if constexpr (Order>3) v=d+=v;
        if constexpr (Order>2) v=c+=v;
        if constexpr (Order>1) v=b+=v;
        if constexpr (Order>0) v=a+=v;
        if (v<low || v>high) return false;
        s[i]=(T)v;
    }
    return true;
}
// Decode one subframe: constant, verbatim, fixed or LPC, with optional wasted
// bits scaled back after reconstruction. `depth` is the effective sample depth
// for this channel (one extra bit for a side channel).
template<class T> bool Subframe(Bits& b, T* s, unsigned n, unsigned depth) {
    if (b.Get(1)) return false;
    unsigned type = b.Get(6), wasted = b.Get(1);
    if (wasted) wasted += b.Unary(depth);
    if (wasted >= depth) return false;
    unsigned bits = depth - wasted;
    const int64_t low = -(int64_t(1) << (bits - 1)), high = (int64_t(1) << (bits - 1)) - 1;
    if (type == 0) {
        T v = (T)b.Signed(bits);
        std::fill_n(s, n, v);
    } else if (type == 1) {
        for (unsigned i = 0; i < n; ++i) s[i] = (T)b.Signed(bits);
    } else {
        bool fixed = type >= 8 && type <= 12;
        if (!fixed && type < 32) return false;
        unsigned order = fixed ? type - 8 : type - 31;
        if (order > n) return false;
        for (unsigned i = 0; i < order; ++i) s[i] = (T)b.Signed(bits);
        int coefficients[32]{};
        unsigned shift = 0;
        if (!fixed) {
            unsigned precision = b.Get(4) + 1;
            if (precision == 16) return false;
            int sh = (int)b.Signed(5);
            if (sh < 0) return false;
            shift = (unsigned)sh;
            for (unsigned i = 0; i < order; ++i) coefficients[i] = (int)b.Signed(precision);
        }
        if (!Residual(b, s, n, order)) return false;
        if (fixed) {
            bool restored=false;
            switch (order) {
#define FIXED_CASE(N) case N: restored=RestoreFixed<N>(s,n,low,high);break;
            FIXED_CASE(0) FIXED_CASE(1) FIXED_CASE(2) FIXED_CASE(3) FIXED_CASE(4)
#undef FIXED_CASE
            }
            if (!restored) return false;
        } else {
            bool restored=true;
            int64_t magnitude=0;
            for (unsigned j=0;j<order;++j) magnitude+=std::abs(coefficients[j]);
            bool narrow=sizeof(T)==4 && magnitude*(int64_t(1)<<(bits-1))<=INT32_MAX;
            switch (order) {
#define LPC_CASE(N) case N: restored=narrow?RestoreLpc<N,int32_t>(s,n,coefficients,shift,low,high):RestoreLpc<N,int64_t>(s,n,coefficients,shift,low,high); break;
            LPC_CASE(1) LPC_CASE(2) LPC_CASE(3) LPC_CASE(4)
            LPC_CASE(5) LPC_CASE(6) LPC_CASE(7) LPC_CASE(8)
            LPC_CASE(9) LPC_CASE(10) LPC_CASE(11) LPC_CASE(12)
#undef LPC_CASE
            default:
            for (unsigned i = order; i < n; ++i) {
                int64_t sum = 0;
                for (unsigned j = 0; j < order; ++j) sum += (int64_t)coefficients[j] * s[i - j - 1];
                int64_t v = (sum >> shift) + s[i];
                if (v < low || v > high) return false;
                s[i] = (T)v;
            }
            }
            if (!restored) return false;
        }
    }
    if (wasted) for (unsigned i = 0; i < n; ++i) s[i] = (T)((int64_t)s[i] * (int64_t(1) << wasted));
    return b.Good();
}

// Native FLAC streaming decoder. Frames are decoded on demand; a sparse seek
// index (at most one point per two seconds) is built both from an STREAMINFO
// seek table at open and as playback advances. Reads return whole frames only.
// An exact decoder (FlacExact) buffers the frames' integers instead of float.
class FlacDecoder final : public Decoder {
public:
    explicit FlacDecoder(bool exact = false) : exact_(exact) {}
    bool Open(File file, const std::wstring& path);
    size_t Read(float* out, size_t frames) override { return Copy(out, pcm_, 2, frames); }
    size_t ReadExact(int32_t* out, size_t frames) { return Copy(out, exactPcm_, channels_, frames); }
    bool Seek(double seconds) override;
    bool FastSeek() const override {
        const uint64_t gap = uint64_t(rate_) * 3;
        uint64_t previous = 0;
        for (const auto& point : seek_) {
            if (point.sample > previous + gap) return false;
            previous = point.sample;
        }
        return rate_ && total_ <= previous + gap;
    }
    bool CanPreview() const override { return FastSeek() || (!media_.matroska && total_ && file_.Size() > start_); }
    bool PreviewSeek(double& seconds) override;
    double Duration() const override { return (double)(total_-media_.skip) / rate_; }
    unsigned Rate() const override { return rate_; }
    bool Failed() const override { return failed_; }
    // STREAMINFO's MD5 covers the whole stream, so a Matroska track, which may be trimmed, claims none.
    PcmFormat ExactFormat() const {
        PcmFormat f;
        f.rate = rate_; f.channels = channels_; f.bits = bits_; f.frames = total_ - media_.skip;
        if (!media_.matroska) f.md5 = md5_;
        return f;
    }
private:
    struct Point { uint64_t sample, offset; };
    bool Frame();
    template<class T> bool DecodeFrame(const uint8_t* p, size_t bytes, const Header& h, size_t& used);
    // Pull and copy frames from `buffer` (`width` values per frame) until the request is filled
    // or the stream ends.
    template<class S> size_t Copy(S* out, const std::vector<S>& buffer, unsigned width, size_t frames);
    File file_;
    PacketFile media_;
    unsigned rate_ = 0, bits_ = 0, channels_ = 0, maxBlock_ = 0, fixedBlock_ = 0;
    size_t maxFrame_ = 0, buffered_ = 0, cursor_ = 0;
    uint64_t start_ = 0, offset_ = 0, total_ = 0, pos_ = 0, nextSample_ = 0;
    bool exact_, failed_ = false;
    std::array<uint8_t, 16> md5_{};
    std::vector<Point> seek_;
    std::vector<int32_t> samples32_;
    std::vector<int64_t> samples64_;
    std::vector<float> pcm_;
    std::vector<int32_t> exactPcm_;
};

// Without a seek table, find a nearby frame by its authenticated header and use its own sample
// number for the preview. The complete decode still starts at the exact requested position.
bool FlacDecoder::PreviewSeek(double& seconds) {
    if (FastSeek()) return Seek(seconds);
    if (!CanPreview() || !std::isfinite(seconds) || seconds < 0) return false;
    const uint64_t bytes = file_.Size() - start_;
    const double target = std::clamp((seconds * rate_ + media_.skip) / (double)total_, 0.0, 0.999999999);
    const uint64_t offset = start_ + (uint64_t)(bytes * target);
    const size_t length = (size_t)std::min<uint64_t>(65536, file_.Size() - offset);
    const uint8_t* data = file_.View(offset, length);
    if (!data) return false;
    for (size_t i = 0; i + 6 <= length; ++i) {
        Header h;
        if (data[i] != 255 || !ParseHeader(data + i, length - i, rate_, bits_, channels_, h) || h.frames > maxBlock_)
            continue;
        const uint64_t sample = h.variable ? h.number : h.number * fixedBlock_;
        if (sample < media_.skip || sample >= total_) continue;
        offset_ = offset + i;
        pos_ = nextSample_ = sample;
        cursor_ = buffered_ = 0;
        failed_ = false;
        if (!Frame()) return false;  // Frame validates the complete frame, not just the header.
        seconds = (double)(sample - media_.skip) / rate_;
        return true;
    }
    return false;
}

// Open a native FLAC stream or a FLAC track muxed in Matroska. Metadata blocks
// are walked with STREAMINFO first; when the total sample count is absent the
// last frame is located by scanning the tail for a CRC-valid header.
bool FlacDecoder::Open(File file, const std::wstring& path) {
    InitRiceGroups();
    file_ = std::move(file);
    file_.Sequential();
    const uint8_t* p = file_.View(0, 4);
    if (p && U32(p,true)==0x1a45dfa3) {
        media_.file=std::move(file_);
        if (!media_.OpenMatroska(path) || media_.codec!=AudioFlac) return false;
    }
    auto view=[&](uint64_t offset,size_t size)->const uint8_t* {
        if (!media_.matroska) return file_.View(offset,size);
        if (offset>media_.config.size() || size>media_.config.size()-offset) return nullptr;
        return media_.config.data()+offset;
    };
    p=view(0,4);
    if (!p || !Is(p, "fLaC")) return false;
    uint64_t at = 4;
    bool first = true;
    for (;;) {
        p = view(at, 4);
        if (!p) return false;
        unsigned type = p[0] & 127, n = (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
        bool last = (p[0] & 128) != 0;
        at += 4;
        uint64_t metadataSize=media_.matroska?media_.config.size():file_.Size();
        if (at > metadataSize || n > metadataSize - at || type == 127 || (first && type != 0)) return false;
        if (type == 0) {
            if (!first || n != 34) return false;
            p = view(at, n);
            if (!p) return false;
            unsigned minBlock = U16(p, true);
            maxBlock_ = U16(p + 2, true);
            if (minBlock < 16 || maxBlock_ < minBlock) return false;
            maxFrame_ = (size_t)p[7] << 16 | (size_t)p[8] << 8 | p[9];
            uint64_t info = U64(p + 10, true);
            rate_ = (unsigned)(info >> 44);
            channels_ = (unsigned)((info >> 41) & 7) + 1;
            bits_ = (unsigned)((info >> 36) & 31) + 1;
            total_ = info & ((uint64_t(1) << 36) - 1);
            memcpy(md5_.data(), p + 18, 16);
            if (!rate_ || bits_ < 4 || bits_ > 32) return false;
        } else if (type == 3) {
            if (n % 18) return false;
            // At most one entry per roughly two seconds is needed; bound metadata memory.
            for (unsigned i = 0; i < n; i += 18) {
                p = view(at + i, 18);
                if (!p) return false;
                uint64_t sample = U64(p, true), offset = U64(p + 8, true);
                if (sample == UINT64_MAX) continue;
                if ((!seek_.empty() && (sample <= seek_.back().sample || offset <= seek_.back().offset)) ||
                    (total_ && sample >= total_)) return false;
                if (seek_.empty() || sample - seek_.back().sample >= rate_ * 2ull) seek_.push_back({sample, offset});
            }
        }
        first = false;
        at += n;
        if (last) break;
    }
    if (media_.matroska) {
        seek_.clear();media_.frames=0;
        for (size_t i=0;i<media_.packets.size();++i) {
            auto& packet=media_.packets[i];p=media_.Data(i);Header h;
            if (!p || !ParseHeader(p,packet.bytes,rate_,bits_,channels_,h) || h.frames>maxBlock_) return false;
            if (seek_.empty() || media_.frames>=seek_.back().sample+rate_*2ull) seek_.push_back({media_.frames,i});
            packet.start=media_.frames;packet.frames=h.frames;media_.frames+=h.frames;
        }
        if (!media_.Trim(rate_)) return false;
        total_=media_.skip+media_.valid;start_=offset_=0;
        return Frame() && (!media_.skip || Seek(0));
    }
    start_ = offset_ = at;
    if (at >= file_.Size()) return total_ == 0;
    for (auto& point : seek_) { if (point.offset >= file_.Size() - start_) return false; point.offset += start_; }
    if (seek_.empty() || seek_[0].sample != 0) seek_.insert(seek_.begin(), {0, start_});
    // A FLAC frame-size field is 24 bits. Unknown sizes use the full format bound,
    // mapping virtual pages only; physical/private working memory stays block-sized.
    if (!maxFrame_) maxFrame_ = 16777215;
    if (!Frame()) return false;
    if (!total_) {
        // STREAMINFO can omit the total. Locate the last complete frame using its
        // header and CRC rather than decoding the whole song just to open it.
        uint64_t tail = file_.Size() > maxFrame_ ? file_.Size() - maxFrame_ : start_;
        tail = std::max(tail, start_);
        size_t n = (size_t)(file_.Size() - tail);
        p = file_.View(tail, n);
        if (!p) return false;
        for (size_t i = n; i-- > 0;) {
            Header h;
            if (ParseHeader(p + i, n - i, rate_, bits_, channels_, h) &&
                Crc16(p + i, n - i) == 0) {
                total_ = (h.variable ? h.number : h.number * fixedBlock_) + h.frames;
                break;
            }
        }
        if (!total_) return false;
    }
    return true;
}

// Decode one frame body into planar samples, verify the CRC16, undo the stereo
// decorrelation in place, and interleave into pcm_ (or exactPcm_). Side channels
// carry one extra bit of depth beyond the stream's sample size.
template<class T> bool FlacDecoder::DecodeFrame(const uint8_t* p, size_t bytes, const Header& h, size_t& used) {
    auto& data = [&]() -> std::vector<T>& { if constexpr (sizeof(T) == 4) return samples32_; else return samples64_; }();
    data.resize((size_t)h.frames * channels_);
    Bits b(p + h.bytes, bytes - h.bytes);
    for (unsigned c = 0; c < channels_; ++c) {
        unsigned depth = bits_ + ((h.assignment == 8 && c == 1) || (h.assignment == 9 && c == 0) || (h.assignment == 10 && c == 1));
        if (!Subframe(b, data.data() + (size_t)c * h.frames, h.frames, depth)) return false;
    }
    unsigned pad = (unsigned)((8 - b.Position() % 8) % 8);
    if (b.Get(pad) != 0) return false;
    size_t body = h.bytes + b.Position() / 8;
    uint16_t stored = (uint16_t)b.Get(16);
    used = body + 2;
    if (!b.Good() || used > bytes || Crc16(p, body) != stored) return false;
    T* left = data.data();
    T* right = channels_ > 1 ? left + h.frames : left;
    // Subframe already held independent channels to the stream's depth; a restored
    // side-coded pair is checked here.
    if (h.assignment >= 8) {
        const int64_t bound = int64_t(1) << (bits_ - 1);
        for (unsigned i = 0; i < h.frames; ++i) {
            int64_t l = left[i], r = right[i];
            if (h.assignment == 8) r = l - r;
            else if (h.assignment == 9) l += r;
            else {
                int64_t mid = 2 * l + (r & 1);
                l = (mid + r) >> 1; r = (mid - r) >> 1;
            }
            if (l < -bound || l >= bound || r < -bound || r >= bound) return false;
            left[i] = (T)l; right[i] = (T)r;
        }
    }
    if (exact_) {
        exactPcm_.resize((size_t)h.frames * channels_);
        for (unsigned c = 0; c < channels_; ++c)
            for (unsigned i = 0; i < h.frames; ++i) exactPcm_[(size_t)i * channels_ + c] = (int32_t)data[(size_t)c * h.frames + i];
        return true;
    }
    pcm_.resize((size_t)h.frames * 2);
    float scale = std::ldexp(1.0f, 1 - (int)bits_);
    if (channels_ <= 2) {
        unsigned i = 0;
        if constexpr (sizeof(T) == 4) {
            // Up to 24-bit output is exactly representable in float. Convert four planar frames
            // together, then interleave; mono uses the same plane for both channels.
            const __m128 gain = _mm_set1_ps(scale);
            for (; i + 4 <= h.frames; i += 4) {
                const __m128 l = _mm_mul_ps(_mm_cvtepi32_ps(_mm_loadu_si128((const __m128i*)(left + i))), gain);
                const __m128 r = _mm_mul_ps(_mm_cvtepi32_ps(_mm_loadu_si128((const __m128i*)(right + i))), gain);
                _mm_storeu_ps(pcm_.data() + i * 2, _mm_unpacklo_ps(l, r));
                _mm_storeu_ps(pcm_.data() + i * 2 + 4, _mm_unpackhi_ps(l, r));
            }
        }
        for (; i < h.frames; ++i) { pcm_[i * 2] = (float)left[i] * scale; pcm_[i * 2 + 1] = (float)right[i] * scale; }
    } else {
        for (unsigned i = 0; i < h.frames; ++i) {
            float channels[8]{};
            for (unsigned c = 0; c < channels_; ++c) channels[c] = (float)data[(size_t)c * h.frames + i] * scale;
            Mix(channels, channels_, pcm_.data() + i * 2);
        }
    }
    return true;
}

// Decode the frame at the current offset and make it the current buffer,
// rejecting a frame whose first sample does not continue the stream.
bool FlacDecoder::Frame() {
    uint64_t end=media_.matroska?media_.packets.size():file_.Size();
    if (failed_ || (total_ && nextSample_>=total_)) return false;
    if (offset_ >= end) { failed_ = total_ && nextSample_ < total_; return false; }
    size_t n = media_.matroska?media_.packets[(size_t)offset_].bytes:(size_t)std::min<uint64_t>(maxFrame_, file_.Size() - offset_);
    const uint8_t* p = media_.matroska?media_.Data((size_t)offset_):file_.View(offset_, n);
    CORDINAL_CORE_SCOPE;
    Header h;
    if (!p || !ParseHeader(p, n, rate_, bits_, channels_, h) || h.frames > maxBlock_) { failed_ = true; return false; }
    if (!fixedBlock_) fixedBlock_ = h.frames;
    uint64_t firstSample = media_.matroska?media_.packets[(size_t)offset_].start:h.variable ? h.number : h.number * fixedBlock_;
    if (firstSample != nextSample_ || (!media_.matroska && total_ && h.frames > total_ - std::min(total_, firstSample))) {
        failed_ = true; return false;
    }
    size_t used = 0;
    bool ok = bits_ <= 24 ? DecodeFrame<int32_t>(p, n, h, used) : DecodeFrame<int64_t>(p, n, h, used);
    if (!ok) { failed_ = true; return false; }
    // Playback builds a sparse index even when the encoder supplied no seek table.
    if (seek_.empty() || firstSample >= seek_.back().sample + rate_ * 2ull) seek_.push_back({firstSample, offset_});
    buffered_=total_?(size_t)std::min<uint64_t>(h.frames,total_-firstSample):h.frames;
    nextSample_ = firstSample + buffered_;
    offset_ += media_.matroska?1:used;
    cursor_ = 0;
    return true;
}

template<class S> size_t FlacDecoder::Copy(S* out, const std::vector<S>& buffer, unsigned width, size_t frames) {
    size_t done = 0;
    while (done < frames) {
        if (cursor_ == buffered_ && !Frame()) break;
        size_t n = std::min(frames - done, buffered_ - cursor_);
        memcpy(out + done * width, buffer.data() + cursor_ * width, n * width * sizeof(S));
        cursor_ += n; pos_ += n; done += n;
    }
    return done;
}

// Seek to a sample. A target already in the current buffer just moves the
// cursor; otherwise the nearest index point is used and frames are decoded
// forward to the exact sample.
bool FlacDecoder::Seek(double seconds) {
    if (!std::isfinite(seconds)) return false;
    uint64_t target = seconds <= 0 ? media_.skip : seconds >= Duration() ? total_ : media_.skip+(uint64_t)(seconds * rate_);
    uint64_t bufferedStart = nextSample_ - buffered_;
    if (!failed_ && target >= bufferedStart && target <= nextSample_) {
        cursor_ = (size_t)(target - bufferedStart); pos_ = target; return true;
    }
    failed_ = false;
    if (target == total_) { offset_ = media_.matroska?media_.packets.size():file_.Size(); buffered_ = cursor_ = 0; pos_ = nextSample_ = total_; return true; }
    auto point = std::upper_bound(seek_.begin(), seek_.end(), target,
                                  [](uint64_t sample, const Point& p) { return sample < p.sample; });
    if (point != seek_.begin()) --point;
    offset_ = point->offset; nextSample_ = pos_ = point->sample; buffered_ = cursor_ = 0;
    while (pos_ <= target) {
        if (!Frame()) return false;
        if (target < nextSample_) { cursor_ = (size_t)(target - pos_); pos_ = target; return true; }
        pos_ = nextSample_; cursor_ = buffered_;
    }
    return false;
}
} // namespace

std::unique_ptr<Decoder> OpenFlacDecoder(const std::wstring& path) {
    File file;
    auto d = std::make_unique<FlacDecoder>();
    if (!file.Open(path) || !d->Open(std::move(file), path)) return nullptr;
    return d;
}

std::unique_ptr<Decoder> OpenFlacDecoder(const uint8_t* data, size_t size) {
    if (!data || size < 4 || !Is(data, "fLaC")) return nullptr;
    File file;
    file.Borrow(data, size);
    auto d = std::make_unique<FlacDecoder>();
    if (!d->Open(std::move(file), {})) return nullptr;
    return d;
}

namespace {
// Exact reading through the same frame decoder and its CRC checks; PcmReader adds the
// end-of-stream length and STREAMINFO MD5 checks.
class FlacExact final : public PcmReader {
public:
    bool Open(File file, const std::wstring& path) {
        if (!decoder_.Open(std::move(file), path)) return false;
        format_ = decoder_.ExactFormat();
        return true;
    }
private:
    size_t Next(int32_t* out, size_t frames) override {
        size_t n = decoder_.ReadExact(out, frames);
        damaged_ = decoder_.Failed();
        return n;
    }
    FlacDecoder decoder_{true};
};
} // namespace

std::unique_ptr<PcmReader> OpenFlacExact(const std::wstring& path) {
    File file;
    auto r = std::make_unique<FlacExact>();
    if (!file.Open(path) || !r->Open(std::move(file), path)) return nullptr;
    return r;
}
