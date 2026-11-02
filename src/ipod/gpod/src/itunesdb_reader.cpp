// iTunesDB reader: the parse half of upstream src/itdb_itunesdb.c.
//
// The parser walks the on-disk hunk tree (mhbd > mhsd > mhlt/mhlp > mhit/mhyp >
// mhod) and fills an Itdb; writing lives in itunesdb_writer.cpp.
//
// Byte order is detected from the mhbd fourcc.  Databases for some mobile
// devices store every hunk reversed, in which case all reads are swapped.  The
// smart playlist (SLst) and chapter (sean/chap/name) sections are always stored
// big-endian relative to the surrounding database, so they are read with the
// opposite order.
//
// Unknown hunks and MHODs are skipped by their declared length.  Structural
// damage (bad fourcc, zero-length hunk, read past the end of the file) raises
// ErrorCode::Corrupt.  Recent play counts ("Play Counts", "iTunesStats",
// "PlayCounts.plist") and On-The-Go playlists are advisory: failing to read
// them never fails the library, because the iPod regenerates both.
//
// Not implemented here: the album (mhsd 6) and artist (mhsd 8) sections and the
// library playlist index MHODs (52/53).  Upstream ignores them as well, because
// write_itunesdb() rebuilds them.

#include "gpod/itunesdb.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gpod/error.hpp"
#include "gpod/hash.hpp"
#include "gpod/io.hpp"
#include "gpod/paths.hpp"
#include "gpod/plist.hpp"
#include "gpod/smartplaylist.hpp"
#include "gpod/time.hpp"
#include "gpod/types.hpp"
#include "gpod/zlib.hpp"

namespace gpod {
namespace {

// Play Counts entries use -1 to say "the iPod did not record a rating".
constexpr std::int32_t kNoPlaycount = -1;

// Minimum header sizes accepted from the on-disk format.  A hunk whose header
// is shorter than this cannot contain the fields the parser reads from it.
constexpr std::uint32_t kMhbdMinHeader = 32;
constexpr std::uint32_t kMhsdMinHeader = 32;
constexpr std::uint32_t kMhitMinHeader = 0x9c;
constexpr std::uint32_t kMhypMinHeader = 48;
constexpr std::uint32_t kMhipMinHeader = 36;
constexpr std::uint32_t kMhodMinHeader = 16;
constexpr std::uint32_t kMhbdCompressionFlag = 0xa8;

// MHOD identifiers.  New fields are added by Apple without changing the
// database version, so unknown ids must be skipped rather than rejected.
enum MhodId : std::uint32_t {
    kMhodTitle = 1,
    kMhodPath = 2,
    kMhodAlbum = 3,
    kMhodArtist = 4,
    kMhodGenre = 5,
    kMhodFiletype = 6,
    kMhodComment = 8,
    kMhodCategory = 9,
    kMhodComposer = 12,
    kMhodGrouping = 13,
    kMhodDescription = 14,
    kMhodPodcastUrl = 15,
    kMhodPodcastRss = 16,
    kMhodChapterData = 17,
    kMhodSubtitle = 18,
    kMhodTvShow = 19,
    kMhodTvEpisode = 20,
    kMhodTvNetwork = 21,
    kMhodAlbumArtist = 22,
    kMhodSortArtist = 23,
    kMhodKeywords = 24,
    kMhodSortTitle = 27,
    kMhodSortAlbum = 28,
    kMhodSortAlbumArtist = 29,
    kMhodSortComposer = 30,
    kMhodSortTvShow = 31,
    kMhodSmartPrefs = 50,
    kMhodSmartRules = 51,
    kMhodLibraryPlaylistIndex = 52,
    kMhodLibraryJumpTable = 53,
    kMhodPlaylist = 100,
    kMhodAlbumName = 200,
    kMhodAlbumArtistName = 201,
    kMhodAlbumSortArtist = 202,
    kMhodAlbumArtistMhii = 300,
};

[[noreturn]] void corrupt(const std::filesystem::path& filename, std::string detail) {
    fail(ErrorCode::Corrupt,
         "iTunesDB corrupt ('" + filename.string() + "'): " + std::move(detail));
}

// Byte reader for one database file.  Every accessor bounds-checks and reports
// damage as ErrorCode::Corrupt with the offset, so call sites do not repeat the
// boilerplate that upstream spread over check_seek() and the CHECK_ERROR macro.
class Db {
public:
    Db(Bytes data, std::filesystem::path filename)
        : data_(std::move(data)), filename_(std::move(filename)) {}

    void set_order(ByteOrder order) noexcept { order_ = order; }
    [[nodiscard]] ByteOrder order() const noexcept { return order_; }
    [[nodiscard]] const std::filesystem::path& filename() const noexcept { return filename_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_.data(); }

    void replace_data(Bytes data) { data_ = std::move(data); }

    [[nodiscard]] bool has(std::size_t pos, std::size_t len) const noexcept {
        return pos <= data_.size() && len <= data_.size() - pos;
    }

    void require(std::size_t pos, std::size_t len) const {
        if (has(pos, len)) {
            return;
        }
        fail(ErrorCode::Corrupt,
             "iTunesDB '" + filename_.string() + "' is truncated: read of " +
                 std::to_string(len) + " byte(s) at offset " + std::to_string(pos) +
                 " but the file is " + std::to_string(data_.size()) + " bytes");
    }

