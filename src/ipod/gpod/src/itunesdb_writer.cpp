// iTunesDB writer.
//
// Port of the writing half of itdb_itunesdb.c: the in-memory model is
// serialised into the binary database that iTunes and the iPod firmware read.
// The record layouts are reproduced field by field and in the reference order
// so that the two implementations can be compared side by side.  Every offset
// comment below refers to the start of the record being written.
//
// Known gaps:
//   * compressed databases (iTunesCDB) and the sqlite shadow database are not
//     produced; such devices are rejected with ErrorCode::Unsupported instead
//     of being written a database they cannot read;
//   * the mhod 52 collation keys use the case folding of gpod/smartplaylist.hpp
//     and therefore approximate g_utf8_collate_key().
//
// Checksums are written through gpod/hash.hpp when that header is available;
// without it the hash areas stay zero and devices that verify them reject the
// database.

#include "gpod/itunesdb.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <ctime>
#include <initializer_list>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gpod/error.hpp"
#include "gpod/hash.hpp"
#include "gpod/io.hpp"
#include "gpod/paths.hpp"
#include "gpod/smartplaylist.hpp"
#include "gpod/time.hpp"
#include "gpod/zlib.hpp"

namespace gpod {
namespace {

// The first (track) id handed out when writing.  Ids 1..51 are reserved by
// iTunes for internal purposes.
constexpr std::uint32_t kFirstIpodId = 52;

// Database version written into the mhbd header; see mk_mhbd() for the list of
// iTunes versions.  Newer iPods refuse databases with too old a version.
constexpr std::uint32_t kDbVersion = 0x30;

// mhod types (enum MHOD_ID in itdb_itunesdb.c).
constexpr std::uint32_t kMhodTitle = 1;
constexpr std::uint32_t kMhodPath = 2;
constexpr std::uint32_t kMhodAlbum = 3;
constexpr std::uint32_t kMhodArtist = 4;
constexpr std::uint32_t kMhodGenre = 5;
constexpr std::uint32_t kMhodFiletype = 6;
constexpr std::uint32_t kMhodComment = 8;
constexpr std::uint32_t kMhodCategory = 9;
constexpr std::uint32_t kMhodComposer = 12;
constexpr std::uint32_t kMhodGrouping = 13;
constexpr std::uint32_t kMhodDescription = 14;
constexpr std::uint32_t kMhodPodcastUrl = 15;
constexpr std::uint32_t kMhodPodcastRss = 16;
constexpr std::uint32_t kMhodChapterData = 17;
constexpr std::uint32_t kMhodSubtitle = 18;
constexpr std::uint32_t kMhodTvShow = 19;
constexpr std::uint32_t kMhodTvEpisode = 20;
constexpr std::uint32_t kMhodTvNetwork = 21;
constexpr std::uint32_t kMhodAlbumArtist = 22;
constexpr std::uint32_t kMhodSortArtist = 23;
constexpr std::uint32_t kMhodKeywords = 24;
constexpr std::uint32_t kMhodSortTitle = 27;
constexpr std::uint32_t kMhodSortAlbum = 28;
constexpr std::uint32_t kMhodSortAlbumArtist = 29;
constexpr std::uint32_t kMhodSortComposer = 30;
constexpr std::uint32_t kMhodSortTvShow = 31;
constexpr std::uint32_t kMhodSplPref = 50;
constexpr std::uint32_t kMhodSplRules = 51;
constexpr std::uint32_t kMhodLibPlaylistIndex = 52;
constexpr std::uint32_t kMhodLibPlaylistJumpTable = 53;
constexpr std::uint32_t kMhodPlaylist = 100;
constexpr std::uint32_t kMhodAlbumAlbum = 200;
constexpr std::uint32_t kMhodAlbumArtistName = 201;
constexpr std::uint32_t kMhodAlbumSortArtist = 202;
constexpr std::uint32_t kMhodAlbumArtistMhii = 300;

// Sort types of the mhod 52/53 tables (enum MHOD52_SORTTYPE).
constexpr std::uint32_t kSortTitle = 0x03;
constexpr std::uint32_t kSortAlbum = 0x04;
constexpr std::uint32_t kSortArtist = 0x05;
constexpr std::uint32_t kSortGenre = 0x07;
constexpr std::uint32_t kSortComposer = 0x12;

// Size of the fixed mhit header (0x248); the mhods follow it.
constexpr std::uint32_t kMhitHeaderSize = 0x248;

[[nodiscard]] std::uint64_t random_u64() {
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    return generator();
}

// Model strings are NUL-terminated in the reference implementation; a string
// with an embedded NUL would truncate there, so match that behaviour.
[[nodiscard]] std::string_view without_nul(std::string_view value) {
    const std::size_t nul = value.find('\0');
    return nul == std::string_view::npos ? value : value.substr(0, nul);
}

[[nodiscard]] char16_t upper_case(char16_t unit) {
    if (unit >= u'a' && unit <= u'z') {
        return static_cast<char16_t>(unit - 0x20);
    }
    // Latin-1 letters have the same 0x20 distance; sharp s and y-diaeresis
    // have no upper-case form inside Latin-1, so they keep their own value.
    if (unit >= 0x00e0 && unit <= 0x00fe && unit != 0x00df) {
        return static_cast<char16_t>(unit - 0x20);
    }
    return unit;
}

// Sort key for the mhod 52/53 tables.  glib's g_utf8_collate_key() is not part
// of the pure C++ build; case folding keeps the order the tables express, but
// accent-insensitive collation is not reproduced.
// TODO: reproduce g_utf8_collate_key() byte for byte.
[[nodiscard]] std::string collation_key(std::string_view value) {
    return detail::casefold(without_nul(value));
}

// First alphanumeric character of @value as an upper-case UTF-16 unit, or '0'
// when the string starts with a digit or has no alphanumerics at all.  These
// letters form the index of the jump table (mhod 53).
[[nodiscard]] std::uint16_t jump_table_letter(std::string_view value) {
    const std::u16string units = utf8_to_utf16(without_nul(value));
    for (char16_t unit : units) {
        if (unit < 0x80) {
            if (unit >= u'0' && unit <= u'9') {
                return u'0';
            }
            if ((unit >= u'a' && unit <= u'z') || (unit >= u'A' && unit <= u'Z')) {
                return static_cast<std::uint16_t>(upper_case(unit));
            }
            continue;
        }
        if (unit >= 0x00c0 && unit <= 0x00ff && unit != 0x00d7 && unit != 0x00f7) {
            return static_cast<std::uint16_t>(upper_case(unit));
        }
    }
    return u'0';
}

// g_ascii_strncasecmp() against "The ": only ASCII letters are folded, so an
// accented first word prevents the rewrite, as in the reference.
[[nodiscard]] bool starts_with_the(std::string_view value) {
    constexpr std::string_view kArticle = "the ";
    if (value.size() < kArticle.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kArticle.size(); ++i) {
        const auto unit = static_cast<unsigned char>(value[i]);
        const auto folded =
            (unit >= 'A' && unit <= 'Z') ? static_cast<unsigned char>(unit + 0x20) : unit;
        if (folded != static_cast<unsigned char>(kArticle[i])) {
            return false;
        }
    }
    return true;
}

// Artist string used for sorting: an explicit sort key wins, otherwise "The
// Artist" becomes "Artist, The" followed by five 0x01 characters the way
// iTunes does it.
[[nodiscard]] std::string sort_artist_value(const Track& track) {
    if (!track.sort_artist.empty()) {
        return track.sort_artist;
    }
    if (track.artist.empty()) {
        return {};
    }
    if (starts_with_the(track.artist)) {
        std::string value = track.artist.substr(4);
        value += ", The";
        value.append(5, '\x01');
        return value;
    }
    return track.artist;
}

// Album equality as the reference defines it (itdb_album_equal()): the show and
// the album name have to match, and then either the album artists of both
// tracks or, when one of them has none, the track artists.
[[nodiscard]] bool same_album(const Track& left, const Track& right) {
    if (left.tvshow != right.tvshow || left.album != right.album) {
        return false;
    }
    if (!left.albumartist.empty() && !right.albumartist.empty()) {
        return left.albumartist == right.albumartist;
    }
    return left.artist == right.artist;
}

// One entry of the mhod 52 table: the collation keys plus the letters used by
// the matching mhod 53 jump table.
struct CollateTrack {
    std::string album;
    std::string title;
    std::string artist;
    std::string genre;
    std::string composer;
    std::int32_t track_nr = 0;
    std::int32_t cd_nr = 0;
    std::uint32_t index = 0;
    std::uint16_t letter_album = u'0';
    std::uint16_t letter_title = u'0';
    std::uint16_t letter_artist = u'0';
    std::uint16_t letter_genre = u'0';
    std::uint16_t letter_composer = u'0';
};

struct JumpEntry {
    std::uint16_t letter = u'0';
    std::uint32_t start = 0;
    std::uint32_t count = 0;
};

[[nodiscard]] int compare_keys(const std::string& left, const std::string& right) {
    return left.compare(right);
}

class Export {
  public:
    Export(Itdb& itdb, ByteWriter& writer)
        : itdb_(itdb),
          w_(writer),
          tz_shift_(itdb.device != nullptr ? itdb.device->timezone_shift : 0) {}

