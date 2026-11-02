#include "gpod/paths.hpp"

#include <algorithm>

namespace gpod {

std::filesystem::path control_dir(const std::filesystem::path& mountpoint) {
    return mountpoint / "iPod_Control";
}

std::filesystem::path itunes_dir(const std::filesystem::path& mountpoint) {
    return control_dir(mountpoint) / "iTunes";
}

std::filesystem::path music_dir(const std::filesystem::path& mountpoint) {
    return control_dir(mountpoint) / "Music";
}

std::filesystem::path artwork_dir(const std::filesystem::path& mountpoint) {
    return control_dir(mountpoint) / "Artwork";
}

std::filesystem::path photos_dir(const std::filesystem::path& mountpoint) {
    return control_dir(mountpoint) / "Photos";
}

std::filesystem::path photos_thumb_dir(const std::filesystem::path& mountpoint) {
    return photos_dir(mountpoint) / "Thumbs";
}

std::filesystem::path device_dir(const std::filesystem::path& mountpoint) {
    return control_dir(mountpoint) / "Device";
}

std::filesystem::path itunesdb_path(const std::filesystem::path& mountpoint) {
    return itunes_dir(mountpoint) / "iTunesDB";
}

std::filesystem::path itunescdb_path(const std::filesystem::path& mountpoint) {
    return itunes_dir(mountpoint) / "iTunesCDB";
}

std::filesystem::path itunessd_path(const std::filesystem::path& mountpoint) {
    return itunes_dir(mountpoint) / "iTunesSD";
}

std::filesystem::path artworkdb_path(const std::filesystem::path& mountpoint) {
    return artwork_dir(mountpoint) / "ArtworkDB";
}

std::filesystem::path photodb_path(const std::filesystem::path& mountpoint) {
    // iTunes names this file "Photo Database"; some tools use "PhotoDB".
    return photos_dir(mountpoint) / "Photo Database";
}

std::filesystem::path path_in(const std::filesystem::path& directory,
                              const std::filesystem::path& file) {
    return directory / file;
}

std::filesystem::path ipod_path_to_fs(std::string_view ipod_path) {
    std::string converted(ipod_path);
    std::replace(converted.begin(), converted.end(), ':', '/');
    return std::filesystem::path(converted);
}

std::string fs_path_to_ipod(const std::filesystem::path& path) {
    std::string converted = path.generic_string();
    std::replace(converted.begin(), converted.end(), '/', ':');
    return converted;
}

}  // namespace gpod
