// ArtworkDB reading and writing.
//
// The ArtworkDB is the index of the .ithmb thumbnail files on the iPod.  It
// holds one mhii record per track that has artwork, and each mhii lists the
// thumbnail sizes of that artwork (mhod -> mhni), together with the .ithmb
// file name, offset and pixel size of every thumbnail.  This module ports
// db-artwork-parser.c and db-artwork-writer.c; the pixels themselves stay in
// the .ithmb files.
//
// The writer assigns artwork ids and packs decoded Thumb::image pixels into the
// cover-art .ithmb files before indexing them.  Thumbnails that are already on
// the device (ThumbSource::Ipod) keep their pixels and offsets; no image-file
// decoder is part of the library, so File/Memory thumbnails need the caller to
// provide an RGBA Thumb::image.

#ifndef GPOD_ARTWORKDB_HPP
#define GPOD_ARTWORKDB_HPP

#include <filesystem>

#include "gpod/model.hpp"

namespace gpod {

// Read the ArtworkDB that belongs to @itdb (iPod_Control/Artwork/ArtworkDB)
// and attach the artwork records to its tracks by dbid and mhii_link.  Tracks
// that receive artwork also get has_artwork, artwork_count and artwork_size
// refreshed to describe it, matching itdb_track_set_thumbnails(); upstream
// leaves those fields to the iTunesDB reader.
//
// Throws ErrorCode::NotFound when the device has no ArtworkDB, ErrorCode::Corrupt
// for malformed or truncated records and ErrorCode::InvalidArgument when the
// database has no mountpoint.  Upstream silently ignores a device that reports no
// artwork support; callers that want that can check Device::supports_artwork().
void read_artworkdb(Itdb& itdb);

// Same, for an explicit file.  The byte order is taken from the mhfd header
// rather than from Itdb::device, because a reversed header identifies a
// big-endian database on its own.
void read_artworkdb_file(Itdb& itdb, const std::filesystem::path& filename);

// Rewrite iPod_Control/Artwork/ArtworkDB for @itdb, creating the Artwork
// directory when needed.  Tracks without thumbnails lose their mhii_link.
// Artwork supplied as a decoded Thumb::image is first packed into the
// device's cover-art .ithmb files; thumbnails that are already on the iPod
// keep their existing pixels and offsets.
void write_artworkdb(Itdb& itdb);

// Assign artwork ids and pack every new Thumb::image into the cover-art
// .ithmb files.  Thumbnails stored as a file name or encoded image_data need an
// image decoder, which this dependency-free library does not include: callers
// must set Thumb::image (decoding stays with the application).  Thumbnails
// without an image keep whatever iPod items they already have.
void materialize_artwork_thumbnails(Itdb& itdb);

// Same, into an explicit file.  Artwork ids are assigned before writing: ids
// are numbered from 0x64 in track order, and identical artwork within one album
// shares a single record when the device supports sparse artwork.  Thumbnail
// items without an ArtworkFormat are skipped because they cannot be described.
void write_artworkdb_file(Itdb& itdb, const std::filesystem::path& filename);

}  // namespace gpod

#endif  // GPOD_ARTWORKDB_HPP