    Export(const Export&) = delete;
    Export& operator=(const Export&) = delete;

    void run();

  private:
    struct ItemId {
        std::uint32_t id = 0;
        std::uint64_t sql_id = 0;
    };

    struct AlbumEntry {
        const Track* representative = nullptr;
        ItemId item;
    };

    struct ArtistEntry {
        const Track* representative = nullptr;
        ItemId item;
    };

    // --- primitive writers -------------------------------------------------
    [[nodiscard]] std::size_t pos() const noexcept { return w_.size(); }
    [[nodiscard]] bool reversed() const noexcept { return w_.order() == ByteOrder::Big; }

    void fourcc(std::string_view tag) {
        if (reversed()) {
            std::array<std::uint8_t, 4> bytes{};
            for (std::size_t i = 0; i < 4; ++i) {
                bytes[i] = static_cast<std::uint8_t>(tag[3 - i]);
            }
            w_.bytes(bytes);
            return;
        }
        w_.bytes(tag);
    }

    void u8(std::uint8_t value) { w_.u8(value); }
    void u16(std::uint16_t value) { w_.u16(value); }
    void u32(std::uint32_t value) { w_.u32(value); }
    void u64(std::uint64_t value) { w_.u64(value); }
    void f32(float value) { w_.f32(value); }
    void zeros(std::size_t count) { w_.zeros(count); }
    void u16_n0(std::size_t count) { w_.zeros(2 * count); }
    void u32_n0(std::size_t count) { w_.zeros(4 * count); }

    // Values stored in the opposite byte order of the database.  The smart
    // playlist rule blob, the chapter "sean" atom and the iTunesSD format are
    // big endian even inside little endian databases.
    void u16_flipped(std::uint16_t value) {
        if (reversed()) {
            w_.u16le(value);
        } else {
            w_.u16be(value);
        }
    }
    void u32_flipped(std::uint32_t value) {
        if (reversed()) {
            w_.u32le(value);
        } else {
            w_.u32be(value);
        }
    }
    void u64_flipped(std::uint64_t value) {
        if (reversed()) {
            w_.u64le(value);
        } else {
            w_.u64be(value);
        }
    }

    void patch_size(std::size_t header_seek) {
        w_.patch_u32(header_seek + 8, static_cast<std::uint32_t>(pos() - header_seek));
    }
    void patch_short_size(std::size_t header_seek) {
        w_.patch_u32(header_seek + 4, static_cast<std::uint32_t>(pos() - header_seek));
    }

    [[nodiscard]] std::uint64_t mac_time(std::time_t value) const {
        return unix_time_to_mac(value, tz_shift_);
    }

    // --- id preparation ----------------------------------------------------
    void prepare();
    void reorder_tracks_like_master_playlist();
    [[nodiscard]] std::uint32_t album_id_for(const Track& track);

    // --- records -----------------------------------------------------------
    void write_mhbd_header(std::uint32_t children);
    void write_mhsd_header(std::uint32_t type);
    void write_mhlt_header(std::uint32_t tracks);
    void write_mhit_header(const Track& track);
    void write_mhia_header(const ItemId& item);
    void write_mhla_header(std::uint32_t albums);
    void write_mhlp_header();
    void write_mhii_header(const ItemId& item);
    void write_mhli_header(std::uint32_t artists);
    void write_mhip_header(std::uint32_t childcount, std::uint32_t podcast_group_flag,
                           std::uint32_t podcast_group_id, std::uint32_t track_id,
                           std::uint32_t timestamp, std::uint32_t podcast_group_ref);

    void write_mhod_string(std::uint32_t type, std::string_view value);
    void write_mhod_url(std::uint32_t type, std::string_view value);
    void write_mhod_playlist_position(std::uint32_t position);
    void write_mhod_chapterdata(const ChapterData& chapterdata);
    void write_mhod_splpref(const SmartPlaylistPrefs& prefs);
    void write_mhod_splrules(const SmartPlaylistRules& rules);
    void write_long_mhod_playlist();

    [[nodiscard]] std::vector<JumpEntry> write_mhod52(std::uint32_t sort_type,
                                                      std::vector<CollateTrack>& tracks);
    void write_mhod53(std::uint32_t sort_type, const std::vector<JumpEntry>& entries);

    // --- sections ----------------------------------------------------------
    void write_mhsd_tracks();
    void write_mhsd_playlists(std::uint32_t mhsd_type);
    void write_mhsd_albums();
    void write_mhsd_artists();
    void write_mhsd_empty(std::uint32_t type);
    void write_mhsd_genius();

    void write_playlist(const Playlist& playlist, std::uint32_t mhsd_type);
    void write_playlist_mhips(const Playlist& playlist, std::size_t mhyp_seek);
    void write_podcast_mhips(const Playlist& playlist, std::size_t mhyp_seek);
    void write_mhod52_tables(const Playlist& playlist);

    [[nodiscard]] static std::vector<CollateTrack> make_collate_tracks(const Playlist& playlist);

    Itdb& itdb_;
    ByteWriter& w_;
    std::int32_t tz_shift_ = 0;

