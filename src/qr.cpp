// QR code encoder (qr.h). The standard's construction in order: the data bits, Reed-Solomon blocks
// interleaved, the function patterns, the zigzag placement, and the mask with the lowest penalty.
#include "qr.h"

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace qr {
namespace {
// Level M, by version (index 0 unused): check bytes in each block, and the number of blocks.
constexpr uint8_t kCheckPerBlock[41] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28,
    26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28};
constexpr uint8_t kBlocks[41] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17,
    18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49};

// Modules left for data and check bytes once a version's function patterns are drawn.
int RawModules(int version) {
    int n = (16 * version + 128) * version + 64;
    if (version >= 2) {
        int align = version / 7 + 2;
        n -= (25 * align - 10) * align - 55;
        if (version >= 7) n -= 36;
    }
    return n;
}
int DataBytes(int version) { return RawModules(version) / 8 - kCheckPerBlock[version] * kBlocks[version]; }

uint8_t Multiply(uint8_t a, uint8_t b) {
    int r = 0;
    for (int i = 7; i >= 0; --i) {
        r = (r << 1) ^ ((r >> 7) * 0x11D);
        r ^= ((b >> i) & 1) * a;
    }
    return (uint8_t)r;
}

long Penalty(const std::vector<uint8_t>& m, int size) {
    auto at = [&](int x, int y) { return m[(size_t)y * size + x]; };
    long score = 0;
    // Rows (pass 0) and columns: runs of five or more alike, and the finder look-alike 1:1:3:1:1 with
    // four light modules on one side (outside the symbol counts as light, as the quiet zone is).
    for (int pass = 0; pass < 2; ++pass) for (int i = 0; i < size; ++i) {
        auto get = [&](int j) { return pass ? at(i, j) : at(j, i); };
        for (int j = 0, run = 0; j < size; ++j) {
            run = j && get(j) == get(j - 1) ? run + 1 : 1;
            score += run == 5 ? 3 : run > 5 ? 1 : 0;
        }
        auto light = [&](int from, int to) {
            for (int k = std::max(from, 0); k < std::min(to, size); ++k) if (get(k)) return false;
            return true;
        };
        for (int j = 0; j + 7 <= size; ++j) {
            if (get(j) && !get(j + 1) && get(j + 2) && get(j + 3) && get(j + 4) && !get(j + 5) && get(j + 6) &&
                (light(j - 4, j) || light(j + 7, j + 11))) score += 40;
        }
    }
    long dark = 0, total = (long)size * size;
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        dark += at(x, y);
        if (x + 1 < size && y + 1 < size && at(x, y) == at(x + 1, y) && at(x, y) == at(x, y + 1) &&
            at(x, y) == at(x + 1, y + 1)) score += 3;
    }
    return score + ((std::labs(dark * 20 - total * 10) + total - 1) / total - 1) * 10;
}
}  // namespace

std::vector<uint8_t> CheckBytes(const std::vector<uint8_t>& data, int count) {
    // The generator is the product of (x - 2^i) for i below count; its leading 1 is implied.
    std::vector<uint8_t> divisor(count), result(count);
    divisor.back() = 1;
    for (int i = 0, root = 1; i < count; ++i, root = Multiply((uint8_t)root, 2))
        for (int j = 0; j < count; ++j) {
            divisor[j] = Multiply(divisor[j], (uint8_t)root);
            if (j + 1 < count) divisor[j] ^= divisor[j + 1];
        }
    for (uint8_t b : data) {
        uint8_t factor = b ^ result[0];
        result.erase(result.begin()); result.push_back(0);
        for (int i = 0; i < count; ++i) result[i] ^= Multiply(divisor[i], factor);
    }
    return result;
}

