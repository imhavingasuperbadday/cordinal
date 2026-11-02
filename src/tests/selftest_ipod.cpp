// Classic iPod detection and iTunesDB sync, against a fake empty iPod directory.
#include "selftest_common.h"
#include "ipod.h"

#include <gpod/model.hpp>

namespace {

std::wstring IpodPath(const std::wstring& scratch) { return scratch + L"\\ipod"; }

ipod::SourceTrack Source(const std::wstring& file, const char* title, bool missing = false) {
    ipod::SourceTrack track;
    track.path = file;
    track.title = title;
    track.artist = "Artist";
    track.album = "Album";
    std::error_code error;
    track.size = std::filesystem::file_size(file, error);
    if (error) track.size = 0;
    track.durationMs = 1000;
    track.missing = missing;
    return track;
}

void MakeEmptyIpod(const std::wstring& root) {
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\iPod_Control").c_str(), nullptr);
    CreateDirectoryW((root + L"\\iPod_Control\\Music").c_str(), nullptr);
    CreateDirectoryW((root + L"\\iPod_Control\\Music\\F00").c_str(), nullptr);
    CreateDirectoryW((root + L"\\iPod_Control\\Device").c_str(), nullptr);
}

}  // namespace

void TestIpod(const std::wstring& scratch) {
    using namespace app;
    testlog::Group group(L"classic iPod");
    testlog::Batch batch;
    CreateDirectoryW(scratch.c_str(), nullptr);
    const std::wstring root = IpodPath(scratch);
    MakeEmptyIpod(root);
    const std::wstring file = scratch + L"\\track.mp3";
    Ready(WriteTestBytes(file, "ID3fake audio"), L"the fake audio file is written");

    Check(!ipod::Inspect(scratch).has_value(), L"a folder without iPod_Control is not an iPod");
    const auto empty = ipod::Inspect(root);
    Check(empty.has_value() && empty->classic && !empty->initialized,
          L"an empty iPod is detected, classic, and not initialized");

    ipod::SyncOptions options;
    ipod::SyncResult refused = ipod::Sync({Source(file, "Track")}, root, options, {});
    Check(refused.added == 0 && !refused.errors.empty() &&
              !std::filesystem::exists(root + L"\\iPod_Control\\iTunes\\iTunesDB"),
          L"syncing an empty iPod requires explicit initialization");

    options.initializeIfNeeded = true;
    ipod::SyncResult first = ipod::Sync({Source(file, "Track")}, root, options, {});
    batch.Next(L"the first sync initializes the database and copies the file");
    Check(first.initialized && first.added == 1 && first.databaseWritten,
          L"one track is copied and the iTunesDB is written");
    Check(std::filesystem::exists(root + L"\\iPod_Control\\iTunes\\iTunesDB"),
          L"the iTunesDB exists after the first sync");
    Check(std::filesystem::exists(root + L"\\iPod_Control\\Music\\F00"), L"the F00 directory is available");

    const auto initialized = ipod::Inspect(root);
    Check(initialized.has_value() && initialized->initialized, L"the initialized iPod is recognized");

    auto database = gpod::Itdb::parse(root);
    Check(database && database->tracks.size() == 1 && database->tracks[0]->title == "Track",
          L"the written database round-trips the copied track");

    ipod::SyncResult again = ipod::Sync({Source(file, "Track")}, root, options, {});
    batch.Next(L"a repeated sync leaves the existing track alone");
    Check(again.added == 0 && again.skippedExisting == 1, L"the same metadata and size is skipped as existing");
    Check(std::filesystem::exists(root + L"\\iPod_Control\\iTunes\\iTunesDB.cordinal-backup"),
          L"the existing database is backed up before a later write");

    const std::wstring protectedFile = scratch + L"\\protected.m4p";
    const std::wstring unsupported = scratch + L"\\unsupported.flac";
    Ready(WriteTestBytes(protectedFile, "fake"), L"the protected fixture is written");
    Ready(WriteTestBytes(unsupported, "fake"), L"the unsupported fixture is written");
    ipod::SyncResult skipped = ipod::Sync(
        {Source(protectedFile, "Protected"), Source(unsupported, "Unsupported"), Source(scratch + L"\\gone.mp3", "Gone", true)},
        root, options, {});
    Check(skipped.added == 0 && skipped.skippedProtected == 1 && skipped.skippedUnsupported == 1 &&
              skipped.skippedMissing == 1,
          L"protected, unsupported and missing files are reported separately");

    Check(ipod::IsSupportedAudio("song.MP3") && ipod::IsSupportedAudio("book.m4b") &&
              !ipod::IsSupportedAudio("song.flac"),
          L"classic iPod audio extensions are selected case-insensitively");
    Check(ipod::IsProtectedAudio("song.M4P") && ipod::IsProtectedAudio("book.aax"),
          L"protected purchase extensions are recognized");

    SetEnvironmentVariableW(L"CORDINAL_IPOD_PATH", root.c_str());
    const auto detected = ipod::Detect();
    SetEnvironmentVariableW(L"CORDINAL_IPOD_PATH", nullptr);
    Check(std::any_of(detected.begin(), detected.end(), [&root](const ipod::Device& device) {
              return device.mountpoint == std::filesystem::path(root);
          }),
          L"the explicit mount override is included in detection");

    std::filesystem::remove_all(root);
    DeleteFileW(file.c_str());
    DeleteFileW(protectedFile.c_str());
    DeleteFileW(unsupported.c_str());
}