    std::uint32_t next_id_ = kFirstIpodId;
    std::uint32_t next_album_id_ = 1;
    std::vector<AlbumEntry> album_order_;
    std::uint32_t next_artist_id_ = 1;
    std::map<std::string, ItemId> artists_;
    std::vector<ArtistEntry> artist_order_;
    std::uint32_t next_composer_id_ = 1;
    std::map<std::string, ItemId> composers_;
};

void Export::reorder_tracks_like_master_playlist() {
    // The track list has to follow the master playlist, otherwise the On-The-Go
    // playlists show the wrong content.  Podcasts are not part of the master
    // playlist and therefore stay at the end.
    const Playlist* master = itdb_.master_playlist();
    if (master == nullptr) {
        return;
    }
    for (auto member = master->members.rbegin(); member != master->members.rend(); ++member) {
        if (*member == nullptr) {
            continue;
        }
        const auto found = std::find_if(itdb_.tracks.begin(), itdb_.tracks.end(),
                                        [track = *member](const std::unique_ptr<Track>& entry) {
                                            return entry.get() == track;
                                        });
        if (found == itdb_.tracks.end()) {
            continue;
        }
        std::unique_ptr<Track> moved = std::move(*found);
        itdb_.tracks.erase(found);
        itdb_.tracks.insert(itdb_.tracks.begin(), std::move(moved));
    }
}

std::uint32_t Export::album_id_for(const Track& track) {
    if (track.album.empty()) {
        return 0;
    }
    // Two tracks share an album when the show and album names match and either
    // both album artists or both track artists match (itdb_album_equal()).  The
    // rule is not transitive, so the existing albums are scanned in creation
    // order rather than looked up by key.
    for (const AlbumEntry& entry : album_order_) {
        if (same_album(*entry.representative, track)) {
            return entry.item.id;
        }
    }
    const ItemId item{next_album_id_++, random_u64()};
    album_order_.push_back({&track, item});
    return item.id;
}

void Export::prepare() {
    reorder_tracks_like_master_playlist();

    next_id_ = kFirstIpodId;
    for (const std::unique_ptr<Track>& entry : itdb_.tracks) {
        Track& track = *entry;
        track.id = next_id_++;
        track.album_id = album_id_for(track);

        if (!track.artist.empty()) {
            const auto found = artists_.find(track.artist);
            if (found != artists_.end()) {
                track.artist_id = found->second.id;
            } else {
                const ItemId item{next_artist_id_++, random_u64()};
                artists_.emplace(track.artist, item);
                artist_order_.push_back({&track, item});
                track.artist_id = item.id;
            }
        }

        if (!track.composer.empty()) {
            const auto found = composers_.find(track.composer);
            if (found != composers_.end()) {
                track.composer_id = found->second.id;
            } else {
                const ItemId item{next_composer_id_++, random_u64()};
                composers_.emplace(track.composer, item);
                track.composer_id = item.id;
            }
        }
    }
}

void Export::write_mhbd_header(std::uint32_t children) {
    fourcc("mhbd");
    u32(244);            // header size
    u32(0);              // size of the whole database, patched later
    // 2 on iPhone 3.0 and Nano 5G, 1 on iPod Color and iPod Classic; the
    // Classic 3G refuses 2, so tie it to compressed database support.
    u32(itdb_.device != nullptr && itdb_.device->supports_compressed_itunesdb() ? 2U : 1U);
    itdb_.version = kDbVersion;
    u32(itdb_.version);
    u32(children);
    u64(itdb_.id);
    // 0x20
    u16(itdb_.platform);   // OS type, 1 = MacOS X, 2 = Windows
    u16(itdb_.unk_0x22);
    u64(itdb_.id_0x24);
    u32(0);
    // 0x30: hashing scheme.  write_checksum() overwrites this together with
    // the hash bytes after the complete image has been serialized.
    u16(0);
    u16_n0(10);
    // 0x46
    u16(itdb_.language);
    u64(itdb_.pid);
    // 0x50: seen 0x05/0x01 (nano 3G / iPod Color) and 0x4d/0x0f.
    u32(static_cast<std::uint32_t>(itdb_.unk_0x50));
    u32(static_cast<std::uint32_t>(itdb_.unk_0x54));
    u32_n0(5);             // hash58
    u32(static_cast<std::uint32_t>(itdb_.tzoffset));
    // 0x70: checksum flavour marker (4 = hashAB, 2 = hash72, 0 = none).
    const ChecksumType checksum =
        itdb_.device != nullptr ? itdb_.device->checksum_type() : ChecksumType::None;
    switch (checksum) {
        case ChecksumType::HashAB:
            u16(4);
            break;
        case ChecksumType::Hash72:
            u16(2);
            break;
        default:
            u16(0);
            break;
    }
    u16(0);                // hash72
    u32_n0(11);            // hash72
    // 0xa0
    u16(static_cast<std::uint16_t>(itdb_.audio_language));
    u16(static_cast<std::uint16_t>(itdb_.subtitle_language));
    u16(static_cast<std::uint16_t>(itdb_.unk_0xa4));
    u16(static_cast<std::uint16_t>(itdb_.unk_0xa6));
    u16(static_cast<std::uint16_t>(itdb_.unk_0xa8));
    u8(0);
    // 0xab: hashAB
    u8(0);
    u32_n0(14);            // hashAB
    u32_n0(4);             // dummy space
}

void Export::write_mhsd_header(std::uint32_t type) {
    fourcc("mhsd");
    u32(96);               // header size
    u32(0);                // size of the whole section, patched later
    u32(type);             // 1 = tracks, 2 = playlists, ...
    u32_n0(20);
}

void Export::write_mhlt_header(std::uint32_t tracks) {
    fourcc("mhlt");
    u32(92);               // header size
    u32(tracks);
    u32_n0(20);
}

void Export::write_mhit_header(const Track& track) {
    fourcc("mhit");
    u32(kMhitHeaderSize);
    u32(0);                // size of the whole record, patched later
    u32(0);                // number of mhods, patched later
    // 0x10
    u32(track.id);
    u32(track.visible);
    u32(track.filetype_marker);
    u8(track.type1);
    u8(track.type2);
    u8(track.compilation);
    u8(static_cast<std::uint8_t>(track.rating));
    // 0x20
    u32(static_cast<std::uint32_t>(mac_time(track.time_modified)));
    u32(track.size);
    u32(static_cast<std::uint32_t>(track.tracklen));
    u32(static_cast<std::uint32_t>(track.track_nr));
    // 0x30
    u32(static_cast<std::uint32_t>(track.tracks));
    u32(static_cast<std::uint32_t>(track.year));
    u32(static_cast<std::uint32_t>(track.bitrate));
    u32((static_cast<std::uint32_t>(track.samplerate) << 16) |
        static_cast<std::uint32_t>(track.samplerate_low));
    // 0x40
    u32(static_cast<std::uint32_t>(track.volume));
    u32(track.starttime);
    u32(track.stoptime);
    u32(track.soundcheck);
    // 0x50
    u32(track.playcount);
    u32(track.playcount2);
    u32(static_cast<std::uint32_t>(mac_time(track.time_played)));
    u32(static_cast<std::uint32_t>(track.cd_nr));
    // 0x60
    u32(static_cast<std::uint32_t>(track.cds));
    u32(track.drm_userid);
    u32(static_cast<std::uint32_t>(mac_time(track.time_added)));
    u32(track.bookmark_time);
    // 0x70
    u64(track.dbid);
    u8(track.checked != 0 ? 1 : 0);
    u8(track.app_rating);
    u16(static_cast<std::uint16_t>(track.bpm));
    u16(track.artwork_count);
    u16(track.unk126);
    // 0x80
    u32(track.artwork_size);
    u32(track.unk132);
    f32(track.samplerate2);
    u32(static_cast<std::uint32_t>(mac_time(track.time_released)));
    // 0x90
    u16(track.unk144);
    u16(track.explicit_flag);
    u32(track.unk148);
    u32(track.unk152);
    // 0x9c: since database version 0x0c
    u32(track.skipcount);
    // 0xa0
    u32(static_cast<std::uint32_t>(mac_time(track.last_skipped)));
    u8(track.has_artwork);
    u8(track.skip_when_shuffling);
    u8(track.remember_playback_position);
    u8(track.flag4);
    u64(track.dbid2);
    // 0xb0
    u8(track.lyrics_flag);
    u8(track.movie_flag);
    u8(track.mark_unplayed);
    u8(track.unk179);
    u32(track.unk180);
    u32(track.pregap);
    u64(track.samplecount);
    u32(track.unk196);
    u32(track.postgap);
    u32(track.unk204);
    // 0xd0
    u32(static_cast<std::uint32_t>(track.mediatype));
    u32(track.season_nr);
    u32(track.episode_nr);
    u32(track.unk220);
    // 0xe0
    u32(track.unk224);
    u32(track.unk228);
    u32(track.unk232);
    u32(track.unk236);
    // 0xf0
    u32(track.unk240);
    u32(track.unk244);
    u32(track.gapless_data);
    u32(track.unk252);
    // 0x100
    u16(track.gapless_track_flag);
    u16(track.gapless_album_flag);
    u32_n0(7);
    // 0x120
    u32(track.album_id);
    u64(itdb_.id_0x24);    // same as mhbd+0x24, purpose unknown
    u32(track.size);       // appears to be the file size again
    // 0x130
    u32(0);
    u64(0x808080808080ULL);
    u32(0);
    // 0x140
    u32_n0(2);
    if (has_flag(track.mediatype, Mediatype::EpubBook) ||
        has_flag(track.mediatype, Mediatype::PdfBook)) {
        u16(1);
        u16(1);
    } else {
        u32(0);
    }
    u32_n0(5);
    // 0x160: mhii_link matches mhii::song_id in the ArtworkDB and is needed on
    // fat nanos and the iPod Classic to get artwork in the side pane.
    u32(track.mhii_link);
    u32(0);
    u32(1);
    u32(0);
    // 0x170
    u32_n0(28);
    // 0x1e0
    u32(track.artist_id);
    u32_n0(4);
    // 0x1f4
    u32(track.composer_id);
    u32_n0(20);            // padding
}

void Export::write_mhia_header(const ItemId& item) {
    fourcc("mhia");
    u32(88);
    u32(0);                // total size, patched later
    u32(0);                // number of child mhods, patched later
    u32(item.id);
    u64(item.sql_id);
    u32(2);
    u32_n0(14);
}

void Export::write_mhla_header(std::uint32_t albums) {
    fourcc("mhla");
    u32(92);
    u32(albums);
    u32_n0(20);
}

void Export::write_mhlp_header() {
    fourcc("mhlp");
    u32(92);
    u32(0);                // number of playlists, patched later
    u32_n0(20);
}

void Export::write_mhii_header(const ItemId& item) {
    fourcc("mhii");
    u32(80);
    u32(0);                // total size, patched later
    u32(0);                // number of child mhods, patched later
    u32(item.id);
    u64(item.sql_id);
    u32(2);
    u32_n0(12);
}

void Export::write_mhli_header(std::uint32_t artists) {
    fourcc("mhli");
    u32(92);
    u32(artists);
    u32_n0(20);
}

void Export::write_mhip_header(std::uint32_t childcount, std::uint32_t podcast_group_flag,
                               std::uint32_t podcast_group_id, std::uint32_t track_id,
                               std::uint32_t timestamp, std::uint32_t podcast_group_ref) {
    fourcc("mhip");
    u32(76);
    u32(0);                // total size, patched later
    u32(childcount);
    u32(podcast_group_flag);
    u32(podcast_group_id);
    u32(track_id);
    u32(static_cast<std::uint32_t>(mac_time(static_cast<std::time_t>(timestamp))));
    u32(podcast_group_ref);
    u32_n0(10);
}

void Export::write_mhod_string(std::uint32_t type, std::string_view value) {
    const std::string_view text = without_nul(value);
    if (reversed()) {
        // Reverse endian databases store these strings as UTF-8.
        const std::uint32_t length = static_cast<std::uint32_t>(text.size());
        fourcc("mhod");
        u32(24);
        u32(length + 40);
        u32(type);
        u32_n0(2);
        // end of header, start of data
        u32(2);            // string type UTF-8
        u32(length);
        u8(1);
        u8(0);
        u8(0);
        u8(0);
        u32(0);
        w_.bytes(text);
        return;
    }
    const std::u16string units = utf8_to_utf16(text);
    const std::uint32_t length = static_cast<std::uint32_t>(units.size());
    fourcc("mhod");
    u32(24);
    u32(2 * length + 40);
    u32(type);
    u32_n0(2);
    // end of header, start of data
    u32(1);                // string type UTF-16
    u32(2 * length);       // size of the string in bytes
    u32(1);
    u32(0);
    w_.utf16le(units);
}

void Export::write_mhod_url(std::uint32_t type, std::string_view value) {
    // Podcast URLs are UTF-8 regardless of the database byte order and carry no
    // string header.
    const std::string_view text = without_nul(value);
    const std::uint32_t length = static_cast<std::uint32_t>(text.size());
    fourcc("mhod");
    u32(24);
    u32(24 + length);
    u32(type);
    u32_n0(2);
    w_.bytes(text);
}

void Export::write_mhod_playlist_position(std::uint32_t position) {
    fourcc("mhod");
    u32(24);
    u32(44);
    u32(kMhodPlaylist);
    u32_n0(2);
    // end of header, start of data
    u32(position);
    u32_n0(4);
}

void Export::write_mhod_chapterdata(const ChapterData& chapterdata) {
    const std::size_t mhod_seek = pos();
    fourcc("mhod");
    u32(24);
    u32(0);                // total size, patched later
    u32(kMhodChapterData);
    u32_n0(2);

    // The chapter blob is big endian even in little endian databases.
    u32_flipped(chapterdata.unk024);
    u32_flipped(chapterdata.unk028);
    u32_flipped(chapterdata.unk032);
    const std::size_t atom_seek = pos();
    u32_flipped(0);        // total length of the sean atom, patched later
    fourcc("sean");
    u32_flipped(1);
    u32_flipped(static_cast<std::uint32_t>(chapterdata.chapters.size() + 1));
    u32_flipped(0);
    for (const Chapter& chapter : chapterdata.chapters) {
        const std::u16string units = utf8_to_utf16(without_nul(chapter.chapter_title));
        const std::uint32_t length = static_cast<std::uint32_t>(units.size());
        u32_flipped(42 + 2 * length);
        fourcc("chap");
        u32_flipped(chapter.startpos);
        u32_flipped(1);
        u32_flipped(0);
        u32_flipped(22 + 2 * length);
        fourcc("name");
        u32_flipped(1);
        u32_n0(2);
        u16_flipped(static_cast<std::uint16_t>(length));
        w_.utf16be(units);
    }
    u32_flipped(28);
    fourcc("hedr");
    u32_flipped(1);
    u32_flipped(0);
    u32_n0(2);
    u32_flipped(1);
    // The atom length includes its own four bytes.
    w_.patch_u32be(atom_seek, static_cast<std::uint32_t>(pos() - atom_seek));

    patch_size(mhod_seek);
}

void Export::write_mhod_splpref(const SmartPlaylistPrefs& prefs) {
    fourcc("mhod");
    u32(24);
    u32(96);
    u32(kMhodSplPref);
    u32_n0(2);
    // end of header, start of data
    u8(prefs.live_update);
    u8(prefs.check_rules != 0 ? 1 : 0);
    u8(prefs.check_limits);
    u8(static_cast<std::uint8_t>(prefs.limit_type));
    u8(static_cast<std::uint8_t>(prefs.limit_sort & 0xffU));
    u8(0);
    u8(0);
    u8(0);
    u32(prefs.limit_value);
    u8(prefs.match_checked_only);
    u8(static_cast<std::uint8_t>((prefs.limit_sort & 0x80000000U) != 0 ? 1 : 0));
    u8(0);
    u8(0);
    u32_n0(14);
}

void Export::write_mhod_splrules(const SmartPlaylistRules& rules) {
    const std::size_t mhod_seek = pos();
    fourcc("mhod");
    u32(24);
    u32(0);                // total size, patched later
    u32(kMhodSplRules);
    u32_n0(2);

    // The smart playlist blob is the only part of the database that is always
    // big endian.
    fourcc("SLst");
    u32_flipped(rules.unk004);
    u32_flipped(static_cast<std::uint32_t>(rules.rules.size()));
    u32_flipped(static_cast<std::uint32_t>(rules.match_operator));
    u32_n0(30);

    for (const SmartPlaylistRule& original : rules.rules) {
        // The reference implementation normalises every rule before writing it
        // (itdb_splr_validate()); a copy keeps the caller's model intact.
        SmartPlaylistRule rule = original;
        validate(rule);
        const SmartPlaylistFieldType type = field_type(rule);
        u32_flipped(static_cast<std::uint32_t>(rule.field));
        u32_flipped(static_cast<std::uint32_t>(rule.action));
        u32_n0(11);
        if (type == SmartPlaylistFieldType::String) {
            const std::u16string units = utf8_to_utf16(without_nul(rule.string));
            u32_flipped(static_cast<std::uint32_t>(units.size() * 2));
            w_.utf16be(units);
            continue;
        }
        std::uint64_t from_value = rule.from_value;
        std::uint64_t to_value = rule.to_value;
        const SmartPlaylistActionType atype = action_type(rule);
        if (type == SmartPlaylistFieldType::Date &&
            (atype == SmartPlaylistActionType::Date ||
             atype == SmartPlaylistActionType::RangeDate)) {
            from_value = mac_time(static_cast<std::time_t>(from_value));
            to_value = mac_time(static_cast<std::time_t>(to_value));
        }
        u32_flipped(0x44);
        u64_flipped(from_value);
        u64_flipped(static_cast<std::uint64_t>(rule.from_date));
        u64_flipped(rule.from_units);
        u64_flipped(to_value);
        u64_flipped(static_cast<std::uint64_t>(rule.to_date));
        u64_flipped(rule.to_units);
        u32_flipped(rule.unk052);
        u32_flipped(rule.unk056);
        u32_flipped(rule.unk060);
        u32_flipped(rule.unk064);
        u32_flipped(rule.unk068);
    }
    patch_size(mhod_seek);
}

void Export::write_long_mhod_playlist() {
    // A hard-coded playlist settings mhod.  iTunes shows no data at all when it
    // is missing, so the values are copied from the reference implementation.
    const std::size_t mhod_seek = pos();
    fourcc("mhod");
    u32(0x18);
    u32(0x0288);
    u32(kMhodPlaylist);
    u32_n0(6);
    u32(0x010084);
    u32(0x05);
    u32(0x09);
    u32(0x03);
    u32(0x120001);
    u32_n0(3);
    u32(0xc80002);
    u32_n0(3);
    u32(0x3c000d);
    u32_n0(3);
    u32(0x7d0004);
    u32_n0(3);
    u32(0x7d0003);
    u32_n0(3);
    u32(0x640008);
    u32_n0(3);
    u32(0x640017);
    u32(0x01);
    u32_n0(2);
    u32(0x500014);
    u32(0x01);
    u32_n0(2);
    u32(0x7d0015);
    u32(0x01);
    u32_n0(114);
    // The reference implementation writes 0x0288 as the record length; patch
    // it from the actual position so the two can never drift apart.
    w_.patch_u32(mhod_seek + 8, static_cast<std::uint32_t>(pos() - mhod_seek));
}

std::vector<JumpEntry> Export::write_mhod52(std::uint32_t sort_type,
                                            std::vector<CollateTrack>& tracks) {
    // Sort by the key belonging to @sort_type.  Ties keep the playlist order
    // because std::stable_sort is used, matching g_list_sort().
    switch (sort_type) {
        case kSortTitle:
            std::stable_sort(tracks.begin(), tracks.end(),
                             [](const CollateTrack& a, const CollateTrack& b) {
                                 return compare_keys(a.title, b.title) < 0;
                             });
            break;
        case kSortAlbum:
            std::stable_sort(tracks.begin(), tracks.end(),
                             [](const CollateTrack& a, const CollateTrack& b) {
                                 int result = compare_keys(a.album, b.album);
                                 if (result == 0) {
                                     result = a.cd_nr - b.cd_nr;
                                 }
                                 if (result == 0) {
                                     result = a.track_nr - b.track_nr;
                                 }
                                 if (result == 0) {
                                     result = compare_keys(a.title, b.title);
                                 }
                                 return result < 0;
                             });
            break;
        case kSortArtist:
            std::stable_sort(tracks.begin(), tracks.end(),
                             [](const CollateTrack& a, const CollateTrack& b) {
                                 int result = compare_keys(a.artist, b.artist);
                                 if (result == 0) {
                                     result = compare_keys(a.album, b.album);
                                 }
                                 if (result == 0) {
                                     result = a.cd_nr - b.cd_nr;
                                 }
                                 if (result == 0) {
                                     result = a.track_nr - b.track_nr;
                                 }
                                 if (result == 0) {
                                     result = compare_keys(a.title, b.title);
                                 }
                                 return result < 0;
                             });
            break;
        case kSortGenre:
            std::stable_sort(tracks.begin(), tracks.end(),
                             [](const CollateTrack& a, const CollateTrack& b) {
                                 int result = compare_keys(a.genre, b.genre);
                                 if (result == 0) {
                                     result = compare_keys(a.artist, b.artist);
                                 }
                                 if (result == 0) {
                                     result = compare_keys(a.album, b.album);
                                 }
                                 if (result == 0) {
                                     result = a.cd_nr - b.cd_nr;
                                 }
                                 if (result == 0) {
                                     result = a.track_nr - b.track_nr;
                                 }
                                 if (result == 0) {
                                     result = compare_keys(a.title, b.title);
                                 }
                                 return result < 0;
                             });
            break;
        case kSortComposer:
            std::stable_sort(tracks.begin(), tracks.end(),
                             [](const CollateTrack& a, const CollateTrack& b) {
                                 int result = compare_keys(a.composer, b.composer);
                                 if (result == 0) {
                                     result = compare_keys(a.album, b.album);
                                 }
                                 if (result == 0) {
                                     result = a.cd_nr - b.cd_nr;
                                 }
                                 if (result == 0) {
                                     result = a.track_nr - b.track_nr;
                                 }
                                 if (result == 0) {
                                     result = compare_keys(a.title, b.title);
                                 }
                                 return result < 0;
                             });
            break;
        default:
            break;
    }

    const std::uint32_t numtracks = static_cast<std::uint32_t>(tracks.size());
    fourcc("mhod");
    u32(24);
    u32(4 * numtracks + 72);
    u32(kMhodLibPlaylistIndex);
    u32_n0(2);
    // end of header, start of data
    u32(sort_type);
    u32(numtracks);
    u32_n0(10);

    std::vector<JumpEntry> entries;
    std::uint32_t index = 0;
    for (const CollateTrack& track : tracks) {
        u32(track.index);

        std::uint16_t letter = u'0';
        switch (sort_type) {
            case kSortTitle:
                letter = track.letter_title;
                break;
            case kSortAlbum:
                letter = track.letter_album;
                break;
            case kSortArtist:
                letter = track.letter_artist;
                break;
            case kSortGenre:
                letter = track.letter_genre;
                break;
            case kSortComposer:
                letter = track.letter_composer;
                break;
            default:
                break;
        }
        if (entries.empty() || entries.back().letter != letter) {
            entries.push_back(JumpEntry{letter, index, 0});
        }
        ++entries.back().count;
        ++index;
    }
    return entries;
}

void Export::write_mhod53(std::uint32_t sort_type, const std::vector<JumpEntry>& entries) {
    const std::uint32_t count = static_cast<std::uint32_t>(entries.size());
    fourcc("mhod");
    u32(24);
    u32(12 * count + 40);
    u32(kMhodLibPlaylistJumpTable);
    u32_n0(2);
    u32(sort_type);
    u32(count);
    u32_n0(2);
    for (const JumpEntry& entry : entries) {
        u16(entry.letter);
        u16(0);
        u32(entry.start);
        u32(entry.count);
    }
}

void Export::write_mhsd_tracks() {
    const std::size_t mhsd_seek = pos();
    write_mhsd_header(1);
    write_mhlt_header(static_cast<std::uint32_t>(itdb_.tracks.size()));

    for (const std::unique_ptr<Track>& entry : itdb_.tracks) {
        const Track& track = *entry;
        const std::size_t mhit_seek = pos();
        write_mhit_header(track);

        std::uint32_t mhod_count = 0;
        const auto add_string = [&](std::uint32_t type, const std::string& value) {
            if (value.empty()) {
                return;
            }
            write_mhod_string(type, value);
            ++mhod_count;
        };
        add_string(kMhodTitle, track.title);
        add_string(kMhodArtist, track.artist);
        add_string(kMhodAlbum, track.album);
        add_string(kMhodFiletype, track.filetype);
        add_string(kMhodComment, track.comment);
        add_string(kMhodPath, track.ipod_path);
        add_string(kMhodGenre, track.genre);
        add_string(kMhodCategory, track.category);
        add_string(kMhodComposer, track.composer);
        add_string(kMhodGrouping, track.grouping);
        add_string(kMhodDescription, track.description);
        add_string(kMhodSubtitle, track.subtitle);
        add_string(kMhodTvShow, track.tvshow);
        add_string(kMhodTvEpisode, track.tvepisode);
        add_string(kMhodTvNetwork, track.tvnetwork);
        add_string(kMhodAlbumArtist, track.albumartist);
        add_string(kMhodKeywords, track.keywords);
        if (!track.podcasturl.empty()) {
            write_mhod_url(kMhodPodcastUrl, track.podcasturl);
            ++mhod_count;
        }
        if (!track.podcastrss.empty()) {
            write_mhod_url(kMhodPodcastRss, track.podcastrss);
            ++mhod_count;
        }
        add_string(kMhodSortArtist, track.sort_artist);
        add_string(kMhodSortTitle, track.sort_title);
        add_string(kMhodSortAlbum, track.sort_album);
        add_string(kMhodSortAlbumArtist, track.sort_albumartist);
        add_string(kMhodSortComposer, track.sort_composer);
        add_string(kMhodSortTvShow, track.sort_tvshow);
        if (track.chapterdata != nullptr && !track.chapterdata->chapters.empty()) {
            write_mhod_chapterdata(*track.chapterdata);
            ++mhod_count;
        }

        w_.patch_u32(mhit_seek + 8, static_cast<std::uint32_t>(pos() - mhit_seek));
        w_.patch_u32(mhit_seek + 12, mhod_count);
    }

    patch_size(mhsd_seek);
}

void Export::write_mhsd_playlists(std::uint32_t mhsd_type) {
    const std::size_t mhsd_seek = pos();
    write_mhsd_header(mhsd_type);
    const std::size_t mhlp_seek = pos();
    write_mhlp_header();

    std::uint32_t playlist_count = 0;
    if (mhsd_type == 5) {
        for (const Playlist* playlist : itdb_.mhsd5_playlists) {
            if (playlist == nullptr) {
                continue;
            }
            write_playlist(*playlist, mhsd_type);
            ++playlist_count;
        }
    } else {
        for (const std::unique_ptr<Playlist>& entry : itdb_.playlists) {
            write_playlist(*entry, mhsd_type);
            ++playlist_count;
        }
    }

    w_.patch_u32(mhlp_seek + 8, playlist_count);
    patch_size(mhsd_seek);
}

void Export::write_mhsd_albums() {
    const std::size_t mhsd_seek = pos();
    write_mhsd_header(4);
    write_mhla_header(static_cast<std::uint32_t>(album_order_.size()));
    for (const AlbumEntry& entry : album_order_) {
        const std::size_t mhia_seek = pos();
        write_mhia_header(entry.item);

        std::uint32_t mhod_count = 0;
        const Track& track = *entry.representative;
        if (!track.album.empty()) {
            write_mhod_string(kMhodAlbumAlbum, track.album);
            ++mhod_count;
        }
        if (!track.albumartist.empty()) {
            write_mhod_string(kMhodAlbumArtistName, track.albumartist);
            ++mhod_count;
        } else if (!track.artist.empty()) {
            write_mhod_string(kMhodAlbumArtistName, track.artist);
            ++mhod_count;
        }
        if (!track.sort_albumartist.empty()) {
            write_mhod_string(kMhodAlbumSortArtist, track.sort_albumartist);
            ++mhod_count;
        } else if (!track.sort_artist.empty()) {
            write_mhod_string(kMhodAlbumSortArtist, track.sort_artist);
            ++mhod_count;
        }

        w_.patch_u32(mhia_seek + 8, static_cast<std::uint32_t>(pos() - mhia_seek));
        w_.patch_u32(mhia_seek + 12, mhod_count);
    }
    patch_size(mhsd_seek);
}

void Export::write_mhsd_artists() {
    const std::size_t mhsd_seek = pos();
    write_mhsd_header(8);
    write_mhli_header(static_cast<std::uint32_t>(artist_order_.size()));
    for (const ArtistEntry& entry : artist_order_) {
        const std::size_t mhii_seek = pos();
        write_mhii_header(entry.item);

        std::uint32_t mhod_count = 0;
        if (!entry.representative->artist.empty()) {
            write_mhod_string(kMhodAlbumArtistMhii, entry.representative->artist);
            ++mhod_count;
        }
        w_.patch_u32(mhii_seek + 8, static_cast<std::uint32_t>(pos() - mhii_seek));
        w_.patch_u32(mhii_seek + 12, mhod_count);
    }
    patch_size(mhsd_seek);
}

void Export::write_mhsd_empty(std::uint32_t type) {
    const std::size_t mhsd_seek = pos();
    write_mhsd_header(type);
    write_mhlt_header(0);
    patch_size(mhsd_seek);
}

void Export::write_mhsd_genius() {
    const std::size_t mhsd_seek = pos();
    write_mhsd_header(9);
    w_.bytes(without_nul(itdb_.genius_cuid));
    patch_size(mhsd_seek);
}

std::vector<CollateTrack> Export::make_collate_tracks(const Playlist& playlist) {
    std::vector<CollateTrack> tracks;
    tracks.reserve(playlist.members.size());

    std::uint32_t index = 0;
    for (const Track* member : playlist.members) {
        if (member == nullptr) {
            continue;
        }
        const Track& track = *member;
        CollateTrack entry;
        entry.index = index++;

        const std::string album_source =
            !track.sort_album.empty() ? track.sort_album : track.album;
        entry.album = collation_key(album_source);
        entry.letter_album = jump_table_letter(album_source);

        const std::string title_source =
            !track.sort_title.empty() ? track.sort_title : track.title;
        entry.title = collation_key(title_source);
        entry.letter_title = jump_table_letter(title_source);

        const std::string artist_source = sort_artist_value(track);
        entry.artist = collation_key(artist_source);
        entry.letter_artist = jump_table_letter(artist_source);

        entry.genre = collation_key(track.genre);
        entry.letter_genre = jump_table_letter(track.genre);

        const std::string composer_source =
            !track.sort_composer.empty() ? track.sort_composer : track.composer;
        entry.composer = collation_key(composer_source);
        entry.letter_composer = jump_table_letter(composer_source);

        entry.track_nr = track.track_nr;
        entry.cd_nr = track.cd_nr;
        tracks.push_back(std::move(entry));
    }
    return tracks;
}

void Export::write_mhod52_tables(const Playlist& playlist) {
    std::vector<CollateTrack> tracks = make_collate_tracks(playlist);
    if (tracks.empty()) {
        return;
    }
    // The same list is sorted five times, as in the reference implementation;
    // every comparator is a total order on the collation keys, so each pass
    // produces the order of the current key.
    for (const std::uint32_t sort_type :
         {kSortTitle, kSortArtist, kSortAlbum, kSortGenre, kSortComposer}) {
        const std::vector<JumpEntry> entries = write_mhod52(sort_type, tracks);
        write_mhod53(sort_type, entries);
    }
}

void Export::write_playlist_mhips(const Playlist& playlist, std::size_t mhyp_seek) {
    std::uint32_t position = 0;
    for (const Track* member : playlist.members) {
        if (member == nullptr) {
            continue;
        }
        const std::size_t mhip_seek = pos();
        write_mhip_header(1, 0, 0, member->id, 0, 0);
        // With iTunes 4.9 the mhod is counted as a child of the mhip, so the
        // size of the mhip covers the mhod as well.
        write_mhod_playlist_position(position);
        patch_size(mhip_seek);
        ++position;
    }
    w_.patch_u32(mhyp_seek + 16, position);
}

void Export::write_podcast_mhips(const Playlist& playlist, std::size_t mhyp_seek) {
    // Podcasts are grouped into one mhip per album, with the episodes as child
    // mhips pointing at the group.
    std::vector<std::pair<std::string, std::vector<const Track*>>> groups;
    for (const Track* member : playlist.members) {
        if (member == nullptr) {
            continue;
        }
        const auto found = std::find_if(groups.begin(), groups.end(),
                                        [&member](const auto& group) {
                                            return group.first == member->album;
                                        });
        if (found == groups.end()) {
            groups.push_back({member->album, {member}});
        } else {
            found->second.push_back(member);
        }
    }

    std::uint32_t written = 0;
    for (const auto& group : groups) {
        const std::uint32_t group_id = next_id_++;
        std::size_t mhip_seek = pos();
        write_mhip_header(1, 256, group_id, 0, 0, 0);
        write_mhod_string(kMhodTitle, group.first);
        patch_size(mhip_seek);
        ++written;

        for (const Track* member : group.second) {
            const std::uint32_t mhip_id = next_id_++;
            mhip_seek = pos();
            write_mhip_header(1, 0, mhip_id, member->id, 0, group_id);
            // The reference implementation stores the mhip id here rather than
            // the position inside the playlist.
            write_mhod_playlist_position(mhip_id);
            patch_size(mhip_seek);
            ++written;
        }
    }
    w_.patch_u32(mhyp_seek + 16, written);
}

void Export::write_playlist(const Playlist& playlist, std::uint32_t mhsd_type) {
    const std::size_t mhyp_seek = pos();
    fourcc("mhyp");
    u32(108);              // header length
    u32(0);                // total size, patched later

    const bool master_with_members =
        playlist.type == PlaylistType::Master && !playlist.members.empty();
    // The count has to match the mhods written below: the title, the playlist
    // settings, and either the mhod 52/53 tables or the two smart playlist
    // mhods.
    std::uint32_t mhod_count = 2;
    if (master_with_members) {
        mhod_count += 10;  // five mhod 52 lists and five mhod 53 tables
    } else if (playlist.is_smart) {
        mhod_count += 2;   // smart playlist preferences and rules
    }
    u32(mhod_count);
    u32(0);                // number of mhips, patched later
    u8(static_cast<std::uint8_t>(playlist.type));
    u8(playlist.flag1);
    u8(playlist.flag2);
    u8(playlist.flag3);
    u32(static_cast<std::uint32_t>(mac_time(playlist.timestamp)));
    u64(playlist.id);
    u32(0);
    u16(1);                // string mhod count
    u16(static_cast<std::uint16_t>(playlist.podcast_flag));
    u32(static_cast<std::uint32_t>(playlist.sort_order));
    if (mhsd_type == 5) {
        u32_n0(8);
        // 0x50
        const std::uint16_t mhsd5_type = static_cast<std::uint16_t>(playlist.mhsd5_type);
        u16(mhsd5_type);
        u16(mhsd5_type);
        // 0x54: 1 for movie rentals and ringtones
        if (playlist.mhsd5_type == Mhsd5Type::MovieRentals ||
            playlist.mhsd5_type == Mhsd5Type::Ringtones) {
            u32(1);
        } else {
            u32(0);
        }
        u32_n0(5);
    } else {
        u32_n0(15);
    }

    write_mhod_string(kMhodTitle, playlist.name);
    write_long_mhod_playlist();

    if (master_with_members) {
        write_mhod52_tables(playlist);
    } else if (playlist.is_smart) {
        write_mhod_splpref(playlist.spl_prefs);
        write_mhod_splrules(playlist.spl_rules);
    }

    if (playlist.is_podcasts() && mhsd_type == 3) {
        write_podcast_mhips(playlist, mhyp_seek);
    } else if (mhsd_type == 5) {
        w_.patch_u32(mhyp_seek + 16, 0);
    } else {
        write_playlist_mhips(playlist, mhyp_seek);
    }

    patch_size(mhyp_seek);
}

void Export::run() {
    prepare();

    std::uint32_t children = 8;
    if (!itdb_.genius_cuid.empty()) {
        ++children;
    }

    const std::size_t mhbd_seek = pos();
    write_mhbd_header(children);

    write_mhsd_tracks();          // mhsd type 1: the track list
    write_mhsd_playlists(3);      // mhsd type 3: podcast flavour playlists
    write_mhsd_playlists(2);      // mhsd type 2: standard playlists
    write_mhsd_albums();          // mhsd type 4
    write_mhsd_artists();         // mhsd type 8
    write_mhsd_empty(6);          // mhsd type 6, always empty
    write_mhsd_empty(10);         // mhsd type 10, always empty
    write_mhsd_playlists(5);      // mhsd type 5: podcasts, movies, ...
    if (!itdb_.genius_cuid.empty()) {
        write_mhsd_genius();      // mhsd type 9
    }

    patch_size(mhbd_seek);

    // Checksums are applied to the complete image by write_itunesdb() after
    // the writer has been taken, so no bytes can change afterwards.
}

// ---------------------------------------------------------------------------
// iTunesSD (iPod Shuffle)
// ---------------------------------------------------------------------------

[[nodiscard]] bool contains_any(std::string_view haystack,
                                std::initializer_list<std::string_view> needles) {
    return std::any_of(needles.begin(), needles.end(),
                       [haystack](std::string_view needle) {
                           return haystack.find(needle) != std::string_view::npos;
                       });
}

// Converts an iTunesDB file type description into the iTunesSD file type code.
[[nodiscard]] std::uint32_t convert_filetype(const std::string& filetype) {
    if (contains_any(filetype, {"AAC", "MP4", "M4A", "aac", "mp4", "m4a"})) {
        return 0x02;
    }
    if (contains_any(filetype, {"WAV", "wav"})) {
        return 0x04;
    }
    return 0x01;           // MP3 is the default
}

class ShuffleExport {
  public:
    ShuffleExport(Itdb& itdb, ByteWriter& writer, bool legacy)
        : itdb_(itdb), w_(writer), legacy_(legacy) {}

