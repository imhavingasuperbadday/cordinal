// The classic-iPod subset cordinal compiles does not include ArtworkDB, PhotoDB or SQLite.
//
// libgpod's model and database code still reference those entry points. Keeping the
// stubs here makes the omission explicit: artwork is left untouched on the device,
// and an iOS/SQLite database is reported as unsupported rather than half-written.

#include "gpod/artworkdb.hpp"
#include "gpod/error.hpp"
#include "gpod/photodb.hpp"
#include "gpod/sqlite.hpp"

namespace gpod {

void read_artworkdb(Itdb&) {}
void write_artworkdb(Itdb&) {}
void materialize_artwork_thumbnails(Itdb&) {}

std::unique_ptr<PhotoDb> parse_photodb(const std::filesystem::path&) {
    fail(ErrorCode::Unsupported, "PhotoDB support is not compiled into cordinal");
}

void write_photodb(PhotoDb&) {
    fail(ErrorCode::Unsupported, "PhotoDB support is not compiled into cordinal");
}

void generate_sqlite_databases(Itdb&) {
    fail(ErrorCode::Unsupported, "SQLite shadow databases are not compiled into cordinal");
}

}  // namespace gpod
