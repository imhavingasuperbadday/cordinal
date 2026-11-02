// Pure C++ entry points for iTunesDB parsing and writing.
//
// The reader and writer are split into separate translation units.  Callers
// normally use Itdb::parse / Itdb::write; these free functions exist so the
// format code can be tested in isolation.

#ifndef GPOD_ITUNESDB_HPP
#define GPOD_ITUNESDB_HPP

#include <filesystem>
#include <memory>

#include "gpod/model.hpp"

namespace gpod {

[[nodiscard]] std::unique_ptr<Itdb> parse_itunesdb(const std::filesystem::path& mountpoint);
[[nodiscard]] std::unique_ptr<Itdb> parse_itunesdb_file(const std::filesystem::path& filename);

// Serialize to @filename.  A device that supports compressed databases gets an
// iTunesCDB image there; no other file (SQLite, iTunesSD, iTunesDB) is touched.
// Use Itdb::write() for a complete mount-point sync.
void write_itunesdb(Itdb& itdb, const std::filesystem::path& filename);

// Write through the device mount point: pick iTunesCDB or iTunesDB, refresh the
// Shuffle iTunesSD and empty a stale iTunesDB on compressed devices.  Album,
// artist and composer ids are assigned as a side effect.  SQLite shadow
// databases are written by Itdb::write(), not here.
void write_itunesdb(Itdb& itdb);

}  // namespace gpod

#endif  // GPOD_ITUNESDB_HPP