Code Encode(std::string_view text) {
    Code code;
    int version = 1;
    while (version <= 40 && 4 + (version < 10 ? 8 : 16) + (long)text.size() * 8 > DataBytes(version) * 8L) ++version;
    if (version > 40) return code;

    // Byte mode: the mode, the count, the bytes, up to four terminator bits, then alternating pad bytes.
    const int capacity = DataBytes(version);
    std::vector<uint8_t> data;
    uint32_t pending = 0;
    int bits = 0;
    auto put = [&](uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            pending = pending << 1 | ((value >> i) & 1);
            if (++bits == 8) { data.push_back((uint8_t)pending); pending = 0; bits = 0; }
        }
    };
    put(4, 4); put((uint32_t)text.size(), version < 10 ? 8 : 16);
    for (unsigned char c : text) put(c, 8);
    put(0, std::min(4, capacity * 8 - ((int)data.size() * 8 + bits)));
    if (bits) put(0, 8 - bits);
    for (uint8_t pad = 0xEC; (int)data.size() < capacity; pad ^= 0xEC ^ 0x11) data.push_back(pad);

    // Split into blocks (the later ones one byte longer), add each block's check bytes, interleave.
    const int blocks = kBlocks[version], check = kCheckPerBlock[version], raw = RawModules(version) / 8;
    const int shortBlocks = blocks - raw % blocks, shortLength = raw / blocks;
    std::vector<std::vector<uint8_t>> parts;
    for (int i = 0, at = 0; i < blocks; ++i) {
        int length = shortLength - check + (i < shortBlocks ? 0 : 1);
        std::vector<uint8_t> part(data.begin() + at, data.begin() + at + length);
        at += length;
        auto ecc = CheckBytes(part, check);
        if (i < shortBlocks) part.push_back(0);  // a gap that keeps every column aligned; skipped below
        part.insert(part.end(), ecc.begin(), ecc.end());
        parts.push_back(std::move(part));
    }
    std::vector<uint8_t> codewords;
    for (int i = 0; i <= shortLength; ++i)
        for (int j = 0; j < blocks; ++j)
            if (i != shortLength - check || j >= shortBlocks) codewords.push_back(parts[j][i]);

    const int size = version * 4 + 17;
    std::vector<uint8_t> dark((size_t)size * size), function((size_t)size * size);
    auto set = [&](int x, int y, bool on) { dark[(size_t)y * size + x] = on; function[(size_t)y * size + x] = 1; };
    for (int i = 0; i < size; ++i) { set(6, i, i % 2 == 0); set(i, 6, i % 2 == 0); }
    const int finders[3][2] = {{3, 3}, {size - 4, 3}, {3, size - 4}};
    for (auto [cx, cy] : finders)
        for (int dy = -4; dy <= 4; ++dy) for (int dx = -4; dx <= 4; ++dx) {
            int d = std::max(std::abs(dx), std::abs(dy)), x = cx + dx, y = cy + dy;
            if (x >= 0 && x < size && y >= 0 && y < size) set(x, y, d != 2 && d != 4);
        }
    std::vector<int> align;
    if (version > 1) {
        int count = version / 7 + 2, step = (version * 8 + count * 3 + 5) / (count * 4 - 4) * 2;
        for (int i = 0, pos = size - 7; i < count - 1; ++i, pos -= step) align.insert(align.begin(), pos);
        align.insert(align.begin(), 6);
    }
    const size_t last = align.empty() ? 0 : align.size() - 1;
    for (size_t i = 0; i < align.size(); ++i) for (size_t j = 0; j < align.size(); ++j) {
        if ((i == 0 && (j == 0 || j == last)) || (i == last && j == 0)) continue;  // the finders' corners
        for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx)
            set(align[i] + dx, align[j] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }
    auto format = [&](int mask) {
        int rem = mask;  // level M's two format bits are 00
        for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int value = (mask << 10 | rem) ^ 0x5412;
        auto bit = [&](int i) { return ((value >> i) & 1) != 0; };
        for (int i = 0; i <= 5; ++i) set(8, i, bit(i));
        set(8, 7, bit(6)); set(8, 8, bit(7)); set(7, 8, bit(8));
        for (int i = 9; i < 15; ++i) set(14 - i, 8, bit(i));
        for (int i = 0; i < 8; ++i) set(size - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i) set(8, size - 15 + i, bit(i));
        set(8, size - 8, true);
    };
    format(0);  // reserves the format modules before placement; redrawn with the chosen mask
    if (version >= 7) {
        int rem = version;
        for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        long value = (long)version << 12 | rem;
        for (int i = 0; i < 18; ++i) {
            bool on = ((value >> i) & 1) != 0;
            set(size - 11 + i % 3, i / 3, on); set(i / 3, size - 11 + i % 3, on);
        }
    }

    // Two-module columns from the right, upward then downward, skipping the vertical timing column.
    size_t bit = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; ++vert) for (int j = 0; j < 2; ++j) {
            int x = right - j, y = ((right + 1) & 2) == 0 ? size - 1 - vert : vert;
            size_t at = (size_t)y * size + x;
            if (!function[at] && bit < codewords.size() * 8) {
                dark[at] = (codewords[bit >> 3] >> (7 - (bit & 7))) & 1;
                ++bit;
            }
        }
    }
    auto flip = [&](int mask) {
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            bool invert = mask == 0 ? (x + y) % 2 == 0 : mask == 1 ? y % 2 == 0 : mask == 2 ? x % 3 == 0 :
                mask == 3 ? (x + y) % 3 == 0 : mask == 4 ? (x / 3 + y / 2) % 2 == 0 :
                mask == 5 ? x * y % 2 + x * y % 3 == 0 : mask == 6 ? (x * y % 2 + x * y % 3) % 2 == 0 :
                ((x + y) % 2 + x * y % 3) % 2 == 0;
            size_t at = (size_t)y * size + x;
            if (invert && !function[at]) dark[at] ^= 1;
        }
    };
    int best = 0;
    long lowest = LONG_MAX;
    for (int mask = 0; mask < 8; ++mask) {
        flip(mask); format(mask);
        long score = Penalty(dark, size);
        if (score < lowest) { lowest = score; best = mask; }
        flip(mask);
    }
    flip(best); format(best);
    code.size = size; code.modules = std::move(dark);
    return code;
}
}  // namespace qr
