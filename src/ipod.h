// Classic iPod detection and database sync.
//
// The database work is the pure C++ libgpod subset under src/ipod/gpod. This
// header keeps Win32 out of the worker: the UI captures plain track metadata,
// the worker copies files and writes the iTunesDB, and the result comes back as
// data. Only classic iPods (iTunesDB/iTunesCDB) are handled; iOS devices use a
// different, undocumented database and are reported as unsupported.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ipod {

struct Device {
    std::filesystem::path mountpoint;
    std::wstring name;
    std::string model;
    uint64_t capacity = 0, free = 0;
    bool initialized = false;
    bool classic = false;  // the database can be written by this path
};

// Everything the worker needs from one library track, copied on the UI thread.
struct SourceTrack {
    std::filesystem::path path;
    std::string title, artist, album, albumArtist, genre, composer;
    uint64_t size = 0;
    uint32_t durationMs = 0, year = 0;
    uint16_t trackNo = 0, discNo = 0, bitrate = 0, sampleRate = 0;
    uint32_t playCount = 0, skipCount = 0;
    uint8_t rating = 0;
    bool missing = false;
    bool protectedFile = false;
    bool audiobook = false;
};

struct SyncOptions {
    bool initializeIfNeeded = false;
};

struct SyncResult {
    size_t added = 0, skippedExisting = 0, skippedMissing = 0, skippedProtected = 0,
           skippedUnsupported = 0, failed = 0;
    uint64_t bytesCopied = 0;
    bool initialized = false, databaseWritten = false, cancelled = false;
    std::vector<std::string> errors;
};

// Return false to stop after the current track. `done` counts the track being
// started, `total` is the snapshot size.
using Progress = std::function<bool(size_t done, size_t total, const std::wstring& title)>;

// Enumerates mounted drives and the optional CORDINAL_IPOD_PATH override.
std::vector<Device> Detect();
std::optional<Device> Inspect(const std::filesystem::path& mountpoint);

// Copies the tracks into the master playlist. The caller must pass an iPod
// mount point and decide explicitly whether an empty device may be initialized.
SyncResult Sync(const std::vector<SourceTrack>& tracks, const std::filesystem::path& mountpoint,
                const SyncOptions& options, const Progress& progress);

bool IsSupportedAudio(const std::filesystem::path& path);
bool IsProtectedAudio(const std::filesystem::path& path);

}  // namespace ipod