    void run();

  private:
    [[nodiscard]] std::size_t pos() const noexcept { return w_.size(); }
    void fourcc(std::string_view tag) {
        if (w_.order() == ByteOrder::Big) {
            std::array<std::uint8_t, 4> bytes{};
            for (std::size_t i = 0; i < 4; ++i) {
                bytes[i] = static_cast<std::uint8_t>(tag[3 - i]);
            }
            w_.bytes(bytes);
            return;
        }
        w_.bytes(tag);
    }
    void u8(std::uint8_t value) { w_.u8(value); }
    void u16(std::uint16_t value) { w_.u16(value); }
    void u32(std::uint32_t value) { w_.u32(value); }
    void u64(std::uint64_t value) { w_.u64(value); }
    void u32_n0(std::size_t count) { w_.zeros(4 * count); }
    void u16_n0(std::size_t count) { w_.zeros(2 * count); }
    void u24_be(std::uint32_t value) {
        u8(static_cast<std::uint8_t>((value >> 16) & 0xffU));
        u8(static_cast<std::uint8_t>((value >> 8) & 0xffU));
        u8(static_cast<std::uint8_t>(value & 0xffU));
    }
    void patch_short_size(std::size_t header_seek) {
        w_.patch_u32(header_seek + 4, static_cast<std::uint32_t>(pos() - header_seek));
    }

