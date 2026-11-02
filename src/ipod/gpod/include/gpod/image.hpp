// Image file decoding for artwork and photo input.
//
// The original library left pixels to gdk-pixbuf: itdb_artwork_import_artwork()
// read a file and asked GDK-Pixbuf to turn it into a GdkPixbuf.  The rewrite has
// no image library, so this module turns PNG and JPEG bytes into the gpod::Image
// (RGBA8) canvas that Thumb::image holds, using only the C++ standard library.
// image_png.cpp carries its own DEFLATE decompressor; nothing here links zlib,
// libjpeg or gdk-pixbuf.
//
// Supported PNG variants: color types 0 (gray), 2 (RGB), 3 (palette), 4
// (gray+alpha) and 6 (RGBA) at the bit depths the format gives them (1/2/4/8/16
// for gray and 1/2/4/8 for palette, 8/16 for the others), PLTE with tRNS,
// multiple IDAT chunks and Adam7 interlacing.  16-bit samples keep their high
// byte, which is PNG's own scaling to 8 bits (the low byte is dropped, not
// rounded); 1/2/4-bit samples are scaled up by the exact factor 255/max.
//
// Supported JPEG variants: baseline (SOF0) and extended sequential (SOF1)
// Huffman frames with 8-bit precision, one (grayscale) or three (YCbCr)
// components, sampling factors 1x1, 2x1, 1x2 and 2x2, arbitrary quantization and
// Huffman tables and restart intervals.  Chroma is upsampled by pixel
// replication, three-component frames are always treated as YCbCr (an Adobe
// "RGB" transform is ignored), and JFIF/EXIF orientation is ignored: no
// auto-rotation.
//
// Rejected (ErrorCode::Unsupported): progressive, lossless and arithmetic-coded
// JPEG, CMYK JPEG, JPEG sampling factors above 2 and non-interleaved baseline
// scans; PNG color types/bit depth combinations that the format does not define
// and PNG compression/filter/interlace methods other than 0, 0 and 0/1.  Both
// decoders reject images above 64 megapixels.

#ifndef GPOD_IMAGE_HPP
#define GPOD_IMAGE_HPP

#include <cstdint>
#include <filesystem>
#include <span>

#include "gpod/model.hpp"

namespace gpod {

enum class ImageFileFormat { Unknown, Png, Jpeg };

[[nodiscard]] const char* to_string(ImageFileFormat format) noexcept;

// Identify @bytes by its magic number.  Never throws: empty input and
// unrecognized data both give ImageFileFormat::Unknown, so a caller can sniff a
// file before deciding whether it is worth decoding.
[[nodiscard]] ImageFileFormat detect_image_format(std::span<const std::uint8_t> bytes) noexcept;

// Decode @bytes with the codec that detect_image_format() reports.  Throws
// ErrorCode::InvalidArgument for empty input and ErrorCode::Unsupported when the
// data is neither PNG nor JPEG; other failures come from the codec.
[[nodiscard]] Image decode_image(std::span<const std::uint8_t> bytes);

// Decode one PNG image into an RGBA8 canvas.  Throws ErrorCode::InvalidArgument
// for empty input, ErrorCode::Unsupported when the data is not a PNG at all or
// uses a variant listed above as unsupported, and ErrorCode::Corrupt for damaged
// data: truncated or oversized chunks, bad CRCs, a broken zlib stream, an
// unknown row filter or an out-of-range palette index.
[[nodiscard]] Image decode_png(std::span<const std::uint8_t> bytes);

// Decode one JPEG image into an RGBA8 canvas.  Throws ErrorCode::InvalidArgument
// for empty input, ErrorCode::Unsupported when the data is not a JPEG or uses an
// unsupported frame type, and ErrorCode::Corrupt for damaged data: malformed
// markers or segments, invalid Huffman codes, entropy data that ends early or
// missing/undefined tables.
[[nodiscard]] Image decode_jpeg(std::span<const std::uint8_t> bytes);

// Read @path and decode it.  The file is read with read_file(), so a missing
// path gives ErrorCode::NotFound and an unreadable one ErrorCode::Io; the
// decoding failures are the same as decode_image().
[[nodiscard]] Image load_image(const std::filesystem::path& path);

}  // namespace gpod

#endif  // GPOD_IMAGE_HPP
