// Canonical iPod directory layout.  These replace the gchar* helpers from
// itdb_itunesdb.c with std::filesystem::path.

#ifndef GPOD_PATHS_HPP
#define GPOD_PATHS_HPP

#include <filesystem>

namespace gpod {

[[nodiscard]] std::filesystem::path control_dir(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path itunes_dir(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path music_dir(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path artwork_dir(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path photos_dir(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path photos_thumb_dir(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path device_dir(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path itunesdb_path(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path itunescdb_path(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path itunessd_path(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path artworkdb_path(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path photodb_path(const std::filesystem::path& mountpoint);
[[nodiscard]] std::filesystem::path path_in(const std::filesystem::path& directory,
                                           const std::filesystem::path& file);

// Convert between the ":"-separated paths stored in iTunesDB and a native
// filesystem path.
[[nodiscard]] std::filesystem::path ipod_path_to_fs(std::string_view ipod_path);
[[nodiscard]] std::string fs_path_to_ipod(const std::filesystem::path& path);

}  // namespace gpod

#endif  // GPOD_PATHS_HPP
