// SQLite shadow databases for Nano 5G / iPod Touch / iPhone devices.
//
// Port of upstream src/itdb_sqlite.c together with the schema strings from
// src/itdb_sqlite_queries.h.  Devices that read a compressed iTunesCDB also
// read a set of SQLite files from "iPod_Control/iTunes/iTunes Library.itlp":
//
//   Library.itdb        items, containers, album/artist/composer tables and
//                       the genre/location-kind maps (the main database)
//   Dynamic.itdb        playback statistics and per-playlist UI state
//   Extras.itdb         chapter data blobs
//   Genius.itdb         empty Genius tables (upstream never fills these in)
//   Locations.itdb      where every file lives on the device
//   Locations.itdb.cbk  SHA-1 block checksums over Locations.itdb
//
// This port needs neither SQLite headers nor a link dependency: sqlite.cpp
// loads the platform SQLite at run time (winsqlite3.dll, then sqlite3.dll on
// Windows; libsqlite3.so.0, libsqlite3.so, libsqlite3.dylib elsewhere) and
// calls it through a private function pointer table.  sqlite_available()
// reports whether that succeeded; the generators throw
// Error(ErrorCode::Unsupported) when it did not.
//
// Ordering requirement: the album, artist and composer columns of Library.itdb
// are filled from Track::album_id, Track::artist_id and Track::composer_id.
// The iTunesDB writer assigns those ids while it prepares a database, so write
// (or at least prepare) the iTunesDB before generating the shadow databases.
// Generated from an unprepared model the three columns stay 0 and the device
// cannot resolve albums, artists or composers.
//
// Fidelity: the schema, insert order and inserted values follow upstream.  Two
// upstream defects are deliberately not reproduced, because they lose data
// this port has: video_info.sort_series_name is bound on the statement it
// belongs to instead of on the item statement (upstream leaves it NULL), and
// failed statements raise ErrorCode::Sqlite instead of being printed and
// ignored.  The libimobiledevice lockdown path for post process commands is
// out of scope; the SysInfoExtended source for those commands is implemented.

#ifndef GPOD_SQLITE_HPP
#define GPOD_SQLITE_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gpod {

class Itdb;

// Whether a SQLite library could be loaded from the platform search path.  The
// answer is cached after the first call.
[[nodiscard]] bool sqlite_available() noexcept;

// Directory the generators write into: <mountpoint>/iPod_Control/iTunes/
// "iTunes Library.itlp".  The directory is not required to exist.
[[nodiscard]] std::filesystem::path sqlite_directory(const std::filesystem::path& mountpoint);

// Generate all six files below the device mount point, creating the
// "iTunes Library.itlp" directory when it is missing.  Files are built in a
// temporary directory and copied into place, like upstream, so a failure
// leaves the previous database untouched.
//
// Throws Error(ErrorCode::InvalidArgument) when @itdb has no mount point,
// Error(ErrorCode::Unsupported) when no SQLite library is available and
// Error(ErrorCode::Io)/Error(ErrorCode::Sqlite) when writing or querying
// fails.  A checksum failure keeps the code the checksum routine reported.
void generate_sqlite_databases(Itdb& itdb);

// Same, but writes the six files directly into @output_directory (which is
// created when missing) without needing a device.  This is the entry point for
// tests and for copying a shadow database elsewhere.
void generate_sqlite_databases(Itdb& itdb, const std::filesystem::path& output_directory);

// One column value of a query result: a string for any non-NULL value (numbers
// in their SQLite text form, blobs as their raw bytes) and nullopt for NULL.
using SqliteCell = std::optional<std::string>;
using SqliteRow = std::vector<SqliteCell>;

// Run a read-only statement against @database through the internal loader and
// return every row.  Used by the tests and by callers that want to inspect a
// generated shadow database without linking SQLite themselves.
//
// Throws Error(ErrorCode::Unsupported) when no SQLite library is available,
// Error(ErrorCode::NotFound) when the file is missing and ErrorCode::Sqlite
// when the statement fails to compile or run.
[[nodiscard]] std::vector<SqliteRow> sqlite_query(const std::filesystem::path& database,
                                                  std::string_view sql);

}  // namespace gpod

#endif  // GPOD_SQLITE_HPP
