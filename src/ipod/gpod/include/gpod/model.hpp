// In-memory model for the pure C++ libgpod rewrite.
//
// Ownership rules:
//   * Itdb owns tracks and playlists.
//   * Playlist members are non-owning Track pointers.
//   * PhotoDb owns photos and albums; PhotoAlbum members are non-owning.
//   * Track owns its Artwork; Artwork owns its Thumb.
//
// These rules replace GList/GHashTable ownership with unique_ptr vectors and
// keep object addresses stable when records are inserted or reordered.

#ifndef GPOD_MODEL_HPP
#define GPOD_MODEL_HPP

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gpod/types.hpp"

namespace gpod {

class Itdb;
class Device;
class Playlist;
class PhotoDb;
struct SysInfoExtended;

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] bool empty() const noexcept {
        return width <= 0 || height <= 0 || rgba.empty();
    }
};

struct Chapter {
    std::uint32_t startpos = 0;
    std::string chapter_title;
};

struct ChapterData {
    std::vector<Chapter> chapters;
    std::uint32_t unk024 = 0;
    std::uint32_t unk028 = 0;
    std::uint32_t unk032 = 0;

    [[nodiscard]] std::unique_ptr<ChapterData> clone() const;
    Chapter& add_chapter(std::uint32_t startpos, std::string title);
    void remove_chapter(std::size_t index);
    void clear() noexcept { chapters.clear(); }
};

enum class ThumbSource : std::uint8_t { Invalid, File, Memory, Ipod };

struct IpodThumbItem {
    const ArtworkFormat* format = nullptr;
    std::filesystem::path filename;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::int16_t width = 0;
    std::int16_t height = 0;
    std::int16_t horizontal_padding = 0;
    std::int16_t vertical_padding = 0;
};

struct Thumb {
    ThumbSource source = ThumbSource::Invalid;
    unsigned rotation = 0;
    std::filesystem::path filename;
    std::vector<std::uint8_t> image_data;
    std::shared_ptr<Image> image;
    std::vector<IpodThumbItem> ipod_items;

    [[nodiscard]] std::unique_ptr<Thumb> clone() const;
};

struct Artwork {
    std::unique_ptr<Thumb> thumbnail;
    std::uint32_t id = 0;
    std::uint64_t dbid = 0;
    std::int32_t unk028 = 0;
    std::uint32_t rating = 0;
    std::int32_t unk036 = 0;
    std::time_t creation_date = 0;
    std::time_t digitized_date = 0;
    std::uint32_t artwork_size = 0;

    [[nodiscard]] std::unique_ptr<Artwork> clone() const;
};

class Track {
public:
    Track();
    ~Track() = default;
    Track(const Track&) = delete;
    Track& operator=(const Track&) = delete;
    Track(Track&&) = default;
    Track& operator=(Track&&) = default;

    Itdb* itdb = nullptr;

    std::string title;
    std::string ipod_path;
    std::string album;
    std::string artist;
    std::string genre;
    std::string filetype;
    std::string comment;
    std::string category;
    std::string composer;
    std::string grouping;
    std::string description;
    std::string podcasturl;
    std::string podcastrss;
    std::unique_ptr<ChapterData> chapterdata;
    std::string subtitle;
    std::string tvshow;
    std::string tvepisode;
    std::string tvnetwork;
    std::string albumartist;
    std::string keywords;
    std::string sort_artist;
    std::string sort_title;
    std::string sort_album;
    std::string sort_albumartist;
    std::string sort_composer;
    std::string sort_tvshow;

