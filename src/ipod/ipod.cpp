// Classic iPod detection and iTunesDB sync. See ipod.h for the ownership split.
#include "ipod.h"

#include "library.h"  // ToUtf8/FromUtf8, shared with the rest of cordinal

#include <gpod/error.hpp>
#include <gpod/fileops.hpp>
#include <gpod/model.hpp>
#include <gpod/paths.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ipod {
namespace {

using namespace std::filesystem;

std::wstring Wide(const path& value) {
#ifdef _WIN32
    return value.wstring();
#else
    return FromUtf8(value.string());
#endif
}

std::string LowerExtension(const path& value) {
    std::string extension = value.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

bool IsClassicAudioExtension(std::string_view extension) {
    return extension == ".mp3" || extension == ".m4a" || extension == ".m4b" ||
           extension == ".aac" || extension == ".wav" || extension == ".aiff" ||
           extension == ".aif" || extension == ".alac";
}

std::string FileTypeName(const path& value) {
    const std::string extension = LowerExtension(value);
    if (extension == ".mp3") return "MP3";
    if (extension == ".wav") return "WAV";
    if (extension == ".aiff" || extension == ".aif") return "AIFF";
    if (extension == ".m4b") return "AAC audiobook";
    return "AAC";
}

std::string ModelName(const gpod::IpodInfo& info) {
    std::string name(gpod::to_string(info.generation));
    if (info.model != gpod::IpodModel::Invalid && info.model != gpod::IpodModel::Unknown) {
        name += " / ";
        name += gpod::to_string(info.model);
    }
    return name;
}

#ifdef _WIN32
std::wstring VolumeLabel(const path& mountpoint) {
    wchar_t label[MAX_PATH + 1]{};
    if (GetVolumeInformationW(mountpoint.c_str(), label, static_cast<DWORD>(std::size(label)),
                              nullptr, nullptr, nullptr, nullptr, 0) != 0 &&
        label[0] != L'\0') {
        return label;
    }
    return {};
}
#else
std::wstring VolumeLabel(const path&) { return {}; }
#endif

bool SameFile(const path& path, std::error_code& error) {
    return is_regular_file(path, error) && !error;
}

// libgpod has already copied the media file; this is the rollback list for a database write
// that fails afterwards, so an interrupted sync does not leave unindexed files behind.
void RemoveCopiedFiles(const std::vector<path>& files) {
    std::error_code error;
    for (const path& file : files) remove(file, error);
}

bool ExistingTrackMatches(const gpod::Track& existing, const SourceTrack& source, uint64_t size) {
    return existing.title == source.title && existing.artist == source.artist &&
           existing.album == source.album &&
           existing.size == static_cast<std::uint32_t>(
                                std::min<std::uint64_t>(
                                    size, std::numeric_limits<std::uint32_t>::max()));
}

void Error(SyncResult& result, std::string text) {
    result.errors.push_back(std::move(text));
}

}  // namespace

bool IsSupportedAudio(const path& file) {
    return IsClassicAudioExtension(LowerExtension(file));
}

bool IsProtectedAudio(const path& file) {
    const std::string extension = LowerExtension(file);
    return extension == ".m4p" || extension == ".aa" || extension == ".aax";
}

std::optional<Device> Inspect(const path& mountpoint) {
    std::error_code error;
    if (mountpoint.empty() || !is_directory(mountpoint / "iPod_Control", error)) {
        return std::nullopt;
    }
    Device result;
    result.mountpoint = mountpoint;
    result.initialized = is_regular_file(gpod::itunesdb_path(mountpoint), error) ||
                         is_regular_file(gpod::itunescdb_path(mountpoint), error);
    gpod::Device device;
    try {
        device.set_mountpoint(mountpoint);
        const gpod::IpodInfo info = device.ipod_info();
        result.model = ModelName(info);
        result.classic = !device.supports_sqlite_db();
        if (const auto storage = device.storage_info()) {
            result.capacity = storage->capacity;
            result.free = storage->free;
        }
    } catch (const std::exception&) {
        result.model = "Unknown iPod";
        result.classic = false;
    }
    if (result.capacity == 0) {
        const auto spaceInfo = space(mountpoint, error);
        if (!error) {
            result.capacity = spaceInfo.capacity;
            result.free = spaceInfo.available;
        }
    }
    result.name = VolumeLabel(mountpoint);
    if (result.name.empty()) result.name = Wide(mountpoint.filename());
    if (result.name.empty()) result.name = L"iPod";
    return result;
}

std::vector<Device> Detect() {
    std::vector<Device> devices;
    const auto consider = [&devices](const path& root) {
        if (const auto device = Inspect(root)) {
            const bool duplicate = std::any_of(
                devices.begin(), devices.end(), [&device](const Device& existing) {
                    return existing.mountpoint == device->mountpoint;
                });
            if (!duplicate) devices.push_back(*device);
        }
    };
#ifdef _WIN32
    const DWORD mask = GetLogicalDrives();
    for (int index = 0; index < 26; ++index) {
        if ((mask & (1UL << static_cast<unsigned>(index))) == 0) continue;
        wchar_t root[] = {static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
        const UINT type = GetDriveTypeW(root);
        if (type == DRIVE_REMOVABLE || type == DRIVE_FIXED || type == DRIVE_REMOTE) consider(root);
    }
    wchar_t manual[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"CORDINAL_IPOD_PATH", manual,
                                                 static_cast<DWORD>(std::size(manual)));
    if (length > 0 && length < std::size(manual)) consider(path(manual));
#else
    // Linux mounts removable media below one of these roots, usually with a user-name
    // directory in between (/media/<user>/<label>). Walk those two levels.
    for (const char* base : {"/media", "/mnt", "/run/media", "/Volumes"}) {
        std::error_code error;
        for (const auto& entry : directory_iterator(base, error)) {
            if (!entry.is_directory(error)) continue;
            consider(entry.path());
            std::error_code childError;
            for (const auto& child : directory_iterator(entry.path(), childError)) {
                if (child.is_directory(childError)) consider(child.path());
            }
        }
    }
#endif
    std::sort(devices.begin(), devices.end(), [](const Device& left, const Device& right) {
        return left.mountpoint < right.mountpoint;
    });
    return devices;
}

SyncResult Sync(const std::vector<SourceTrack>& tracks, const path& mountpoint,
                const SyncOptions& options, const Progress& progress) {
    SyncResult result;
    const auto device = Inspect(mountpoint);
    if (!device) {
        Error(result, "The selected folder is not an iPod (iPod_Control is missing)");
        return result;
    }
    if (!device->classic) {
        Error(result, "This is an iOS or SQLite iPod; its music database cannot be written here");
        return result;
    }
    std::error_code error;
    if (!device->initialized && !options.initializeIfNeeded) {
        Error(result, "This iPod has no iTunes database yet; enable initialization first");
        return result;
    }

    result.initialized = true;
    if (!device->initialized) {
        create_directories(gpod::itunes_dir(mountpoint), error);
        create_directories(gpod::music_dir(mountpoint), error);
        create_directories(gpod::device_dir(mountpoint), error);
        for (int index = 0; index < 20; ++index) {
            std::ostringstream name;
            name << 'F' << std::setw(2) << std::setfill('0') << index;
            create_directories(gpod::music_dir(mountpoint) / name.str(), error);
        }
        if (error) {
            Error(result, "Could not create the iPod folder layout: " + error.message());
            return result;
        }
    }

    // Back up the database once, before changing anything. libgpod writes atomically, but a
    // corrupt or unsupported database is easier to recover with the previous file beside it.
    // A device may have only the compressed iTunesCDB; missing files are not an error.
    if (device->initialized) {
        std::error_code existsError;
        const path database = gpod::itunesdb_path(mountpoint);
        if (is_regular_file(database, existsError)) {
            copy_file(database, gpod::itunes_dir(mountpoint) / "iTunesDB.cordinal-backup",
                      copy_options::overwrite_existing, error);
        } else if (existsError != std::errc::no_such_file_or_directory) {
            error = existsError;
        }
        if (!error) {
            const path compressed = gpod::itunescdb_path(mountpoint);
            if (is_regular_file(compressed, existsError)) {
                copy_file(compressed, gpod::itunes_dir(mountpoint) / "iTunesCDB.cordinal-backup",
                          copy_options::overwrite_existing, error);
            } else if (existsError != std::errc::no_such_file_or_directory) {
                error = existsError;
            }
        }
        if (error) {
            Error(result, "Could not back up the iPod database: " + error.message());
            return result;
        }
    }

    std::unique_ptr<gpod::Itdb> database;
    try {
        database = device->initialized ? gpod::Itdb::parse(mountpoint) : gpod::Itdb::create();
        if (!database) {
            Error(result, "Could not read the iPod database");
            return result;
        }
        if (!device->initialized) {
            database->set_mountpoint(mountpoint);
            auto master = std::make_unique<gpod::Playlist>();
            master->name = "iPod";
            master->set_master();
            database->add_playlist(std::move(master));
        }
    } catch (const std::exception& exception) {
        Error(result, std::string("Could not open the iPod database: ") + exception.what());
        return result;
    }

    gpod::Playlist* master = database->master_playlist();
    if (master == nullptr) {
        auto playlist = std::make_unique<gpod::Playlist>();
        playlist->name = "iPod";
        playlist->set_master();
        master = database->add_playlist(std::move(playlist));
    }

    uint64_t freeBytes = device->free;
    bool changed = !device->initialized;
    std::vector<path> copied;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const SourceTrack& source = tracks[index];
        if (progress && !progress(index, tracks.size(), Wide(source.path.filename()))) {
            result.cancelled = true;
            break;
        }
        if (source.missing || !SameFile(source.path, error)) {
            ++result.skippedMissing;
            Error(result, "Missing: " + ToUtf8(Wide(source.path)));
            continue;
        }
        if (source.protectedFile || IsProtectedAudio(source.path)) {
            ++result.skippedProtected;
            Error(result, "Protected purchase was not copied: " + source.title);
            continue;
        }
        if (!IsSupportedAudio(source.path)) {
            ++result.skippedUnsupported;
            Error(result, "Not supported by classic iPods: " + ToUtf8(Wide(source.path)));
            continue;
        }
        std::error_code sizeError;
        const uint64_t byteCount = file_size(source.path, sizeError);
        const uint64_t size = sizeError ? source.size : byteCount;
        if (std::any_of(database->tracks.begin(), database->tracks.end(),
                        [&source, size](const std::unique_ptr<gpod::Track>& existing) {
                            return ExistingTrackMatches(*existing, source, size);
                        })) {
            ++result.skippedExisting;
            continue;
        }
        if (size > std::numeric_limits<std::uint32_t>::max()) {
            ++result.skippedUnsupported;
            Error(result, "File is too large for an iTunesDB entry: " + source.title);
            continue;
        }
        // Keep a little room for the database and filesystem metadata.
        if (freeBytes != 0 && size + (1u << 20) > freeBytes) {
            ++result.failed;
            Error(result, "Not enough free space for " + source.title);
            break;
        }

        auto destination = std::make_unique<gpod::Track>();
        destination->title = source.title;
        destination->artist = source.artist;
        destination->album = source.album;
        destination->albumartist = source.albumArtist;
        destination->genre = source.genre;
        destination->composer = source.composer;
        destination->tracklen = static_cast<std::int32_t>(
            std::min<std::uint64_t>(source.durationMs,
                                    static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())));
        destination->track_nr = static_cast<std::int32_t>(source.trackNo);
        destination->cd_nr = static_cast<std::int32_t>(source.discNo);
        destination->year = static_cast<std::int32_t>(source.year);
        destination->bitrate = static_cast<std::int32_t>(
            std::min<std::uint32_t>(source.bitrate, static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())));
        destination->samplerate = source.sampleRate;
        destination->rating = std::min<uint32_t>(source.rating, 100);
        destination->playcount = source.playCount;
        destination->skipcount = source.skipCount;
        destination->filetype = FileTypeName(source.path);
        destination->mediatype = source.audiobook ? gpod::Mediatype::Audiobook : gpod::Mediatype::Audio;
        destination->time_added = std::time(nullptr);
        destination->time_modified = destination->time_added;
        gpod::Track* added = database->add_track(std::move(destination));
        try {
            gpod::copy_track_to_ipod(*added, source.path);
        } catch (const std::exception& exception) {
            database->remove_track(added);
            ++result.failed;
            Error(result, "Could not copy " + source.title + ": " + exception.what());
            continue;
        }
        master->add_track(added);
        copied.push_back(added->filename_on_ipod());
        ++result.added;
        result.bytesCopied += size;
        freeBytes = size + (1u << 20) >= freeBytes ? 0 : freeBytes - size - (1u << 20);
        changed = true;
    }

    if (result.cancelled && result.added == 0) changed = false;
    if (changed) {
        try {
            database->write();
            result.databaseWritten = true;
        } catch (const std::exception& exception) {
            Error(result, std::string("Could not write the iPod database: ") + exception.what());
            RemoveCopiedFiles(copied);
            return result;
        }
        // The database now references the copied files, so a rename failure must not delete them.
        try {
            gpod::rename_files(mountpoint);
        } catch (const std::exception& exception) {
            Error(result, std::string("The database was written, but file names were not normalized: ") +
                               exception.what());
        }
    }
    return result;
}

}  // namespace ipod
