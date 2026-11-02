#include "gpod/model.hpp"

#include <algorithm>
#include <random>

#include "gpod/artworkdb.hpp"
#include "gpod/error.hpp"
#include "gpod/itunesdb.hpp"
#include "gpod/paths.hpp"
#include "gpod/photodb.hpp"
#include "gpod/sqlite.hpp"

namespace gpod {
namespace {

std::uint64_t random_u64() {
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    return generator();
}

bool haystack(const std::string& filetype, std::initializer_list<std::string_view> needles) {
    return std::any_of(needles.begin(), needles.end(), [&](std::string_view needle) {
        return filetype.find(needle) != std::string::npos;
    });
}

void apply_track_defaults(Track& track, Itdb& itdb) {
    if (track.mark_unplayed == 0) {
        track.mark_unplayed = 0x01;
    }

    if (track.unk126 == 0) {
        if (haystack(track.filetype, {"MPEG", "MP3", "mpeg", "mp3"})) {
            track.unk126 = 0xffff;
        } else if (haystack(track.filetype, {"AAC", "MP4", "M4A", "aac", "mp4", "m4a"})) {
            track.unk126 = haystack(track.filetype, {"Audible", "audible", "Book", "book"})
                               ? 0x0001
                               : 0xffff;
        } else {
            track.unk126 = 0x0000;
        }
    }

    if (track.unk144 == 0) {
        if (haystack(track.filetype, {"MPEG", "MP3", "mpeg", "mp3"})) {
            track.unk144 = 0x000c;
        } else if (haystack(track.filetype, {"AAC", "MP4", "M4A", "aac", "mp4", "m4a"})) {
            track.unk144 = haystack(track.filetype, {"Audible", "audible", "Book", "book"})
                               ? 0x0029
                               : 0x0033;
        } else {
            track.unk144 = 0x0000;
        }
    }

    if (itdb.device != nullptr && itdb.device->supports_video() &&
        (track.mediatype == Mediatype::None || track.mediatype == Mediatype::Audio)) {
        if (haystack(track.filetype, {"M4V", "MP4", "MP4V", "m4v", "mp4", "mp4v"}) ||
            haystack(track.filetype, {"MOV", "mov"})) {
            track.mediatype = Mediatype::Movie;
        } else if (track.mediatype == Mediatype::None) {
            track.mediatype = Mediatype::Audio;
        }
    }

    track.samplerate2 = static_cast<float>(track.samplerate);

    if (track.dbid == 0) {
        std::uint64_t id = 0;
        do {
            id = random_u64();
            for (const auto& candidate : itdb.tracks) {
                if (candidate->dbid == id) {
                    id = 0;
                    break;
                }
            }
        } while (id == 0);
        track.dbid = id;
        track.dbid2 = id;
    }
    if (track.dbid2 == 0) {
        track.dbid2 = track.dbid;
    }
}

}  // namespace

Track::Track() {
    visible = 1;
    artwork = std::make_unique<Artwork>();
    chapterdata = std::make_unique<ChapterData>();
}

std::unique_ptr<ChapterData> ChapterData::clone() const {
    auto copy = std::make_unique<ChapterData>();
    copy->chapters = chapters;
    copy->unk024 = unk024;
    copy->unk028 = unk028;
    copy->unk032 = unk032;
    return copy;
}

std::unique_ptr<Thumb> Thumb::clone() const {
    auto copy = std::make_unique<Thumb>();
    copy->source = source;
    copy->rotation = rotation;
    copy->filename = filename;
    copy->image_data = image_data;
    copy->image = image;
    copy->ipod_items = ipod_items;
    return copy;
}

std::unique_ptr<Artwork> Artwork::clone() const {
    auto copy = std::make_unique<Artwork>();
    copy->thumbnail = thumbnail ? thumbnail->clone() : nullptr;
    copy->id = id;
    copy->dbid = dbid;
    copy->unk028 = unk028;
    copy->rating = rating;
    copy->unk036 = unk036;
    copy->creation_date = creation_date;
    copy->digitized_date = digitized_date;
    copy->artwork_size = artwork_size;
    return copy;
}

std::unique_ptr<Track> Track::clone() const {
    auto copy = std::make_unique<Track>();
    copy->itdb = nullptr;

#define GPOD_COPY_FIELD(field) copy->field = field
    GPOD_COPY_FIELD(title);
    GPOD_COPY_FIELD(ipod_path);
    GPOD_COPY_FIELD(album);
    GPOD_COPY_FIELD(artist);
    GPOD_COPY_FIELD(genre);
    GPOD_COPY_FIELD(filetype);
    GPOD_COPY_FIELD(comment);
    GPOD_COPY_FIELD(category);
    GPOD_COPY_FIELD(composer);
    GPOD_COPY_FIELD(grouping);
    GPOD_COPY_FIELD(description);
    GPOD_COPY_FIELD(podcasturl);
    GPOD_COPY_FIELD(podcastrss);
    copy->chapterdata = chapterdata ? chapterdata->clone() : nullptr;
    GPOD_COPY_FIELD(subtitle);
    GPOD_COPY_FIELD(tvshow);
    GPOD_COPY_FIELD(tvepisode);
    GPOD_COPY_FIELD(tvnetwork);
    GPOD_COPY_FIELD(albumartist);
    GPOD_COPY_FIELD(keywords);
    GPOD_COPY_FIELD(sort_artist);
    GPOD_COPY_FIELD(sort_title);
    GPOD_COPY_FIELD(sort_album);
    GPOD_COPY_FIELD(sort_albumartist);
    GPOD_COPY_FIELD(sort_composer);
    GPOD_COPY_FIELD(sort_tvshow);

    GPOD_COPY_FIELD(id);
    GPOD_COPY_FIELD(size);
    GPOD_COPY_FIELD(tracklen);
    GPOD_COPY_FIELD(cd_nr);
    GPOD_COPY_FIELD(cds);
    GPOD_COPY_FIELD(track_nr);
    GPOD_COPY_FIELD(tracks);
    GPOD_COPY_FIELD(bitrate);
    GPOD_COPY_FIELD(samplerate);
    GPOD_COPY_FIELD(samplerate_low);
    GPOD_COPY_FIELD(year);
    GPOD_COPY_FIELD(volume);
    GPOD_COPY_FIELD(soundcheck);
    GPOD_COPY_FIELD(time_added);
    GPOD_COPY_FIELD(time_modified);
    GPOD_COPY_FIELD(time_played);
    GPOD_COPY_FIELD(bookmark_time);
    GPOD_COPY_FIELD(rating);
    GPOD_COPY_FIELD(playcount);
    GPOD_COPY_FIELD(playcount2);
    GPOD_COPY_FIELD(recent_playcount);
    GPOD_COPY_FIELD(transferred);
    GPOD_COPY_FIELD(bpm);
    GPOD_COPY_FIELD(app_rating);
    GPOD_COPY_FIELD(type1);
    GPOD_COPY_FIELD(type2);
    GPOD_COPY_FIELD(compilation);
    GPOD_COPY_FIELD(starttime);
    GPOD_COPY_FIELD(stoptime);
    GPOD_COPY_FIELD(checked);
    GPOD_COPY_FIELD(dbid);
    GPOD_COPY_FIELD(drm_userid);
    GPOD_COPY_FIELD(visible);
    GPOD_COPY_FIELD(filetype_marker);
    GPOD_COPY_FIELD(artwork_count);
    GPOD_COPY_FIELD(artwork_size);
    GPOD_COPY_FIELD(samplerate2);
    GPOD_COPY_FIELD(unk126);
    GPOD_COPY_FIELD(unk132);
    GPOD_COPY_FIELD(time_released);
    GPOD_COPY_FIELD(unk144);
    GPOD_COPY_FIELD(explicit_flag);
    GPOD_COPY_FIELD(unk148);
    GPOD_COPY_FIELD(unk152);
    GPOD_COPY_FIELD(skipcount);
    GPOD_COPY_FIELD(recent_skipcount);
    GPOD_COPY_FIELD(last_skipped);
    GPOD_COPY_FIELD(has_artwork);
    GPOD_COPY_FIELD(skip_when_shuffling);
    GPOD_COPY_FIELD(remember_playback_position);
    GPOD_COPY_FIELD(flag4);
    GPOD_COPY_FIELD(dbid2);
    GPOD_COPY_FIELD(lyrics_flag);
    GPOD_COPY_FIELD(movie_flag);
    GPOD_COPY_FIELD(mark_unplayed);
    GPOD_COPY_FIELD(unk179);
    GPOD_COPY_FIELD(unk180);
    GPOD_COPY_FIELD(pregap);
    GPOD_COPY_FIELD(samplecount);
    GPOD_COPY_FIELD(unk196);
    GPOD_COPY_FIELD(postgap);
    GPOD_COPY_FIELD(unk204);
    GPOD_COPY_FIELD(mediatype);
    GPOD_COPY_FIELD(season_nr);
    GPOD_COPY_FIELD(episode_nr);
    GPOD_COPY_FIELD(unk220);
    GPOD_COPY_FIELD(unk224);
    GPOD_COPY_FIELD(unk228);
    GPOD_COPY_FIELD(unk232);
    GPOD_COPY_FIELD(unk236);
    GPOD_COPY_FIELD(unk240);
    GPOD_COPY_FIELD(unk244);
    GPOD_COPY_FIELD(gapless_data);
    GPOD_COPY_FIELD(unk252);
    GPOD_COPY_FIELD(gapless_track_flag);
    GPOD_COPY_FIELD(gapless_album_flag);
    GPOD_COPY_FIELD(obsolete);

    copy->artwork = artwork ? artwork->clone() : nullptr;
    GPOD_COPY_FIELD(mhii_link);
    GPOD_COPY_FIELD(album_id);
    GPOD_COPY_FIELD(artist_id);
    GPOD_COPY_FIELD(composer_id);
#undef GPOD_COPY_FIELD
    return copy;
}

bool Track::has_thumbnails() const noexcept {
    return artwork != nullptr && artwork->thumbnail != nullptr;
}

void Track::remove_thumbnails() noexcept {
    if (artwork) {
        artwork->thumbnail.reset();
    }
    artwork_count = 0;
    artwork_size = 0;
    mhii_link = 0;
    // 2 means "explicitly no artwork", distinct from 0/unset.
    has_artwork = 0x02;
}

std::filesystem::path Track::filename_on_ipod() const {
    if (ipod_path.empty()) {
        return {};
    }
    std::filesystem::path relative = ipod_path_to_fs(ipod_path);
    // On Windows a path such as "/iPod_Control/..." has a root directory but
    // no root name, so is_absolute() is false.  Strip the root explicitly
    // before appending it to the mountpoint.
    if (relative.has_root_directory()) {
        relative = relative.relative_path();
    }
    if (itdb != nullptr && itdb->device != nullptr && !itdb->device->mountpoint.empty()) {
        return itdb->device->mountpoint / relative;
    }
    return relative;
}

std::unique_ptr<Playlist> Playlist::clone() const {
    auto copy = std::make_unique<Playlist>();
    copy->itdb = nullptr;
    copy->name = name;
    copy->type = type;
    copy->flag1 = flag1;
    copy->flag2 = flag2;
    copy->flag3 = flag3;
    copy->members = members;
    copy->is_smart = is_smart;
    copy->timestamp = timestamp;
    copy->id = 0;
    copy->sort_order = sort_order;
    copy->podcast_flag = podcast_flag;
    copy->spl_prefs = spl_prefs;
    copy->spl_rules = spl_rules;
    copy->mhsd5_type = mhsd5_type;
    return copy;
}

bool Playlist::is_audiobooks() const noexcept {
    if (members.empty()) {
        return false;
    }
    return std::all_of(members.begin(), members.end(), [](const Track* track) {
        return track != nullptr && has_flag(track->mediatype, Mediatype::Audiobook);
    });
}

bool Playlist::contains(const Track* track) const noexcept {
    return std::find(members.begin(), members.end(), track) != members.end();
}

void Playlist::add_track(Track* track, std::optional<std::size_t> position) {
    if (track == nullptr) {
        return;
    }
    if (itdb != nullptr) {
        track->itdb = itdb;
    }
    if (position && *position <= members.size()) {
        members.insert(members.begin() + static_cast<std::ptrdiff_t>(*position), track);
    } else {
        members.push_back(track);
    }
}

void Playlist::remove_track(Track* track) {
    const auto found = std::find(members.begin(), members.end(), track);
    if (found != members.end()) {
        members.erase(found);
    }
}

void Playlist::randomize() {
    static thread_local std::mt19937 generator{std::random_device{}()};
    std::shuffle(members.begin(), members.end(), generator);
}

Itdb::Itdb() : device(std::make_unique<Device>()) {
    version = 0x13;
    id = random_u64();
    pid = random_u64();
    language = 0x656e;
    platform = 1;
}

Itdb::~Itdb() = default;

std::unique_ptr<Itdb> Itdb::create() {
    return std::make_unique<Itdb>();
}

std::unique_ptr<Itdb> Itdb::parse(const std::filesystem::path& mountpoint) {
    auto itdb = parse_itunesdb(mountpoint);
    // Artwork is supplementary: a missing or damaged ArtworkDB must not make
    // the track database unreadable (upstream ignores this result too).
    if (itdb && itdb->device != nullptr && itdb->device->supports_artwork()) {
        try {
            read_artworkdb(*itdb);
        } catch (const Error&) {
        }
    }
    return itdb;
}

std::unique_ptr<Itdb> Itdb::parse_file(const std::filesystem::path& filename) {
    return parse_itunesdb_file(filename);
}

void Itdb::write() {
    if (device != nullptr && device->supports_artwork()) {
        try {
            write_artworkdb(*this);
        } catch (const Error&) {
            // The track database remains valid without refreshed artwork.
        }
    }
    // The mount-point overload picks iTunesCDB for compressed devices, writes
    // iTunesSD for a Shuffle and empties a stale iTunesDB.
    write_itunesdb(*this);
    // The writer's prepare pass has now assigned album, artist and composer
    // ids, which Library.itdb uses as its foreign keys.
    if (device != nullptr && device->supports_sqlite_db() && !mountpoint().empty()) {
        generate_sqlite_databases(*this);
    }
}

void Itdb::write_file(const std::filesystem::path& target) {
    if (filename.empty()) {
        filename = target;
    }
    write_itunesdb(*this, target);
    if (device != nullptr && device->supports_sqlite_db() && !mountpoint().empty()) {
        generate_sqlite_databases(*this);
    }
}

std::uint32_t Itdb::non_transferred_track_count() const noexcept {
    return static_cast<std::uint32_t>(std::count_if(
        tracks.begin(), tracks.end(),
        [](const std::unique_ptr<Track>& track) { return !track->transferred; }));
}

Playlist* Itdb::master_playlist() noexcept {
    for (const auto& playlist : playlists) {
        if (playlist->is_master()) {
            return playlist.get();
        }
    }
    return nullptr;
}

const Playlist* Itdb::master_playlist() const noexcept {
    for (const auto& playlist : playlists) {
        if (playlist->is_master()) {
            return playlist.get();
        }
    }
    return nullptr;
}

Playlist* Itdb::podcasts_playlist() noexcept {
    for (const auto& playlist : playlists) {
        if (playlist->is_podcasts()) {
            return playlist.get();
        }
    }
    return nullptr;
}

Playlist* Itdb::playlist_by_id(std::uint64_t value) noexcept {
    for (const auto& playlist : playlists) {
        if (playlist->id == value) {
            return playlist.get();
        }
    }
    return nullptr;
}

Playlist* Itdb::playlist_by_name(std::string_view value) noexcept {
    for (const auto& playlist : playlists) {
        if (playlist->name == value) {
            return playlist.get();
        }
    }
    return nullptr;
}

Playlist* Itdb::playlist_by_nr(std::size_t nr) noexcept {
    if (nr < playlists.size()) {
        return playlists[nr].get();
    }
    return nullptr;
}

Track* Itdb::track_by_id(std::uint32_t value) noexcept {
    for (const auto& track : tracks) {
        if (track->id == value) {
            return track.get();
        }
    }
    return nullptr;
}

const Track* Itdb::track_by_id(std::uint32_t value) const noexcept {
    for (const auto& track : tracks) {
        if (track->id == value) {
            return track.get();
        }
    }
    return nullptr;
}

Track* Itdb::add_track(std::unique_ptr<Track> track, std::optional<std::size_t> position) {
    if (!track) {
        return nullptr;
    }
    track->itdb = this;
    apply_track_defaults(*track, *this);
    Track* raw = track.get();
    if (position && *position <= tracks.size()) {
        tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(*position), std::move(track));
    } else {
        tracks.push_back(std::move(track));
    }
    return raw;
}

