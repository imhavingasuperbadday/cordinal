// Copy to folder or device: copies tracks into a folder (an SD card, a USB stick, a music player, a
// folder a phone syncs with) or onto a phone through Windows' copy engine, named by an Organize
// pattern, optionally converting lossless files to AAC (.m4a). Originals are only ever read. A
// copy is written as ".part" and renamed when complete, and a manifest in the destination records
// what cordinal put there, so the next run copies only what is new or changed and can remove only
// files it copied itself. No UI; the library is read on the UI thread only (PlanCopy).
#pragma once
#include "library.h"
#include "tagwriter.h"
#include "audio_encode.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

// The order matches the Format list of the Copy window and the "Format" value in settings.ini.
enum class CopyFormat : uint8_t { Original, Aac192, Aac128, Count };
unsigned CopyKbps(CopyFormat f);  // 0 for Original

// The manifest's name in the destination folder.
constexpr wchar_t kCopyManifest[] = L".cordinal-copy";
// Marks a file copied from a selection rather than a playlist: no playlist run ever removes it.
constexpr wchar_t kCopySelectionSet[] = L"*";

// One file cordinal put in the destination: where it is, what it was made from, and the playlists
// that wanted it.
struct CopyRecord {
    std::wstring rel, from;  // rel: under the destination, '\\'-separated
    uint64_t size = 0, time = 0;  // the source's when it was copied
    unsigned format = 0;          // 0 = copied as it was, else the AAC bitrate
    std::vector<std::wstring> sets;
};

// A missing manifest reads as empty; one that can't be read returns false. `device` is a shell
// parsing name (a phone's folder) rather than a file system path.
bool ReadCopyManifest(const std::wstring& dest, bool device, std::vector<CopyRecord>& out);
std::string CopyManifestText(const std::vector<CopyRecord>& records);
bool ParseCopyManifest(const std::string& text, std::vector<CopyRecord>& out);

struct CopyItem {
    TrackId id = 0;
    std::wstring from, rel;  // the library path, and its place under the destination with extension
    uint64_t size = 0, time = 0;
    bool convert = false;   // lossless source to AAC
    bool done = false;      // already there from an earlier copy, unchanged
    std::wstring skip;      // why it can't be copied (empty = it can)
    uint64_t estimate = 0;  // bytes the copy will take
    TagEdit tags;           // written into the copy: ReplayGain, and every field for a conversion
};

// UI thread (reads the library). Names every track by `pattern`, decides what converts, and
// compares with the manifest: an unchanged earlier copy is `done`. A name taken by another track,
// or by a file in the folder that cordinal didn't put there, gets " (2)", " (3)"...
std::vector<CopyItem> PlanCopy(const std::vector<TrackId>& ids, const std::wstring& dest, bool device,
                               const std::wstring& pattern, CopyFormat format, const std::vector<CopyRecord>& manifest);

// The manifest's files that `set` wanted before and this plan no longer does (and no other
// playlist or selection wants): what "remove songs no longer in the playlist" would remove.
std::vector<std::wstring> StaleCopies(const std::vector<CopyItem>& plan, const std::vector<CopyRecord>& manifest,
                                      const std::wstring& set);

struct CopySettings {
    std::wstring dest;
    bool device = false;
    std::wstring staging;   // a private temp folder on this PC, removed when the run ends
    std::wstring set = kCopySelectionSet;
    std::wstring playlistName;  // writes "<name>.m3u8" next to the music when not empty
    std::string playlist;       // its text: relative paths (M3UText)
    bool removeStale = false;
    unsigned kbps = 0;
};

struct CopyResult {
    size_t copied = 0, converted = 0, removed = 0;
    uint64_t bytes = 0;
    std::vector<std::pair<std::wstring, std::wstring>> failed;  // file -> what went wrong
    bool cancelled = false;
};

// Runs on any thread. Copies, converts and tags each planned file, then the folders' cover images
// and the playlist, keeps the manifest current as it goes, and removes stale copies last when
// asked. Stops between files (and inside a long copy) when `cancel` is set; a file in progress
// leaves nothing under its real name.
CopyResult ExecuteCopy(const std::vector<CopyItem>& plan, const CopySettings& settings, std::vector<CopyRecord> manifest,
                       const std::atomic<bool>& cancel, const std::function<void(size_t)>& progress);
