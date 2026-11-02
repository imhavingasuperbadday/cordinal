// Minimal zlib/DEFLATE support used by compressed iTunesCDB databases.
//
// The library has no zlib dependency: this is a small RFC 1950/1951 codec.
// Compression emits stored DEFLATE blocks, which are valid zlib streams and
// keep the writer self-contained; decompression handles stored, fixed-Huffman
// and dynamic-Huffman streams.

#ifndef GPOD_ZLIB_HPP
#define GPOD_ZLIB_HPP

#include <cstddef>
#include <cstdint>
#include <span>

#include "gpod/io.hpp"

namespace gpod {

// Compress @data into a zlib stream.  The output is larger than the input by
// a few bytes per 64 KiB block, but every zlib implementation accepts it.
[[nodiscard]] Bytes zlib_compress(std::span<const std::uint8_t> data);

// Decompress a zlib stream.  Throws ErrorCode::Corrupt for malformed input and
// Unsupported for preset dictionaries or unknown compression methods.
[[nodiscard]] Bytes zlib_decompress(std::span<const std::uint8_t> data);

}  // namespace gpod

#endif  // GPOD_ZLIB_HPP