    // Compares a fourcc, honouring the database byte order: reversed databases
    // store "mhbd" as "dbhm".
    [[nodiscard]] bool matches(std::size_t pos, std::string_view fourcc) const {
        if (fourcc.size() != 4 || !has(pos, 4)) {
            return false;
        }
        for (std::size_t i = 0; i < 4; ++i) {
            const char expected =
                order_ == ByteOrder::Little ? fourcc[i] : fourcc[3 - i];
            if (static_cast<char>(data_[pos + i]) != expected) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::uint8_t u8(std::size_t pos) const {
        require(pos, 1);
        return ByteReader(data_, order_).u8(pos);
    }

    [[nodiscard]] std::uint16_t u16(std::size_t pos) const {
        require(pos, 2);
        return ByteReader(data_, order_).u16(pos);
    }

    [[nodiscard]] std::uint32_t u24(std::size_t pos) const {
        require(pos, 3);
        return ByteReader(data_, order_).u24(pos);
    }

    [[nodiscard]] std::uint32_t u32(std::size_t pos) const {
        require(pos, 4);
        return ByteReader(data_, order_).u32(pos);
    }

    [[nodiscard]] std::uint64_t u64(std::size_t pos) const {
        require(pos, 8);
        return ByteReader(data_, order_).u64(pos);
    }

    // Little-endian read regardless of the database order; the iTunesCDB
    // compression header keeps the plain little-endian layout.
    [[nodiscard]] std::uint32_t u32le(std::size_t pos) const {
        require(pos, 4);
        return ByteReader(data_, order_).u32le(pos);
    }

    [[nodiscard]] float f32(std::size_t pos) const {
        require(pos, 4);
        return ByteReader(data_, order_).f32(pos);
    }

    // Reads in the opposite order of the surrounding database.  The smart
    // playlist and chapter sections are stored this way.
    [[nodiscard]] std::uint16_t swapped_u16(std::size_t pos) const {
        require(pos, 2);
        const ByteReader reader(data_, order_);
        return order_ == ByteOrder::Little ? reader.u16be(pos) : reader.u16le(pos);
    }

    [[nodiscard]] std::uint32_t swapped_u32(std::size_t pos) const {
        require(pos, 4);
        const ByteReader reader(data_, order_);
        return order_ == ByteOrder::Little ? reader.u32be(pos) : reader.u32le(pos);
    }

    [[nodiscard]] std::uint64_t swapped_u64(std::size_t pos) const {
        require(pos, 8);
        const ByteReader reader(data_, order_);
        return order_ == ByteOrder::Little ? reader.u64be(pos) : reader.u64le(pos);
    }

    [[nodiscard]] std::u16string utf16(std::size_t pos, std::size_t units) const {
        require(pos, units * 2);
        return ByteReader(data_, order_).utf16(pos, units);
    }

    [[nodiscard]] std::u16string swapped_utf16(std::size_t pos, std::size_t units) const {
        require(pos, units * 2);
        const ByteReader reader(data_, order_);
        return order_ == ByteOrder::Little ? reader.utf16be(pos, units)
                                           : reader.utf16le(pos, units);
    }

    [[nodiscard]] std::string_view raw(std::size_t pos, std::size_t len) const {
        require(pos, len);
        return {reinterpret_cast<const char*>(data_.data() + pos), len};
    }

private:
    Bytes data_;
    std::filesystem::path filename_;
    ByteOrder order_ = ByteOrder::Little;
};

[[nodiscard]] std::u16string cut_at_nul(std::u16string value) {
    if (const auto nul = value.find(u'\0'); nul != std::u16string::npos) {
        value.resize(nul);
    }
    return value;
}

[[nodiscard]] std::string cut_at_nul(std::string value) {
    if (const auto nul = value.find('\0'); nul != std::string::npos) {
        value.resize(nul);
    }
    return value;
}

// True for well-formed UTF-8 without embedded NULs.  Upstream rejects MHOD
// strings that are not valid UTF-8 instead of passing garbage to the caller.
[[nodiscard]] bool is_utf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<std::uint8_t>(value[i]);
        if (lead == 0) {
            return false;
        }
        std::size_t extra = 0;
        std::uint32_t code_point = 0;
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        if ((lead & 0xe0U) == 0xc0U) {
            code_point = lead & 0x1fU;
            extra = 1;
        } else if ((lead & 0xf0U) == 0xe0U) {
            code_point = lead & 0x0fU;
            extra = 2;
        } else if ((lead & 0xf8U) == 0xf0U) {
            code_point = lead & 0x07U;
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= value.size()) {
            return false;
        }
        for (std::size_t j = 1; j <= extra; ++j) {
            const auto continuation = static_cast<std::uint8_t>(value[i + j]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6) | (continuation & 0x3fU);
        }
        if (code_point > 0x10ffffU || (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

[[nodiscard]] bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto l = static_cast<unsigned char>(left[i]);
        const auto r = static_cast<unsigned char>(right[i]);
        if (std::tolower(l) != std::tolower(r)) {
            return false;
        }
    }
    return true;
}

// Resolves one path component the way upstream's itdb_resolve_path() does for a
// single element: a vfat volume can keep a different case than the name the
// code was given.
[[nodiscard]] std::optional<std::filesystem::path> find_existing(
    const std::filesystem::path& directory, std::string_view name) {
    std::error_code error;
    const std::filesystem::path direct = directory / std::filesystem::path(std::string(name));
    if (std::filesystem::is_regular_file(direct, error) && !error) {
        return direct;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        if (!entry.is_regular_file(error)) {
            continue;
        }
        if (ascii_iequals(entry.path().filename().string(), name)) {
            return entry.path();
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::uintmax_t file_size_or_zero(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

[[nodiscard]] bool is_otg_header(const Db& db) { return db.matches(0, "mhpo"); }

[[nodiscard]] bool is_playcounts_header(const Db& db) { return db.matches(0, "mhdp"); }

// A track position inside a playlist (MHOD type 100) together with the track
// it belongs to; the mhips are stored newest-first and sorted before use.
struct PosEntry {
    std::uint32_t track_id = 0;
    std::int32_t track_pos = 0;
};

// One entry of the "Play Counts" or "iTunesStats" file.
struct PlayCount {
    std::uint32_t playcount = 0;
    std::uint32_t skipcount = 0;
    std::time_t time_played = 0;
    std::time_t last_skipped = 0;
    std::uint32_t bookmark_time = 0;
    std::int32_t rating = kNoPlaycount;
};

struct MhodHeader {
    std::uint32_t type = 0;
    std::uint32_t length = 0;
    std::uint32_t header_length = 0;
};

// Decoded MHOD payload.  Only the member matching the mhod type is filled in.
struct MhodData {
    std::uint32_t type = 0xffffffffU;
    std::uint32_t length = 0;
    bool valid = false;
    std::string text;
    std::int32_t track_pos = 0;
    std::unique_ptr<ChapterData> chapters;
    std::optional<SmartPlaylistPrefs> prefs;
    std::optional<SmartPlaylistRules> rules;
};

class Parser {
public:
    Parser(Bytes data, std::filesystem::path filename, std::unique_ptr<Itdb> itdb,
           bool compressed)
        : db_(std::move(data), std::move(filename)),
          itdb_(std::move(itdb)),
          compressed_(compressed) {}

    [[nodiscard]] std::unique_ptr<Itdb> run() {
        detect_byte_order();
        if (compressed_) {
            decompress();
        }
        itdb_->filename = db_.filename();
        read_recent_playcounts();
        parse_database();
        read_otg_playlists();
        return std::move(itdb_);
    }

private:
    // --- header and hunk helpers -------------------------------------------

    void detect_byte_order() {
        db_.require(0, 4);
        if (db_.matches(0, "mhbd")) {
            db_.set_order(ByteOrder::Little);
            return;
        }
        db_.set_order(ByteOrder::Big);
        if (db_.matches(0, "mhbd")) {
            return;
        }
        fail(ErrorCode::Corrupt,
             "not an iTunesDB: '" + db_.filename().string() + "' has no mhbd header");
    }

    void decompress() {
        const std::uint32_t header_len = db_.u32le(4);
        const std::uint32_t total_len = db_.u32le(8);
        if (header_len < kMhbdCompressionFlag + 1) {
            corrupt(db_.filename(), "header is too small for iTunesCDB (" +
                                         std::to_string(header_len) + " bytes)");
        }
        if (total_len < header_len || total_len > db_.size()) {
            corrupt(db_.filename(), "iTunesCDB size " + std::to_string(total_len) +
                                         " is outside the file");
        }
        Bytes inflated;
        try {
            inflated = zlib_decompress(
                std::span<const std::uint8_t>(db_.data() + header_len,
                                              total_len - header_len));
        } catch (const Error& error) {
            corrupt(db_.filename(), "iTunesCDB could not be decompressed: " +
                                         std::string(error.what()));
        }
        Bytes combined(db_.data(), db_.data() + header_len);
        // The stored copy is uncompressed now; the writer decides the flag again.
        combined[kMhbdCompressionFlag] = 0;
        combined.insert(combined.end(), inflated.begin(), inflated.end());
        db_.replace_data(std::move(combined));
    }

    [[nodiscard]] std::optional<std::size_t> find_mhsd(std::uint32_t want) {
        std::uint32_t header_len = db_.u32(4);
        if (header_len < kMhsdMinHeader) {
            corrupt(db_.filename(), "header length of mhsd hunk smaller than expected (" +
                                         std::to_string(header_len) + " < " +
                                         std::to_string(kMhsdMinHeader) + ")");
        }
        const std::uint32_t children = db_.u32(20);
        std::size_t seek = 0;
        for (std::uint32_t i = 0; i < children; ++i) {
            seek += header_len;
            if (!db_.matches(seek, "mhsd")) {
                corrupt(db_.filename(), "mhsd expected at offset " + std::to_string(seek));
            }
            header_len = db_.u32(seek + 8);
            if (header_len == 0) {
                corrupt(db_.filename(),
                        "hunk length 0 for the mhsd at offset " + std::to_string(seek));
            }
            if (db_.u32(seek + 12) == want) {
                return seek;
            }
        }
        return std::nullopt;
    }

    // Finds the next occurrence of @fourcc between the start of @container and
    // @start by walking hunk lengths.  Returns nothing when the container ends
    // first, which is not an error (the caller decides what a missing hunk
    // means).
    [[nodiscard]] std::optional<std::size_t> find_next_in(std::size_t container,
                                                          std::size_t start,
                                                          std::string_view fourcc) {
        const auto total = static_cast<std::int64_t>(db_.u32(container + 8));
        const auto begin = static_cast<std::int64_t>(start);
        if (begin < static_cast<std::int64_t>(container)) {
            corrupt(db_.filename(), "search for '" + std::string(fourcc) +
                                         "' starts before its parent hunk");
        }
        auto offset = begin - static_cast<std::int64_t>(container);
        std::size_t pos = start;
        do {
            const std::uint32_t length = db_.u32(pos + 4);
            if (length == 0) {
                corrupt(db_.filename(),
                        "hunk length 0 for hunk at offset " + std::to_string(pos));
            }
            offset += static_cast<std::int64_t>(length);
            pos = container + static_cast<std::size_t>(offset);
        } while (offset < total - 4 && !db_.matches(pos, fourcc));
        if (offset >= total) {
            return std::nullopt;
        }
        return pos;
    }

    void require_hunk(std::size_t pos, std::string_view fourcc) {
        if (!db_.has(pos, 4)) {
            db_.require(pos, 4);
        }
        if (!db_.matches(pos, fourcc)) {
            corrupt(db_.filename(), "'" + std::string(fourcc) + "' expected at offset " +
                                         std::to_string(pos));
        }
    }

    // --- database body ------------------------------------------------------

    void parse_database() {
        const std::uint32_t header_len = db_.u32(4);
        if (header_len < kMhbdMinHeader) {
            corrupt(db_.filename(), "header length of mhbd hunk smaller than expected (" +
                                         std::to_string(header_len) + " < " +
                                         std::to_string(kMhbdMinHeader) + ")");
        }
        const std::uint32_t children = db_.u32(20);

        const auto mhsd1 = find_mhsd(1);
        const auto mhsd2 = find_mhsd(2);
        const auto mhsd3 = find_mhsd(3);
        const auto mhsd5 = find_mhsd(5);
        const auto mhsd9 = find_mhsd(9);

        db_.require(0x10, 4);
        itdb_->version = db_.u32(0x10);
        db_.require(0x18, 8);
        itdb_->id = db_.u64(0x18);

        // Optional header fields.  Old databases declare a 0x68 byte header and
        // simply do not contain the newer fields; reading them from the
        // following hunk (what upstream does) would invent values.
        const auto present = [&](std::size_t offset, std::size_t len) {
            return offset + len <= header_len && db_.has(offset, len);
        };
        if (present(0x20, 2)) {
            itdb_->platform = db_.u16(0x20);
        }
        if (present(0x22, 2)) {
            itdb_->unk_0x22 = db_.u16(0x22);
        }
        if (present(0x24, 8)) {
            itdb_->id_0x24 = db_.u64(0x24);
        }
        if (present(0x46, 2)) {
            itdb_->language = db_.u16(0x46);
        }
        if (present(0x48, 8)) {
            itdb_->pid = db_.u64(0x48);
        }
        if (present(0x50, 4)) {
            itdb_->unk_0x50 = static_cast<std::int32_t>(db_.u32(0x50));
        }
        if (present(0x54, 4)) {
            itdb_->unk_0x54 = static_cast<std::int32_t>(db_.u32(0x54));
        }
        if (present(0x6c, 4)) {
            itdb_->tzoffset = static_cast<std::int32_t>(db_.u32(0x6c));
        }
        if (present(0xa0, 2)) {
            itdb_->audio_language = static_cast<std::int16_t>(db_.u16(0xa0));
        }
        if (present(0xa2, 2)) {
            itdb_->subtitle_language = static_cast<std::int16_t>(db_.u16(0xa2));
        }
        if (present(0xa4, 2)) {
            itdb_->unk_0xa4 = static_cast<std::int16_t>(db_.u16(0xa4));
        }
        if (present(0xa6, 2)) {
            itdb_->unk_0xa6 = static_cast<std::int16_t>(db_.u16(0xa6));
        }
        if (present(0xa8, 2)) {
            itdb_->unk_0xa8 = static_cast<std::int16_t>(db_.u16(0xa8));
        }

        itdb_->device->byte_order = db_.order();

        if (!mhsd1) {
            if (children == 0) {
                // A database skeleton without sections holds no tracks.
                return;
            }
            corrupt(db_.filename(),
                    "could not find tracklist (no mhsd type 1 section found)");
        }

        parse_tracks(*mhsd1);
        if (mhsd3) {
            parse_playlists(*mhsd3);
        } else if (mhsd2) {
            parse_playlists(*mhsd2);
        } else {
            corrupt(db_.filename(),
                    "could not find playlists (no mhsd type 2 or 3 section found)");
        }
        if (mhsd5) {
            parse_playlists(*mhsd5);
        }
        if (mhsd9) {
            parse_genius(*mhsd9);
        }
    }

    void parse_tracks(std::size_t mhsd) {
        require_hunk(mhsd, "mhsd");
        const auto mhlt = find_next_in(mhsd, mhsd, "mhlt");
        if (!mhlt) {
            corrupt(db_.filename(), "no section 'mhlt' found in section 'mhsd' at offset " +
                                         std::to_string(mhsd));
        }
        const std::uint32_t declared = db_.u32(*mhlt + 8);
        auto seek = find_next_in(mhsd, *mhlt, "mhit");
        for (std::uint32_t i = 0; i < declared; ++i) {
            if (!seek) {
                // Fewer mhits than the mhlt announced: keep what was read, the
                // same way upstream continues after warning.
                break;
            }
            seek = parse_mhit(*seek);
        }
    }

    void parse_playlists(std::size_t mhsd) {
        require_hunk(mhsd, "mhsd");
        const std::uint32_t mhsd_type = db_.u32(mhsd + 12);
        const auto mhlp = find_next_in(mhsd, mhsd, "mhlp");
        if (!mhlp) {
            corrupt(db_.filename(), "no section 'mhlp' found in section 'mhsd' at offset " +
                                         std::to_string(mhsd));
        }
        const std::uint32_t declared = db_.u32(*mhlp + 8);

        id_tree_.clear();
        for (const auto& track : itdb_->tracks) {
            id_tree_.insert_or_assign(track->id, track.get());
        }
        auto seek = find_next_in(mhsd, *mhlp, "mhyp");
        for (std::uint32_t i = 0; i < declared; ++i) {
            if (!seek) {
                break;
            }
            seek = parse_mhyp(mhsd_type, *seek);
        }
        id_tree_.clear();
    }

    void parse_genius(std::size_t mhsd) {
        require_hunk(mhsd, "mhsd");
        const std::uint32_t header_len = db_.u32(mhsd + 4);
        const std::uint32_t total_len = db_.u32(mhsd + 8);
        if (total_len < header_len) {
            corrupt(db_.filename(), "genius section at offset " + std::to_string(mhsd) +
                                         " is shorter than its header");
        }
        const std::uint32_t length = total_len - header_len;
        itdb_->genius_cuid = cut_at_nul(std::string(db_.raw(mhsd + header_len, length)));
    }

    // --- mhit ---------------------------------------------------------------

    [[nodiscard]] std::time_t mac_to_time(std::uint64_t value) const {
        return mac_time_to_unix(value, itdb_->device->timezone_shift);
    }

    std::size_t parse_mhit(std::size_t mhit) {
        require_hunk(mhit, "mhit");
        const std::uint32_t header_len = db_.u32(mhit + 4);
        if (header_len < kMhitMinHeader) {
            corrupt(db_.filename(), "header length of 'mhit' smaller than expected (" +
                                         std::to_string(header_len) + " < " +
                                         std::to_string(kMhitMinHeader) + ")");
        }
        db_.require(mhit, header_len);

        const std::uint32_t mhod_count = db_.u32(mhit + 12);
        auto track = std::make_unique<Track>();

        track->id = db_.u32(mhit + 16);
        track->visible = db_.u32(mhit + 20);
        track->filetype_marker = db_.u32(mhit + 24);
        track->type1 = db_.u8(mhit + 28);
        track->type2 = db_.u8(mhit + 29);
        track->compilation = db_.u8(mhit + 30);
        track->rating = db_.u8(mhit + 31);
        track->time_modified = mac_to_time(db_.u32(mhit + 32));
        track->size = db_.u32(mhit + 36);
        track->tracklen = static_cast<std::int32_t>(db_.u32(mhit + 40));
        track->track_nr = static_cast<std::int32_t>(db_.u32(mhit + 44));
        track->tracks = static_cast<std::int32_t>(db_.u32(mhit + 48));
        track->year = static_cast<std::int32_t>(db_.u32(mhit + 52));
        track->bitrate = static_cast<std::int32_t>(db_.u32(mhit + 56));
        const std::uint32_t sample = db_.u32(mhit + 60);
        track->samplerate = static_cast<std::uint16_t>(sample >> 16);
        track->samplerate_low = static_cast<std::uint16_t>(sample & 0xffffU);
        track->volume = static_cast<std::int32_t>(db_.u32(mhit + 64));
        track->starttime = db_.u32(mhit + 68);
        track->stoptime = db_.u32(mhit + 72);
        track->soundcheck = db_.u32(mhit + 76);
        track->playcount = db_.u32(mhit + 80);
        track->playcount2 = db_.u32(mhit + 84);
        track->time_played = mac_to_time(db_.u32(mhit + 88));
        track->cd_nr = static_cast<std::int32_t>(db_.u32(mhit + 92));
        track->cds = static_cast<std::int32_t>(db_.u32(mhit + 96));
        track->drm_userid = db_.u32(mhit + 100);
        track->time_added = mac_to_time(db_.u32(mhit + 104));
        track->bookmark_time = db_.u32(mhit + 108);
        track->dbid = db_.u64(mhit + 112);
        track->checked = db_.u8(mhit + 120);
        track->app_rating = db_.u8(mhit + 121);
        track->bpm = static_cast<std::int16_t>(db_.u16(mhit + 122));
        track->artwork_count = db_.u16(mhit + 124);
        track->unk126 = db_.u16(mhit + 126);
        track->artwork_size = db_.u32(mhit + 128);
        track->unk132 = db_.u32(mhit + 132);
        track->samplerate2 = db_.f32(mhit + 136);
        track->time_released = mac_to_time(db_.u32(mhit + 140));
        track->unk144 = db_.u16(mhit + 144);
        track->explicit_flag = db_.u16(mhit + 146);
        track->unk148 = db_.u32(mhit + 148);
        track->unk152 = db_.u32(mhit + 152);

        if (header_len >= 0xf4) {
            track->skipcount = db_.u32(mhit + 156);
            track->last_skipped =
                static_cast<std::uint32_t>(mac_to_time(db_.u32(mhit + 160)));
            track->has_artwork = db_.u8(mhit + 164);
            track->skip_when_shuffling = db_.u8(mhit + 165);
            track->remember_playback_position = db_.u8(mhit + 166);
            track->flag4 = db_.u8(mhit + 167);
            track->dbid2 = db_.u64(mhit + 168);
            track->lyrics_flag = db_.u8(mhit + 176);
            track->movie_flag = db_.u8(mhit + 177);
            track->mark_unplayed = db_.u8(mhit + 178);
            track->unk179 = db_.u8(mhit + 179);
            track->unk180 = db_.u32(mhit + 180);
            track->pregap = db_.u32(mhit + 184);
            track->samplecount = db_.u64(mhit + 188);
            track->unk196 = db_.u32(mhit + 196);
            track->postgap = db_.u32(mhit + 200);
            track->unk204 = db_.u32(mhit + 204);
            track->mediatype = static_cast<Mediatype>(db_.u32(mhit + 208));
            track->season_nr = db_.u32(mhit + 212);
            track->episode_nr = db_.u32(mhit + 216);
            track->unk220 = db_.u32(mhit + 220);
            track->unk224 = db_.u32(mhit + 224);
            track->unk228 = db_.u32(mhit + 228);
            track->unk232 = db_.u32(mhit + 232);
            track->unk236 = db_.u32(mhit + 236);
            track->unk240 = db_.u32(mhit + 240);
        }
        if (header_len >= 0x148) {
            track->unk244 = db_.u32(mhit + 244);
            track->gapless_data = db_.u32(mhit + 248);
            track->unk252 = db_.u32(mhit + 252);
            track->gapless_track_flag = db_.u16(mhit + 256);
            track->gapless_album_flag = db_.u16(mhit + 258);
        }
        if (header_len >= 0x184) {
            track->mhii_link = db_.u32(mhit + 352);
        }
        // The album/artist/composer ids are rebuilt by the writer, but keeping
        // the stored values preserves round-trips of an unmodified database.
        if (header_len >= 0x124) {
            track->album_id = db_.u32(mhit + 0x120);
        }
        if (header_len >= 0x1e4) {
            track->artist_id = db_.u32(mhit + 0x1e0);
        }
        if (header_len >= 0x1f8) {
            track->composer_id = db_.u32(mhit + 0x1f4);
        }

        // Everything read from an iTunesDB is by definition already on the iPod.
        track->transferred = true;

        std::size_t seek = mhit + header_len;
        for (std::uint32_t i = 0; i < mhod_count; ++i) {
            const std::optional<MhodHeader> header = try_mhod_header(seek);
            if (!header) {
                break;
            }
            if (header->type == kMhodChapterData) {
                MhodData mhod = read_mhod(seek);
                if (mhod.valid && mhod.chapters) {
                    track->chapterdata = std::move(mhod.chapters);
                }
            } else {
                assign_track_string(*track, header->type, seek);
            }
            seek += header->length;
        }

        apply_playcount(*track);
        itdb_->add_track(std::move(track));
        return seek;
    }

    // Assigns the UTF-16 MHOD string at @pos to the matching track field.
    // MHODs that do not carry a track string are ignored.
    void assign_track_string(Track& track, std::uint32_t type, std::size_t pos) {
        switch (type) {
            case kMhodTitle:
            case kMhodPath:
            case kMhodAlbum:
            case kMhodArtist:
            case kMhodGenre:
            case kMhodFiletype:
            case kMhodComment:
            case kMhodCategory:
            case kMhodComposer:
            case kMhodGrouping:
            case kMhodDescription:
            case kMhodPodcastUrl:
            case kMhodPodcastRss:
            case kMhodSubtitle:
            case kMhodTvShow:
            case kMhodTvEpisode:
            case kMhodTvNetwork:
            case kMhodAlbumArtist:
            case kMhodKeywords:
            case kMhodSortArtist:
            case kMhodSortTitle:
            case kMhodSortAlbum:
            case kMhodSortAlbumArtist:
            case kMhodSortComposer:
            case kMhodSortTvShow:
                break;
            default:
                return;
        }

        MhodData mhod = read_mhod(pos);
        if (!mhod.valid) {
            return;
        }
        switch (type) {
            case kMhodTitle:
                track.title = std::move(mhod.text);
                break;
            case kMhodPath:
                track.ipod_path = std::move(mhod.text);
                break;
            case kMhodAlbum:
                track.album = std::move(mhod.text);
                break;
            case kMhodArtist:
                track.artist = std::move(mhod.text);
                break;
            case kMhodGenre:
                track.genre = std::move(mhod.text);
                break;
            case kMhodFiletype:
                track.filetype = std::move(mhod.text);
                break;
            case kMhodComment:
                track.comment = std::move(mhod.text);
                break;
            case kMhodCategory:
                track.category = std::move(mhod.text);
                break;
            case kMhodComposer:
                track.composer = std::move(mhod.text);
                break;
            case kMhodGrouping:
                track.grouping = std::move(mhod.text);
                break;
            case kMhodDescription:
                track.description = std::move(mhod.text);
                break;
            case kMhodPodcastUrl:
                track.podcasturl = std::move(mhod.text);
                break;
            case kMhodPodcastRss:
                track.podcastrss = std::move(mhod.text);
                break;
            case kMhodSubtitle:
                track.subtitle = std::move(mhod.text);
                break;
            case kMhodTvShow:
                track.tvshow = std::move(mhod.text);
                break;
            case kMhodTvEpisode:
                track.tvepisode = std::move(mhod.text);
                break;
            case kMhodTvNetwork:
                track.tvnetwork = std::move(mhod.text);
                break;
            case kMhodAlbumArtist:
                track.albumartist = std::move(mhod.text);
                break;
            case kMhodKeywords:
                track.keywords = std::move(mhod.text);
                break;
            case kMhodSortArtist:
                track.sort_artist = std::move(mhod.text);
                break;
            case kMhodSortTitle:
                track.sort_title = std::move(mhod.text);
                break;
            case kMhodSortAlbum:
                track.sort_album = std::move(mhod.text);
                break;
            case kMhodSortAlbumArtist:
                track.sort_albumartist = std::move(mhod.text);
                break;
            case kMhodSortComposer:
                track.sort_composer = std::move(mhod.text);
                break;
            case kMhodSortTvShow:
                track.sort_tvshow = std::move(mhod.text);
                break;
            default:
                break;
        }
    }

    // --- mhyp / mhip --------------------------------------------------------

    std::size_t parse_mhyp(std::uint32_t mhsd_type, std::size_t mhyp) {
        require_hunk(mhyp, "mhyp");
        const std::uint32_t header_len = db_.u32(mhyp + 4);
        if (header_len < kMhypMinHeader) {
            corrupt(db_.filename(), "header length of 'mhyp' smaller than expected (" +
                                         std::to_string(header_len) + " < " +
                                         std::to_string(kMhypMinHeader) + ")");
        }
        db_.require(mhyp, header_len);

        const std::size_t next = mhyp + db_.u32(mhyp + 8);
        if (next <= mhyp) {
            corrupt(db_.filename(), "mhyp at offset " + std::to_string(mhyp) +
                                         " does not declare a usable size");
        }
        const std::uint32_t mhod_count = db_.u32(mhyp + 12);
        const std::uint32_t mhip_count = db_.u32(mhyp + 16);

        auto playlist = std::make_unique<Playlist>();
        playlist->type = db_.u8(mhyp + 20) == 1 ? PlaylistType::Master : PlaylistType::Normal;
        playlist->flag1 = db_.u8(mhyp + 21);
        playlist->flag2 = db_.u8(mhyp + 22);
        playlist->flag3 = db_.u8(mhyp + 23);
        playlist->timestamp = mac_to_time(db_.u32(mhyp + 24));
        playlist->id = db_.u64(mhyp + 28);
        playlist->podcast_flag = db_.u16(mhyp + 42);
        playlist->sort_order = static_cast<PlaylistSortOrder>(db_.u32(mhyp + 44));
        if (header_len >= 0x6c) {
            playlist->mhsd5_type = static_cast<Mhsd5Type>(db_.u16(mhyp + 0x50));
        }

        std::size_t seek = mhyp + header_len;
        for (std::uint32_t i = 0; i < mhod_count; ++i) {
            const std::optional<MhodHeader> header = try_mhod_header(seek);
            if (!header) {
                break;
            }
            switch (header->type) {
                case kMhodTitle: {
                    MhodData mhod = read_mhod(seek);
                    if (mhod.valid) {
                        // Some databases carry two title MHODs; the last wins.
                        playlist->name = std::move(mhod.text);
                    }
                    break;
                }
                case kMhodSmartPrefs: {
                    MhodData mhod = read_mhod(seek);
                    if (mhod.valid && mhod.prefs) {
                        playlist->is_smart = true;
                        playlist->spl_prefs = *mhod.prefs;
                    }
                    break;
                }
                case kMhodSmartRules: {
                    MhodData mhod = read_mhod(seek);
                    if (mhod.valid && mhod.rules) {
                        playlist->is_smart = true;
                        playlist->spl_rules = std::move(*mhod.rules);
                    }
                    break;
                }
                default:
                    // Other MHODs (playlist settings, library indexes) are not
                    // needed to rebuild the database.
                    break;
            }
            seek += header->length;
        }

        if (playlist->name.empty()) {
            if (playlist->is_master()) {
                playlist->name = "Master-PL";
            } else if (playlist->is_podcasts()) {
                playlist->name = "Podcasts";
            } else {
                playlist->name = "Playlist";
            }
        }

        positions_.clear();
        for (std::uint32_t i = 0; i < mhip_count; ++i) {
            require_hunk(seek, "mhip");
            seek = parse_mhip(seek);
        }

        // The mhips are stored in reverse order; insert newest-last so member
        // order matches the position MHODs.
        const auto add_members = [this](Playlist& target) {
            std::sort(positions_.begin(), positions_.end(),
                      [](const PosEntry& left, const PosEntry& right) {
                          return left.track_pos > right.track_pos;
                      });
            for (const PosEntry& entry : positions_) {
                const auto found = id_tree_.find(entry.track_id);
                if (found != id_tree_.end()) {
                    target.add_track(found->second, 0);
                }
            }
            positions_.clear();
        };

        if (mhsd_type == 5 && playlist->id != 0) {
            // Devices repeat the playlists of the type 5 section (the podcasts,
            // movies and TV menus) in the type 2 one.  Itdb keeps one object per
            // playlist and lists it in mhsd5_playlists as well, so the repeat is
            // merged into the playlist that was already read instead of being
            // added a second time.
            if (Playlist* existing = itdb_->playlist_by_id(playlist->id)) {
                existing->mhsd5_type = playlist->mhsd5_type;
                if (playlist->is_podcasts()) {
                    existing->set_podcasts();
                }
                if (mhip_count > 0) {
                    existing->members.clear();
                    add_members(*existing);
                }
                positions_.clear();
                itdb_->mhsd5_playlists.push_back(existing);
                return next;
            }
        }

        Playlist* added = itdb_->add_playlist(std::move(playlist));
        if (mhsd_type == 5) {
            // Playlists of the type 5 section are owned by Itdb::playlists like
            // every other playlist and additionally listed here so the writer
            // puts them back into mhsd type 5.
            itdb_->mhsd5_playlists.push_back(added);
        }
        add_members(*added);
        return next;
    }

    std::size_t parse_mhip(std::size_t mhip) {
        require_hunk(mhip, "mhip");
        const std::uint32_t header_len = db_.u32(mhip + 4);
        if (header_len < kMhipMinHeader) {
            corrupt(db_.filename(), "header length of 'mhip' smaller than expected (" +
                                         std::to_string(header_len) + " < " +
                                         std::to_string(kMhipMinHeader) + ")");
        }
        db_.require(mhip, header_len);

        const std::uint32_t total_len = db_.u32(mhip + 8);
        if (total_len < header_len) {
            corrupt(db_.filename(), "mhip at offset " + std::to_string(mhip) +
                                         " is smaller than its header");
        }
        const std::uint32_t mhod_count = db_.u32(mhip + 12);
        const std::uint32_t track_id = db_.u32(mhip + 24);

        std::size_t seek = mhip + header_len;
        bool first_entry = true;
        for (std::uint32_t i = 0; i < mhod_count; ++i) {
            const std::optional<MhodHeader> header = try_mhod_header(seek);
            if (!header) {
                break;
            }
            if (header->type == kMhodPlaylist) {
                MhodData mhod = read_mhod(seek);
                if (mhod.valid && first_entry) {
                    positions_.push_back(PosEntry{track_id, mhod.track_pos});
                    // A corrupt database can repeat the position MHOD; only the
                    // first one is the position in the playlist.
                    first_entry = false;
                }
            }
            seek += header->length;
        }

        // Firmware up to iTunes 4.7 wrote mhip_len == mhip_hlen, so the next
        // mhip can only be found by walking the MHODs.
        if (total_len == header_len && mhod_count > 0) {
            return seek;
        }
        return mhip + total_len;
    }

    // --- mhod ---------------------------------------------------------------

    [[nodiscard]] MhodHeader mhod_header(std::size_t pos) {
        if (!db_.has(pos, kMhodMinHeader)) {
            db_.require(pos, kMhodMinHeader);
        }
        if (!db_.matches(pos, "mhod")) {
            corrupt(db_.filename(),
                    "no MHOD at offset " + std::to_string(pos));
        }
        MhodHeader header;
        header.length = db_.u32(pos + 8);
        header.type = db_.u32(pos + 12);
        header.header_length = db_.u32(pos + 4);
        if (header.length < kMhodMinHeader) {
            corrupt(db_.filename(), "MHOD at offset " + std::to_string(pos) +
                                         " declares length " +
                                         std::to_string(header.length));
        }
        if (header.header_length > header.length) {
            corrupt(db_.filename(), "MHOD at offset " + std::to_string(pos) +
                                         " has a header longer than the hunk");
        }
        db_.require(pos, header.length);
        return header;
    }

    // Returns nothing when there is no MHOD at @pos, which happens when a hunk
    // declares more MHODs than it contains.  That is treated as the end of the
    // list (what upstream does after warning) instead of failing the database.
    [[nodiscard]] std::optional<MhodHeader> try_mhod_header(std::size_t pos) {
        if (!db_.has(pos, 4) || !db_.matches(pos, "mhod")) {
            return std::nullopt;
        }
        return mhod_header(pos);
    }

    [[nodiscard]] MhodData read_mhod(std::size_t pos) {
        const MhodHeader header = mhod_header(pos);
        MhodData result;
        result.type = header.type;
        result.length = header.length;
        const std::size_t body = pos + header.header_length;

        switch (header.type) {
            case kMhodPlaylist:
                // Position of the track in the playlist.
                if (header.length < 0x1c) {
                    corrupt(db_.filename(), "playlist MHOD at offset " + std::to_string(pos) +
                                                 " is too short for a track position");
                }
                result.track_pos = static_cast<std::int32_t>(db_.u32(pos + 24));
                break;
            case kMhodTitle:
            case kMhodPath:
            case kMhodAlbum:
            case kMhodArtist:
            case kMhodGenre:
            case kMhodFiletype:
            case kMhodComment:
            case kMhodCategory:
            case kMhodComposer:
            case kMhodGrouping:
            case kMhodDescription:
            case kMhodSubtitle:
            case kMhodTvShow:
            case kMhodTvEpisode:
            case kMhodTvNetwork:
            case kMhodAlbumArtist:
            case kMhodKeywords:
            case kMhodSortArtist:
            case kMhodSortTitle:
            case kMhodSortAlbum:
            case kMhodSortAlbumArtist:
            case kMhodSortComposer:
            case kMhodSortTvShow:
            case kMhodAlbumName:
            case kMhodAlbumArtistName:
            case kMhodAlbumSortArtist:
            case kMhodAlbumArtistMhii: {
                const std::optional<std::string> text = read_mhod_text(body);
                if (!text) {
                    // Not valid UTF-8: keep the field empty but do not fail.
                    return result;
                }
                result.text = *text;
                break;
            }
            case kMhodPodcastUrl:
            case kMhodPodcastRss: {
                // These store raw UTF-8/ASCII without a length prefix.
                const std::size_t length = header.length - header.header_length;
                result.text = cut_at_nul(std::string(db_.raw(body, length)));
                break;
            }
            case kMhodChapterData:
                result.chapters = read_chapter_data(body);
                break;
            case kMhodSmartPrefs:
                result.prefs = read_smart_prefs(body);
                break;
            case kMhodSmartRules:
                result.rules = read_smart_rules(body);
                break;
            default:
                // Library indexes and anything newer are not needed here.
                return result;
        }

        result.valid = true;
        return result;
    }

    [[nodiscard]] std::optional<std::string> read_mhod_text(std::size_t body) {
        const std::uint32_t string_type = db_.u32(body);
        const std::uint32_t length = db_.u32(body + 4);
        if (string_type == 2) {
            // UTF-8: some databases (and reversed ones) use it for all strings.
            const std::string value = cut_at_nul(std::string(db_.raw(body + 16, length)));
            if (!is_utf8(value)) {
                return std::nullopt;
            }
            return value;
        }
        const std::size_t units = length / 2;
        const std::u16string value = cut_at_nul(db_.utf16(body + 16, units));
        return utf16_to_utf8(value);
    }

    [[nodiscard]] std::unique_ptr<ChapterData> read_chapter_data(std::size_t body) {
        auto data = std::make_unique<ChapterData>();
        data->unk024 = db_.u32(body);
        data->unk028 = db_.u32(body + 4);
        data->unk032 = db_.u32(body + 8);
        std::size_t seek = body + 12;

        if (!db_.matches(seek + 4, "sean")) {
            return data;
        }
        const std::uint32_t declared = db_.swapped_u32(seek + 12);
        // The count includes the "sean" atom itself.
        const std::uint32_t chapters = declared == 0 ? 0 : declared - 1;
        seek += 20;
        for (std::uint32_t i = 0; i < chapters; ++i) {
            if (!db_.matches(seek + 4, "chap")) {
                break;
            }
            const std::uint32_t startpos = db_.swapped_u32(seek + 8);
            const std::uint32_t children = db_.swapped_u32(seek + 12);
            seek += 20;
            for (std::uint32_t j = 0; j < children; ++j) {
                const std::uint32_t child_len = db_.swapped_u32(seek);
                if (child_len == 0) {
                    corrupt(db_.filename(),
                            "chapter atom at offset " + std::to_string(seek) +
                                " declares length 0");
                }
                if (db_.matches(seek + 4, "name")) {
                    const std::uint16_t units = db_.swapped_u16(seek + 20);
                    if (units > 0) {
                        const std::u16string value =
                            cut_at_nul(db_.swapped_utf16(seek + 22, units));
                        data->add_chapter(startpos, utf16_to_utf8(value));
                    }
                }
                seek += child_len;
            }
        }
        if (db_.matches(seek + 4, "hedr")) {
            const std::uint32_t hedr_len = db_.swapped_u32(seek);
            if (hedr_len > 0) {
                seek += hedr_len;
            }
        }
        return data;
    }

    [[nodiscard]] SmartPlaylistPrefs read_smart_prefs(std::size_t body) {
        db_.require(body, 14);
        SmartPlaylistPrefs prefs;
        prefs.live_update = db_.u8(body);
        prefs.check_rules = db_.u8(body + 1);
        prefs.check_limits = db_.u8(body + 2);
        prefs.limit_type = db_.u8(body + 3);
        prefs.limit_sort = db_.u8(body + 4);
        prefs.limit_value = db_.u32(body + 8);
        prefs.match_checked_only = db_.u8(body + 12);
        // The byte after match_checked_only carries the high bit of limit_sort;
        // see the note on Itdb_SPLPref::limitsort in the upstream headers.
        if (db_.u8(body + 13) != 0) {
            prefs.limit_sort |= 0x80000000U;
        }
        return prefs;
    }

    [[nodiscard]] SmartPlaylistRules read_smart_rules(std::size_t body) {
        if (!db_.matches(body, "SLst")) {
            corrupt(db_.filename(), "no SLst at offset " + std::to_string(body));
        }
        db_.require(body, 136);
        SmartPlaylistRules rules;
        rules.unk004 = db_.swapped_u32(body + 4);
        const std::uint32_t count = db_.swapped_u32(body + 8);
        rules.match_operator = static_cast<SmartPlaylistMatch>(db_.swapped_u32(body + 12));
        std::size_t seek = body + 136;

        for (std::uint32_t i = 0; i < count; ++i) {
            db_.require(seek, 56);
            SmartPlaylistRule rule;
            rule.field = static_cast<SmartPlaylistField>(db_.swapped_u32(seek));
            rule.action = static_cast<SmartPlaylistAction>(db_.swapped_u32(seek + 4));
            seek += 52;
            const std::uint32_t length = db_.swapped_u32(seek);
            const SmartPlaylistFieldType type = field_type(rule);
            if (type == SmartPlaylistFieldType::String) {
                const std::size_t units = length / 2;
                rule.string = utf16_to_utf8(cut_at_nul(db_.swapped_utf16(seek + 4, units)));
            } else {
                // Every non-string rule stores six 64-bit values and five
                // unknowns (0x44 bytes).
                db_.require(seek, 72);
                rule.from_value = db_.swapped_u64(seek + 4);
                rule.from_date = static_cast<std::int64_t>(db_.swapped_u64(seek + 12));
                rule.from_units = db_.swapped_u64(seek + 20);
                rule.to_value = db_.swapped_u64(seek + 28);
                rule.to_date = static_cast<std::int64_t>(db_.swapped_u64(seek + 36));
                rule.to_units = db_.swapped_u64(seek + 44);
                rule.unk052 = db_.swapped_u32(seek + 52);
                rule.unk056 = db_.swapped_u32(seek + 56);
                rule.unk060 = db_.swapped_u32(seek + 60);
                rule.unk064 = db_.swapped_u32(seek + 64);
                rule.unk068 = db_.swapped_u32(seek + 68);
                // Only point and range date rules store a Mac timestamp; "in
                // the last N" keeps a unit count.
                const SmartPlaylistActionType act = action_type(rule);
                if (type == SmartPlaylistFieldType::Date &&
                    (act == SmartPlaylistActionType::Date ||
                     act == SmartPlaylistActionType::RangeDate)) {
                    rule.from_value =
                        static_cast<std::uint64_t>(mac_to_time(rule.from_value));
                    rule.to_value = static_cast<std::uint64_t>(mac_to_time(rule.to_value));
                }
            }
            seek += length + 4;
            rules.rules.push_back(std::move(rule));
        }
        return rules;
    }

    // --- recent play counts -------------------------------------------------

    void read_recent_playcounts() {
        const std::filesystem::path dir = db_.filename().parent_path();
        if (dir.empty()) {
            return;
        }
        try {
            if (const auto path = find_existing(dir, "Play Counts")) {
                // A zero-length file is common after dosfsck; skip it.
                if (file_size_or_zero(*path) >= 0x60) {
                    read_playcounts(read_file(*path), *path);
                }
                return;
            }
            if (const auto path = find_existing(dir, "iTunesStats")) {
                if (file_size_or_zero(*path) >= 6) {
                    read_itunes_stats(read_file(*path), *path);
                }
                return;
            }
            if (const auto path = find_existing(dir, "PlayCounts.plist")) {
                if (file_size_or_zero(*path) > 0) {
                    read_playcounts_plist(*path);
                }
            }
        } catch (const Error&) {
            // Recent play counts are advisory; a damaged file must not stop the
            // library from loading.
            playcounts_.clear();
            playcounts_by_dbid_.clear();
        }
    }

    void read_playcounts(Bytes data, const std::filesystem::path& path) {
        Db db(std::move(data), path);
        if (!is_playcounts_header(db)) {
            db.set_order(ByteOrder::Big);
            if (!is_playcounts_header(db)) {
                corrupt(path, "not a Play Counts file (missing mhdp header)");
            }
        }
        const std::uint32_t header_len = db.u32(4);
        const std::uint32_t entry_len = db.u32(8);
        const std::uint32_t count = db.u32(12);
        if (header_len < 0x60) {
            corrupt(path, "header length smaller than expected (" +
                              std::to_string(header_len) + " < 96)");
        }
        if (entry_len < 0x0c) {
            corrupt(path, "entry length smaller than expected (" +
                              std::to_string(entry_len) + " < 12)");
        }
        playcounts_.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t seek = header_len + static_cast<std::size_t>(entry_len) * i;
            db.require(seek, entry_len);
            PlayCount entry;
            entry.playcount = db.u32(seek);
            entry.time_played = mac_to_time(db.u32(seek + 4));
            entry.bookmark_time = db.u32(seek + 8);
            // Ratings exist for firmware 2.0 and later.
            if (entry_len >= 0x10) {
                entry.rating = static_cast<std::int32_t>(db.u32(seek + 12));
            }
            // Skip counts were added with iTunesDB version 0x13.
            if (entry_len >= 0x1c) {
                entry.skipcount = db.u32(seek + 20);
                entry.last_skipped = mac_to_time(db.u32(seek + 24));
            }
            playcounts_.push_back(entry);
        }
    }

    void read_itunes_stats(Bytes data, const std::filesystem::path& path) {
        Db db(std::move(data), path);
        playcounts_.clear();

        // The Shuffle (shadow database V1) writes 24-bit fields; everything else
        // uses the newer 32-bit layout.
        const bool old_format =
            itdb_->device->shadow_db_version() == ShadowDbVersion::V1;
        if (old_format) {
            const std::uint32_t count = db.u24(0);
            std::size_t seek = 6;
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t entry_len = db.u24(seek);
                if (entry_len < 18) {
                    corrupt(path, "entry length smaller than expected (" +
                                      std::to_string(entry_len) + " < 18)");
                }
                db.require(seek, entry_len);
                PlayCount entry;
                entry.bookmark_time = db.u24(seek + 3);
                entry.playcount = db.u24(seek + 12);
                playcounts_.push_back(entry);
                seek += entry_len;
            }
            return;
        }

        const std::uint32_t count = db.u32(0);
        std::size_t seek = 8;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t entry_len = db.u32(seek);
            if (entry_len < 32) {
                corrupt(path, "entry length smaller than expected (" +
                                  std::to_string(entry_len) + " < 32)");
            }
            db.require(seek, entry_len);
            PlayCount entry;
            entry.bookmark_time = db.u32(seek + 4);
            entry.playcount = db.u32(seek + 8);
            entry.time_played = mac_to_time(db.u32(seek + 12));
            entry.last_skipped = mac_to_time(db.u32(seek + 20));
            playcounts_.push_back(entry);
            seek += entry_len;
        }
    }

    // iTunes 8+ keeps recent play counts in a property list keyed by the
    // persistent track id instead of a flat "Play Counts" file.
    void read_playcounts_plist(const std::filesystem::path& path) {
        const plist::Value root = plist::parse_file(path);
        const plist::Value* tracks = root.find("tracks");
        if (tracks == nullptr || !tracks->is_array()) {
            return;
        }
        for (const plist::Value& entry : tracks->array()) {
            if (!entry.is_dictionary()) {
                continue;
            }
            const plist::Value* id = entry.find("persistentID");
            if (id == nullptr || !id->is_integer()) {
                continue;
            }
            PlayCount playcount;
            playcount.playcount =
                static_cast<std::uint32_t>(entry.integer_of("playCount"));
            playcount.bookmark_time =
                static_cast<std::uint32_t>(entry.integer_of("bookmarkTimeInMS"));
            playcount.time_played =
                mac_to_time(static_cast<std::uint64_t>(entry.integer_of("playMacOSDate")));
            playcount.skipcount = static_cast<std::uint32_t>(entry.integer_of("skipCount"));
            playcount.last_skipped =
                mac_to_time(static_cast<std::uint64_t>(entry.integer_of("skipMacOSDate")));
            const std::int64_t rating = entry.integer_of("userRating");
            playcount.rating = rating == 0 ? kNoPlaycount : static_cast<std::int32_t>(rating);
            playcounts_by_dbid_.insert_or_assign(
                static_cast<std::uint64_t>(id->integer()), playcount);
        }
    }

    // Merges the next pending entry into @track.  Entries from "Play Counts" and
    // "iTunesStats" are consumed in file order, which matches the mhit order;
    // PlayCounts.plist is looked up by persistent id instead.
    void apply_playcount(Track& track) {
        const PlayCount* found = nullptr;
        if (playcount_pos_ < playcounts_.size()) {
            found = &playcounts_[playcount_pos_++];
        } else {
            const auto entry = playcounts_by_dbid_.find(track.dbid);
            if (entry != playcounts_by_dbid_.end()) {
                found = &entry->second;
            }
        }
        if (found == nullptr) {
            return;
        }
        const PlayCount& entry = *found;
        if (entry.rating >= 0 && entry.rating != kNoPlaycount &&
            static_cast<std::int32_t>(track.rating) != entry.rating) {
            // Keep the rating the application assigned before the iPod saw it.
            track.app_rating = static_cast<std::uint8_t>(track.rating);
            track.rating = static_cast<std::uint32_t>(entry.rating);
        }
        if (entry.time_played != 0) {
            track.time_played = entry.time_played;
        }
        if (entry.bookmark_time != 0) {
            track.bookmark_time = entry.bookmark_time;
        }
        track.playcount += entry.playcount;
        if (entry.playcount != 0) {
            track.mark_unplayed = 0x01;
        }
        track.recent_playcount = entry.playcount;
        track.skipcount += entry.skipcount;
        track.recent_skipcount = entry.skipcount;
    }

    // --- On-The-Go playlists ------------------------------------------------

    void read_otg_playlists() {
        const std::filesystem::path dir = db_.filename().parent_path();
        if (dir.empty()) {
            return;
        }
        for (std::uint32_t index = 1;; ++index) {
            const std::string name =
                index == 1 ? "OTGPlaylistInfo" : "OTGPlaylistInfo_" + std::to_string(index - 1);
            const auto path = find_existing(dir, name);
            if (!path) {
                return;
            }
            try {
                read_otg_playlist(*path, index);
            } catch (const Error&) {
                // A stale or damaged On-The-Go file must not invalidate the
                // library; the iPod rewrites it on eject.
            }
        }
    }

    void read_otg_playlist(const std::filesystem::path& path, std::uint32_t number) {
        Db db(read_file(path), path);
        if (!is_otg_header(db)) {
            db.set_order(ByteOrder::Big);
            if (!is_otg_header(db)) {
                corrupt(path, "not an OTG playlist file (missing mhpo header)");
            }
        }
        const std::uint32_t header_len = db.u32(4);
        const std::uint32_t entry_len = db.u32(8);
        const std::uint32_t count = db.u32(12);
        if (header_len < 0x14) {
            corrupt(path, "header length smaller than expected (" +
                              std::to_string(header_len) + " < 20)");
        }
        if (entry_len < 4) {
            corrupt(path, "entry length smaller than expected (" +
                              std::to_string(entry_len) + " < 4)");
        }
        if (count == 0) {
            return;
        }

        auto playlist = std::make_unique<Playlist>();
        playlist->name = "OTG Playlist " + std::to_string(number);
        Playlist* added = itdb_->add_playlist(std::move(playlist));
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t index =
                db.u32(header_len + static_cast<std::size_t>(entry_len) * i);
            if (index >= itdb_->tracks.size()) {
                corrupt(path, "reference to non-existent track (" +
                                  std::to_string(index) + ")");
            }
            added->add_track(itdb_->tracks[index].get());
        }
    }

    Db db_;
    std::unique_ptr<Itdb> itdb_;
    bool compressed_ = false;
    std::vector<PlayCount> playcounts_;
    std::map<std::uint64_t, PlayCount> playcounts_by_dbid_;
    std::size_t playcount_pos_ = 0;
    std::vector<PosEntry> positions_;
    std::map<std::uint32_t, Track*> id_tree_;
};

// Derives the iPod mount point from "<mount>/iPod_Control/iTunes/iTunesDB" so a
// repository or backup file can still resolve track paths.
[[nodiscard]] std::optional<std::filesystem::path> infer_mountpoint(
    const std::filesystem::path& filename) {
    const std::filesystem::path itunes = filename.parent_path();
    if (itunes.empty() || !ascii_iequals(itunes.filename().string(), "iTunes")) {
        return std::nullopt;
    }
    const std::filesystem::path control = itunes.parent_path();
    if (control.empty() || !ascii_iequals(control.filename().string(), "iPod_Control")) {
        return std::nullopt;
    }
    return control.parent_path();
}

}  // namespace

std::unique_ptr<Itdb> parse_itunesdb(const std::filesystem::path& mountpoint) {
    const std::filesystem::path compressed_path = itunescdb_path(mountpoint);
    const std::filesystem::path plain_path = itunesdb_path(mountpoint);

    bool compressed = false;
    std::filesystem::path filename;
    std::error_code error;
    if (std::filesystem::is_regular_file(compressed_path, error) && !error) {
        filename = compressed_path;
        compressed = true;
    } else if (std::filesystem::is_regular_file(plain_path, error) && !error) {
        filename = plain_path;
    } else {
        fail(ErrorCode::NotFound, "couldn't find an iPod database on '" +
                                      mountpoint.string() + "'");
    }

    auto itdb = std::make_unique<Itdb>();
    itdb->set_mountpoint(mountpoint);
    // Timestamps are stored in device local time, so the device timezone has to
    // be known before any mac time is converted.
    update_timezone_shift(*itdb->device);
    itdb->filename = filename;

    Bytes data = read_file(filename);
    if (itdb->device != nullptr) {
        // hash72 devices cache their signature parameters in HashInfo.  The
        // signature covers the stored image, so this runs before any
        // iTunesCDB decompression.
        (void)hash72_extract_hash_info(*itdb->device, data);
    }
    return Parser(std::move(data), filename, std::move(itdb), compressed).run();
}

std::unique_ptr<Itdb> parse_itunesdb_file(const std::filesystem::path& filename) {
    Bytes data = read_file(filename);
    auto itdb = std::make_unique<Itdb>();
    itdb->filename = filename;
    if (const auto mountpoint = infer_mountpoint(filename)) {
        itdb->set_mountpoint(*mountpoint);
        // Convert timestamps exactly like parse_itunesdb() does for the same
        // file in place.
        update_timezone_shift(*itdb->device);
    }
    if (itdb->device != nullptr) {
        (void)hash72_extract_hash_info(*itdb->device, data);
    }
    // An iTunesCDB handed over by name is not recognisable from its content, so
    // only parse_itunesdb() treats it as compressed, like upstream.
    return Parser(std::move(data), filename, std::move(itdb), false).run();
}

}  // namespace gpod