void Itdb::remove_track(Track* track) {
    if (track == nullptr) {
        return;
    }
    for (auto& playlist : playlists) {
        playlist->remove_track(track);
    }
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                [track](const std::unique_ptr<Track>& candidate) {
                                    return candidate.get() == track;
                                }),
                 tracks.end());
}

Playlist* Itdb::add_playlist(std::unique_ptr<Playlist> playlist,
                             std::optional<std::size_t> position) {
    if (!playlist) {
        return nullptr;
    }
    playlist->itdb = this;
    if (playlist->id == 0) {
        std::uint64_t candidate_id = 0;
        do {
            candidate_id = random_u64();
            for (const auto& candidate : playlists) {
                if (candidate->id == candidate_id) {
                    candidate_id = 0;
                    break;
                }
            }
        } while (candidate_id == 0);
        playlist->id = candidate_id;
    }
    if (playlist->timestamp == 0) {
        playlist->timestamp = std::time(nullptr);
    }
    Playlist* raw = playlist.get();
    if (position && *position <= playlists.size()) {
        playlists.insert(playlists.begin() + static_cast<std::ptrdiff_t>(*position),
                         std::move(playlist));
    } else {
        playlists.push_back(std::move(playlist));
    }
    return raw;
}

void Itdb::remove_playlist(Playlist* playlist) {
    std::erase(mhsd5_playlists, playlist);
    playlists.erase(std::remove_if(playlists.begin(), playlists.end(),
                                   [playlist](const std::unique_ptr<Playlist>& candidate) {
                                       return candidate.get() == playlist;
                                   }),
                    playlists.end());
}

