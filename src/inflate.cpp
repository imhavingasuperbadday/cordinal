// Inflate for gzip/DEFLATE, used to decompress VGZ game-music files and zipped packs (inflate.h). All three DEFLATE
// block types (stored, fixed Huffman, dynamic Huffman) are decoded. Every output is bounded by
// maxOut, so a corrupt or hostile file cannot exhaust memory.
#include "inflate.h"

#include <algorithm>

namespace {

// Deflate packs bits starting from the least significant bit of each byte.
struct Bits {
    const unsigned char* p;
    size_t n;
    size_t pos = 0;
    uint32_t buf = 0;
    int have = 0;
    bool overrun = false;

    uint32_t Take(int count) {
        while (have < count) {
            if (pos >= n) {
                overrun = true;
                return 0;
            }
            buf |= (uint32_t)p[pos++] << have;
            have += 8;
        }
        const uint32_t v = buf & ((1u << count) - 1);
        buf >>= count;
        have -= count;
        return v;
    }
    // Drops the rest of the current byte. Whole bytes are only loaded on demand, so fewer
    // than 8 bits are ever left over.
    void ToByte() {
        buf = 0;
        have = 0;
    }
};

// A canonical Huffman code: how many codes there are of each length, and the symbols sorted
// by code. Codes of one length are consecutive, so decoding walks length by length.
constexpr int kMaxBits = 15;
struct Code {
    uint16_t perLength[kMaxBits + 1];
    uint16_t sorted[288];
};

bool Build(Code& c, const uint8_t* lengths, int count) {
    std::fill(std::begin(c.perLength), std::end(c.perLength), (uint16_t)0);
    for (int i = 0; i < count; ++i) ++c.perLength[lengths[i]];
    c.perLength[0] = 0;
    int unused = 1;  // codes still free at the current length
    for (int len = 1; len <= kMaxBits; ++len) {
        unused = unused * 2 - c.perLength[len];
        if (unused < 0) return false;  // more codes than the lengths allow
    }
    uint16_t next[kMaxBits + 1] = {};
    for (int len = 1; len < kMaxBits; ++len) next[len + 1] = (uint16_t)(next[len] + c.perLength[len]);
    for (int i = 0; i < count; ++i)
        if (lengths[i]) c.sorted[next[lengths[i]]++] = (uint16_t)i;
    return true;
}

// Reads one symbol, or -1 for a code the table doesn't have or running out of input.
int Decode(Bits& in, const Code& c) {
    int code = 0;   // bits read so far, most significant first
    int first = 0;  // first code of the current length
    int index = 0;  // where that length's symbols start in `sorted`
    for (int len = 1; len <= kMaxBits; ++len) {
        code |= (int)in.Take(1);
        if (in.overrun) return -1;
        const int count = c.perLength[len];
        if (code - first < count) return c.sorted[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

// Match lengths for symbols 257..285 and distances for symbols 0..29 (RFC 1951 section 3.2.5).
constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// The order code-length code lengths are stored in a dynamic block header.
constexpr uint8_t kLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// Expands the literal/length symbol stream into `out` until the end-of-block symbol. False on a
// bad symbol or distance, a match reaching before the start of the output, or any output past maxOut.
bool Symbols(Bits& in, const Code& lit, const Code& dist, size_t maxOut, std::string& out) {
    for (;;) {
        int sym = Decode(in, lit);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= maxOut) return false;
            out += (char)sym;
            continue;
        }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        const size_t length = kLengthBase[sym] + in.Take(kLengthExtra[sym]);
        const int d = Decode(in, dist);
        if (d < 0 || d >= 30) return false;
        const size_t back = kDistBase[d] + in.Take(kDistExtra[d]);
        if (in.overrun || back > out.size() || length > maxOut - out.size()) return false;
        // Byte by byte: a match may overlap the bytes it is producing.
        size_t from = out.size() - back;
        for (size_t i = 0; i < length; ++i) out += out[from + i];
    }
}

// Stored (uncompressed) block: a length, its complement, then that many raw bytes.
bool Stored(Bits& in, size_t maxOut, std::string& out) {
    in.ToByte();
    if (in.n - in.pos < 4) return false;
    const unsigned char* h = in.p + in.pos;
    const unsigned len = h[0] | h[1] << 8, inverse = h[2] | h[3] << 8;
    in.pos += 4;
    if ((len ^ 0xFFFF) != inverse || in.n - in.pos < len || len > maxOut - out.size()) return false;
    out.append((const char*)in.p + in.pos, len);
    in.pos += len;
    return true;
}

// Fixed-Huffman block: the tables RFC 1951 prescribes, built once and shared.
bool Fixed(Bits& in, size_t maxOut, std::string& out) {
    static const struct Tables {
        Code lit, dist;
        Tables() {
            uint8_t l[288];
            std::fill(l, l + 144, (uint8_t)8);
            std::fill(l + 144, l + 256, (uint8_t)9);
            std::fill(l + 256, l + 280, (uint8_t)7);
            std::fill(l + 280, l + 288, (uint8_t)8);
            Build(lit, l, 288);
            uint8_t d[30];
            std::fill(d, d + 30, (uint8_t)5);
            Build(dist, d, 30);
        }
    } tables;
    return Symbols(in, tables.lit, tables.dist, maxOut, out);
}

// Dynamic-Huffman block: the code lengths are themselves run-length coded, then two canonical codes
// (literal/length and distance) are built from them.
bool Dynamic(Bits& in, size_t maxOut, std::string& out) {
    const int litCount = (int)in.Take(5) + 257, distCount = (int)in.Take(5) + 1, lenCount = (int)in.Take(4) + 4;
    if (in.overrun || litCount > 286 || distCount > 30) return false;
    uint8_t lengths[286 + 30] = {};
    for (int i = 0; i < lenCount; ++i) lengths[kLengthOrder[i]] = (uint8_t)in.Take(3);
    Code lenCode;
    if (in.overrun || !Build(lenCode, lengths, 19)) return false;

    // The literal and distance code lengths, run-length coded as one sequence.
    std::fill(std::begin(lengths), std::end(lengths), (uint8_t)0);
    const int total = litCount + distCount;
    for (int i = 0; i < total;) {
        const int sym = Decode(in, lenCode);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        uint8_t value = 0;
        int repeat;
        if (sym == 16) {
            if (i == 0) return false;
            value = lengths[i - 1];
            repeat = 3 + (int)in.Take(2);
        } else if (sym == 17) {
            repeat = 3 + (int)in.Take(3);
        } else {
            repeat = 11 + (int)in.Take(7);
        }
        if (in.overrun || i + repeat > total) return false;
        std::fill(lengths + i, lengths + i + repeat, value);
        i += repeat;
    }
    if (lengths[256] == 0) return false;  // a block has to be able to end
    Code lit, dist;
    if (!Build(lit, lengths, litCount) || !Build(dist, lengths + litCount, distCount)) return false;
    return Symbols(in, lit, dist, maxOut, out);
}

uint32_t Le32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// The blocks of one deflate stream, up to and including the one marked last.
bool Blocks(Bits& bits, size_t maxOut, std::string& out) {
    bool last = false;
    while (!last) {
        last = bits.Take(1) != 0;
        const uint32_t type = bits.Take(2);
        if (bits.overrun) return false;
        bool ok = type == 0 ? Stored(bits, maxOut, out) : type == 1 ? Fixed(bits, maxOut, out)
                            : type == 2                ? Dynamic(bits, maxOut, out)
                                                       : false;
        if (!ok) return false;
    }
    return true;
}

}  // namespace

bool Inflate(const unsigned char* in, size_t n, size_t maxOut, std::string& out) {
    out.clear();
    Bits bits{in, n};
    return Blocks(bits, maxOut, out);
}

uint32_t Crc32(const unsigned char* p, size_t n, uint32_t crc) {
    static const struct Table {
        uint32_t v[256];
        Table() {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
                v[i] = c;
            }
        }
    } table;
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table.v[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool IsGzip(const unsigned char* in, size_t n) { return n >= 2 && in[0] == 0x1F && in[1] == 0x8B; }

bool Gunzip(const unsigned char* in, size_t n, size_t maxOut, std::string& out) {
    out.clear();
    // Header: magic, method 8 (deflate), flags, time, extra flags, OS.
    if (n < 18 || !IsGzip(in, n) || in[2] != 8 || (in[3] & 0xE0)) return false;
    const unsigned flags = in[3];
    size_t pos = 10;
    if (flags & 4) {  // FEXTRA: a length, then that many bytes
        if (n - pos < 2) return false;
        pos += 2 + (in[pos] | in[pos + 1] << 8);
    }
    for (unsigned field : {8u, 16u}) {  // FNAME and FCOMMENT: zero-terminated text
        if (!(flags & field)) continue;
        while (pos < n && in[pos]) ++pos;
        ++pos;
    }
    if (flags & 2) pos += 2;  // FHCRC
    if (pos >= n) return false;

    // The trailer's size is a hint for reserving; it is checked again at the end.
    const uint32_t expectSize = Le32(in + n - 4);
    if (expectSize <= maxOut) out.reserve(expectSize);

    Bits bits{in + pos, n - pos};
    if (!Blocks(bits, maxOut, out)) return false;
    bits.ToByte();
    const size_t end = pos + bits.pos;
    if (n - end < 8) return false;
    return Le32(in + end) == Crc32((const unsigned char*)out.data(), out.size()) && Le32(in + end + 4) == (uint32_t)out.size();
}
