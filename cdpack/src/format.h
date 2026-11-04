// Versioned CDP1 framing shared by the independent packer and freestanding loader.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace cdp {
struct Header {
    uint8_t magic[8];
    uint32_t version, metadata_bytes, metadata_adler;
    uint32_t memory_rva, memory_bytes, compressed_offset, compressed_bytes, memory_adler;
    uint32_t file_bytes, native_header_bytes, gap_count, copy_count, tls_index_rva, chunk_count;
    uint8_t file_sha256[32];
};
struct ResourceCopy {
    uint32_t source, destination, bytes;
};
// Version 2 decodes the memory image as independent streams, one per chunk, in parallel. `output`
// is relative to memory_rva and `input` to compressed_offset; `adler` covers the chunk's filtered
// bytes. Chunks are in output order, contiguous and cover the whole image.
struct Chunk {
    uint32_t output, output_bytes, input, input_bytes, adler;
};
constexpr uint32_t version = 2, max_chunks = 8;
// Per decoding thread: one model set in its own slot after the 4096-byte state page.
constexpr uint32_t workspace_slot = 32768;
struct Anchor {
    uint8_t magic[16];
    uint32_t self, metadata, metadata_bytes, workspace;
};
static_assert(sizeof(Header) == 96 && sizeof(Anchor) == 32 && sizeof(ResourceCopy) == 12 && sizeof(Chunk) == 20);

inline uint16_t read16(const uint8_t *p) { return uint16_t(p[0] | unsigned(p[1]) << 8); }
inline uint32_t read32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t read64(const uint8_t *p) { return read32(p) | uint64_t(read32(p + 4)) << 32; }
inline void write32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = uint8_t(v >> (8 * i));
}
inline void write64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = uint8_t(v >> (8 * i));
}
inline uint32_t adler32(const uint8_t *p, size_t bytes, size_t skip = SIZE_MAX) {
    uint32_t a = 1, b = 0;
    for (size_t start = 0; start < bytes;) {
        const size_t chunk = bytes - start < 5552 ? bytes - start : 5552;
        const size_t end = start + chunk;
        if (skip == SIZE_MAX) {
            // Four byte contributions can be summed in parallel. The original 5552-byte
            // bound still prevents overflow of both sums before their modular reduction.
            while (end - start >= 4) {
                const uint32_t x0 = p[start], x1 = p[start + 1], x2 = p[start + 2],
                               x3 = p[start + 3];
                b += 4 * a + 4 * x0 + 3 * x1 + 2 * x2 + x3;
                a += x0 + x1 + x2 + x3;
                start += 4;
            }
        }
        while (start < end) {
            a += start >= skip && start - skip < 4 ? 0 : p[start];
            b += a;
            ++start;
        }
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}
} // namespace cdp
