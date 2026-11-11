#include "gpod/fileops.hpp"

#include "gpod/error.hpp"
#include "gpod/paths.hpp"
#include "gpod/sysinfo.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <string_view>
#include <system_error>

namespace gpod {
namespace {

std::filesystem::path resolve_existing(const std::filesystem::path& directory,
                                       std::string_view name) {
    std::error_code ec;
    const std::filesystem::path exact = directory / name;
    if (std::filesystem::exists(exact, ec)) {
        return exact;
    }

    std::string lowered(name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::filesystem::path lower = directory / lowered;
    if (std::filesystem::exists(lower, ec)) {
        return lower;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        std::string candidate = entry.path().filename().string();
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (candidate == lowered) {
            return entry.path();
        }
    }
    return {};
}

std::string lowercase_extension(const std::filesystem::path& source) {
    std::string suffix = source.extension().string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return suffix;
}

std::uint32_t filetype_marker(const std::filesystem::path& destination) {
    std::string suffix = destination.extension().string();
    std::uint32_t marker = 0;
    for (std::size_t i = 1; i <= 4; ++i) {
        marker <<= 8;
        unsigned char c = ' ';
        if (suffix.size() > i) {
            c = static_cast<unsigned char>(suffix[i]);
        }
        marker |= static_cast<std::uint32_t>(std::toupper(c));
    }
    return marker;
}

std::filesystem::path choose_destination(const std::filesystem::path& mountpoint,
                                         int directory_count,
                                         const std::filesystem::path& source) {
    if (directory_count <= 0) {
        fail(ErrorCode::NotFound, "no F.. directories found in " +
                                      music_dir(mountpoint).string());
    }

    // Not a thread_local engine: its 5,000 bytes would be allocated for every thread in the process.
    std::random_device generator;
    std::uniform_int_distribution<int> directory_distribution(0, directory_count - 1);
    const int directory = directory_distribution(generator);
    std::ostringstream directory_name;
    directory_name << 'F' << std::setw(2) << std::setfill('0') << directory;
    const std::filesystem::path parent = music_dir(mountpoint) / directory_name.str();

    const std::string suffix = lowercase_extension(source);
    std::uniform_int_distribution<int> number_distribution(0, 899999);
    int number = number_distribution(generator);
    for (int attempt = 0; attempt < 1'000'000; ++attempt) {
        std::ostringstream name;
        name << "libgpod" << std::setw(6) << std::setfill('0') << (number + attempt) % 1'000'000
             << suffix;
        const std::filesystem::path candidate = parent / name.str();
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    fail(ErrorCode::Io, "could not find a free filename in " + parent.string());
}

}  // namespace

void copy_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::ifstream input(from, std::ios::binary);
    if (!input) {
        fail(ErrorCode::NotFound, "could not open '" + from.string() + "' for reading");
    }
    std::ofstream output(to, std::ios::binary | std::ios::trunc);
    if (!output) {
        fail(ErrorCode::Io, "could not open '" + to.string() + "' for writing");
    }

    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            output.write(buffer.data(), count);
            if (!output) {
                output.close();
                std::error_code ec;
                std::filesystem::remove(to, ec);
                fail(ErrorCode::Io, "error writing '" + to.string() + "'");
            }
        }
    }
    if (!input.eof()) {
        output.close();
        std::error_code ec;
        std::filesystem::remove(to, ec);
        fail(ErrorCode::Io, "error reading '" + from.string() + "'");
    }
    output.close();
    if (!output) {
        std::error_code ec;
        std::filesystem::remove(to, ec);
        fail(ErrorCode::Io, "error closing '" + to.string() + "'");
    }
}

int musicdir_count(const std::filesystem::path& mountpoint) {
    return static_cast<int>(count_musicdirs(mountpoint));
}

int musicdir_count(Device& device) {
    if (device.musicdirs <= 0) {
        device.musicdirs = musicdir_count(device.mountpoint);
    }
    return device.musicdirs;
}

std::filesystem::path destination_filename(Track& track, const std::filesystem::path& source) {
    if (!track.ipod_path.empty()) {
        return track.filename_on_ipod();
    }
    if (track.itdb == nullptr || track.itdb->device == nullptr ||
        track.itdb->device->mountpoint.empty()) {
        fail(ErrorCode::NotFound, "mountpoint not set");
    }
    return choose_destination(track.itdb->device->mountpoint, musicdir_count(*track.itdb->device),
                              source);
}

std::filesystem::path destination_filename(const std::filesystem::path& mountpoint,
                                           const std::filesystem::path& source) {
    return choose_destination(mountpoint, musicdir_count(mountpoint), source);
}

Track& finalize_track(Track& track, const std::filesystem::path& destination,
                      const std::filesystem::path& mountpoint) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(destination, ec);
    if (ec) {
        fail(ErrorCode::NotFound,
             "'" + destination.string() + "' could not be accessed");
    }

    const std::filesystem::path relative = destination.lexically_relative(mountpoint);
    const std::string relative_text = relative.generic_string();
    if (relative.empty() || relative_text == ".." || relative_text.starts_with("../") ||
        relative.is_absolute()) {
        fail(ErrorCode::Corrupt, "destination file '" + destination.string() +
                                     "' is not inside the iPod mountpoint");
    }

    track.transferred = true;
    track.size = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        size, std::numeric_limits<std::uint32_t>::max()));
    track.filetype_marker = filetype_marker(destination);
    track.ipod_path = "/" + fs_path_to_ipod(relative);
    return track;
}

std::unique_ptr<Track> track_from_file(const std::filesystem::path& destination,
                                       const std::filesystem::path& mountpoint) {
    auto track = std::make_unique<Track>();
    finalize_track(*track, destination, mountpoint);
    return track;
}

bool copy_track_to_ipod(Track& track, const std::filesystem::path& source) {
    if (track.transferred) {
        return true;
    }
    if (track.itdb == nullptr || track.itdb->device == nullptr) {
        fail(ErrorCode::NotFound, "track is not attached to an iPod database");
    }

    const std::filesystem::path destination = destination_filename(track, source);
    gpod::copy_file(source, destination);
    finalize_track(track, destination, track.itdb->device->mountpoint);
    return true;
}

void rename_files(const std::filesystem::path& mountpoint) {
    const std::filesystem::path directory = itunes_dir(mountpoint);
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        fail(ErrorCode::NotFound, "no iTunes directory at '" + directory.string() + "'");
    }

    const std::filesystem::path play_counts = resolve_existing(directory, "Play Counts");
    if (!play_counts.empty()) {
        std::filesystem::rename(play_counts, directory / "Play Counts.bak", ec);
        if (ec) {
            fail(ErrorCode::Io, "could not rename '" + play_counts.string() + "'");
        }
    }

    for (const std::string_view name : {"OTGPlaylistInfo", "iTunesShuffle", "iTunesStats"}) {
        const std::filesystem::path transient = resolve_existing(directory, name);
        if (!transient.empty()) {
            std::filesystem::remove(transient, ec);
            if (ec) {
                fail(ErrorCode::Io, "could not remove '" + transient.string() + "'");
            }
        }
    }
}

}  // namespace gpod
