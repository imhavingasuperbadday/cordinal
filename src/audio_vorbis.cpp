// Cordinal Ogg/Vorbis decoder, written from the Xiph Vorbis I specification.
// Assembles packets from Ogg pages (or takes them verbatim from Matroska),
// decodes floor/residue/coupling, and overlap-adds inverse MDCT blocks; a sparse
// packet index is kept for seeking. One packet yields half a block after overlap.
#include "native_audio.h"
#include "audio_file.h"
#include "audio_packets.h"
#include "audio_mdct.h"
#include <array>
#include <bit>
#include <cmath>
#include <numeric>
#include <span>

namespace {
using namespace nativeaudio;
constexpr double pi = 3.14159265358979323846;
unsigned Ilog(uint32_t x) { return std::bit_width(x); }

// LSB-first bit cursor over one assembled packet. Dropping past the end poisons
// the cursor so a truncated packet fails as a whole rather than yielding zeros.
class PacketBits {
public:
    explicit PacketBits(const std::vector<uint8_t>& p, size_t skip = 0) : p_(p.data()+skip), end_(p.data()+p.size()) {}
    uint32_t Peek(unsigned n) {
        if (live_ < n) Refill();
        return (uint32_t)cache_ & (n == 32 ? UINT32_MAX : (1u << n) - 1);
    }
    uint32_t Get(unsigned n) {
        uint32_t v = Peek(n);
        Drop(n);
        return v;
    }
    __declspec(noinline) uint32_t Read(unsigned n) { return Get(n); } // Compact setup/header parsing.
    void Drop(unsigned n) {
        if (live_ < n) { ok_ = false; cache_ = 0; live_ = 0; }
        else { cache_ >>= n; live_ -= n; }
    }
    bool Good() const { return ok_; }
private:
    // Top the cache up to 56..63 bits with one 8-byte load where at least 8 bytes remain:
    // whole bytes are counted in, and the rest of the word is the same data loaded again
    // next time. Bits above live_ are those next bits or zero, never anything else.
    __forceinline void Refill() {
        if (end_ - p_ >= 8) {
            uint64_t w; memcpy(&w,p_,8);
            cache_ |= w << live_; p_ += (63 - live_) >> 3; live_ |= 56;
        } else while (live_ <= 56 && p_ < end_) { cache_ |= (uint64_t)*p_++ << live_; live_ += 8; }
    }
    const uint8_t *p_, *end_;
    uint64_t cache_ = 0;
    unsigned live_ = 0;
    bool ok_ = true;
};

// A codebook: either a single symbol (single >= 0) or a Huffman tree with a
// 9-bit direct lookup. fast[] packs the consumed length in the high byte and
// entry+1 in the low 24 bits; zero falls through to the tree.
struct Book {
    struct Node { int child[2] = {-1,-1}; };
    unsigned dimensions = 0, entries = 0;
    int single = -1;
    std::vector<float> values;
    std::vector<Node> tree;
    // Zero means tree fallback; otherwise length occupies the high byte and
    // entry+1 the low 24 bits. Compact tables fit twice as many books in cache.
    static constexpr unsigned kFastBits = 9;
    std::array<uint32_t, 1u << kFastBits> fast{};
    bool Setup(PacketBits& b, size_t& budget);
    __forceinline int Scalar(PacketBits& b) const {
        if (single >= 0) { b.Get(1); return b.Good() ? single : -1; }
        auto f = fast[b.Peek(kFastBits)];
        if (f >> 24) { b.Drop(f >> 24); return b.Good() ? (int)(f & 0xFFFFFF)-1 : -1; }
        int node = 0;
        for (unsigned i = 0; i < 32; ++i) {
            int v = tree[(size_t)node].child[b.Get(1)];
            if (!b.Good() || v == -1) return -1;
            if (v < -1) return -v - 2;
            node = v;
        }
        return -1;
    }
    __forceinline const float* Vector(PacketBits& b) const {
        int entry = Scalar(b);
        return entry < 0 || values.empty() ? nullptr : values.data() + (size_t)entry * dimensions;
    }
};

// The windowed time block from the inverse MDCT's DCT-IV half (q = n/4): dct[j+q] for j < q,
// -dct[3q-1-j] up to 3q, then -dct[j-3q]. The left slope (leftN long) is centred on q and the
// right one on 3q; the block is zero outside them and unscaled between them. One pass replaces
// reordering, windowing both slopes and clearing the ends separately, with the same products.
void Window(const float* dct,const float* left,unsigned leftN,const float* right,unsigned rightN,float* time,unsigned n) {
    const unsigned q = n/4, ls = q-leftN/2, rs = 3*q-rightN/2;
    const __m128 sign = _mm_set1_ps(-0.0f);
    auto backward = [&](unsigned j) { __m128 v = _mm_loadu_ps(dct+3*q-4-j); return _mm_xor_ps(_mm_shuffle_ps(v,v,0x1B),sign); };
    auto mirrored = [&](unsigned i) { __m128 w = _mm_loadu_ps(right+rightN-4-i); return _mm_shuffle_ps(w,w,0x1B); };
    memset(time,0,ls*sizeof(float));
    for (unsigned j = ls; j < q; j += 4) _mm_storeu_ps(time+j,_mm_mul_ps(_mm_loadu_ps(dct+j+q),_mm_loadu_ps(left+j-ls)));
    for (unsigned j = q; j < q+leftN/2; j += 4) _mm_storeu_ps(time+j,_mm_mul_ps(backward(j),_mm_loadu_ps(left+j-ls)));
    for (unsigned j = q+leftN/2; j < rs; j += 4) _mm_storeu_ps(time+j,backward(j));
    for (unsigned j = rs; j < 3*q; j += 4) _mm_storeu_ps(time+j,_mm_mul_ps(backward(j),mirrored(j-rs)));
    for (unsigned j = 3*q; j < 3*q+rightN/2; j += 4)
        _mm_storeu_ps(time+j,_mm_mul_ps(_mm_xor_ps(_mm_loadu_ps(dct+j-3*q),sign),mirrored(j-rs)));
    memset(time+3*q+rightN/2,0,(n-3*q-rightN/2)*sizeof(float));
}

// Vorbis float32: sign, 10-bit biased exponent, 21-bit mantissa.
float Unpack(uint32_t v) {
    double mantissa = v & 0x1fffff;
    if (v >> 31) mantissa = -mantissa;
    return (float)std::ldexp(mantissa, (int)((v >> 21) & 1023) - 788);
}

// Parse a codebook's Huffman lengths and multiplicands. `budget` bounds the total
// derived-table memory across every codebook in the setup header, so a hostile
// header cannot make Open allocate without limit.
bool Book::Setup(PacketBits& b, size_t& budget) {
    if (b.Read(24) != 0x564342) return false;
    dimensions = b.Read(16); entries = b.Read(24);
    if (!dimensions || !entries || entries > 1048576 || dimensions > 65535 || entries > budget / 16) return false;
    budget -= entries * 16;
    std::vector<uint8_t> lengths(entries);
    if (b.Read(1)) {
        unsigned length = b.Read(5) + 1, at = 0;
        while (at < entries) {
            if (length > 32) return false;
            unsigned n = b.Read(Ilog(entries - at));
            if (n > entries - at || !b.Good()) return false;
            std::fill_n(lengths.data()+at, n, (uint8_t)length);
            at += n; ++length;
        }
    } else {
        bool sparse = b.Read(1) != 0;
        for (auto& length : lengths) if (!sparse || b.Read(1)) length = (uint8_t)(b.Read(5) + 1);
    }
    if (!b.Good()) return false;
    unsigned used = 0;
    for (unsigned i = 0; i < entries; ++i) if (lengths[i]) { ++used; single = (int)i; }
    if (used == 1) { if (lengths[(size_t)single] != 1) return false; }
    else {
        single = -1;
        if (!used) return false;
        tree.emplace_back();
        // A leftmost-available prefix allocator respects entry order even when
        // codeword lengths are unordered (ordinary canonical Huffman does not).
        std::vector<uint32_t> free[33];
        free[0].push_back(0);
        auto greater = [](uint32_t a, uint32_t c) { return a > c; };
        for (unsigned entry = 0; entry < entries; ++entry) {
            unsigned length = lengths[entry];
            if (!length) continue;
            uint32_t prefix = UINT32_MAX;
            unsigned depth = 33;
            for (unsigned j = 0; j <= length; ++j)
                if (!free[j].empty() && (depth == 33 || free[j][0] < prefix)) { prefix = free[j][0]; depth = j; }
            if (depth == 33) return false;
            std::pop_heap(free[depth].begin(), free[depth].end(), greater);
            free[depth].pop_back();
            while (depth < length) {
                uint32_t right = prefix | (uint32_t(1) << (31-depth));
                ++depth;
                free[depth].push_back(right);
                std::push_heap(free[depth].begin(), free[depth].end(), greater);
            }
            uint64_t code = prefix >> (32-length);
            int node = 0;
            uint32_t reversed = 0;
            for (unsigned j = 0; j < length; ++j) {
                unsigned bit = (unsigned)((code >> (length-j-1)) & 1);
                reversed |= bit << j;
                int child = tree[(size_t)node].child[bit];
                if (j + 1 == length) {
                    if (child != -1) return false;
                    tree[(size_t)node].child[bit] = -(int)entry - 2;
                } else {
                    if (child < -1) return false;
                    if (child == -1) { child = (int)tree.size(); tree[(size_t)node].child[bit] = child; tree.emplace_back(); }
                    node = child;
                }
            }
            if (length <= kFastBits) for (unsigned i = reversed; i < fast.size(); i += 1u << length) fast[i] = (length << 24) | (entry+1);
        }
        for (const auto& node : tree) if (node.child[0] == -1 || node.child[1] == -1) return false;
    }
    unsigned lookup = b.Read(4);
    if (lookup > 2) return false;
    if (lookup) {
        float minimum = Unpack(b.Read(32)), delta = Unpack(b.Read(32));
        unsigned width = b.Read(4) + 1;
        bool sequence = b.Read(1) != 0;
        uint64_t count = (uint64_t)entries * dimensions;
        if (count > budget / sizeof(float) || !std::isfinite(minimum) || !std::isfinite(delta)) return false;
        budget -= (size_t)count * sizeof(float);
        unsigned multiplicands = (unsigned)count;
        if (lookup == 1) {
            multiplicands = (unsigned)std::pow((double)entries, 1.0 / dimensions);
            auto powerFits = [&](unsigned v) {
                uint64_t value = 1;
                for (unsigned j = 0; j < dimensions; ++j) { if (value > entries / v) return false; value *= v; }
                return true;
            };
            multiplicands = std::max(1u, multiplicands);
            while (powerFits(multiplicands + 1)) ++multiplicands;
            while (!powerFits(multiplicands)) --multiplicands;
        }
        std::vector<unsigned> mult(multiplicands);
        for (auto& v : mult) v = b.Read(width);
        values.resize((size_t)count);
        for (unsigned i = 0; i < entries; ++i) {
            float last = 0;
            unsigned index = i;
            for (unsigned j = 0; j < dimensions; ++j) {
                unsigned m = lookup == 1 ? mult[index % multiplicands] : mult[(size_t)i * dimensions+j];
                float v = m * delta + minimum + last;
                if (!std::isfinite(v)) return false;
                values[(size_t)i * dimensions+j] = v;
                if (sequence) last = v;
                if (lookup == 1) index /= multiplicands;
            }
        }
    }
    return b.Good();
}

// Spectral floor. Type 0 is line-spectral-pair plus amplitude; type 1 is a
// piecewise-linear curve between explicitly coded positions.
struct Floor {
    unsigned type = 0, multiplier = 0;
    struct Class { unsigned dimensions = 0, subclasses = 0, master = 0; int books[8]{}; };
    std::vector<unsigned> partitions, x, sorted, low, high;
    Class classes[16]{};
    unsigned order = 0, rate = 0, bark = 0, amplitudeBits = 0, amplitudeOffset = 0;
    std::vector<unsigned> books;
    bool Setup(PacketBits& b, unsigned bookCount);
    bool Decode(PacketBits& b, const std::vector<Book>& codebooks, float* curve, unsigned n) const;
};
// Parse floor parameters, validating book indices and the monotonic position
// ordering type 1 requires.
bool Floor::Setup(PacketBits& b, unsigned bookCount) {
    type = b.Read(16);
    if (type == 0) {
        order = b.Read(8); rate = b.Read(16); bark = b.Read(16);
        amplitudeBits = b.Read(6); amplitudeOffset = b.Read(8);
        books.resize(b.Read(4)+1);
        if (!order || !rate || !bark || amplitudeBits > 32) return false;
        for (auto& v : books) { v = b.Read(8); if (v >= bookCount) return false; }
    } else if (type == 1) {
        partitions.resize(b.Read(5));
        int maxClass = -1;
        for (auto& v : partitions) { v = b.Read(4); maxClass = std::max(maxClass, (int)v); }
        for (int i = 0; i <= maxClass; ++i) {
            auto& c = classes[i];
            c.dimensions = b.Read(3) + 1; c.subclasses = b.Read(2);
            if (c.subclasses) { c.master = b.Read(8); if (c.master >= bookCount) return false; }
            for (unsigned j = 0; j < (1u << c.subclasses); ++j) {
                c.books[j] = (int)b.Read(8)-1;
                if (c.books[j] >= (int)bookCount) return false;
            }
        }
        multiplier = b.Read(2) + 1;
        unsigned range = b.Read(4);
        x = {0, 1u << range};
        for (unsigned part : partitions) for (unsigned j = 0; j < classes[part].dimensions; ++j) x.push_back(b.Read(range));
        if (x.size() > 65) return false;
        sorted.resize(x.size()); std::iota(sorted.begin(), sorted.end(), 0);
        std::sort(sorted.begin(), sorted.end(), [&](unsigned a, unsigned c) { return x[a] < x[c]; });
        for (size_t i = 1; i < sorted.size(); ++i) if (x[sorted[i]] == x[sorted[i-1]]) return false;
        low.resize(x.size()); high.resize(x.size());
        for (unsigned i = 2; i < x.size(); ++i) {
            low[i] = 0; high[i] = 1;
            for (unsigned j = 0; j < i; ++j) {
                if (x[j] < x[i] && x[j] > x[low[i]]) low[i] = j;
                if (x[j] > x[i] && x[j] < x[high[i]]) high[i] = j;
            }
        }
    } else return false;
    return b.Good();
}

// 256-entry dB lookup: index 255 is unity (0 dB), lower indices attenuate.
const std::array<float, 256>& DbTable() {
    static const auto table = [] {
        std::array<float,256> values{};
        for (unsigned i = 0; i < 256; ++i) values[i] = (float)std::exp((int(i)-255) * 0.062961308689135045);
        return values;
    }();
    return table;
}
void Fill(float* from, float* to, float value) { for (; from < to; ++from) *from = value; }
// Integer line draw between two (x,y) points; y indexes the dB table.
void Render(unsigned x0, int y0, unsigned x1, int y1, float* curve, unsigned n) {
    int delta = y1-y0, dx = (int)(x1-x0), base = delta/dx, step = base + (delta < 0 ? -1 : 1);
    int remainder = std::abs(delta) - std::abs(base)*dx, error = 0, y = y0;
    auto& db = DbTable();
    if (!base) {
        // A gentle slope moves y one step at a time: the line is y0 + floor(k*|dy|/dx) steps
        // of sign(dy) at k = x-x0. Fill each run of equal y instead of stepping every bin;
        // a run is dx/|dy| bins, one longer when the remainder carries.
        unsigned end = std::min(x1,n), x = x0;
        if (!remainder) { Fill(curve+std::min(x0,end),curve+end,db[(size_t)std::clamp(y,0,255)]); return; }
        unsigned whole = (unsigned)dx/(unsigned)remainder, extra = (unsigned)dx%(unsigned)remainder, carry = (unsigned)remainder-1;
        while (x < end) {
            carry += extra;
            unsigned length = whole;
            if (carry >= (unsigned)remainder) { carry -= remainder; ++length; }
            // Whole vectors may run up to three bins into the next runs, which overwrite them.
            float value = db[(size_t)std::clamp(y,0,255)];
            if (x+((length+3)&~3u) <= end) for (unsigned k = 0; k < length; k += 4) _mm_storeu_ps(curve+x+k,_mm_set1_ps(value));
            else Fill(curve+x,curve+std::min(x+length,end),value);
            x += length; y += step;
        }
        return;
    }
    for (unsigned x = x0; x < std::min(x1,n); ++x) {
        curve[x] = db[(size_t)std::clamp(y,0,255)];
        error += remainder;
        if (error >= dx) { error -= dx; y += step; } else y += base;
    }
}
// Floor 0 evaluates the LSP polynomial in bark space, one value per spectral bin
// and reusing the previous bin while the mapped band is unchanged. Floor 1
// reconstructs the point set (including implicit interpolation values) and draws
// it. Returns false on a missing floor or a truncated packet.
bool Floor::Decode(PacketBits& b, const std::vector<Book>& cb, float* curve, unsigned n) const {
    if (type == 0) {
        uint32_t amplitude = b.Get(amplitudeBits);
        if (!amplitude) return false;
        unsigned number = b.Get(Ilog((uint32_t)books.size()));
        if (number >= books.size()) return false;
        const Book& book = cb[books[number]];
        std::vector<double> lsp(order);
        unsigned at = 0; float last = 0;
        while (at < order) {
            const float* v = book.Vector(b);
            if (!v) return false;
            for (unsigned j = 0; j < book.dimensions && at < order; ++j) lsp[at++] = std::cos(v[j] + last);
            last += v[book.dimensions-1];
        }
        auto toBark = [](double f) { return 13.1*std::atan(0.00074*f) + 2.24*std::atan(0.0000000185*f*f) + 0.0001*f; };
        double factor = bark / toBark(rate*0.5);
        double amp = amplitude * (double)amplitudeOffset / (amplitudeBits == 32 ? UINT32_MAX : (1u << amplitudeBits)-1);
        int previous = -1; float value = 0;
        for (unsigned i = 0; i < n; ++i) {
            int map = std::min((int)bark-1, (int)(toBark(rate*(double)i/(2*n))*factor));
            if (map != previous) {
                double w = std::cos(pi*map/bark), p = 1, q = 1;
                for (unsigned j = 0; j < order; j += 2) q *= 4*(lsp[j]-w)*(lsp[j]-w);
                for (unsigned j = 1; j < order; j += 2) p *= 4*(lsp[j]-w)*(lsp[j]-w);
                if (order & 1) { p *= 1-w*w; q *= 0.25; }
                else { p *= (1-w)*0.5; q *= (1+w)*0.5; }
                value = (float)std::exp(0.11512925*(amp/std::sqrt(p+q)-amplitudeOffset));
                if (!std::isfinite(value)) return false;
                previous = map;
            }
            curve[i] = value;
        }
        return b.Good();
    }
    if (!b.Get(1)) return false;
    constexpr int ranges[] = {256,128,86,64};
    int range = ranges[multiplier-1], y[65]{};
    bool active[65]{};
    y[0] = (int)b.Get(Ilog(range-1)); y[1] = (int)b.Get(Ilog(range-1));
    unsigned at = 2;
    for (unsigned part : partitions) {
        const auto& c = classes[part];
        int value = c.subclasses ? cb[c.master].Scalar(b) : 0;
        if (value < 0) return false;
        for (unsigned j = 0; j < c.dimensions; ++j) {
            int book = c.books[(unsigned)value & ((1u << c.subclasses)-1)];
            value >>= c.subclasses;
            y[at] = book < 0 ? 0 : cb[(size_t)book].Scalar(b);
            if (y[at++] < 0) return false;
        }
    }
    active[0] = active[1] = true;
    for (unsigned i = 2; i < x.size(); ++i) {
        unsigned lo = low[i], hi = high[i];
        int dy = y[hi]-y[lo], off = std::abs(dy)*(int)(x[i]-x[lo])/(int)(x[hi]-x[lo]);
        int predicted = y[lo] + (dy < 0 ? -off : off), value = y[i];
        int highroom = range-predicted, lowroom = predicted, room = 2*std::min(highroom, lowroom);
        if (value) {
            active[lo] = active[hi] = active[i] = true;
            if (value >= room) y[i] = highroom > lowroom ? value-lowroom+predicted : predicted-value+highroom-1;
            else y[i] = predicted + ((value & 1) ? -(value+1)/2 : value/2);
        } else y[i] = predicted;
        y[i] = std::clamp(y[i],0,range-1);
    }
    unsigned lx = 0;
    int ly = y[0]*(int)multiplier;
    for (unsigned index : sorted) if (index && active[index]) {
        int hy = y[index]*(int)multiplier;
        Render(lx,ly,x[index],hy,curve,n);
        lx = x[index]; ly = hy;
    }
    if (lx < n) Render(lx,ly,n,ly,curve,n);
    return b.Good();
}

// Residue coding parameters: partition size, class/VQ books and the per-class
// codebooks used in each of the eight decode passes. Mapping ties channels to
// floor and residue types plus channel coupling; a mode selects a mapping and
// block size.
struct Residue {
    unsigned type = 0, begin = 0, end = 0, partition = 0, classes = 0, classbook = 0;
    int books[64][8]{};
    // Codewords per partition for each class and pass; 0 when the book's dimension does not
    // divide the partition, which fails the packet that uses it.
    unsigned steps[64][8]{};
    // Passes after the last one any class codes are empty; pass 0 always reads the classes.
    unsigned passes = 1;
    // Each classbook entry split into its base-`classes` digits, most significant first,
    // when the table stays small; otherwise the digits are divided out per codeword.
    std::vector<uint8_t> digits;
};
struct Mapping {
    struct Couple { unsigned magnitude, angle; };
    std::vector<Couple> couples;
    unsigned submaps = 0, mux[8]{}, floor[16]{}, residue[16]{};
};
struct Mode { unsigned mapping = 0; bool large = false; };

// Streaming decoder. Header packets are parsed once; audio packets are decoded on
// demand. A packet emits the middle half of its overlap window, so a block is
// buffered and drained across consecutive Read calls.
class VorbisDecoder final : public Decoder {
public:
    bool Open(const std::wstring& path);
    size_t Read(float* out, size_t frames) override;
    bool Seek(double seconds) override;
    bool CanPreview() const override { return !media_.matroska && total_ && file_.Size() > dataOffset_; }
    bool PreviewSeek(double& seconds) override;
    double Duration() const override { return (double)(total_-media_.skip)/rate_; }
    unsigned Rate() const override { return rate_; }
    bool Failed() const override { return failed_; }
private:
    bool Packet();
    bool Setup();
    bool Audio();
    bool DecodeResidue(PacketBits& b, const Residue& r, std::span<const unsigned> bundle, const bool* nonzero, unsigned n);
    File file_;
    PacketFile media_;
    std::array<std::vector<uint8_t>,3> headers_;
    unsigned headerPacket_=0;
    uint64_t offset_ = 0, dataOffset_ = 0, total_ = 0, produced_ = 0, pos_ = 0;
    uint32_t serial_ = 0, sequence_ = 0;
    unsigned segments_ = 0, segment_ = 0;
    uint64_t payload_ = 0, pageEnd_ = 0, granule_ = UINT64_MAX;
    uint8_t laces_[255]{};
    bool seen_ = false, ended_ = false, failed_ = false, previewing_ = false;
    std::vector<uint8_t> packet_,classes_;
    unsigned rate_ = 0, channels_ = 0, block_[2]{}, previous_ = 0;
    std::vector<Book> books_;
    std::vector<Floor> floors_;
    std::vector<Residue> residues_;
    std::vector<Mapping> mappings_;
    std::vector<Mode> modes_;
    Mdct mdct_[2];
    // time_ holds two blocks per channel, used in turn: the previous block's second half is
    // the tail the current one overlaps, so it is never copied out.
    std::vector<float> windows_[2], spectrum_, curves_, time_, dct_, pcm_;
    unsigned current_ = 0;
    size_t cursor_ = 0, buffered_ = 0;
    struct Point {
        uint64_t sample, offset, payload;
        uint32_t sequence;
        unsigned segments, segment, previous;
        bool ended;
        std::array<uint8_t,255> laces;
    };
    std::vector<Point> seek_;
};

// Start a preview after a complete Ogg page. Decoding that page first rebuilds the overlap tail;
// its granule gives the actual position of the audio that follows it.
bool VorbisDecoder::PreviewSeek(double& seconds) {
    if (!CanPreview() || !std::isfinite(seconds) || seconds < 0) return false;
    if (seek_.empty()) {
        Point point{produced_, offset_, payload_, sequence_, segments_, segment_, previous_, ended_, {}};
        memcpy(point.laces.data(), laces_, sizeof laces_);
        seek_.push_back(point);
    }
    const uint64_t bytes = file_.Size() - dataOffset_;
    const double target = std::clamp((seconds * rate_ + media_.skip) / (double)total_, 0.0, 0.999999999);
    const uint64_t from = dataOffset_ + (uint64_t)(bytes * target);
    const size_t length = (size_t)std::min<uint64_t>(65536, file_.Size() - from);
    const uint8_t* data = file_.View(from, length);
    if (!data) return false;
    for (size_t i = 0; i + 27 <= length; ++i) {
        const uint8_t* page = data + i;
        if (!Is(page, "OggS") || page[4] || (page[5] & 5) || U32(page + 14) != serial_) continue;
        unsigned count = page[26];
        if (!count || i + 27u + count > length || page[27 + count - 1] == 255) continue;
        size_t pageSize = 27 + count;
        for (unsigned j = 0; j < count; ++j) pageSize += page[27 + j];
        uint64_t granule = U64(page + 6);
        if (pageSize > length - i || granule == UINT64_MAX || granule < media_.skip || granule >= total_ ||
            OggCrc(page, pageSize) != U32(page + 22)) continue;
        const uint32_t sequence = U32(page + 18);
        const uint64_t end = from + i + pageSize;
        offset_ = from + i;
        sequence_ = sequence - 1;
        seen_ = true;
        ended_ = failed_ = false;
        segments_ = segment_ = previous_ = 0;
        produced_ = pos_ = 0;
        cursor_ = buffered_ = 0;
        previewing_ = true;
        std::fill(time_.begin(), time_.end(), 0.0f);
        while (offset_ < end || segment_ < segments_) if (!Audio()) return false;
        produced_ = pos_ = granule;
        cursor_ = buffered_ = 0;
        seconds = (double)(granule - media_.skip) / rate_;
        return true;
    }
    return false;
}

// Assemble the next packet. In Ogg, a lacing value of 255 continues a packet and
// the first value below 255 ends it; pages are validated for sequence and CRC,
// and the final granule position gives the stream length. Matroska supplies
// whole packets instead.
bool VorbisDecoder::Packet() {
    packet_.clear();
    if (media_.matroska) {
        if (headerPacket_<3) { packet_=headers_[headerPacket_++];return true; }
        if (offset_>=media_.packets.size()) return false;
        size_t index=(size_t)offset_++;const auto& packet=media_.packets[index];const uint8_t* p=media_.Data(index);
        if (!p) return false;packet_.assign(p,p+packet.bytes);ended_=offset_==media_.packets.size();return true;
    }
    bool continuing = false;
    for (;;) {
        if (segment_ == segments_) {
            if (ended_ || offset_ >= file_.Size()) return false;
            const uint8_t* p = file_.View(offset_,27);
            if (!p || !Is(p,"OggS") || p[4] || (p[5] & ~7)) return false;
            unsigned flags = p[5], count = p[26];
            uint32_t serial = U32(p+14), seq = U32(p+18), crc = U32(p+22);
            if (seen_ && (serial != serial_ || seq != sequence_ + 1)) return false;
            if (!seen_ && !(flags & 2)) return false;
            if ((bool)(flags & 1) != continuing) return false;
            granule_ = U64(p+6);
            p = file_.View(offset_,27+count);
            if (!p) return false;
            memcpy(laces_,p+27,count);
            unsigned bytes = 0;
            for (unsigned i = 0; i < count; ++i) bytes += laces_[i];
            size_t n = 27+count+bytes;
            p = file_.View(offset_,n);
            if (!p) return false;
            if (OggCrc(p,n) != crc) return false;
            serial_ = serial; sequence_ = seq; seen_ = true; ended_ = (flags & 4) != 0;
            segments_ = count; segment_ = 0; payload_ = offset_+27+count;
            pageEnd_ = offset_+n; offset_ = pageEnd_;
            if (!count) continue;
        }
        unsigned n = laces_[segment_++];
        const uint8_t* p = file_.View(payload_,n);
        if (!p || packet_.size() + n > 16777216) return false;
        packet_.insert(packet_.end(),p,p+n); payload_ += n;
        if (n < 255) return true;
        continuing = true;
    }
}

// Parse the setup header: codebooks, time bases (reserved), floors, residues,
// mappings and modes. See Book::Setup for the `budget` memory bound.
bool VorbisDecoder::Setup() {
    PacketBits b(packet_,7);
    size_t budget = 32*1024*1024;
    books_.resize(b.Read(8)+1);
    for (auto& book : books_) if (!book.Setup(b,budget)) return false;
    unsigned times = b.Read(6)+1;
    for (unsigned i = 0; i < times; ++i) if (b.Read(16)) return false;
    floors_.resize(b.Read(6)+1);
    for (auto& f : floors_) if (!f.Setup(b,(unsigned)books_.size())) return false;
    residues_.resize(b.Read(6)+1);
    for (auto& r : residues_) {
        r.type = b.Read(16); r.begin = b.Read(24); r.end = b.Read(24); r.partition = b.Read(24)+1;
        r.classes = b.Read(6)+1; r.classbook = b.Read(8);
        if (r.type > 2 || r.begin > r.end || r.classbook >= books_.size()) return false;
        unsigned cascade[64]{};
        for (unsigned i = 0; i < r.classes; ++i) { unsigned lo = b.Read(3); cascade[i] = lo | (b.Read(1) ? b.Read(5) << 3 : 0); }
        for (unsigned i = 0; i < r.classes; ++i) for (unsigned j = 0; j < 8; ++j) {
            int value = (cascade[i] & (1u << j)) ? (int)b.Read(8) : -1;
            if (value >= (int)books_.size() || (value >= 0 && books_[(size_t)value].values.empty())) return false;
            r.books[i][j] = value;
            if (value >= 0) r.passes = std::max(r.passes,j+1);
            if (value >= 0 && !(r.partition % books_[(size_t)value].dimensions)) r.steps[i][j] = r.partition/books_[(size_t)value].dimensions;
        }
        const Book& classbook = books_[r.classbook];
        if ((size_t)classbook.entries*classbook.dimensions <= 65536) {
            r.digits.resize((size_t)classbook.entries*classbook.dimensions);
            for (unsigned e = 0; e < classbook.entries; ++e)
                for (unsigned i = classbook.dimensions, value = e; i-- > 0; value /= r.classes)
                    r.digits[(size_t)e*classbook.dimensions+i] = (uint8_t)(value % r.classes);
        }
    }
    mappings_.resize(b.Read(6)+1);
    for (auto& m : mappings_) {
        if (b.Read(16)) return false;
        m.submaps = b.Read(1) ? b.Read(4)+1 : 1;
        if (b.Read(1)) {
            m.couples.resize(b.Read(8)+1);
            for (auto& pair : m.couples) {
                pair.magnitude = b.Read(Ilog(channels_-1)); pair.angle = b.Read(Ilog(channels_-1));
                if (pair.magnitude == pair.angle || pair.magnitude >= channels_ || pair.angle >= channels_) return false;
            }
        }
        if (b.Read(2)) return false;
        if (m.submaps > 1) for (unsigned c = 0; c < channels_; ++c) { m.mux[c] = b.Read(4); if (m.mux[c] >= m.submaps) return false; }
        for (unsigned j = 0; j < m.submaps; ++j) {
            b.Read(8); m.floor[j] = b.Read(8); m.residue[j] = b.Read(8);
            if (m.floor[j] >= floors_.size() || m.residue[j] >= residues_.size()) return false;
        }
    }
    modes_.resize(b.Read(6)+1);
    for (auto& mode : modes_) {
        mode.large = b.Read(1) != 0;
        if (b.Read(16) || b.Read(16)) return false;
        mode.mapping = b.Read(8);
        if (mode.mapping >= mappings_.size()) return false;
    }
    return b.Read(1) == 1 && b.Good();
}

// Validate the three header packets, build the window/MDCT tables and locate the
// end-of-stream granule for the duration. Matroska carries the headers in
// CodecPrivate instead of as the first packets.
bool VorbisDecoder::Open(const std::wstring& path) {
    if (!file_.Open(path)) return false;
    file_.Sequential();
    const uint8_t* signature=file_.View(0,4);
    if (signature && U32(signature,true)==0x1a45dfa3) {
        media_.file=std::move(file_);
        if (!media_.OpenMatroska(path) || media_.codec!=AudioVorbis || media_.config.empty() || media_.config[0]!=2) return false;
        const auto& config=media_.config;size_t at=1,length[3]{};
        for (unsigned i=0;i<2;++i) {
            unsigned part;do { if (at==config.size()) return false;part=config[at++];length[i]+=part; } while (part==255);
        }
        if (length[0]>config.size()-at || length[1]>config.size()-at-length[0]) return false;
        length[2]=config.size()-at-length[0]-length[1];
        for (unsigned i=0;i<3;++i) { headers_[i].assign(config.begin()+at,config.begin()+at+length[i]);at+=length[i]; }
    }
    if (!Packet() || packet_.size() < 30 || packet_[0] != 1 || memcmp(packet_.data()+1,"vorbis",6)) return false;
    const uint8_t* p = packet_.data();
    if (U32(p+7)) return false;
    channels_ = p[11]; rate_ = U32(p+12);
    unsigned smallExp = p[28] & 15, largeExp = p[28] >> 4;
    if (!channels_ || channels_ > 8 || !rate_ || rate_ > 1048575 || smallExp < 6 || largeExp < smallExp || largeExp > 13 || !(p[29] & 1)) return false;
    block_[0] = 1u << smallExp; block_[1] = 1u << largeExp;
    if (!Packet() || packet_.size() < 7 || packet_[0] != 3 || memcmp(packet_.data()+1,"vorbis",6)) return false;
    if (!Packet() || packet_.size() < 7 || packet_[0] != 5 || memcmp(packet_.data()+1,"vorbis",6) || !Setup()) return false;
    // Headers end on a page boundary in the Vorbis Ogg mapping.
    if (segment_ != segments_) return false;
    dataOffset_ = offset_;
    if (media_.matroska) {
        unsigned previous=0;
        for (size_t i=0;i<media_.packets.size();++i) {
            auto& packet=media_.packets[i];const uint8_t* data=media_.Data(i);if (!data) return false;
            std::vector<uint8_t> bytes(data,data+packet.bytes);PacketBits bits(bytes);
            if (bits.Get(1)) return false;unsigned mode=bits.Read(Ilog((unsigned)modes_.size()-1));
            if (!bits.Good() || mode>=modes_.size()) return false;
            unsigned n=block_[modes_[mode].large?1:0];if (!i) media_.consumedDelay=n/2;
            packet.start=media_.frames;packet.frames=previous?(previous+n)/4:0;
            media_.frames+=packet.frames;previous=n;
        }
        if (!media_.Trim(rate_)) return false;total_=media_.skip+media_.valid;
    } else {
    // The last page is at most 65,307 bytes, regardless of packet size.
    uint64_t from = file_.Size() > 65307 ? file_.Size()-65307 : 0;
    size_t n = (size_t)(file_.Size()-from);
    p = file_.View(from,n);
    if (!p) return false;
    bool found = false;
    for (size_t i = n; i-- > 0;) {
        if (n-i < 27 || !Is(p+i,"OggS") || p[i+4] || !(p[i+5] & 4) || U32(p+i+14) != serial_) continue;
        unsigned count = p[i+26];
        if (n-i < 27u+count) continue;
        size_t bytes = 27+count;
        for (unsigned j = 0; j < count; ++j) bytes += p[i+27+j];
        if (bytes != n-i) continue;
        total_ = U64(p+i+6); found = total_ != UINT64_MAX; break;
    }
    if (!found) return false;
    }
    for (unsigned size = 0; size < 2; ++size) {
        mdct_[size].Init(block_[size]);
        unsigned half = block_[size]/2;
        windows_[size].resize(half);
        for (unsigned i = 0; i < half; ++i) {
            double inner = std::sin(pi*(i+0.5)/(2*half));
            windows_[size][i] = (float)std::sin(pi*0.5*inner*inner);
        }
    }
    unsigned max = block_[1];
    spectrum_.resize((size_t)channels_*max/2); curves_.resize(spectrum_.size());
    time_.resize((size_t)channels_*max*2); dct_.resize(max/2);
    pcm_.resize(max*2);
    return !media_.skip || Seek(0);
}

// Decode one residue type (0 interleaved, 1 per-channel, 2 channel-paired) for the
// channels in `bundle`, accumulating into spectrum_. `nonzero` prunes channels
// whose floor was unused.
bool VorbisDecoder::DecodeResidue(PacketBits& packet, const Residue& r, std::span<const unsigned> bundle, const bool* nonzero, unsigned n) {
    // Decode from a local cursor, so it can stay in registers across the spectrum stores;
    // a failure fails the whole packet, so only success writes it back.
    PacketBits b = packet;
    unsigned ch = (unsigned)bundle.size();
    if (!ch) return true;
    bool any = false;
    for (unsigned c : bundle) any |= nonzero[c];
    if (!any) return true;
    unsigned length = r.type == 2 ? n*ch : n;
    unsigned begin = std::min(r.begin,length), end = std::min(r.end,length), count = (end-begin)/r.partition;
    unsigned vectors = r.type == 2 ? 1 : ch, words = books_[r.classbook].dimensions;
    if (words > 65535) return false;
    classes_.resize((size_t)vectors*(count+words));
    auto put = [&](unsigned vector, unsigned index, float value) {
        unsigned channel = r.type == 2 ? bundle[index % ch] : bundle[vector];
        unsigned sample = r.type == 2 ? index/ch : index;
        spectrum_[(size_t)channel*block_[1]/2+sample] += value;
    };
    for (unsigned pass = 0; pass < r.passes; ++pass) for (unsigned part = 0; part < count;) {
        if (!pass) for (unsigned v = 0; v < vectors; ++v) if (r.type == 2 || nonzero[bundle[v]]) {
            int value = books_[r.classbook].Scalar(b);
            if (value < 0) return false;
            if (!r.digits.empty()) memcpy(classes_.data()+(size_t)v*(count+words)+part,r.digits.data()+(size_t)value*words,words);
            else for (unsigned i = words; i-- > 0;) {
                classes_[(size_t)v*(count+words)+part+i] = (uint8_t)((unsigned)value % r.classes);
                value /= (int)r.classes;
            }
        }
        for (unsigned i = 0; i < words && part < count; ++i, ++part) {
            for (unsigned v = 0; v < vectors; ++v) if (r.type == 2 || nonzero[bundle[v]]) {
                unsigned type = classes_[(size_t)v*(count+words)+part];
                int book = r.books[type][pass];
                if (book < 0) continue;
                const auto& cb = books_[(size_t)book];
                unsigned step = r.steps[type][pass], offset = begin+part*r.partition;
                if (!step) return false;
                // Stereo type-2 vectors alternate channels. Walk those destinations directly
                // instead of dividing the interleaved index for every coefficient.
                if (r.type==2 && ch==2) {
                    float* channel[2]={spectrum_.data()+(size_t)bundle[0]*block_[1]/2,spectrum_.data()+(size_t)bundle[1]*block_[1]/2};
                    if (!(cb.dimensions&1) && !(offset&1)) {
                        // Locals keep the book's fields out of reach of the spectrum stores, which
                        // the compiler would otherwise reload after every one.
                        float* left=channel[0]+offset/2;float* right=channel[1]+offset/2;
                        const unsigned dimensions=cb.dimensions;
                        auto pairs=[&](auto count) {
                            for (unsigned j=0;j<step;++j) {
                                const float* values=cb.Vector(b);
                                if (!values) return false;
                                for (unsigned k=0;k<count;k+=2) { *left+++=values[k];*right+++=values[k+1]; }
                            }
                            return true;
                        };
                        bool decoded=dimensions==2?pairs(std::integral_constant<unsigned,2>{}):
                            dimensions==4?pairs(std::integral_constant<unsigned,4>{}):pairs(dimensions);
                        if (!decoded) return false;
                        continue;
                    }
                    unsigned index=offset;
                    for (unsigned j=0;j<step;++j) {
                        const float* values=cb.Vector(b);
                        if (!values) return false;
                        for (unsigned k=0;k<cb.dimensions;++k,++index) channel[index&1][index>>1]+=values[k];
                    }
                    continue;
                }
                for (unsigned j = 0; j < step; ++j) {
                    const float* values = cb.Vector(b);
                    if (!values) return false;
                    for (unsigned k = 0; k < cb.dimensions; ++k)
                        put(v,offset+(r.type == 0 ? j+k*step : j*cb.dimensions+k),values[k]);
                }
            }
        }
    }
    packet = b;
    return b.Good();
}

// Decode one audio packet: mode/block size, floor, residue, coupling, inverse
// MDCT, window and overlap-add. `previous_` is the last block size because Vorbis
// overlaps unequal-length blocks. Seek points are recorded as it advances.
bool VorbisDecoder::Audio() {
    if (!previewing_ && (seek_.empty() || produced_ >= seek_.back().sample+rate_*2ull)) {
        Point point{produced_,offset_,payload_,sequence_,segments_,segment_,previous_,ended_,{}};
        memcpy(point.laces.data(),laces_,sizeof laces_);
        seek_.push_back(point);
    }
    if (failed_ || produced_ >= total_ || !Packet() || packet_.empty()) return false;
    PacketBits b(packet_);
    if (b.Get(1)) return false;
    unsigned mode = b.Get(Ilog((uint32_t)modes_.size()-1));
    if (mode >= modes_.size()) return false;
    unsigned large = modes_[mode].large ? 1u : 0u, n = block_[large], half = n/2, stride = block_[1]/2;
    bool prevLong = true, nextLong = true;
    if (large) { prevLong = b.Get(1) != 0; nextLong = b.Get(1) != 0; }
    const auto& mapping = mappings_[modes_[mode].mapping];
    bool nonzero[8]{}, floorPresent[8]{};
    std::fill(spectrum_.begin(),spectrum_.end(),0.0f);
    for (unsigned c = 0; c < channels_; ++c) {
        floorPresent[c] = floors_[mapping.floor[mapping.mux[c]]].Decode(b,books_,curves_.data()+(size_t)c*stride,half);
        nonzero[c] = floorPresent[c];
    }
    for (auto pair : mapping.couples) if (nonzero[pair.magnitude] || nonzero[pair.angle]) nonzero[pair.magnitude] = nonzero[pair.angle] = true;
    for (unsigned map = 0; map < mapping.submaps; ++map) {
        unsigned bundle[8],count=0;
        for (unsigned c = 0; c < channels_; ++c) if (mapping.mux[c] == map) bundle[count++]=c;
        if (!DecodeResidue(b,residues_[mapping.residue[map]],{bundle,count},nonzero,half)) return false;
    }
    for (auto it = mapping.couples.rbegin(); it != mapping.couples.rend(); ++it) {
        float* mag = spectrum_.data()+(size_t)it->magnitude*stride;
        float* ang = spectrum_.data()+(size_t)it->angle*stride;
        // With t = a for positive m and -a otherwise: a > 0 gives (m, m-t), else (m+t, m).
        // The signs vary from bin to bin, so masks select the results.
        const __m128 zero = _mm_setzero_ps(), sign = _mm_set1_ps(-0.0f);
        unsigned i = 0;
        for (; i+4 <= half; i += 4) {
            __m128 m = _mm_loadu_ps(mag+i), a = _mm_loadu_ps(ang+i);
            __m128 t = _mm_xor_ps(a,_mm_andnot_ps(_mm_cmpgt_ps(m,zero),sign)), positive = _mm_cmpgt_ps(a,zero);
            _mm_storeu_ps(mag+i,_mm_or_ps(_mm_and_ps(positive,m),_mm_andnot_ps(positive,_mm_add_ps(m,t))));
            _mm_storeu_ps(ang+i,_mm_or_ps(_mm_and_ps(positive,_mm_sub_ps(m,t)),_mm_andnot_ps(positive,m)));
        }
        for (; i < half; ++i) {
            float m = mag[i], a = ang[i];
            if (m > 0) { mag[i] = a > 0 ? m : m+a; ang[i] = a > 0 ? m-a : m; }
            else { mag[i] = a > 0 ? m : m-a; ang[i] = a > 0 ? m+a : m; }
        }
    }
    unsigned leftN = large && !prevLong ? block_[0]/2 : half;
    unsigned rightN = large && !nextLong ? block_[0]/2 : half;
    const size_t block = (size_t)channels_*block_[1];
    float* times = time_.data()+current_*block;
    const float* tails = time_.data()+(current_^1)*block+previous_/2;
    for (unsigned c = 0; c < channels_; ++c) {
        float* spectral = spectrum_.data()+(size_t)c*stride;
        float* time = times+(size_t)c*block_[1];
        if (!floorPresent[c]) memset(time,0,n*sizeof(float));
        else {
            const float* curve = curves_.data()+(size_t)c*stride;
            mdct_[large].Dct(spectral,curve,dct_.data());
            unsigned lw = leftN == block_[0]/2 ? 0u : 1u, rw = rightN == block_[0]/2 ? 0u : 1u;
            Window(dct_.data(),windows_[lw].data(),leftN,windows_[rw].data(),rightN,time,n);
        }
    }
    unsigned emit = previous_ ? (previous_+n)/4 : 0;
    buffered_ = (size_t)std::min<uint64_t>(emit,total_-produced_); cursor_ = 0;
    if (previous_ == n && channels_ <= 2) {
        unsigned i = 0;
        const float* tl = tails;
        const float* tr = tails+(channels_-1)*block_[1];
        const float* cl = times;
        const float* cr = times+(channels_-1)*block_[1];
        for (; i+4 <= buffered_; i += 4) {
            __m128 l = _mm_add_ps(_mm_loadu_ps(tl+i),_mm_loadu_ps(cl+i));
            __m128 r = _mm_add_ps(_mm_loadu_ps(tr+i),_mm_loadu_ps(cr+i));
            _mm_storeu_ps(pcm_.data()+i*2,_mm_unpacklo_ps(l,r));
            _mm_storeu_ps(pcm_.data()+i*2+4,_mm_unpackhi_ps(l,r));
        }
        for (; i < buffered_; ++i) { pcm_[i*2] = tl[i]+cl[i]; pcm_[i*2+1] = tr[i]+cr[i]; }
    } else for (unsigned i = 0; i < buffered_; ++i) {
        float samples[8]{};
        for (unsigned c = 0; c < channels_; ++c) {
            float value = i < previous_/2 ? tails[(size_t)c*block_[1]+i] : 0;
            int index = (int)i + (int)n/4 - (int)previous_/4;
            if (index >= 0 && index < (int)n) value += times[(size_t)c*block_[1]+(unsigned)index];
            samples[c] = value;
        }
        // Vorbis uses FL,FC,FR,RL,RR,LFE for 5.1; retain the previous player's
        // stereo behavior for mono/stereo and normalize surround to WAVE order.
        float ordered[8]{};
        static constexpr unsigned order[6][8] = {
            {0,2,1}, {0,1,2,3}, {0,2,1,3,4}, {0,2,1,5,3,4},
            {0,2,1,6,5,3,4}, {0,2,1,7,5,6,3,4}};
        if (channels_ >= 3) {
            for (unsigned c = 0; c < channels_; ++c) ordered[c] = samples[order[channels_-3][c]];
            memcpy(samples,ordered,channels_*sizeof(float));
        }
        Mix(samples,channels_,pcm_.data()+(size_t)i*2);
    }
    current_ ^= 1;
    previous_ = n; produced_ += buffered_;
    return b.Good();
}

// Copy buffered PCM, decoding packets until the request is met or the stream ends.
size_t VorbisDecoder::Read(float* out, size_t frames) {
    size_t done = 0;
    while (done < frames) {
        if (cursor_ == buffered_) {
            if (!Audio()) { if (produced_ < total_) failed_ = true; break; }
            if (!buffered_) continue;
        }
        size_t n = std::min(frames-done,buffered_-cursor_);
        memcpy(out+done*2,pcm_.data()+cursor_*2,n*2*sizeof(float));
        cursor_ += n; pos_ += n; done += n;
    }
    return done;
}
// Seek by sample. A target inside the current buffer just moves the cursor;
// otherwise the nearest packet index is used with one long block of preroll so
// the overlap tail is reconstructed correctly.
bool VorbisDecoder::Seek(double seconds) {
    if (!std::isfinite(seconds)) return false;
    previewing_ = false;
    uint64_t target = seconds <= 0 ? media_.skip : seconds >= Duration() ? total_ : media_.skip+(uint64_t)(seconds*rate_);
    if (!failed_ && target >= produced_-buffered_ && target <= produced_) {
        cursor_ = (size_t)(target-(produced_-buffered_)); pos_ = target; return true;
    }
    if (seek_.empty()) {
        Point point{0,offset_,payload_,sequence_,segments_,segment_,0,ended_,{}};
        memcpy(point.laces.data(),laces_,sizeof laces_);
        seek_.push_back(point);
    }
    uint64_t preroll = target > block_[1] ? target-block_[1] : 0;
    auto point = std::upper_bound(seek_.begin(),seek_.end(),preroll,[](uint64_t sample,const Point& p) { return sample < p.sample; });
    if (point != seek_.begin()) --point;
    offset_ = point->offset; payload_ = point->payload; sequence_ = point->sequence;
    segments_ = point->segments; segment_ = point->segment; previous_ = point->previous;
    produced_ = point->sample; pos_ = target; cursor_ = buffered_ = 0;
    ended_ = point->ended; failed_ = false;
    memcpy(laces_,point->laces.data(),sizeof laces_);
    // The first block after the seek point overlaps silence.
    memset(time_.data()+(current_^1)*(size_t)channels_*block_[1],0,(size_t)channels_*block_[1]*sizeof(float));
    while (produced_ < target) if (!Audio()) { failed_ = true; return false; }
    cursor_ = (size_t)(target-(produced_-buffered_)); pos_ = target;
    return true;
}
} // namespace

std::unique_ptr<Decoder> OpenVorbisDecoder(const std::wstring& path) {
    auto d = std::make_unique<VorbisDecoder>();
    return d->Open(path) ? std::move(d) : nullptr;
}
