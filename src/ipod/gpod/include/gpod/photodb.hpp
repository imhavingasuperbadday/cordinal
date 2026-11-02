// Pure C++ entry points for PhotoDB parsing, writing and photo album handling.
//
// This is the port of upstream itdb_photoalbum.c, the photo parts of
// itdb_artwork.c / db-image-parser.c / db-artwork-parser.c and the pixel
// packing from ithumb-writer.c.
//
// Ownership follows model.hpp: PhotoDb owns photos and photo albums, album
// members are non-owning Artwork pointers that must belong to the same
// PhotoDb.  Anything that would break that rule (null, foreign or already
// owned objects) throws gpod::Error with ErrorCode::InvalidArgument instead
// of silently doing nothing.

#ifndef GPOD_PHOTODB_HPP
#define GPOD_PHOTODB_HPP

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gpod/io.hpp"
#include "gpod/model.hpp"

namespace gpod {

// Database.  parse() returns a PhotoDb holding the photos and albums stored on
// the device; when the device has a Photos directory but no Photo database yet
// it returns a fresh database with the "Photo Library" album, matching
// itdb_photodb_parse().  A missing Photos directory is ErrorCode::NotFound and
// malformed data is ErrorCode::Corrupt.
//
// write() renumbers photos and albums the way iPods expect, encodes the .ithmb
// thumbnails of photos that only carry pixels, and leaves the thumbnails of
// photos read from the device untouched.  Encoding uses the device's photo
// formats, so a database whose device description has none keeps those pixels
// in memory and writes them on the first save that has a format available.
[[nodiscard]] std::unique_ptr<PhotoDb> parse_photodb(const std::filesystem::path& mountpoint);
void write_photodb(PhotoDb& photodb);

// Photo albums (itdb_photoalbum.c).  New albums get album_type 2 ("normal
// album"); the first album of a database is the "Photo Library" and uses 1.
[[nodiscard]] PhotoAlbum* photoalbum_new(std::string name);
[[nodiscard]] PhotoAlbum* photodb_photoalbum_create(
    PhotoDb& db, std::string name, std::optional<std::size_t> pos = std::nullopt);
void photodb_photoalbum_add(PhotoDb& db, std::unique_ptr<PhotoAlbum> album,
                            std::optional<std::size_t> pos = std::nullopt);
void photodb_photoalbum_remove(PhotoDb& db, PhotoAlbum* album, bool remove_pics);
// Removes the album from its PhotoDb and destroys it; the reference is invalid
// afterwards.  Use photodb_photoalbum_detach() to keep the album alive.
void photodb_photoalbum_unlink(PhotoAlbum& album);
[[nodiscard]] std::unique_ptr<PhotoAlbum> photodb_photoalbum_detach(PhotoAlbum& album);
[[nodiscard]] PhotoAlbum* photodb_photoalbum_by_name(PhotoDb& db, std::string_view name) noexcept;
// The master album: the head of the album list, matching
// itdb_photodb_photoalbum_by_name (db, NULL).
[[nodiscard]] PhotoAlbum* photodb_photo_library(PhotoDb& db) noexcept;

// Photos (itdb_photoalbum.c).  The Image overload takes already decoded RGBA
// pixels: `image.rgba` must hold width * height * 4 bytes; an unusable picture
// or a rotation other than 0/90/180/270 is ErrorCode::InvalidArgument.  The
// path and byte overloads decode PNG or JPEG data with gpod::load_image() /
// gpod::decode_image() first and report their errors unchanged.
// The photo is added to the database and to the Photo Library album, which is
// created when it does not exist yet.
[[nodiscard]] Artwork* photodb_add_photo(PhotoDb& db, Image image,
                                         std::optional<std::size_t> pos = std::nullopt,
                                         int rotation = 0);
[[nodiscard]] Artwork* photodb_add_photo(PhotoDb& db,
                                         const std::filesystem::path& filename,
                                         std::optional<std::size_t> pos = std::nullopt,
                                         int rotation = 0);
[[nodiscard]] Artwork* photodb_add_photo_from_data(
    PhotoDb& db, std::span<const std::uint8_t> image_data,
    std::optional<std::size_t> pos = std::nullopt, int rotation = 0);
void photodb_photoalbum_add_photo(PhotoDb& db, PhotoAlbum& album, Artwork& photo,
                                  std::optional<std::size_t> pos = std::nullopt);
// With a null album, or with the Photo Library album, the photo is removed from
// every album and from the database (destroying it).  With another album it is
// only unlinked from that album.
void photodb_remove_photo(PhotoDb& db, PhotoAlbum* album, Artwork* photo);

// Artwork and thumbnails (photo parts of itdb_artwork.c).  rotation is in
// degrees counter-clockwise: 0, 90, 180 or 270.
[[nodiscard]] Artwork* artwork_new();
void artwork_remove_thumbnails(Artwork& artwork);
void artwork_set_rotation(Artwork& artwork, int rotation);
void artwork_set_thumbnail(Artwork& artwork, Image image, int rotation);
// Largest .ithmb item of the thumbnail, padded dimensions included.  Empty when
// the artwork has no on-device thumbnail (for example an unsaved new photo).
[[nodiscard]] std::optional<std::pair<int, int>> artwork_photo_size(const Artwork& artwork);

namespace detail {

// In-memory .ithmb payloads keyed by the bare name stored in mhni records
// ("F1013_1.ithmb").  `dirty` lists the files that received pixels in this run
// and therefore have to be written back.
struct ThumbFiles {
    std::map<std::string, Bytes> data;
    std::set<std::string> dirty;
};

// Packs `image` for every format with a known encoder, appends the pixels to
// `files` and adds one IpodThumbItem per format to `thumb`.  Formats whose
// pixel layout this port cannot encode (UYVY, I420, recorded 555, ...) are
// skipped rather than written incorrectly.  Used by write_photodb(); not part
// of the stable API.
void encode_thumbnail(Thumb& thumb, const Image& image,
                      const std::vector<ArtworkFormat>& formats, ThumbFiles& files);

}  // namespace detail

}  // namespace gpod

#endif  // GPOD_PHOTODB_HPP
