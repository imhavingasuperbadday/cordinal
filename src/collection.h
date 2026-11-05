// File > Export collection and Import collection: one ZIP that holds the library's music in
// its folders, those folders' cover images, cue sheets and lyrics, and the static playlists as
// relative .m3u8 files. Any unzip tool opens it; Import collection accepts only archives this export
// made (kCollectionManifest, archive.h) and unpacks one into a folder without replacing anything
// already there. Originals are only ever read, and each written file appears whole or not at all:
// the archive as "<name>.part" renamed when complete, each unpacked file through a temp name beside
// it. No UI: the library is read on the UI thread only (CaptureExport); the rest runs on a worker.
#pragma once
#include "library.h"

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace collection {

// The format the manifest's first line names; a newer one is refused with a clear message.
constexpr int kFormat = 1;

// What the export worker needs, copied from the library on the UI thread.
struct ExportSource {
    std::vector<std::wstring> roots;   // the library folders
    std::vector<std::wstring> tracks;  // every song to export: the library's, then the playlists' others
    struct List {
        std::wstring name;
        std::vector<std::wstring> paths;  // in playlist order, duplicates kept
        std::vector<std::string> infos;   // each one's #EXTINF line
    };
    std::vector<List> playlists;  // the static playlists
    uint64_t created = 0;         // Unix seconds, for the manifest and the README
};
// What "Select what to export" offers: the songs the Library tab shows as artist > album > song (a
// song's album artist, else its artist), and the static playlists.
struct ExportChoices {
    struct Song {
        std::wstring title, path;  // title: the tag, else the file name
    };
    struct Album {
        std::wstring name;  // empty: songs with no album
        std::vector<Song> songs;  // by disc and track number, then title
    };
    struct Artist {
        std::wstring name;  // as first met; empty for the songs with no artist
        std::vector<Album> albums;  // by name ignoring case, the unnamed one last
        size_t songs = 0;
    };
    struct List {
        std::wstring name;
        size_t songs = 0;
    };
    std::vector<Artist> artists;  // by name ignoring case, the unnamed one last
    std::vector<List> playlists;  // in tab order
};
// UI thread.
ExportChoices ListChoices(const Library& library, const std::vector<Playlist>& playlists, bool showOriginals);

// The ticked songs (by path) and playlists (by name), so a library or playlist list that changes
// while the choice is open still exports what was ticked.
struct ExportSelection {
    std::vector<std::wstring> songs, playlists;
};

// UI thread. The songs the Library tab shows (with kept originals only when they are shown), and
// every song a static playlist holds, even one outside the library. With a `selection`, only the
// ticked library songs and the ticked playlists (with all their songs).
ExportSource CaptureExport(const Library& library, const std::vector<Playlist>& playlists,
                           const std::vector<std::wstring>& roots, bool showOriginals,
                           const ExportSelection* selection = nullptr);

// Progress of a run, for the status bar: bytes and files done of the totals.
struct Progress {
    uint64_t bytes = 0, totalBytes = 0;
    size_t files = 0, totalFiles = 0;
};
using ProgressFn = std::function<void(const Progress&)>;

struct ExportResult {
    bool ok = false, cancelled = false;
    size_t songs = 0, files = 0, playlists = 0;
    uint64_t bytes = 0;
    std::vector<std::pair<std::wstring, std::wstring>> skipped;  // file -> why it isn't in the archive
    std::wstring error;  // why there is no archive at all
};
// Any thread. Writes the archive to `zip`, replacing a file the user chose to replace only once the
// new archive is complete; a failed or cancelled run leaves no archive.
ExportResult Export(const ExportSource& source, const std::wstring& zip, const std::atomic<bool>& cancel,
                    const ProgressFn& progress);

// A validated collection archive, ready to unpack.
struct ImportPlan {
    std::wstring zip;
    struct File {
        std::wstring rel;  // where it goes under the destination: this platform's separators, safe names
        std::string name;  // as stored, which playlist entries name
        uint64_t size = 0, local = 0;
        uint32_t crc = 0, time = 0;  // time: Unix seconds, 0 = unknown
        bool playlist = false;
    };
    std::vector<File> files;
    size_t songs = 0, playlists = 0;
    uint64_t bytes = 0;
    std::vector<std::wstring> folders;  // the archive's top folders that hold music, as `rel` names them
};
// Any thread. False with `error` in words when `zip` isn't a collection export cordinal can unpack.
bool ReadCollection(const std::wstring& zip, ImportPlan& plan, std::wstring& error);

struct ImportResult {
    bool cancelled = false;
    size_t written = 0, same = 0, renamed = 0;  // same: identical files already there; renamed: kept beside one
    uint64_t bytes = 0;
    std::vector<std::wstring> folders;  // the unpacked top folders with music: library folders to add
    struct List {
        std::wstring name;
        std::vector<std::wstring> paths;
    };
    std::vector<List> playlists;
    std::vector<std::pair<std::wstring, std::wstring>> failed;  // file -> what went wrong
    std::wstring error;  // why nothing more could be unpacked (a full drive, too little space)
};
// Any thread. Unpacks into `dest`, never replacing a file: an identical one is left as it is, a
// different one keeps its name and the archive's copy is written beside it as "name (2)". Each
// file's CRC-32 is checked before it gets its real name. The playlists are written into
// dest/Playlists with paths relative to that folder, and returned resolved for the library.
ImportResult Import(const ImportPlan& plan, const std::wstring& dest, const std::atomic<bool>& cancel,
                    const ProgressFn& progress);

// "cordinal collection 2026-11-04.zip": the name the save dialog offers.
std::wstring DefaultName(uint64_t unixTime);
// "1.4 GB", "120 MB": sizes in messages.
std::wstring SizeText(uint64_t bytes);

// A ZIP written front to back: stored entries, UTF-8 names, an extended timestamp, and ZIP64 when an
// entry, an offset or the count needs it (or always, with `zip64`, so tests reach those fields).
class ZipWriter {
public:
    explicit ZipWriter(const std::wstring& path, bool zip64 = false);
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;
    bool Valid() const;
    bool Add(std::string_view name, std::string_view bytes, uint32_t unixTime);
    // Copies a file in. `chunk` hears each piece copied and returns false to stop. On failure the
    // archive is cut back to where it was, so a file that can't be read whole leaves no trace.
    bool AddFile(std::string_view name, const std::wstring& file, const std::function<bool(uint64_t)>& chunk,
                 std::wstring& error);
    bool Finish();  // the central directory and end records; then the file is flushed and closed
    uint64_t Size() const { return offset_; }
    size_t Count() const { return entries_.size(); }
    const std::wstring& Error() const { return error_; }  // why the archive itself couldn't be written

private:
    struct Entry {
        std::string name;
        uint64_t size = 0, offset = 0;
        uint32_t crc = 0, unixTime = 0, dos = 0;
    };
    bool Write(std::string_view bytes);
    std::string LocalHeader(const Entry& e) const;
    HANDLE handle_;
    bool zip64_, ok_;
    uint64_t offset_ = 0;
    std::vector<Entry> entries_;
    std::wstring error_;
};

}  // namespace collection