    std::uint32_t id = 0;
    std::uint32_t size = 0;
    std::int32_t tracklen = 0;
    std::int32_t cd_nr = 0;
    std::int32_t cds = 0;
    std::int32_t track_nr = 0;
    std::int32_t tracks = 0;
    std::int32_t bitrate = 0;
    std::uint16_t samplerate = 0;
    std::uint16_t samplerate_low = 0;
    std::int32_t year = 0;
    std::int32_t volume = 0;
    std::uint32_t soundcheck = 0;
    std::time_t time_added = 0;
    std::time_t time_modified = 0;
    std::time_t time_played = 0;
    std::uint32_t bookmark_time = 0;
    std::uint32_t rating = 0;
    std::uint32_t playcount = 0;
    std::uint32_t playcount2 = 0;
    std::uint32_t recent_playcount = 0;
    bool transferred = false;
    std::int16_t bpm = 0;
    std::uint8_t app_rating = 0;
    std::uint8_t type1 = 0;
    std::uint8_t type2 = 0;
    std::uint8_t compilation = 0;
    std::uint32_t starttime = 0;
    std::uint32_t stoptime = 0;
    std::uint8_t checked = 0;
    std::uint64_t dbid = 0;
    std::uint32_t drm_userid = 0;
    std::uint32_t visible = 0;
    std::uint32_t filetype_marker = 0;
    std::uint16_t artwork_count = 0;
    std::uint32_t artwork_size = 0;
    float samplerate2 = 0.0F;
    std::uint16_t unk126 = 0;
    std::uint32_t unk132 = 0;
    std::time_t time_released = 0;
    std::uint16_t unk144 = 0;
    std::uint16_t explicit_flag = 0;
    std::uint32_t unk148 = 0;
    std::uint32_t unk152 = 0;
    std::uint32_t skipcount = 0;
    std::uint32_t recent_skipcount = 0;
    std::uint32_t last_skipped = 0;
    std::uint8_t has_artwork = 0;
    std::uint8_t skip_when_shuffling = 0;
    std::uint8_t remember_playback_position = 0;
    std::uint8_t flag4 = 0;
    std::uint64_t dbid2 = 0;
    std::uint8_t lyrics_flag = 0;
    std::uint8_t movie_flag = 0;
    std::uint8_t mark_unplayed = 0;
    std::uint8_t unk179 = 0;
    std::uint32_t unk180 = 0;
    std::uint32_t pregap = 0;
    std::uint64_t samplecount = 0;
    std::uint32_t unk196 = 0;
    std::uint32_t postgap = 0;
    std::uint32_t unk204 = 0;
    Mediatype mediatype = Mediatype::Audio;
    std::uint32_t season_nr = 0;
    std::uint32_t episode_nr = 0;
    std::uint32_t unk220 = 0;
    std::uint32_t unk224 = 0;
    std::uint32_t unk228 = 0;
    std::uint32_t unk232 = 0;
    std::uint32_t unk236 = 0;
    std::uint32_t unk240 = 0;
    std::uint32_t unk244 = 0;
    std::uint32_t gapless_data = 0;
    std::uint32_t unk252 = 0;
    std::uint16_t gapless_track_flag = 0;
    std::uint16_t gapless_album_flag = 0;
    std::uint16_t obsolete = 0;

    std::unique_ptr<Artwork> artwork;
    std::uint32_t mhii_link = 0;

    std::uint32_t album_id = 0;
    std::uint32_t artist_id = 0;
    std::uint32_t composer_id = 0;

    [[nodiscard]] std::unique_ptr<Track> clone() const;
    [[nodiscard]] bool has_thumbnails() const noexcept;
    void remove_thumbnails() noexcept;
    [[nodiscard]] std::filesystem::path filename_on_ipod() const;
};

class Playlist {
public:
    Playlist() = default;
    ~Playlist() = default;
    Playlist(const Playlist&) = delete;
    Playlist& operator=(const Playlist&) = delete;
    Playlist(Playlist&&) = default;
    Playlist& operator=(Playlist&&) = default;

    Itdb* itdb = nullptr;
    std::string name;
    PlaylistType type = PlaylistType::Normal;
    std::uint8_t flag1 = 0;
    std::uint8_t flag2 = 0;
    std::uint8_t flag3 = 0;
    std::vector<Track*> members;
    bool is_smart = false;
    std::time_t timestamp = 0;
    std::uint64_t id = 0;
    PlaylistSortOrder sort_order = PlaylistSortOrder::Manual;
    std::uint32_t podcast_flag = 0;
    SmartPlaylistPrefs spl_prefs;
    SmartPlaylistRules spl_rules;
    Mhsd5Type mhsd5_type = Mhsd5Type::None;

