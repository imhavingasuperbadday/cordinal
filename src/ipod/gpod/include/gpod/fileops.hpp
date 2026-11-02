// Media-file transfer helpers.  These are the pure C++ equivalents of
// itdb_cp(), itdb_cp_get_dest_filename(), itdb_cp_finalize(),
// itdb_cp_track_to_ipod() and itdb_rename_files().

#ifndef GPOD_FILEOPS_HPP
#define GPOD_FILEOPS_HPP

#include <filesystem>
#include <memory>

#include "gpod/model.hpp"

namespace gpod {

// Copies bytes from one file to another.  The destination is removed if the
// copy fails, so a partial file is not left behind.
void copy_file(const std::filesystem::path& from, const std::filesystem::path& to);

// Counts the F00..Fnn directories under iPod_Control/Music and caches the
// result in device.musicdirs.  Returns 0 when the Music directory is absent.
int musicdir_count(Device& device);
int musicdir_count(const std::filesystem::path& mountpoint);

// Chooses a free "libgpodNNNNNN.ext" path for source.  If track already has
// an ipod_path, that path is returned instead.
[[nodiscard]] std::filesystem::path destination_filename(Track& track,
                                                         const std::filesystem::path& source);
[[nodiscard]] std::filesystem::path destination_filename(
    const std::filesystem::path& mountpoint, const std::filesystem::path& source);

// Fills ipod_path, filetype_marker, size and transferred after a successful
// copy.  Throws ErrorCode::Corrupt when destination is outside the mountpoint.
Track& finalize_track(Track& track, const std::filesystem::path& destination,
                      const std::filesystem::path& mountpoint);
[[nodiscard]] std::unique_ptr<Track> track_from_file(
    const std::filesystem::path& destination, const std::filesystem::path& mountpoint);

// Copies a track's media file to the iPod and finalizes it.  Returns true
// immediately when track.transferred is already set.
bool copy_track_to_ipod(Track& track, const std::filesystem::path& source);

// Renames Play Counts to Play Counts.bak and removes the transient OTG and
// Shuffle files, matching itdb_rename_files().
void rename_files(const std::filesystem::path& mountpoint);

}  // namespace gpod

#endif  // GPOD_FILEOPS_HPP