void Itdb::set_mountpoint(const std::filesystem::path& path) {
    device->set_mountpoint(path);
    if (filename.empty()) {
        filename = itunesdb_path(path);
    }
}

const std::filesystem::path& Itdb::mountpoint() const {
    static const std::filesystem::path empty;
    return device ? device->mountpoint : empty;
}

int Itdb::musicdirs() const {
    if (device && device->musicdirs > 0) {
        return device->musicdirs;
    }
    int count = 0;
    const std::filesystem::path directory = music_dir(mountpoint());
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (!entry.is_directory(error)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.size() == 3 && name[0] == 'F' && name[1] >= '0' && name[1] <= '9' &&
            name[2] >= '0' && name[2] <= '9') {
            ++count;
        }
    }
    return count;
}

std::unique_ptr<PhotoAlbum> PhotoAlbum::clone() const {
    auto copy = std::make_unique<PhotoAlbum>();
    copy->photodb = photodb;
    copy->name = name;
    copy->members = members;
    copy->album_type = album_type;
    copy->playmusic = playmusic;
    copy->repeat = repeat;
    copy->random = random;
    copy->show_titles = show_titles;
    copy->transition_direction = transition_direction;
    copy->slide_duration = slide_duration;
    copy->transition_duration = transition_duration;
    copy->song_id = song_id;
    copy->unk024 = unk024;
    copy->unk028 = unk028;
    copy->unk044 = unk044;
    copy->unk048 = unk048;
    copy->album_id = album_id;
    copy->prev_album_id = prev_album_id;
    return copy;
}

PhotoDb::PhotoDb() = default;
PhotoDb::~PhotoDb() = default;

std::unique_ptr<PhotoDb> PhotoDb::create(const std::filesystem::path& mountpoint) {
    auto db = std::make_unique<PhotoDb>();
    db->device = std::make_unique<Device>();
    db->device->set_mountpoint(mountpoint);
    return db;
}

std::unique_ptr<PhotoDb> PhotoDb::parse(const std::filesystem::path& mountpoint) {
    return parse_photodb(mountpoint);
}

void PhotoDb::write() { write_photodb(*this); }

PhotoAlbum* PhotoDb::album_by_name(std::string_view value) noexcept {
    for (const auto& album : photoalbums) {
        if (album->name == value) {
            return album.get();
        }
    }
    return nullptr;
}

}  // namespace gpod