    [[nodiscard]] std::unique_ptr<Playlist> clone() const;
    [[nodiscard]] bool is_master() const noexcept { return type == PlaylistType::Master; }
    [[nodiscard]] bool is_podcasts() const noexcept { return podcast_flag == 1; }
    [[nodiscard]] bool is_audiobooks() const noexcept;
    [[nodiscard]] std::size_t track_count() const noexcept { return members.size(); }
    [[nodiscard]] bool contains(const Track* track) const noexcept;
    void set_master() noexcept { type = PlaylistType::Master; }
    void set_podcasts() noexcept { podcast_flag = 1; }
    void add_track(Track* track, std::optional<std::size_t> position = std::nullopt);
    void remove_track(Track* track);
    void randomize();
};

struct StorageInfo {
    std::uint64_t capacity = 0;
    std::uint64_t free = 0;
};

class Device {
public:
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    std::filesystem::path mountpoint;
    std::int32_t musicdirs = 0;
    ByteOrder byte_order = ByteOrder::Little;
    std::map<std::string, std::string, std::less<>> sysinfo;
    std::shared_ptr<SysInfoExtended> sysinfo_extended;
    bool sysinfo_changed = false;
    std::int32_t timezone_shift = 0;
    int iphone_sync_nest_level = 0;

    std::vector<ArtworkFormat> cover_art_formats;
    std::vector<ArtworkFormat> photo_formats;
    std::vector<ArtworkFormat> chapter_image_formats;

    // Capacity and free bytes of the volume holding the mount point, like
    // itdb_device_get_storage_info().  Empty when the mount point is unset or
    // the platform cannot report its space.
    [[nodiscard]] std::optional<StorageInfo> storage_info() const;

    [[nodiscard]] IpodInfo ipod_info() const;
    [[nodiscard]] bool supports_artwork() const;
    [[nodiscard]] bool supports_chapter_image() const;
    [[nodiscard]] bool supports_video() const;
    [[nodiscard]] bool supports_photo() const;
    [[nodiscard]] bool supports_podcast() const;
    [[nodiscard]] bool supports_sparse_artwork() const;
    [[nodiscard]] bool supports_compressed_itunesdb() const;
    [[nodiscard]] bool supports_sqlite_db() const;
    [[nodiscard]] ChecksumType checksum_type() const;
    [[nodiscard]] ShadowDbVersion shadow_db_version() const;
    [[nodiscard]] std::string sysinfo_value(std::string_view field) const;
    void set_sysinfo(std::string field, std::string value);
    void set_mountpoint(const std::filesystem::path& path);
    [[nodiscard]] std::string uuid() const;
    [[nodiscard]] std::string firewire_id() const;
};

class Itdb {
public:
    Itdb();
    ~Itdb();
    Itdb(const Itdb&) = delete;
    Itdb& operator=(const Itdb&) = delete;
    Itdb(Itdb&&) = delete;
    Itdb& operator=(Itdb&&) = delete;

    std::vector<std::unique_ptr<Track>> tracks;
    std::vector<std::unique_ptr<Playlist>> playlists;
    std::filesystem::path filename;
    std::unique_ptr<Device> device;
    std::uint32_t version = 0;
    std::uint64_t id = 0;
    std::int32_t tzoffset = 0;

    std::uint16_t platform = 0;
    std::uint16_t unk_0x22 = 0;
    std::uint64_t id_0x24 = 0;
    std::uint16_t language = 0;
    std::uint64_t pid = 0;
    std::int32_t unk_0x50 = 0;
    std::int32_t unk_0x54 = 0;
    std::int16_t audio_language = 0;
    std::int16_t subtitle_language = 0;
    std::int16_t unk_0xa4 = 0;
    std::int16_t unk_0xa6 = 0;
    std::int16_t unk_0xa8 = 0;
    std::string genius_cuid;
    std::vector<Playlist*> mhsd5_playlists;

    [[nodiscard]] static std::unique_ptr<Itdb> create();
    [[nodiscard]] static std::unique_ptr<Itdb> parse(const std::filesystem::path& mountpoint);
    [[nodiscard]] static std::unique_ptr<Itdb> parse_file(const std::filesystem::path& filename);