    void write_legacy();
    void write_bdhs();
    void write_hths();
    void write_rths(const Track& track);
    void write_hphs();
    void write_lphs(const Playlist& playlist);

    Itdb& itdb_;
    ByteWriter& w_;
    bool legacy_ = false;
};

void ShuffleExport::write_rths(const Track& track) {
    const std::size_t rths_seek = pos();
    // The path field is 256 bytes long; keep the truncation in sync with the
    // padding below.
    std::string path = track.ipod_path.substr(0, 256);
    std::replace(path.begin(), path.end(), ':', '/');

    fourcc("rths");
    u32(0);                // length, patched below
    u32(track.starttime);
    u32(track.stoptime);
    u32(static_cast<std::uint32_t>(track.volume));
    u32(convert_filetype(track.filetype));
    w_.bytes(path.c_str());   // writes up to the terminating NUL
    w_.zeros(256 - path.size());
    u32(track.bookmark_time);
    // This field has the opposite value of the iTunesDB one: 1 skips the track
    // in shuffle mode.
    u8(track.skip_when_shuffling != 0 ? 0 : 1);
    u8(track.remember_playback_position);
    u8(0);                 // gapless album, best guess
    u8(0);
    u32(track.pregap);
    u32(track.postgap);
    u32(static_cast<std::uint32_t>(track.samplecount));
    u32(0);
    u32(track.gapless_data);
    u32(0);
    u32(track.album_id);
    u16(static_cast<std::uint16_t>(track.track_nr));
    u16(static_cast<std::uint16_t>(track.cd_nr));
    u32_n0(2);
    u64(track.dbid);       // also the voiceover file name
    u32(track.artist_id);
    u32_n0(8);
    patch_short_size(rths_seek);
}

void ShuffleExport::write_hths() {
    const std::size_t hths_seek = pos();
    const std::uint32_t track_count = static_cast<std::uint32_t>(itdb_.tracks.size());
    std::uint32_t non_standard = 0;

    // 36 is the "track header offset" field of the bdhs header.
    w_.patch_u32(36, static_cast<std::uint32_t>(pos()));

    fourcc("hths");
    u32(0);                // length, patched below
    u32(track_count);
    u32_n0(2);
    const std::size_t offset_seek = pos();
    u32_n0(track_count);
    patch_short_size(hths_seek);

    std::size_t track_offset = offset_seek;
    for (const std::unique_ptr<Track>& entry : itdb_.tracks) {
        const Track& track = *entry;
        w_.patch_u32(track_offset, static_cast<std::uint32_t>(pos()));
        track_offset += 4;
        write_rths(track);
        if (has_flag(track.mediatype, Mediatype::Audiobook) ||
            has_flag(track.mediatype, Mediatype::Podcast)) {
            ++non_standard;
        }
    }
    // 32 is the "number of tracks excluding podcasts and audiobooks" field.
    w_.patch_u32(32, track_count - non_standard);
}

void ShuffleExport::write_lphs(const Playlist& playlist) {
    const std::size_t lphs_seek = pos();

    // 1 master, 2 normal, 3 podcasts, 4 audiobooks.
    std::uint32_t shuffle_type = 2;
    if (playlist.type == PlaylistType::Master) {
        shuffle_type = 1;
    } else if (playlist.is_podcasts()) {
        shuffle_type = 3;
    } else if (playlist.is_audiobooks()) {
        shuffle_type = 4;
    }

    fourcc("lphs");
    u32(0);                // length, patched below
    u32(static_cast<std::uint32_t>(playlist.members.size()));
    u32(0);                // non podcast/audiobook tracks, patched below
    if (shuffle_type == 1) {
        u32_n0(2);
    } else {
        u64(playlist.id);
    }
    u32(shuffle_type);
    u32_n0(4);

    std::uint32_t non_standard = 0;
    for (const Track* member : playlist.members) {
        if (member == nullptr) {
            continue;
        }
        if (has_flag(member->mediatype, Mediatype::Audiobook) ||
            has_flag(member->mediatype, Mediatype::Podcast)) {
            ++non_standard;
        }
        std::uint32_t index = 0;
        for (const std::unique_ptr<Track>& entry : itdb_.tracks) {
            if (entry->dbid == member->dbid) {
                break;
            }
            ++index;
        }
        u32(index);
    }

    w_.patch_u32(lphs_seek + 12,
                 static_cast<std::uint32_t>(playlist.members.size()) - non_standard);
    patch_short_size(lphs_seek);
}

void ShuffleExport::write_hphs() {
    const std::size_t hphs_seek = pos();
    std::uint32_t playlist_count = 0;
    std::uint32_t podcast_count = 0;
    std::uint32_t master_count = 0;
    std::uint32_t audiobook_count = 0;

    for (const std::unique_ptr<Playlist>& entry : itdb_.playlists) {
        const Playlist& playlist = *entry;
        if (playlist.members.empty()) {
            continue;
        }
        ++playlist_count;
        if (playlist.type == PlaylistType::Master) {
            ++master_count;
        } else if (playlist.is_podcasts()) {
            ++podcast_count;
        } else if (playlist.is_audiobooks()) {
            ++audiobook_count;
        }
    }

    // 16 and 40 are fields of the bdhs header written before this one.
    w_.patch_u32(16, playlist_count);
    w_.patch_u32(40, static_cast<std::uint32_t>(pos()));

    fourcc("hphs");
    u32(0);                // length, patched below
    u16(static_cast<std::uint16_t>(playlist_count));
    u16_n0(1);
    u16(static_cast<std::uint16_t>(playlist_count - podcast_count));
    u16(static_cast<std::uint16_t>(master_count));
    u16(static_cast<std::uint16_t>(playlist_count - audiobook_count));
    u16_n0(1);
    const std::size_t offset_seek = pos();
    u32_n0(playlist_count);
    patch_short_size(hphs_seek);

    std::size_t playlist_offset = offset_seek;
    for (const std::unique_ptr<Playlist>& entry : itdb_.playlists) {
        if (entry->members.empty()) {
            continue;
        }
        w_.patch_u32(playlist_offset, static_cast<std::uint32_t>(pos()));
        playlist_offset += 4;
        write_lphs(*entry);
    }
}

void ShuffleExport::write_bdhs() {
    const std::size_t bdhs_seek = pos();
    fourcc("bdhs");
    u32(0x02000003);
    u32(0);                // length, patched below
    u32(static_cast<std::uint32_t>(itdb_.tracks.size()));
    u32(0);                // nonempty playlist count, patched by write_hphs()
    u32_n0(2);
    u8(0);                 // limit max volume: off
    u8(1);                 // voiceover: on
    u16_n0(1);
    // The following three offsets are patched by write_hths() and write_hphs();
    // keep them in sync with those patched values if this header changes.
    u32(0);                // number of tracks excluding podcasts/audiobooks
    u32(0);                // track header offset
    u32(0);                // playlist header offset
    u32_n0(5);
    w_.patch_u32(bdhs_seek + 8, static_cast<std::uint32_t>(pos() - bdhs_seek));
}

void ShuffleExport::write_legacy() {
    u24_be(static_cast<std::uint32_t>(itdb_.tracks.size()));
    u24_be(0x010600);
    u24_be(0x12);          // header size
    u24_be(0);
    u24_be(0);
    u24_be(0);

    for (const std::unique_ptr<Track>& entry : itdb_.tracks) {
        const Track& track = *entry;
        u24_be(0x00022e);
        u24_be(0x5aa501);
        // The legacy format stores the start and stop times in 256 ms steps.
        u24_be(track.starttime / 256);
        u24_be(0);
        u24_be(0);
        u24_be(track.stoptime / 256);
        u24_be(0);
        u24_be(0);
        // Volume ranges from -255 to +255 and has to map to 0..200.
        u24_be(static_cast<std::uint32_t>(((track.volume + 255) * 201) / 511));
        u24_be(convert_filetype(track.filetype));
        u24_be(0x200);

        std::string path = track.ipod_path;
        std::replace(path.begin(), path.end(), ':', '/');
        std::u16string units = utf8_to_utf16(path);
        if (units.size() > 261) {
            units.resize(261);
        }
        w_.utf16le(units);
        u16_n0(261 - units.size());

        u8(track.skip_when_shuffling);
        u8(track.remember_playback_position);
        u8(0);
    }
}

void ShuffleExport::run() {
    if (legacy_) {
        write_legacy();
        return;
    }
    write_bdhs();
    write_hths();
    write_hphs();
}

[[nodiscard]] bool device_is_shuffle(const Itdb& itdb) {
    return itdb.device != nullptr && itdb.device->shadow_db_version() != ShadowDbVersion::Unknown;
}

}  // namespace

void write_itunesdb(Itdb& itdb, const std::filesystem::path& filename) {
    if (filename.empty()) {
        fail(ErrorCode::InvalidArgument, "no filename given to write the iTunesDB to");
    }

    ByteWriter writer(itdb.device != nullptr ? itdb.device->byte_order : ByteOrder::Little);
    Export export_itdb(itdb, writer);
    export_itdb.run();

    Bytes image = writer.take();
    if (itdb.device != nullptr && itdb.device->supports_compressed_itunesdb()) {
        if (image.size() < 0xa9 || image[0] != 'm' || image[1] != 'h' ||
            image[2] != 'b' || image[3] != 'd') {
            fail(ErrorCode::Corrupt, "cannot compress a malformed mhbd image");
        }
        const std::uint32_t header_len = ByteReader(image, ByteOrder::Little).u32le(4);
        if (header_len < 0xa9 || header_len > image.size()) {
            fail(ErrorCode::Corrupt, "iTunesCDB header length is invalid");
        }
        image[0xa8] = 1;  // compressed
        const Bytes compressed = zlib_compress(
            std::span<const std::uint8_t>(image.data() + header_len,
                                          image.size() - header_len));
        Bytes packed(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(header_len));
        packed.insert(packed.end(), compressed.begin(), compressed.end());
        if (packed.size() > std::numeric_limits<std::uint32_t>::max()) {
            fail(ErrorCode::Unsupported, "compressed iTunesCDB exceeds 4 GiB");
        }
        const auto total = static_cast<std::uint32_t>(packed.size());
        packed[8] = static_cast<std::uint8_t>(total & 0xffU);
        packed[9] = static_cast<std::uint8_t>((total >> 8U) & 0xffU);
        packed[10] = static_cast<std::uint8_t>((total >> 16U) & 0xffU);
        packed[11] = static_cast<std::uint8_t>((total >> 24U) & 0xffU);
        image = std::move(packed);
    }
    // iPods from the Classic and Nano 3G onwards verify a checksum over the whole
    // image.  Only little endian databases carry one, and write_checksum() is a
    // no-op for devices that need none.
    if (itdb.device != nullptr && itdb.device->byte_order == ByteOrder::Little &&
        image.size() >= 4 && image[0] == 'm' && image[1] == 'h' &&
        image[2] == 'b' && image[3] == 'd') {
        write_checksum(*itdb.device, image);
    }

    write_file_atomic(filename, image);
    itdb.filename = filename;
}

void write_itunesdb(Itdb& itdb) {
    const bool compressed =
        itdb.device != nullptr && itdb.device->supports_compressed_itunesdb();
    std::filesystem::path target = itdb.filename;
    if (compressed && !itdb.mountpoint().empty()) {
        target = itunescdb_path(itdb.mountpoint());
    } else if (target.empty()) {
        if (itdb.mountpoint().empty()) {
            fail(ErrorCode::InvalidArgument,
                 "no filename and no mount point to write the iTunesDB to");
        }
        target = itunesdb_path(itdb.mountpoint());
    }
    write_itunesdb(itdb, target);

    // An iPod Shuffle reads the simplified iTunesSD in addition to the
    // iTunesDB.  Only the mount point based entry point writes it, mirroring
    // the reference implementation's itdb_write().
    if (device_is_shuffle(itdb) && !itdb.mountpoint().empty()) {
        ByteWriter writer(itdb.device->byte_order);
        ShuffleExport shuffle(itdb, writer,
                              itdb.device->shadow_db_version() == ShadowDbVersion::V1);
        shuffle.run();
        write_file_atomic(itunessd_path(itdb.mountpoint()), writer.data());
    }

    if (compressed && !itdb.mountpoint().empty()) {
        // An iPhone gets confused when a compressed iTunesCDB and a non-empty
        // iTunesDB are both present.
        write_file_atomic(itunesdb_path(itdb.mountpoint()), Bytes{});
    }
}

}  // namespace gpod
