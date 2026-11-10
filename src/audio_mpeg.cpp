// Cordinal MPEG audio decoder, from ISO/IEC 11172-3 and 13818-3.
// The fixed codeword and synthesis coefficients are format data, not decoder code.
#include "native_audio.h"
#include "audio_live.h"
#include "audio_file.h"
#include "audio_packets.h"
#include "audio_kernels.h"
#include "audio_bits.h"
#include "audio_mpeg_tables.h"
#include "audio_mpeg_alloc.h"
#include "audio_polyphase.h"
#include <array>
#include <cmath>
#include <bit>

namespace {
using namespace nativeaudio;
using namespace nativeaudio::mpeg;
constexpr double pi = 3.14159265358979323846;

// MSB-first cursor over one MPEG frame: side info, Layer I/II fields and each granule's
// position and limit in the reservoir. Layer III's main data is read through a BitWindow.
class MpegBits {
public:
    MpegBits(const uint8_t* p, size_t bytes) : data(p), size(bytes), limit(bytes*8) {}
    uint32_t Peek(unsigned n) const {
        if (!n) return 0;
        size_t at = pos/8;
        uint64_t value = 0;
        if (at <= size && size-at >= 8) value = U64(data+at,true);
        else for (size_t i = 0; i < 8 && at+i < size; ++i) value |= (uint64_t)data[at+i] << (56-i*8);
        return (uint32_t)(value >> (64-n-(pos & 7))) & (n == 32 ? UINT32_MAX : (1u << n)-1);
    }
    uint32_t Get(unsigned n) {
        uint32_t v = Peek(n);
        Drop(n);
        return v;
    }
    void Drop(unsigned n) { pos += n; if (pos > limit) good = false; }
    const uint8_t* data;
    size_t size, pos = 0, limit;
    bool good = true;
};
// First-lookup table for one Huffman book. An entry either names a leaf symbol,
// or sets bit 15 and packs (bits consumed << 8) | symbol when the code resolves
// within 8 bits. Otherwise the entry is the node index to continue from.
constexpr auto FastBook(const HuffNode* tree) {
    std::array<uint16_t,256> out{};
    if (!tree) return out;
    for (unsigned i = 0; i < 256; ++i) {
        int node = 0;
        unsigned bits = 0;
        while (bits < 8 && node >= 0) { node = tree[node].child[(i >> (7-bits)) & 1]; ++bits; }
        out[i] = node < 0 ? (uint16_t)(0x8000 | (bits << 8) | (-node-1)) : (uint16_t)node;
    }
    return out;
}
using FastBooksTable = std::array<std::array<uint16_t,256>,17>;
// Huffman tables 4 and 14 are reserved/absent in the spec, so the live books are
// listed here; bookIndex maps a table number to its slot in bookNumbers.
constexpr unsigned bookNumbers[] = {1,2,3,5,6,7,8,9,10,11,12,13,15,16,24,32,33};
constexpr uint8_t bookIndex[] = {0,0,1,2,0,3,4,5,6,7,8,9,10,11,0,12,13,13,13,13,13,13,13,13,14,14,14,14,14,14,14,14,15,16};
// Built on first open into .bss rather than folded into the image: the 8.7 KB of
// first-lookup tables are cheap to derive and need not be carried read-only in the file.
FastBooksTable fastBooks;
// 12-bit first-lookup tables that resolve a codeword straight to a signed (x,y)
// pair, including the sign bits after the code and the linear escape values.
std::array<std::array<uint16_t,4096>,15> signedBooks;
// 10-bit first-lookup tables for the two count1 books: a quadruple's code and sign bits never
// exceed ten bits. An entry holds the bits consumed (low four) and four 2-bit signed values.
std::array<std::array<uint16_t,1024>,2> countBooks;
// Readable bytes kept past the reservoir's data. A damaged granule is only compared with its
// limit after the big-values regions, and 288 pairs of 47 bits are 1,692 bytes.
constexpr size_t reservoirPad = 2048;
void InitFastBooks() {
    static const bool ready=[] {
        for (unsigned i = 0; i < 17; ++i) fastBooks[i] = FastBook(huffman[bookNumbers[i]]);
        for (unsigned book=0;book<15;++book) for (unsigned prefix=0;prefix<4096;++prefix) {
            int node=0; unsigned bits=0;
            while (node>=0 && bits<12) node=huffman[bookNumbers[book]][node].child[(prefix>>(11-bits++))&1];
            if (node>=0) continue;
            int pair=-node-1,x=pair>>4,y=pair&15;
            if (book>=13 && (x==15 || y==15)) continue; // Escape bits precede signs.
            if (x) { if (bits==12) continue; if ((prefix>>(11-bits++))&1) x=-x; }
            if (y) { if (bits==12) continue; if ((prefix>>(11-bits++))&1) y=-y; }
            signedBooks[book][prefix]=(uint16_t)(0x8000|(bits<<10)|((x&31)<<5)|(y&31));
        }
        for (unsigned book=0;book<2;++book) for (unsigned prefix=0;prefix<1024;++prefix) {
            int node=0; unsigned bits=0;
            while (node>=0 && bits<10) node=huffman[32+book][node].child[(prefix>>(9-bits++))&1];
            if (node>=0) continue;
            unsigned packed=(unsigned)(-node-1),entry=0;
            for (unsigned j=0;j<4 && bits<=10;++j) if ((packed>>(3-j))&1) {
                if (bits==10) { bits=11;break; }
                entry|=((prefix>>(9-bits++))&1?3u:1u)<<(4+j*2);
            }
            if (bits<=10) countBooks[book][prefix]=(uint16_t)(entry|bits);
        }
        return true;
    }();
    (void)ready;
}
// Decoded frame header. version: 0 = MPEG 2.5, 2 = MPEG 2, 3 = MPEG 1;
// layer: 1..3. channels drops to 1 in single-channel mode.
struct MpegHeader {
    unsigned version = 0, layer = 0, rate = 0, rateIndex = 0, channels = 0, bytes = 0, samples = 0, mode = 0, extension = 0, bitrate = 0;
    bool crc = false;
};
// Validate and decode a 4-byte frame header, deriving frame length from the
// bitrate table. Free-format streams (bitrate index 0) need freeSize, resolved
// by the caller, before h.bytes is meaningful.
bool Header(const uint8_t* p, size_t size, MpegHeader& h, unsigned freeSize = 0, bool partial = false) {
    if (size < 4 || p[0] != 255 || (p[1] & 0xE0) != 0xE0) return false;
    h.version = (p[1] >> 3) & 3; h.layer = 4-((p[1] >> 1) & 3);
    unsigned bitrate = p[2] >> 4, index = (p[2] >> 2) & 3, padding = (p[2] >> 1) & 1;
    if (h.version == 1 || h.layer == 4 || bitrate == 15 || index == 3 || (p[3] & 3) == 2) return false;
    constexpr unsigned rates[] = {44100,48000,32000};
    constexpr unsigned kbps[2][3][15] = {
        {{0,32,64,96,128,160,192,224,256,288,320,352,384,416,448},
         {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384},
         {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320}},
        {{0,32,48,56,64,80,96,112,128,144,160,176,192,224,256},
         {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160},
         {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160}}};
    h.rate = rates[index] >> (h.version == 3 ? 0 : h.version == 2 ? 1 : 2);
    h.rateIndex = h.version == 0 ? 2 : index + (h.version == 3 ? 3 : 0);
    h.mode = p[3] >> 6; h.extension = (p[3] >> 4) & 3; h.channels = h.mode == 3 ? 1 : 2;
    h.crc = !(p[1] & 1);
    unsigned bitRate = kbps[h.version == 3 ? 0 : 1][h.layer-1][bitrate]*1000;
    h.bitrate = bitRate;
    h.samples = h.layer == 1 ? 384 : h.layer == 3 && h.version != 3 ? 576 : 1152;
    h.bytes = h.layer == 1 ? (12*bitRate/h.rate+padding)*4 :
              (h.layer == 3 && h.version != 3 ? 72 : 144)*bitRate/h.rate+padding;
    if (!bitrate) {
        if (!freeSize) { h.bytes = 0; return true; }
        h.bytes = freeSize+padding*(h.layer == 1 ? 4 : 1);
        h.bitrate = h.layer == 1 ? freeSize/4*h.rate/12 : freeSize*h.rate/(h.layer == 3 && h.version != 3 ? 72 : 144);
    }
    return h.bytes >= 4 && (partial || h.bytes <= size);
}
// Scalefactor-band boundaries (in spectral lines) per sample rate, long and
// short blocks, plus the 8 kHz variants. Format data from ISO/IEC 11172-3.
constexpr unsigned longBands[6][23] = {
    {0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576},
    {0,6,12,18,24,30,36,44,54,66,80,96,114,136,162,194,232,278,330,394,464,540,576},
    {0,6,12,18,24,30,36,44,54,66,80,96,116,140,168,200,238,284,336,396,464,522,576},
    {0,4,8,12,16,20,24,30,36,44,52,62,74,90,110,134,162,196,238,288,342,418,576},
    {0,4,8,12,16,20,24,30,36,42,50,60,72,88,106,128,156,190,230,276,330,384,576},
    {0,4,8,12,16,20,24,30,36,44,54,66,82,102,126,156,194,240,296,364,448,550,576}};
constexpr unsigned shortBands[6][14] = {
    {0,4,8,12,18,24,32,42,56,74,100,132,174,192},
    {0,4,8,12,18,26,36,48,62,80,104,136,180,192},
    {0,4,8,12,18,26,36,48,62,80,104,134,174,192},
    {0,4,8,12,16,22,30,40,52,66,84,106,136,192},
    {0,4,8,12,16,22,28,38,50,64,80,100,126,192},
    {0,4,8,12,16,22,30,42,58,78,104,138,180,192}};
constexpr unsigned long8k[] = {0,12,24,36,48,60,72,88,108,132,160,192,232,280,336,400,476,566,568,570,572,574,576};
constexpr unsigned short8k[] = {0,8,16,24,36,52,72,96,124,160,162,164,166,192};

// All per-granule side information plus the decode scratch for one channel of
// one granule (quantized lines and the requantized spectrum). The scratch is not
// cleared: Spectral writes quant[0, end) and Requantize all of spectral.
struct Granule {
    unsigned length = 0, pairs = 0, gain = 0, compress = 0, type = 0, tables[3]{}, subgain[3]{}, region[2]{}, reuse = 0;
    unsigned end = 0;
    bool mixed = false, pre = false, scale = false, countTable = false;
    int sfLong[23]{}, sfShort[13][3]{}, invalidLong[23]{}, invalidShort[13][3]{};
    int16_t quant[576];
    float spectral[576];
};
// MPEG-1 CRC: the last two header bytes followed by the side info (or the whole
// frame for Layer I/II), fed bit by bit MSB-first.
bool CheckCrc(const uint8_t* frame,const uint8_t* body,size_t bits) {
    uint16_t crc = 0xFFFF;
    auto bit = [&](unsigned v) { bool high = (crc >> 15) != 0; crc = (uint16_t)(crc << 1); if (high != (v != 0)) crc ^= 0x8005; };
    for (unsigned i = 16; i < 32; ++i) bit((frame[i/8] >> (7-i%8)) & 1);
    for (size_t i = 0; i < bits; ++i) bit((body[i/8] >> (7-i%8)) & 1);
    return crc == U16(frame+4,true);
}
// Layer III side info: per granule/channel lengths and table choices. `back` is
// the main-data carry-over (bit reservoir) length in bytes; it is only returned,
// not yet applied.
bool SideInfo(MpegBits& b, const MpegHeader& h, Granule (&gr)[2][2], unsigned& back) {
    back = b.Get(h.version == 3 ? 9 : 8);
    b.Get(h.version == 3 ? (h.channels == 1 ? 5 : 3) : (h.channels == 1 ? 1 : 2));
    if (h.version == 3) for (unsigned ch = 0; ch < h.channels; ++ch) gr[1][ch].reuse = b.Get(4);
    for (unsigned g = 0; g < (h.version == 3 ? 2u : 1u); ++g) for (unsigned ch = 0; ch < h.channels; ++ch) {
        auto& v = gr[g][ch];
        v.length = b.Get(12); v.pairs = b.Get(9); v.gain = b.Get(8); v.compress = b.Get(h.version == 3 ? 4 : 9);
        if (v.pairs > 288) return false;
        if (b.Get(1)) {
            v.type = b.Get(2); v.mixed = b.Get(1) != 0;
            if (!v.type) return false;
            v.tables[0] = b.Get(5); v.tables[1] = b.Get(5);
            for (auto& gain : v.subgain) gain = b.Get(3);
            v.region[0] = v.type == 2 && !v.mixed ? 8 : 7; v.region[1] = 20-v.region[0];
        } else {
            for (auto& table : v.tables) table = b.Get(5);
            v.region[0] = b.Get(4); v.region[1] = b.Get(3);
        }
        if (h.version == 3) v.pre = b.Get(1) != 0;
        v.scale = b.Get(1) != 0; v.countTable = b.Get(1) != 0;
    }
    return b.good;
}
// Scalefactor bit widths and band counts depend on the compressed-length index
// and block type; MPEG-2 uses a different variable-length scheme from MPEG-1.
// Invalid (all-ones) scalefactors are remembered so intensity stereo skips them.
bool ScaleFactors(MpegBits& b, const MpegHeader& h, Granule& gr, const Granule& first, unsigned channel) {
    // The reservoir is padded (reservoirPad), so the fields come from a register window.
    BitWindow w(b.data,b.pos);
    auto get = [&](unsigned n) { w.Refill(); return w.GetWide(n); };
    if (h.version == 3) {
        constexpr unsigned slen1[] = {0,0,0,0,3,1,1,1,2,2,2,3,3,3,4,4};
        constexpr unsigned slen2[] = {0,1,2,3,0,1,2,3,1,2,3,1,2,3,2,3};
        unsigned a = slen1[gr.compress], c = slen2[gr.compress];
        if (gr.type == 2) {
            if (gr.mixed) for (unsigned band = 0; band < 8; ++band) gr.sfLong[band] = (int)get(a);
            for (unsigned band = gr.mixed ? 3 : 0; band < 12; ++band)
                for (unsigned win = 0; win < 3; ++win) gr.sfShort[band][win] = (int)get(band < 6 ? a : c);
        } else {
            constexpr unsigned edges[] = {0,6,11,16,21};
            for (unsigned group = 0; group < 4; ++group) for (unsigned band = edges[group]; band < edges[group+1]; ++band)
                gr.sfLong[band] = gr.reuse & (8u >> group) ? first.sfLong[band] : (int)get(band < 11 ? a : c);
        }
        for (auto& v : gr.invalidLong) v = 7;
        for (auto& band : gr.invalidShort) for (auto& v : band) v = 7;
    } else {
        unsigned c = gr.compress, group = 0, slen[4]{};
        bool intensity = channel == 1 && h.mode == 1 && (h.extension & 1);
        if (!intensity) {
            if (c < 400) { slen[0] = (c >> 4)/5; slen[1] = (c >> 4)%5; slen[2] = (c & 15) >> 2; slen[3] = c & 3; }
            else if (c < 500) { c -= 400; group = 1; slen[0] = (c >> 2)/5; slen[1] = (c >> 2)%5; slen[2] = c & 3; }
            else { c -= 500; group = 2; slen[0] = c/3; slen[1] = c%3; gr.pre = true; }
        } else {
            c >>= 1;
            if (c < 180) { group = 3; slen[0] = c/36; slen[1] = c/6%6; slen[2] = c%6; }
            else if (c < 244) { c -= 180; group = 4; slen[0] = c >> 4; slen[1] = c >> 2 & 3; slen[2] = c & 3; }
            else { c -= 244; group = 5; slen[0] = c/3; slen[1] = c%3; }
        }
        constexpr unsigned counts[6][3][4] = {
            {{6,5,5,5},{9,9,9,9},{6,9,9,9}}, {{6,5,7,3},{9,9,12,6},{6,9,12,6}},
            {{11,10,0,0},{18,18,0,0},{15,18,0,0}}, {{7,7,7,0},{12,12,12,0},{6,15,12,0}},
            {{6,6,6,3},{12,9,9,6},{6,12,9,6}}, {{8,8,5,0},{15,12,9,0},{6,18,9,0}}};
        unsigned kind = gr.type == 2 ? (gr.mixed ? 2u : 1u) : 0u, at = 0;
        for (unsigned g = 0; g < 4; ++g) for (unsigned i = 0; i < counts[group][kind][g]; ++i,++at) {
            int value = (int)get(slen[g]), invalid = (1 << slen[g])-1;
            if (kind == 0 || (kind == 2 && at < 6)) { gr.sfLong[at] = value; gr.invalidLong[at] = invalid; }
            else {
                unsigned off = kind == 2 ? at-6+9 : at;
                gr.sfShort[off/3][off%3] = value; gr.invalidShort[off/3][off%3] = invalid;
            }
        }
    }
    gr.sfLong[21] = gr.sfLong[20]; gr.invalidLong[21] = gr.invalidLong[20];
    for (unsigned win = 0; win < 3; ++win) { gr.sfShort[12][win] = gr.sfShort[11][win]; gr.invalidShort[12][win] = gr.invalidShort[11][win]; }
    b.pos = w.Position();
    if (b.pos > b.limit) b.good = false;
    return b.good;
}

// Decode the quantized spectral lines. Region boundaries select the Huffman
// table per pair; count1 quadruples use tables 32/33. Any remaining main data is
// skipped so the next granule starts at the right bit. The bits come from a register
// window over the padded reservoir, so the granule's limit is compared once after the
// big values: running past it there fails the frame, as each bounded read used to.
bool Spectral(MpegBits& b, const MpegHeader& h, Granule& gr) {
    const unsigned* bands = h.rate == 8000 ? long8k : longBands[h.rateIndex];
    unsigned region1 = gr.type == 2 ? (h.rate == 8000 ? 72 : 36) : bands[std::min(22u,gr.region[0]+1)];
    unsigned region2 = gr.type == 2 ? 576 : bands[std::min(22u,gr.region[0]+gr.region[1]+2)];
    unsigned at = 0;
    const unsigned pairsEnd = gr.pairs*2;
    BitWindow w(b.data,b.pos);
    for (unsigned region = 0; region < 3; ++region) {
        const unsigned stop = std::min(region == 0 ? region1 : region == 1 ? region2 : pairsEnd,pairsEnd);
        if (at >= stop) continue;
        const unsigned table = gr.tables[region];
        if (!table) { memset(gr.quant+at,0,(stop-at)*sizeof(int16_t)); at = stop; continue; }
        if (!huffman[table]) return false;
        // The 12-bit signed book resolves most pairs with their sign bits in one load. Longer
        // codes and escapes walk the tree from its 8-bit first lookup; a pair is at most 47 bits.
        const uint16_t* book = signedBooks[bookIndex[table]].data();
        const uint16_t* first = fastBooks[bookIndex[table]].data();
        const HuffNode* tree = huffman[table];
        const unsigned escape = linbits[table];
        do {
            w.Refill();
            if (unsigned entry = book[w.Peek(12)]) {
                w.Drop((entry>>10)&15);
                gr.quant[at] = (int16_t)(int32_t(entry<<22)>>27);
                gr.quant[at+1] = (int16_t)(int32_t(entry<<27)>>27);
            } else {
                unsigned fast = first[w.Peek(8)];
                int pair;
                if (fast & 0x8000) { w.Drop((fast >> 8) & 15); pair = (int)(fast & 255); }
                else {
                    w.Drop(8);
                    int node = (int)fast;
                    for (unsigned i = 8; i < 20 && node >= 0; ++i) node = tree[node].child[w.Get(1)];
                    if (node >= 0) return false;
                    pair = -node-1;
                }
                int x = pair >> 4, y = pair & 15;
                if (x == 15 && escape) x += (int)w.Get(escape);
                if (x && w.Get(1)) x = -x;
                if (y == 15 && escape) y += (int)w.Get(escape);
                if (y && w.Get(1)) y = -y;
                gr.quant[at] = (int16_t)x; gr.quant[at+1] = (int16_t)y;
            }
            at += 2;
        } while (at < stop);
    }
    size_t pos = w.Position();
    if (pos > b.limit) return false;
    // A quadruple that runs past the granule is dropped and ends the region.
    const uint16_t* quads = countBooks[gr.countTable].data();
    for (; at+4 <= 576 && pos < b.limit; at += 4) {
        w.Refill();
        const unsigned entry = quads[w.Peek(10)];
        if (!entry) return false;
        w.Drop(entry&15); pos += entry&15;
        if (pos > b.limit) break;
        for (unsigned j = 0; j < 4; ++j) gr.quant[at+j] = (int16_t)(int32_t(entry<<(26-j*2))>>30);
    }
    gr.end = at;
    b.pos = b.limit;
    return true;
}
struct RequantTables {
    std::array<float,8207> powers;
    std::array<float,576> gains;
};
// Both tables are always used together; one initialization guard is sufficient.
const RequantTables& Quantization() {
    static const auto values=[] {
        RequantTables v;
        for (unsigned i=0;i<v.powers.size();++i) v.powers[i]=(float)std::pow(i,4.0/3);
        for (int n=-512;n<64;++n) v.gains[n+512]=std::exp2(n*0.25f);
        return v;
    }();
    return values;
}
// Requantize quantized lines to spectrum: |x|^(4/3) scaled by 2^(gain/4). The
// exponent is assembled from the global gain, scalefactors and pre-emphasis.
void Requantize(const MpegHeader& h, Granule& g) {
    // Lines after the last nonzero one are zero; synthesis still runs to flush the
    // preceding overlap. Short blocks interleave their windows, so they clear first.
    unsigned nonzero=g.end;
    while (nonzero && !g.quant[nonzero-1]) --nonzero;
    if (!nonzero || g.type == 2) memset(g.spectral,0,sizeof g.spectral);
    else memset(g.spectral+nonzero,0,(576-nonzero)*sizeof(float));
    if (!nonzero) return;
    constexpr unsigned pre[] = {0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,3,2,0};
    const unsigned* lb = h.rate == 8000 ? long8k : longBands[h.rateIndex];
    const unsigned* sb = h.rate == 8000 ? short8k : shortBands[h.rateIndex];
    const auto& tables=Quantization();
    const auto& powers=tables.powers;
    const auto& gains=tables.gains;
    // Scalefactors are read from the bitstream and are not otherwise range-checked. Clamp
    // the quarter-power index so a malformed value cannot read past the gains table.
    auto gainOf = [&](int exponent) { return gains[(size_t)std::clamp(exponent, -512, 63) + 512]; };
    auto value = [&](unsigned i,float gain) {
        // Moving the sign to the gain preserves the product's bits and avoids
        // an unpredictable branch for each quantized spectral line.
        uint32_t bits=(uint32_t)g.quant[i],sign=0u-(bits>>31),magnitude=(bits^sign)-sign;
        float factor=std::bit_cast<float>(std::bit_cast<uint32_t>(gain)^(sign<<31));
        return powers[magnitude]*factor;
    };
    int gain = (int)g.gain-210, scale = g.scale ? 4 : 2;
    unsigned longCount = g.type == 2 ? (g.mixed ? (h.version == 3 ? 8u : 6u) : 0u) : 22u;
    for (unsigned band = 0; band < longCount && lb[band]<nonzero; ++band) {
        int exponent=gain-scale*((band == 21 ? 0 : g.sfLong[band])+(g.pre ? pre[band] : 0));
        float factor=gainOf(exponent);
        const unsigned end=std::min(lb[band+1],nonzero);
        for (unsigned i = lb[band]; i < end; ++i) g.spectral[i] = value(i,factor);
    }
    if (g.type == 2) {
        unsigned at = longCount ? lb[longCount] : 0;
        for (unsigned band = g.mixed ? 3u : 0u; band < 13 && at<nonzero; ++band)
        for (unsigned win = 0; win < 3 && at<nonzero; ++win) {
            int exponent=gain-8*(int)g.subgain[win]-scale*(band == 12 ? 0 : g.sfShort[band][win]);
            float factor=gainOf(exponent);
            for (unsigned i = sb[band]; i < sb[band+1] && at<nonzero; ++i) g.spectral[i*3+win] = value(at++,factor);
        }
    }
}

// Joint-stereo post-processing for dual/intensity/MS modes: intensity stereo
// spreads the left channel's energy to the right, then mid/side is unwound.
// Both are no-ops unless the header signalled them.
void Stereo(const MpegHeader& h, Granule& left, Granule& right) {
    if (h.channels != 2 || h.mode != 1) return;
    bool ms = (h.extension & 2) != 0;
    if (!(h.extension & 1)) {
        if (ms) {
            const __m128 scale = _mm_set1_ps(0.7071067811865475f);
            for (unsigned i = 0; i < 576; i += 4) {
                __m128 l = _mm_loadu_ps(left.spectral+i), r = _mm_loadu_ps(right.spectral+i);
                _mm_storeu_ps(left.spectral+i,_mm_mul_ps(_mm_add_ps(l,r),scale));
                _mm_storeu_ps(right.spectral+i,_mm_mul_ps(_mm_sub_ps(l,r),scale));
            }
        }
        return;
    }
    bool intensity[576]{};
    {
        const unsigned* lb = h.rate == 8000 ? long8k : longBands[h.rateIndex];
        const unsigned* sb = h.rate == 8000 ? short8k : shortBands[h.rateIndex];
        auto apply = [&](unsigned start,unsigned end,unsigned step,int position,int invalid) {
            if (position == invalid || position < 0 || (h.version == 3 && position > 6)) return;
            float l = 1,r = 1;
            if (h.version == 3) {
                if (position == 6) r = 0;
                else { float tangent = (float)std::tan(position*pi/12); l = tangent/(1+tangent); r = 1/(1+tangent); }
            } else if (position) {
                float ratio = std::exp2(-((right.compress & 1) ? 0.5f : 0.25f)*((position+1)/2));
                if (position & 1) l = ratio; else r = ratio;
            }
            if (ms) { l *= 1.4142135623730951f; r *= 1.4142135623730951f; }
            for (unsigned i = start; i < end; i += step) { float v = left.spectral[i]; left.spectral[i] = v*l; right.spectral[i] = v*r; intensity[i] = true; }
        };
        if (right.type == 2) {
            for (unsigned w = 0; w < 3; ++w) {
                unsigned first = right.mixed ? 3u : 0u;
                for (unsigned band = first; band < 13; ++band)
                    for (unsigned i = sb[band]; i < sb[band+1]; ++i) if (right.spectral[i*3+w] != 0) first = band+1;
                for (unsigned band = first; band < 13; ++band) apply(sb[band]*3+w,sb[band+1]*3+w,3,right.sfShort[band][w],right.invalidShort[band][w]);
            }
        } else {
            unsigned first = 0;
            for (unsigned band = 0; band < 22; ++band) for (unsigned i = lb[band]; i < lb[band+1]; ++i) if (right.spectral[i] != 0) first = band+1;
            for (unsigned band = first; band < 22; ++band) apply(lb[band],lb[band+1],1,right.sfLong[band],right.invalidLong[band]);
        }
    }
    if (ms) for (unsigned i = 0; i < 576; ++i) if (!intensity[i]) {
        float l = left.spectral[i], r = right.spectral[i];
        left.spectral[i] = (l+r)*0.7071067811865475f; right.spectral[i] = (l-r)*0.7071067811865475f;
    }
}

#pragma float_control(precise, off, push)
struct Complex {
    float r,i;
    Complex operator+(Complex b) const { return {r+b.r,i+b.i}; }
    Complex operator-(Complex b) const { return {r-b.r,i-b.i}; }
    Complex operator*(Complex b) const { return {r*b.r-i*b.i,r*b.i+i*b.r}; }
};
// Radix-3 butterfly using the exact cube root of unity; the 18-point DCT is
// built from three of these.
void Fft3(Complex a,Complex b,Complex c,Complex* out) {
    Complex sum = b+c, diff = b-c;
    Complex middle = a-Complex{sum.r*0.5f,sum.i*0.5f};
    Complex turn{-diff.i*0.8660254037844386f,diff.r*0.8660254037844386f};
    out[0] = a+sum; out[1] = middle+turn; out[2] = middle-turn;
}
// Polyphase synthesis filterbank: 32-point DCT matrixing, the 512-tap window,
// and a 32-input overlap-add per channel. `fast_` selects the AVX2 kernels that
// produce identical output bit for bit.
class Filterbank {
public:
    Filterbank();
    void Reset() { memset(overlap_,0,sizeof overlap_); memset(v_,0,sizeof v_); head_[0] = head_[1] = active_[0] = active_[1] = 0; }
    void Hybrid(Granule& g,unsigned channel,float* pcm);
    void Subband(const float* in,unsigned channel,float* out) { Synthesis(in,channel,out); }
    void Subbands18(const float* in,unsigned channel,float* out) {
        if (fast_) AudioMpegSynthesis18Avx2(in,v_[channel],head_[channel],synthesisCos_,out);
        else for (unsigned i = 0; i < 18; ++i) Synthesis(in+i*32,channel,out+i*32);
    }
private:
    void Synthesis(const float* in,unsigned channel,float* out);
    void Dct18(const float* in,float* out);
    void Long4(const float* in,unsigned type,float* overlap,float* subbands);
    float synthesisCos_[32]{};
    Complex pre18_[9]{},post18_[9]{},twiddle18_[9]{};
    float cosine6_[6][8]{}, windows_[4][36]{};
    float overlap_[2][32][18]{}, v_[2][2048]{};
    unsigned head_[2]{},active_[2]{};
    bool fast_ = FastAudioKernels();
};
#pragma float_control(precise, on, push)
Filterbank::Filterbank() {
    for (unsigned n = 2; n <= 32; n *= 2) for (unsigned i = 0; i < n/2; ++i)
        synthesisCos_[n/2+i] = (float)(1/(2*std::cos(pi*(i+0.5)/n)));
    auto phase = [](double x) { return Complex{(float)std::cos(x),(float)std::sin(x)}; };
    for (unsigned j = 0; j < 9; ++j) { pre18_[j] = phase(pi*j/18); post18_[j] = phase(pi*(j+0.25)/18); }
    for (unsigned s = 0; s < 3; ++s) for (unsigned r = 0; r < 3; ++r) twiddle18_[s*3+r] = phase(2*pi*r*s/9);
    for (unsigned j = 0; j < 6; ++j) for (unsigned k = 0; k < 6; ++k) cosine6_[j][k] = (float)std::cos(pi/6*(j+0.5)*(k+0.5));
    for (unsigned i = 0; i < 36; ++i) {
        windows_[0][i] = (float)std::sin(pi/36*(i+0.5));
        windows_[1][i] = i < 18 ? windows_[0][i] : i < 24 ? 1 : i < 30 ? (float)std::sin(pi/12*(i-18+0.5)) : 0;
        windows_[3][i] = i < 6 ? 0 : i < 12 ? (float)std::sin(pi/12*(i-6+0.5)) : i < 18 ? 1 : windows_[0][i];
        if (i < 12) windows_[2][i] = (float)std::sin(pi/12*(i+0.5));
    }
}
#pragma float_control(pop)
// 18-point DCT as a two-dimensional 3x3 FFT with pre/post twiddles; kept under
// fast-math with a dedicated rotation table.
void Filterbank::Dct18(const float* in,float* out) {
    Complex z[9],columns[3][3],result[9];
    for (unsigned j = 0; j < 9; ++j) z[j] = Complex{in[j*2],-in[17-j*2]}*pre18_[j];
    for (unsigned r = 0; r < 3; ++r) Fft3(z[r],z[r+3],z[r+6],columns[r]);
    for (unsigned s = 0; s < 3; ++s) {
        Complex v[3];
        Fft3(columns[0][s],columns[1][s]*twiddle18_[s*3+1],columns[2][s]*twiddle18_[s*3+2],v);
        for (unsigned r = 0; r < 3; ++r) result[s+r*3] = v[r];
    }
    for (unsigned j = 0; j < 9; ++j) { Complex v = result[j]*post18_[j]; out[j*2] = v.r; out[17-j*2] = v.i; }
}
// Four independent subbands share each rotation constant. SSE2 lanes keep the
// existing 3x3 transform's arithmetic order, including the overlap and sign flip.
// The two boundary subbands of mixed blocks still use the scalar path below.
void Filterbank::Long4(const float* in,unsigned type,float* overlap,float* subbands) {
    struct V { __m128 r,i; };
    auto mul=[](V a,Complex b) {
        __m128 r=_mm_set1_ps(b.r),i=_mm_set1_ps(b.i);
        return V{_mm_sub_ps(_mm_mul_ps(a.r,r),_mm_mul_ps(a.i,i)),
                 _mm_add_ps(_mm_mul_ps(a.r,i),_mm_mul_ps(a.i,r))};
    };
    auto fft=[](V a,V b,V c,V* out) {
        __m128 sr=_mm_add_ps(b.r,c.r),si=_mm_add_ps(b.i,c.i);
        __m128 dr=_mm_sub_ps(b.r,c.r),di=_mm_sub_ps(b.i,c.i);
        __m128 half=_mm_set1_ps(0.5f),root=_mm_set1_ps(0.8660254037844386f);
        __m128 mr=_mm_sub_ps(a.r,_mm_mul_ps(sr,half)),mi=_mm_sub_ps(a.i,_mm_mul_ps(si,half));
        __m128 tr=_mm_mul_ps(di,_mm_set1_ps(-0.8660254037844386f)),ti=_mm_mul_ps(dr,root);
        out[0]={_mm_add_ps(a.r,sr),_mm_add_ps(a.i,si)};
        out[1]={_mm_add_ps(mr,tr),_mm_add_ps(mi,ti)};
        out[2]={_mm_sub_ps(mr,tr),_mm_sub_ps(mi,ti)};
    };
    // Four subbands of 18 values each, transposed so that lane k holds subband k.
    auto columns18=[](const float* p,__m128* out) {
        for (unsigned at : {0u,4u,8u,12u,14u}) {
            __m128 a=_mm_loadu_ps(p+at),b=_mm_loadu_ps(p+18+at),c=_mm_loadu_ps(p+36+at),d=_mm_loadu_ps(p+54+at);
            _MM_TRANSPOSE4_PS(a,b,c,d);
            out[at]=a;out[at+1]=b;out[at+2]=c;out[at+3]=d;
        }
    };
    const __m128 sign=_mm_set1_ps(-0.0f);
    __m128 lines[18],previous[18];
    columns18(in,lines);columns18(overlap,previous);
    V z[9],columns[3][3],result[9];
    for (unsigned j=0;j<9;++j)
        z[j]=mul({lines[j*2],_mm_xor_ps(lines[17-j*2],sign)},pre18_[j]);
    for (unsigned r=0;r<3;++r) fft(z[r],z[r+3],z[r+6],columns[r]);
    for (unsigned s=0;s<3;++s) {
        V v[3];
        fft(columns[0][s],mul(columns[1][s],twiddle18_[s*3+1]),mul(columns[2][s],twiddle18_[s*3+2]),v);
        for (unsigned r=0;r<3;++r) result[s+r*3]=v[r];
    }
    __m128 dct[18];
    for (unsigned j=0;j<9;++j) {
        V v=mul(result[j],post18_[j]);dct[j*2]=v.r;dct[17-j*2]=v.i;
    }
    const __m128 odd=_mm_setr_ps(0.0f,-0.0f,0.0f,-0.0f);
    __m128 next[18];
    for (unsigned i=0;i<18;++i) {
        __m128 first=i<9?dct[i+9]:_mm_xor_ps(dct[26-i],sign);
        __m128 last=_mm_xor_ps(i<9?dct[8-i]:dct[i-9],sign);
        first=_mm_add_ps(_mm_mul_ps(first,_mm_set1_ps(windows_[type][i])),previous[i]);
        if (i&1) first=_mm_xor_ps(first,odd);
        _mm_storeu_ps(subbands+i*32,first);
        next[i]=_mm_mul_ps(last,_mm_set1_ps(windows_[type][i+18]));
    }
    // Back to one row of 18 per subband; the last block overlaps the one before it.
    for (unsigned at : {0u,4u,8u,12u,14u}) {
        __m128 a=next[at],b=next[at+1],c=next[at+2],d=next[at+3];
        _MM_TRANSPOSE4_PS(a,b,c,d);
        _mm_storeu_ps(overlap+at,a);_mm_storeu_ps(overlap+18+at,b);_mm_storeu_ps(overlap+36+at,c);_mm_storeu_ps(overlap+54+at,d);
    }
}
// Recursive even/odd DCT split (the same shape as the AVX2 batch version).
// factors[] holds the precomputed per-stage rotation constants.
template<unsigned N> __forceinline void Dct2(const float* in,float* out,const float* factors) {
    if constexpr (N == 1) *out = *in;
    else {
        constexpr unsigned half = N/2;
        float sum[half],diff[half],even[half],odd[half];
        for (unsigned j = 0; j < half; ++j) { sum[j] = in[j]+in[N-1-j]; diff[j] = (in[j]-in[N-1-j])*factors[half+j]; }
        Dct2<half>(sum,even,factors); Dct2<half>(diff,odd,factors);
        for (unsigned j = 0; j < half; ++j) { out[j*2] = even[j]; out[j*2+1] = odd[j]+(j+1 < half ? odd[j+1] : 0); }
    }
}
// One polyphase step: DCT-IV over the 32 subband samples, write the 64 rotated
// values into the 1024-sample history (duplicated so wrapped reads are
// contiguous), then 32 dot products against the window.
void Filterbank::Synthesis(const float* in,unsigned channel,float* out) {
    unsigned head = head_[channel] = (head_[channel]-64) & 1023;
    if (fast_) { AudioMpegSynthesisAvx2(in,v_[channel],head,synthesisCos_,out); return; }
    float dct[32]; Dct2<32>(in,dct,synthesisCos_);
    float* v = v_[channel]+head;
    for (unsigned i = 0; i < 16; ++i) v[i] = dct[i+16];
    v[16] = 0;
    for (unsigned i = 17; i < 48; ++i) v[i] = -dct[48-i];
    v[48] = -dct[0];
    for (unsigned i = 49; i < 64; ++i) v[i] = -dct[i-48];
    memcpy(v+1024,v,64*sizeof(float));
    for (unsigned j = 0; j < 32; j += 4) {
        __m128 even = _mm_setzero_ps(), odd = _mm_setzero_ps();
        for (unsigned i = 0; i < 8; ++i) {
            unsigned a = head+i*128+j, b = head+i*128+96+j;
            even = _mm_add_ps(even,_mm_mul_ps(_mm_loadu_ps(v_[channel]+a),_mm_loadu_ps(synthesisWindow+i*64+j)));
            odd = _mm_add_ps(odd,_mm_mul_ps(_mm_loadu_ps(v_[channel]+b),_mm_loadu_ps(synthesisWindow+i*64+32+j)));
        }
        _mm_storeu_ps(out+j,_mm_add_ps(even,odd));
    }
}
// One past the last nonzero value of v[0,n), four values at a time.
unsigned NonzeroEnd(const float* v,unsigned n) {
    while (n >= 4 && !_mm_movemask_ps(_mm_cmpneq_ps(_mm_loadu_ps(v+n-4),_mm_setzero_ps()))) n -= 4;
    while (n && v[n-1] == 0) --n;
    return n;
}
float Dot(const float* a,const float* b,unsigned n) {
    __m128 sum = _mm_setzero_ps();
    unsigned i = 0;
    for (; i+4 <= n; i += 4) sum = _mm_add_ps(sum,_mm_mul_ps(_mm_loadu_ps(a+i),_mm_loadu_ps(b+i)));
    float lanes[4]; _mm_storeu_ps(lanes,sum);
    float v = lanes[0]+lanes[1]+lanes[2]+lanes[3];
    for (; i < n; ++i) v += a[i]*b[i];
    return v;
}
// Undo the alias-reduction butterfly, inverse-transform each subband (18- or
// 6-point), overlap-add, then run polyphase synthesis across the 18 slots.
// active_ remembers the last non-zero subband so trailing silence still flushes
// the filterbank's overlap.
void Filterbank::Hybrid(Granule& g,unsigned channel,float* pcm) {
    constexpr float cs[] = {0.8574929257f,0.8817419973f,0.9496286491f,0.9833145925f,0.9955178161f,0.9991605582f,0.9998991952f,0.9999931551f};
    constexpr float ca[] = {-0.5144957554f,-0.4717319686f,-0.3133774542f,-0.1819131996f,-0.0945741925f,-0.0409655829f,-0.0141985686f,-0.0036999747f};
    unsigned nonzero = NonzeroEnd(g.spectral,576);
    // A butterfly whose eight lines below the boundary are all zero leaves its lines zero.
    unsigned alias = std::min(g.type == 2 ? (g.mixed ? 2u : 1u) : 32u,(nonzero+25)/18);
    // Eight butterflies per boundary, four at a time: the lines below the boundary are read and
    // written in reverse so each lane pairs line -1-i with line i.
    for (unsigned sb = 1; sb < alias; ++sb) for (unsigned i = 0; i < 8; i += 4) {
        float* lo = g.spectral+sb*18-4-i;
        float* hi = g.spectral+sb*18+i;
        const __m128 c = _mm_loadu_ps(cs+i), s = _mm_loadu_ps(ca+i);
        __m128 a = _mm_loadu_ps(lo), b = _mm_loadu_ps(hi);
        a = _mm_shuffle_ps(a,a,0x1B);
        __m128 low = _mm_sub_ps(_mm_mul_ps(a,c),_mm_mul_ps(b,s));
        _mm_storeu_ps(hi,_mm_add_ps(_mm_mul_ps(b,c),_mm_mul_ps(a,s)));
        _mm_storeu_ps(lo,_mm_shuffle_ps(low,low,0x1B));
    }
    // Butterflies reach at most 15 lines past the last nonzero one.
    nonzero = NonzeroEnd(g.spectral,std::min(576u,nonzero+16));
    unsigned active=(nonzero+17)/18,limit=std::max(active,active_[channel]);
    active_[channel]=active;
    // Subbands below limit are all written below; the rest stay silent.
    float subbands[18][32];
    if (limit < 32) for (auto& slot : subbands) memset(slot+limit,0,(32-limit)*sizeof(float));
    unsigned sb=0;
    if (g.type!=2) {
        for (;sb+4<=active;sb+=4)
            Long4(g.spectral+sb*18,g.type,overlap_[channel][sb],subbands[0]+sb);
        // The last one to three active subbands share a group with silent ones. The group still
        // runs in lanes; the silent subbands then get what the loop below gives them: their
        // overlap as output and a cleared overlap, or untouched zeros at and past limit.
        if (sb<active && sb+4<=32) {
            float previous[3][18];
            memcpy(previous,overlap_[channel][active],(sb+4-active)*sizeof previous[0]);
            Long4(g.spectral+sb*18,g.type,overlap_[channel][sb],subbands[0]+sb);
            for (unsigned silent=active;silent<sb+4;++silent) {
                const float* before=previous[silent-active];
                float* after=overlap_[channel][silent];
                if (silent<limit) for (unsigned i=0;i<18;++i) {
                    // The scalar path adds the overlap to a zero block, which turns -0 into +0.
                    float value=std::bit_cast<uint32_t>(before[i])==0x80000000u?0.0f:before[i];
                    subbands[i][silent]=(i&silent&1)?-value:value;
                    after[i]=0;
                } else for (unsigned i=0;i<18;++i) { subbands[i][silent]=0;after[i]=before[i]; }
            }
            sb+=4;
        }
    }
    for (; sb < limit; ++sb) {
        unsigned type = g.type == 2 && g.mixed && sb < 2 ? 0u : g.type;
        float time[36]{};
        const float* spectral = g.spectral+sb*18;
        if (sb*18 >= nonzero) { /* Only the preceding granule's overlap remains. */ }
        else if (type == 2) {
            for (unsigned w = 0; w < 3; ++w) {
                float input[8]{}, dct[6];
                for (unsigned k = 0; k < 6; ++k) input[k] = spectral[k*3+w];
                for (unsigned j = 0; j < 6; ++j) dct[j] = Dot(input,cosine6_[j],6);
                for (unsigned i = 0; i < 12; ++i) {
                    float value = i < 3 ? dct[i+3] : i < 9 ? -dct[8-i] : -dct[i-9];
                    time[6+w*6+i] += value*windows_[2][i];
                }
            }
        } else {
            float dct[18];
            Dct18(spectral,dct);
            for (unsigned i = 0; i < 36; ++i) {
                float value = i < 9 ? dct[i+9] : i < 27 ? -dct[26-i] : -dct[i-27];
                time[i] = value*windows_[type][i];
            }
        }
        for (unsigned i = 0; i < 18; ++i) {
            float value = time[i]+overlap_[channel][sb][i];
            subbands[i][sb] = (i & sb & 1) ? -value : value;
            overlap_[channel][sb][i] = time[i+18];
        }
    }
    if (fast_) AudioMpegSynthesis18Avx2(subbands[0],v_[channel],head_[channel],synthesisCos_,pcm);
    else for (unsigned i = 0; i < 18; ++i) Synthesis(subbands[i],channel,pcm+i*32);
}

#pragma float_control(pop)

// Xing/Info streams start from their declared length and index frame offsets as
// needed. Untagged streams are indexed on open to determine their exact length.
// Seeking re-decodes a bounded preroll to rebuild the reservoir and filterbank.
class MpegDecoder final : public Decoder {
public:
    bool Open(const std::wstring& path);
    bool OpenLive(std::shared_ptr<LiveAudioSource> source);
    size_t Read(float* out,size_t frames) override;
    bool Seek(double seconds) override;
    bool FastSeek() const override { return !live_; }
    double Duration() const override { return live_ ? 0 : (double)total_/header_.rate; }
    unsigned Rate() const override { return header_.rate; }
    void Abort() override { if (live_) live_->Abort(); }
    bool Failed() const override { return failed_; }
    std::wstring Problem() const override { return live_ ? live_->Problem() : std::wstring{}; }
private:
    bool IndexThrough(size_t frame);
    bool Frame();
    File file_;
    PacketFile media_;
    std::shared_ptr<LiveAudioSource> live_;
    std::vector<uint8_t> liveFrame_;
    bool liveFirst_ = false;
    MpegHeader header_;
    std::vector<uint64_t> offsets_;
    // Layer III main data. The vector also holds reservoirPad readable bytes after the data.
    std::vector<uint8_t> reservoir_;
    size_t reservoirBytes_ = 0;
    Filterbank filters_;
    size_t frame_ = 0, cursor_ = 0, buffered_ = 0;
    uint64_t frameCount_ = 0, indexAt_ = 0;
    uint64_t total_ = 0, skip_ = 0, raw_ = 0, target_ = 0, pos_ = 0;
    bool failed_ = false;
    unsigned freeSize_ = 0;
    float pcm_[1152*2]{};
};

// Join an already running stream at a frame boundary. A bounded search also skips a leading ID3
// tag; the Layer III reservoir fills naturally before the first complete frame is emitted.
bool MpegDecoder::OpenLive(std::shared_ptr<LiveAudioSource> source) {
    if (!source) return false;
    InitFastBooks();
    live_ = std::move(source);
    uint8_t head[4];
    if (!ReadLiveBytes(*live_, head, sizeof(head)) || !SkipLiveId3(*live_, head, sizeof(head))) return false;
    MpegHeader h;
    for (size_t skipped = 0;; ++skipped) {
        if (Header(head, sizeof(head), h, 0, true) && h.bytes) break;
        if (skipped >= 65536) return false;
        memmove(head, head + 1, 3);
        if (!ReadLiveBytes(*live_, head + 3, 1)) return false;
    }
    liveFrame_.resize(h.bytes);
    memcpy(liveFrame_.data(), head, sizeof(head));
    if (!ReadLiveBytes(*live_, liveFrame_.data() + 4, h.bytes - 4)) return false;
    header_ = h;
    total_ = UINT64_MAX;
    reservoir_.reserve(4096);
    liveFirst_ = true;
    return true;
}

// Layer I/II share one routine: bit allocation, scalefactors (three selectable
// sharing modes), then 12 groups of subband triplets through the filterbank.
bool Layer12(MpegBits& b,const MpegHeader& h,const uint8_t* frame,Filterbank& filters,float (&pcm)[2][1152]) {
    unsigned bound = h.mode == 1 ? (h.extension+1)*4 : 32;
    unsigned limit = 32, table = 0;
    if (h.layer == 2) {
        unsigned kbps = h.bitrate/h.channels/1000;
        table = h.version != 3 ? 4u : kbps <= 48 ? (h.rate == 32000 ? 3u : 2u) :
                kbps <= 80 || h.rate == 48000 ? 0u : 1u;
        limit = subbandLimits[table];
        bound = std::min(bound,limit);
    }
    unsigned allocation[2][32]{}, select[2][32]{}, sf[2][32][3]{};
    for (unsigned sb = 0; sb < limit; ++sb) {
        unsigned width = h.layer == 1 ? 4u : allocations[allocationBands[table][sb]][0].bits;
        for (unsigned c = 0; c < (sb < bound ? h.channels : 1u); ++c) {
            allocation[c][sb] = b.Get(width);
            if (h.layer == 1 && allocation[c][sb] == 15) return false;
        }
        if (sb >= bound && h.channels == 2) allocation[1][sb] = allocation[0][sb];
    }
    if (h.layer == 2) for (unsigned sb = 0; sb < limit; ++sb) for (unsigned c = 0; c < h.channels; ++c)
        if (allocation[c][sb]) select[c][sb] = b.Get(2);
    if (!b.good || (h.crc && !CheckCrc(frame,b.data,b.pos))) return false;
    for (unsigned sb = 0; sb < limit; ++sb) for (unsigned c = 0; c < h.channels; ++c) if (allocation[c][sb]) {
        sf[c][sb][0] = b.Get(6);
        if (h.layer == 1) continue;
        switch (select[c][sb]) {
        case 0: sf[c][sb][1] = b.Get(6); sf[c][sb][2] = b.Get(6); break;
        case 1: sf[c][sb][1] = sf[c][sb][0]; sf[c][sb][2] = b.Get(6); break;
        case 2: sf[c][sb][1] = sf[c][sb][2] = sf[c][sb][0]; break;
        case 3: sf[c][sb][1] = b.Get(6); sf[c][sb][2] = sf[c][sb][1]; break;
        }
    }
    const auto scales = [] {
        std::array<float,64> out{};
        for (unsigned i = 0; i < 63; ++i) out[i] = (float)(2*std::exp2(-(double)i/3));
        return out;
    }();
    unsigned triplet = h.layer == 1 ? 1u : 3u;
    for (unsigned group = 0; group < 12; ++group) {
        float input[3][2][32]{};
        for (unsigned sb = 0; sb < limit; ++sb) for (unsigned c = 0; c < (sb < bound ? h.channels : 1u); ++c) {
            unsigned code = allocation[c][sb];
            if (!code) continue;
            Allocation quant = h.layer == 1 ? Allocation{(uint16_t)((1u << (code+1))-1),(uint8_t)(code+1),3} :
                                             allocations[allocationBands[table][sb]][code];
            unsigned packed = quant.group == 1 ? b.Get(quant.bits) : 0;
            for (unsigned i = 0; i < triplet; ++i) {
                unsigned v = quant.group == 1 ? packed % quant.levels : b.Get(quant.bits);
                if (quant.group == 1) packed /= quant.levels;
                float value = (float)((int)v*2+1-(int)quant.levels)/quant.levels;
                input[i][c][sb] = value*scales[sf[c][sb][h.layer == 1 ? 0 : group/4]];
                if (sb >= bound && h.channels == 2) input[i][1][sb] = value*scales[sf[1][sb][h.layer == 1 ? 0 : group/4]];
            }
        }
        if (!b.good) return false;
        for (unsigned i = 0; i < triplet; ++i) for (unsigned c = 0; c < h.channels; ++c)
            filters.Subband(input[i][c],c,pcm[c]+(group*triplet+i)*32);
    }
    return true;
}

// Locate the first valid frame. A complete Xing/Info header supplies its length
// without reading a multi-hour recording before its first sample can play.
// LAME tags supply the encoder delay/padding used to trim the stream.
bool MpegDecoder::Open(const std::wstring& path) {
    InitFastBooks();
    if (!file_.Open(path)) return false;
    file_.Sequential();
    const uint8_t* signature=file_.View(0,4);
    if (signature && U32(signature,true)==0x1a45dfa3) {
        media_.file=std::move(file_);
        if (!media_.OpenMatroska(path) || media_.codec!=AudioMpeg) return false;
        for (size_t i=0;i<media_.packets.size();++i) {
            auto& packet=media_.packets[i];const uint8_t* bytes=media_.Data(i);MpegHeader h;
            if (!bytes || !Header(bytes,packet.bytes,h) || !h.bytes || h.bytes!=packet.bytes) return false;
            if (!i) header_=h;
            if (h.rate!=header_.rate || h.layer!=header_.layer || h.version!=header_.version || h.channels!=header_.channels) return false;
            packet.start=i*h.samples;packet.frames=h.samples;
            offsets_.push_back(i);
        }
        frameCount_=offsets_.size();
        media_.frames=offsets_.size()*header_.samples;
        if (!media_.Trim(header_.rate)) return false;
        skip_=media_.skip;total_=media_.valid;target_=skip_;reservoir_.reserve(4096);return total_>0;
    }
    uint64_t at = 0;
    const uint8_t* p = file_.View(0,(size_t)std::min<uint64_t>(10,file_.Size()));
    if (!p) return false;
    if (file_.Size() >= 10 && memcmp(p,"ID3",3) == 0) {
        if ((p[6] | p[7] | p[8] | p[9]) & 128) return false;
        at = 10+((uint64_t)p[6] << 21)+((uint64_t)p[7] << 14)+((uint64_t)p[8] << 7)+p[9]+((p[5] & 16) ? 10 : 0);
    }
    // Some rips carry a second, undersized ID3 tag whose frames run past the first 64 KiB, so the
    // first frame can lie up to 2 MiB beyond the tags.
    uint64_t searchEnd = std::min(file_.Size(),at+(2u<<20));
    bool tagged = false;
    unsigned encodedFrames = 0, padding = 0;
    bool streaming = false;
    for (; at+4 <= file_.Size();) {
        size_t n = (size_t)std::min<uint64_t>(65536,file_.Size()-at);
        p = file_.View(at,n);
        MpegHeader h;
        if (!p || !Header(p,n,h,freeSize_)) {
            if (offsets_.empty() && at < searchEnd) { ++at; continue; }
            break;
        }
        if (!h.bytes) {
            unsigned freePadding = ((p[2] >> 1) & 1)*(h.layer == 1 ? 4u : 1u);
            for (unsigned next = 4+freePadding; next+4 <= n && next < 16384; ++next) {
                MpegHeader following;
                if (!Header(p+next,n-next,following) || following.bytes || following.rate != h.rate ||
                    following.layer != h.layer || following.version != h.version || following.channels != h.channels) continue;
                unsigned base = next-freePadding;
                unsigned secondSize = base+((p[next+2] >> 1) & 1)*(h.layer == 1 ? 4u : 1u);
                if (next+secondSize+4 <= n) {
                    MpegHeader third;
                    if (!Header(p+next+secondSize,n-next-secondSize,third,base) || third.rate != h.rate || third.layer != h.layer) continue;
                }
                freeSize_ = base;
                break;
            }
            if (!freeSize_ || !Header(p,n,h,freeSize_) || !h.bytes) {
                // A sync pattern with no bitrate inside tag data is not a free-format stream: keep searching.
                if (offsets_.empty() && at < searchEnd) { ++at; continue; }
                return false;
            }
        }
        if (offsets_.empty()) {
            // A sync pattern inside tag data (a picture, say) can pass Header, so the first frame
            // must be followed by another frame of the same stream. View() may remap, so p is
            // fetched again after this check.
            uint8_t after[4] = {};
            const uint8_t* q = at+h.bytes+4 <= file_.Size() ? file_.View(at+h.bytes,4) : nullptr;
            if (q) memcpy(after,q,4);
            MpegHeader f;
            if (!q || !Header(after,4,f,0,true) || f.rate!=h.rate || f.layer!=h.layer || f.version!=h.version ||
                f.channels!=h.channels) {
                if (at < searchEnd) { ++at; continue; }
                break;
            }
            p = file_.View(at,n);
            if (!p) return false;
            header_ = h;
            unsigned side = h.version == 3 ? (h.channels == 1 ? 17 : 32) : (h.channels == 1 ? 9 : 17);
            size_t xing = 4+(h.crc ? 2 : 0)+side;
            if (h.layer == 3 && !tagged && xing+8 <= h.bytes && (Is(p+xing,"Xing") || Is(p+xing,"Info"))) {
                unsigned flags = U32(p+xing+4,true);
                size_t next = xing+8;
                if (flags & 1) { if (next+4 > h.bytes) return false; encodedFrames = U32(p+next,true); next += 4; }
                if (flags & 2) {
                    if (next+4 > h.bytes) return false;
                    unsigned bytes = U32(p+next,true);
                    // Only a header whose declared bytes are present can defer indexing.
                    // Keep its sample count even after a full scan, so truncation is a failure.
                    streaming = encodedFrames && bytes >= h.bytes && bytes <= file_.Size()-at &&
                                encodedFrames <= (bytes-h.bytes)/4;
                    next += 4;
                }
                if (flags & 4) next += 100;
                if (flags & 8) next += 4;
                if (next+24 <= h.bytes && (memcmp(p+next,"LAME",4) == 0 || memcmp(p+next,"Lavc",4) == 0 || memcmp(p+next,"Lavf",4) == 0)) {
                    unsigned delay = (unsigned)p[next+21]*16+(p[next+22] >> 4);
                    padding = (unsigned)(p[next+22] & 15)*256+p[next+23];
                    if (padding >= 529) { skip_ = delay+529; padding -= 529; }
                }
                tagged = true; at += h.bytes; continue;
            }
        }
        if (h.rate != header_.rate || h.layer != header_.layer || h.version != header_.version || h.channels != header_.channels) break;
        offsets_.push_back(at); at += h.bytes;
        if (streaming) break;
    }
    if (offsets_.empty()) return false;
    indexAt_ = at;
    frameCount_ = encodedFrames ? encodedFrames : offsets_.size();
    offsets_.resize((size_t)std::min<uint64_t>(offsets_.size(),frameCount_));
    uint64_t available = frameCount_*header_.samples;
    total_ = available-std::min<uint64_t>(available,skip_+padding);
    target_ = skip_;
    reservoir_.reserve(4096);
    return total_ > 0;
}

// Playback extends the index one frame at a time; a first seek scans only as far
// as its target. Cached offsets keep later seeks exact without decoding the file.
bool MpegDecoder::IndexThrough(size_t frame) {
    if (frame >= frameCount_) return false;
    while (frame >= offsets_.size()) {
        if (indexAt_ >= file_.Size()) return false;
        size_t bytes = (size_t)std::min<uint64_t>(65536,file_.Size()-indexAt_);
        const uint8_t* p = file_.View(indexAt_,bytes);
        MpegHeader h;
        if (!p || !Header(p,bytes,h,freeSize_) || !h.bytes || h.rate != header_.rate ||
            h.layer != header_.layer || h.version != header_.version || h.channels != header_.channels) return false;
        offsets_.push_back(indexAt_);
        indexAt_ += h.bytes;
    }
    return true;
}

// Decode one indexed frame, carrying the Layer III bit reservoir across frames.
// A partial reservoir returns success with nothing buffered, which is how the
// decoder skips encoder delay/padding.
bool MpegDecoder::Frame() {
    if (failed_) return false;
    const uint8_t* p = nullptr;
    size_t size = 0;
    if (live_) {
        if (liveFirst_) liveFirst_ = false;
        else {
            uint8_t head[4];
            MpegHeader h;
            if (!ReadLiveBytes(*live_, head, sizeof(head)) || !Header(head, sizeof(head), h, 0, true) ||
                !h.bytes || h.rate != header_.rate || h.layer != header_.layer || h.version != header_.version ||
                h.channels != header_.channels) return false;
            liveFrame_.resize(h.bytes);
            memcpy(liveFrame_.data(), head, sizeof(head));
            if (!ReadLiveBytes(*live_, liveFrame_.data() + 4, h.bytes - 4)) return false;
        }
        p = liveFrame_.data();
        size = liveFrame_.size();
    } else {
        if (!IndexThrough(frame_)) return false;
        uint64_t offset = offsets_[frame_++];
        size = media_.matroska?media_.packets[(size_t)offset].bytes:(size_t)std::min<uint64_t>(65536,file_.Size()-offset);
        p = media_.matroska?media_.Data((size_t)offset):file_.View(offset,size);
    }
    CORDINAL_CORE_SCOPE;
    MpegHeader h;
    if (!p || !Header(p,size,h,freeSize_) || !h.bytes) return false;
    unsigned side = h.version == 3 ? (h.channels == 1 ? 17 : 32) : (h.channels == 1 ? 9 : 17);
    unsigned prefix = 4+(h.crc ? 2 : 0);
    if (prefix+(h.layer == 3 ? side : 0) > h.bytes) return false;
    uint64_t first = raw_; raw_ += h.samples;
    buffered_ = cursor_ = 0;
    // Layer III synthesis writes every sample of each channel it decodes.
    float channels[2][1152];
    if (h.layer != 3) {
        memset(channels,0,sizeof channels);
        MpegBits bits(p+prefix,h.bytes-prefix);
        if (!Layer12(bits,h,p,filters_,channels)) return false;
    } else {
    if (h.crc && !CheckCrc(p,p+prefix,side*8)) return false;
    MpegBits info(p+prefix,side);
    Granule gr[2][2];
    unsigned back = 0;
    if (!SideInfo(info,h,gr,back)) return false;
    bool enough = back <= reservoirBytes_;
    size_t retain = std::min<size_t>(back,reservoirBytes_), added = h.bytes-prefix-side;
    if (reservoir_.size() < retain+added+reservoirPad) reservoir_.resize(retain+added+reservoirPad);
    if (retain) memmove(reservoir_.data(),reservoir_.data()+reservoirBytes_-retain,retain);
    memcpy(reservoir_.data()+retain,p+prefix+side,added);
    reservoirBytes_ = retain+added;
    if (!enough) return live_ || raw_ <= target_;
    MpegBits b(reservoir_.data(),reservoirBytes_);
    for (unsigned g = 0; g < (h.version == 3 ? 2u : 1u); ++g) {
        for (unsigned c = 0; c < h.channels; ++c) {
            auto& v = gr[g][c];
            if (v.length > b.size*8-b.pos) return false;
            b.limit = b.pos+v.length;
            if (!ScaleFactors(b,h,v,gr[0][c],c) || !Spectral(b,h,v)) return false;
            Requantize(h,v);
        }
        Stereo(h,gr[g][0],gr[g][1]);
        for (unsigned c = 0; c < h.channels; ++c) filters_.Hybrid(gr[g][c],c,channels[c]+g*576);
    }
    }
    uint64_t begin = std::max(first,target_), end = std::min(raw_,skip_+total_);
    if (begin < end) {
        buffered_ = (size_t)(end-begin);
        size_t sample=(size_t)(begin-first);
        if (!InterleaveStereo(channels[0]+sample,channels[h.channels-1]+sample,pcm_,buffered_)) return false;
    }
    return true;
}
// Copy buffered PCM, decoding frames until the request is met or the stream ends.
size_t MpegDecoder::Read(float* out,size_t frames) {
    if (failed_) return 0;
    size_t done = 0;
    while (done < frames && pos_ < total_) {
        if (cursor_ == buffered_) {
            if (!Frame()) { failed_ = true; break; }
            if (!buffered_) continue;
        }
        size_t n = std::min(frames-done,buffered_-cursor_);
        memcpy(out+done*2,pcm_+cursor_*2,n*2*sizeof(float));
        cursor_ += n; pos_ += n; done += n;
    }
    return done;
}
// Reposition to the frame containing the target, rewinding a fixed number of
// frames so the reservoir refills before playback resumes.
bool MpegDecoder::Seek(double seconds) {
    if (live_) return false;
    if (!std::isfinite(seconds)) return false;
    uint64_t target = seconds <= 0 ? 0 : seconds >= Duration() ? total_ : (uint64_t)(seconds*header_.rate);
    uint64_t absolute = target+skip_;
    uint64_t frame = absolute/header_.samples;
    if (target < total_ && !IndexThrough((size_t)frame)) return false;
    target_ = absolute;
    frame_ = (size_t)(frame > 32 ? frame-32 : 0); raw_ = frame_*header_.samples;
    filters_.Reset(); reservoirBytes_ = 0; buffered_ = cursor_ = 0; failed_ = false; pos_ = target;
    return true;
}
} // namespace

struct nativeaudio::Polyphase::Impl { Filterbank filters; };
nativeaudio::Polyphase::Polyphase() : impl_(std::make_unique<Impl>()) {}
nativeaudio::Polyphase::~Polyphase() = default;
void nativeaudio::Polyphase::Reset() { impl_->filters.Reset(); }
void nativeaudio::Polyphase::Synthesize18(const float* in, unsigned channel, float* out) {
    impl_->filters.Subbands18(in, channel & 1, out);
}

std::unique_ptr<Decoder> OpenMpegDecoder(const std::wstring& path) {
    auto d = std::make_unique<MpegDecoder>();
    return d->Open(path) ? std::move(d) : nullptr;
}

std::unique_ptr<Decoder> OpenLiveMpeg(std::shared_ptr<LiveAudioSource> source) {
    auto d = std::make_unique<MpegDecoder>();
    return d->OpenLive(std::move(source)) ? std::move(d) : nullptr;
}