    // Complete mount-point sync: refresh ArtworkDB, write iTunesDB/iTunesCDB and
    // iTunesSD, then generate the SQLite shadow databases when the device uses
    // them.  Throws ErrorCode::Unsupported on such a device when no runtime
    // SQLite library can be loaded.
    void write();
    // Write the track database to an explicit path.  The SQLite shadow databases
    // still go to the device mount point when loaded; no ArtworkDB is written.
    void write_file(const std::filesystem::path& filename);

    [[nodiscard]] std::size_t track_count() const noexcept { return tracks.size(); }
    [[nodiscard]] std::uint32_t non_transferred_track_count() const noexcept;
    [[nodiscard]] std::size_t playlist_count() const noexcept { return playlists.size(); }

    [[nodiscard]] Playlist* master_playlist() noexcept;
    [[nodiscard]] const Playlist* master_playlist() const noexcept;
    [[nodiscard]] Playlist* podcasts_playlist() noexcept;
    [[nodiscard]] Playlist* playlist_by_id(std::uint64_t id) noexcept;
    [[nodiscard]] Playlist* playlist_by_name(std::string_view name) noexcept;
    [[nodiscard]] Playlist* playlist_by_nr(std::size_t nr) noexcept;
    [[nodiscard]] Track* track_by_id(std::uint32_t id) noexcept;
    [[nodiscard]] const Track* track_by_id(std::uint32_t id) const noexcept;

    Track* add_track(std::unique_ptr<Track> track, std::optional<std::size_t> position = std::nullopt);
    // Also removes the track from every playlist.  Upstream itdb_track_remove()
    // leaves playlist membership behind, which would leave dangling pointers in
    // this owning model.
    void remove_track(Track* track);
    Playlist* add_playlist(std::unique_ptr<Playlist> playlist,
                           std::optional<std::size_t> position = std::nullopt);
    void remove_playlist(Playlist* playlist);

    void set_mountpoint(const std::filesystem::path& mountpoint);
    [[nodiscard]] const std::filesystem::path& mountpoint() const;
    [[nodiscard]] int musicdirs() const;
};

class PhotoAlbum {
public:
    PhotoAlbum() = default;
    ~PhotoAlbum() = default;
    PhotoAlbum(const PhotoAlbum&) = delete;
    PhotoAlbum& operator=(const PhotoAlbum&) = delete;
    PhotoAlbum(PhotoAlbum&&) = default;
    PhotoAlbum& operator=(PhotoAlbum&&) = default;

    PhotoDb* photodb = nullptr;
    std::string name;
    std::vector<Artwork*> members;
    std::uint8_t album_type = 0;
    std::uint8_t playmusic = 0;
    std::uint8_t repeat = 0;
    std::uint8_t random = 0;
    std::uint8_t show_titles = 0;
    std::uint8_t transition_direction = 0;
    std::int32_t slide_duration = 0;
    std::int32_t transition_duration = 0;
    std::int64_t song_id = 0;
    std::int32_t unk024 = 0;
    std::int16_t unk028 = 0;
    std::int32_t unk044 = 0;
    std::int32_t unk048 = 0;
    std::int32_t album_id = 0;
    std::int32_t prev_album_id = 0;

    [[nodiscard]] std::unique_ptr<PhotoAlbum> clone() const;
};

class PhotoDb {
public:
    PhotoDb();
    ~PhotoDb();
    PhotoDb(const PhotoDb&) = delete;
    PhotoDb& operator=(const PhotoDb&) = delete;
    PhotoDb(PhotoDb&&) = delete;
    PhotoDb& operator=(PhotoDb&&) = delete;

    std::vector<std::unique_ptr<Artwork>> photos;
    std::vector<std::unique_ptr<PhotoAlbum>> photoalbums;
    std::unique_ptr<Device> device;

    [[nodiscard]] static std::unique_ptr<PhotoDb> create(const std::filesystem::path& mountpoint);
    [[nodiscard]] static std::unique_ptr<PhotoDb> parse(const std::filesystem::path& mountpoint);
    void write();
    [[nodiscard]] PhotoAlbum* album_by_name(std::string_view name) noexcept;
};

[[nodiscard]] std::span<const IpodInfo> ipod_info_table();
[[nodiscard]] std::string_view to_string(IpodModel model) noexcept;
[[nodiscard]] std::string_view to_string(IpodGeneration generation) noexcept;

}  // namespace gpod

#endif  // GPOD_MODEL_HPP
