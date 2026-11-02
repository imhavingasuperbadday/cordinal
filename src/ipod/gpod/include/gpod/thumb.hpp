// Thumbnail (.ithmb) pixel formats.
//
// Port of the pixel handling that upstream libgpod spread over
// db-image-parser.{c,h} (the packed word layout), itdb_artwork.c (the unpack_*
// decoders plus itdb_thumb_ipod_item_to_pixbuf) and ithumb-writer.c (the pack_*
// encoders and the .ithmb file layout).  gdk-pixbuf is gone: callers hand in and
// receive RGBA gpod::Image data, and image-file decoding (PNG/JPEG) stays with
// the caller.  See thumb.cpp for the format quirks that are preserved.

#ifndef GPOD_THUMB_HPP
#define GPOD_THUMB_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "gpod/io.hpp"
#include "gpod/model.hpp"
#include "gpod/types.hpp"

namespace gpod {

// Byte order of the multi-byte words stored inside an .ithmb pixel buffer.
[[nodiscard]] ByteOrder thumb_byte_order(ThumbFormat format) noexcept;

// Pixels per stored row, including the ArtworkFormat::row_bytes_alignment
// padding that the original writer applied to the 16bpp formats.
[[nodiscard]] int ithmb_row_pixels(const ArtworkFormat& format) noexcept;

// Bytes of pixel data a thumbnail of @format occupies (the size stored in Mhni).
[[nodiscard]] std::size_t ithmb_pixel_bytes(const ArtworkFormat& format) noexcept;

// Bytes one thumbnail occupies in its .ithmb file: ArtworkFormat::padding when
// that is non-zero (the original pads every slot up to it), the pixel bytes
// otherwise.
[[nodiscard]] std::uint32_t ithmb_slot_bytes(const ArtworkFormat& format) noexcept;

// Decode a packed buffer into a format.width x format.height RGBA canvas.  The
// canvas width is ithmb_row_pixels() for the 16bpp formats, so the row-alignment
// columns stay addressable and are decoded as the background colour the writer
// stored there.  Buffers shorter than the packed pixels are rejected (the
// original read past the end of them); I420 accepts a plain 1.5*w*h frame as well
// as the 2*w*h slot the original writer reported.
[[nodiscard]] Image decode_ithmb(std::span<const std::uint8_t> pixels,
                                 const ArtworkFormat& format);

// Encode @image into a packed buffer of ithmb_pixel_bytes().  A smaller image is
// centred on the format canvas and the rest of the canvas is filled with
// ArtworkFormat::back_color (transparent black for the YUV formats, like the
// original).  @rotation is in degrees counterclockwise (0, 90, 180, 270) and is
// applied before placing the image, matching itdb_artwork_set_thumbnail().
[[nodiscard]] Bytes encode_ithmb(const Image& image, const ArtworkFormat& format,
                                 unsigned rotation = 0);

// Same as encode_ithmb(), but places @image at the item's stored
// horizontal/vertical padding instead of centring it.  Negative padding clips
// the image, positive padding leaves background around it.
[[nodiscard]] Bytes encode_ithmb_item(const Image& image, const IpodThumbItem& item,
                                      unsigned rotation = 0);

// Decode an item's packed pixels and crop them to item.width/item.height, undoing
// the padding/truncation described by the Mhni header (see
// itdb_thumb_ipod_item_to_pixbuf).  Returns an empty Image when the stored crop
// rectangle does not overlap the canvas.
[[nodiscard]] Image decode_ithmb_item(std::span<const std::uint8_t> pixels,
                                      const IpodThumbItem& item);

// Rotate counterclockwise by 0, 90, 180 or 270 degrees.  90/270 swap the
// dimensions.
[[nodiscard]] Image rotate_image(const Image& image, unsigned degrees);

// Bilinear scale to exactly @width x @height pixels.
[[nodiscard]] Image scale_image(const Image& image, int width, int height);

// Rotate @image and scale it into @format's box, reproducing
// ithumb_writer_handle_pixbuf_transform(): the result fits inside the box, or
// fills and centre-crops it when ArtworkFormat::crop is set.
[[nodiscard]] Image fit_image(const Image& image, const ArtworkFormat& format,
                              unsigned rotation = 0);

// Copy the rectangle out of @image; the rectangle must lie inside it.
[[nodiscard]] Image crop_image(const Image& image, int x, int y, int width, int height);

// Resolve the .ithmb file that stores @item on @device.  The stored name is
// ":"-separated: artwork thumbnails ("F1007_1.ithmb") are found under
// iPod_Control/Artwork, photo thumbnails (":Thumbs:F1007_1.ithmb") under
// iPod_Control/Photos/Thumbs.
[[nodiscard]] std::filesystem::path ithmb_item_path(const Device& device,
                                                    const IpodThumbItem& item);

// Read @item's pixels from the device and decode them.
[[nodiscard]] Image read_ithmb_item(const Device& device, const IpodThumbItem& item);

// Options for write_ithmb_item().  The 256 MB cap matches upstream's
// ITHUMB_MAX_SIZE: once a file reaches it, the next index is used.
struct IthmbWriteOptions {
    std::uint64_t max_file_size = 256'000'000;
};

// Append @image to the item's .ithmb file, filling the slot up to
// ithmb_slot_bytes().  Updates item.offset/size/width/height/padding and gives
// the item a default "F<format id>_1.ithmb" name when it has none.  Returns the
// number of pixel bytes written.  @device needs a mountpoint; the artwork or
// photo thumbnail directory is created when it is missing.  The file index is
// advanced when the current file has reached @options.max_file_size.
[[nodiscard]] std::uint32_t write_ithmb_item(const Device& device, IpodThumbItem& item,
                                             const Image& image, unsigned rotation = 0,
                                             IthmbWriteOptions options = {});

}  // namespace gpod

#endif  // GPOD_THUMB_HPP
