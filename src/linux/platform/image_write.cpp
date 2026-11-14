// BMP and PNG writers for SDL_SaveBMP / SDL_SavePNG (screenshots and the --capture-ui pictures).
// PNG's zlib stream is our own: LZ77 over a 32 KiB window with hash chains, coded with the fixed
// Huffman tables of RFC 1951, which is small and good enough for screenshots.
#include "platform/platform.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace lx::platform {
namespace {

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t bits = 0;
    int count = 0;
    void Put(uint32_t value, int n) {  // least significant bit first
        bits |= value << count;
        count += n;
        while (count >= 8) {
            out.push_back((uint8_t)bits);
            bits >>= 8;
            count -= 8;
        }
    }
    void PutReversed(uint32_t code, int n) {  // Huffman codes go most significant bit first
        uint32_t r = 0;
        for (int i = 0; i < n; ++i) r |= ((code >> i) & 1) << (n - 1 - i);
        Put(r, n);
    }
    void Flush() {
        if (count > 0) out.push_back((uint8_t)bits);
        bits = 0;
        count = 0;
    }
};

void FixedLiteral(BitWriter& w, int sym) {
    if (sym < 144) w.PutReversed(0x30 + sym, 8);
    else if (sym < 256) w.PutReversed(0x190 + sym - 144, 9);
    else if (sym < 280) w.PutReversed(sym - 256, 7);
    else w.PutReversed(0xc0 + sym - 280, 8);
}

constexpr int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void Match(BitWriter& w, int len, int dist) {
    int l = 28;
    while (kLenBase[l] > len) --l;
    FixedLiteral(w, 257 + l);
    if (kLenExtra[l]) w.Put(len - kLenBase[l], kLenExtra[l]);
    int d = 29;
    while (kDistBase[d] > dist) --d;
    w.PutReversed(d, 5);
    if (kDistExtra[d]) w.Put(dist - kDistBase[d], kDistExtra[d]);
}

}  // namespace

std::vector<uint8_t> Deflate(const uint8_t* data, size_t n) {
    std::vector<uint8_t> out;
    BitWriter w{out};
    w.Put(1, 1);  // the last block
    w.Put(1, 2);  // fixed Huffman codes
    constexpr int kHashBits = 15, kWindow = 32768, kMaxChain = 32;
    std::vector<int32_t> head(1 << kHashBits, -1), prev(n ? n : 1, -1);
    auto hash = [&](size_t i) { return ((data[i] << 10) ^ (data[i + 1] << 5) ^ data[i + 2]) & ((1 << kHashBits) - 1); };
    size_t i = 0;
    while (i < n) {
        int bestLen = 0, bestDist = 0;
        if (i + 2 < n) {
            const int h = hash(i);
            int chain = kMaxChain;
            for (int32_t c = head[h]; c >= 0 && (int64_t)i - c <= kWindow && chain-- > 0; c = prev[c]) {
                const size_t max = std::min<size_t>(258, n - i);
                size_t len = 0;
                while (len < max && data[c + len] == data[i + len]) ++len;
                if ((int)len > bestLen) {
                    bestLen = (int)len, bestDist = (int)(i - c);
                    if (len == max) break;
                }
            }
        }
        const size_t step = bestLen >= 3 ? (size_t)bestLen : 1;
        if (bestLen >= 3) Match(w, bestLen, bestDist);
        else FixedLiteral(w, data[i]);
        for (size_t k = 0; k < step; ++k, ++i) {
            if (i + 2 < n) {
                const int h = hash(i);
                prev[i] = head[h];
                head[h] = (int32_t)i;
            }
        }
    }
    FixedLiteral(w, 256);
    w.Flush();
    return out;
}

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

namespace {

uint32_t Adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return b << 16 | a;
}

void Be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
}

void Chunk(std::vector<uint8_t>& file, const char* type, const std::vector<uint8_t>& body) {
    Be32(file, (uint32_t)body.size());
    const size_t start = file.size();
    file.insert(file.end(), type, type + 4);
    file.insert(file.end(), body.begin(), body.end());
    Be32(file, Crc32(file.data() + start, file.size() - start, 0));
}

bool WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        SetError("Couldn't open " + path + " for writing");
        return false;
    }
    const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (fclose(f) != 0 || !ok) {
        SetError("Couldn't write " + path);
        return false;
    }
    return true;
}

}  // namespace

bool WritePng(const std::string& path, int w, int h, const uint32_t* px, int stride, bool alpha) {
    const int channels = alpha ? 4 : 3;
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * channels));
    std::vector<uint8_t> row((size_t)w * channels), up((size_t)w * channels, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t p = px[(size_t)y * stride + x];
            uint8_t* o = &row[(size_t)x * channels];
            o[0] = (uint8_t)(p >> 16), o[1] = (uint8_t)(p >> 8), o[2] = (uint8_t)p;
            if (alpha) o[3] = (uint8_t)(p >> 24);
        }
        // The "up" filter: screenshots repeat rows a lot.
        raw.push_back(2);
        for (size_t k = 0; k < row.size(); ++k) raw.push_back((uint8_t)(row[k] - up[k]));
        up.swap(row);
    }
    std::vector<uint8_t> z{0x78, 0x01};
    std::vector<uint8_t> body = Deflate(raw.data(), raw.size());
    z.insert(z.end(), body.begin(), body.end());
    Be32(z, Adler32(raw.data(), raw.size()));
    std::vector<uint8_t> file{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    Be32(ihdr, (uint32_t)w);
    Be32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, (uint8_t)(alpha ? 6 : 2), 0, 0, 0});
    Chunk(file, "IHDR", ihdr);
    Chunk(file, "IDAT", z);
    Chunk(file, "IEND", {});
    return WriteFile(path, file);
}

// 24-bit bottom-up BMP, as SDL writes surfaces without alpha.
bool WriteBmp(const std::string& path, int w, int h, const uint32_t* px, int stride) {
    const uint32_t rowBytes = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + rowBytes * (uint32_t)h;
    std::vector<uint8_t> f(size, 0);
    auto le32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) f[at + i] = (uint8_t)(v >> (8 * i));
    };
    f[0] = 'B', f[1] = 'M';
    le32(2, size);
    le32(10, 54);
    le32(14, 40);
    le32(18, (uint32_t)w);
    le32(22, (uint32_t)h);
    f[26] = 1, f[28] = 24;
    le32(34, rowBytes * (uint32_t)h);
    le32(38, 2835), le32(42, 2835);
    for (int y = 0; y < h; ++y) {
        uint8_t* o = &f[54 + (size_t)(h - 1 - y) * rowBytes];
        for (int x = 0; x < w; ++x) {
            const uint32_t p = px[(size_t)y * stride + x];
            o[3 * x] = (uint8_t)p, o[3 * x + 1] = (uint8_t)(p >> 8), o[3 * x + 2] = (uint8_t)(p >> 16);
        }
    }
    return WriteFile(path, f);
}

}  // namespace lx::platform
