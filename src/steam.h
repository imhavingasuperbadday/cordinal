// Finds the music folders of Steam games (steamapps\music in every Steam library), so the
// library can offer the soundtracks people already have on disk. Paths use the host platform's
// separators and comparisons.
#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <utility>
#include "tags.h"

class Library;
// Copied on the UI thread; discovery never reads the live library.
struct SteamSong {
    std::wstring path;
    std::string key;
    uint64_t audioHash = 0;
    uint32_t durationMs = 0;
    Codec codec = Codec::Unknown;
    std::string pathKey;
};
using SteamDuplicates = std::vector<std::pair<std::wstring, std::wstring>>;  // skipped path, kept path
struct SteamImport {
    std::vector<std::wstring> folders;
    SteamDuplicates oneCopy, eachFormat;
    bool multipleFormats = false;
};
std::vector<SteamSong> SnapshotSteamSongs(const Library& library);
bool PrepareSteamImport(SteamImport& result, std::vector<SteamSong> known,
                        const std::atomic<bool>* cancel = nullptr);
// Leaves files, playlists and listening history intact; excluded copies stay out on rescans.
size_t SkipSteamDuplicates(Library& library, const SteamImport& imported, bool eachFormat);

// Every Steam library's soundtrack folders: <library>\steamapps\music when it holds files, plus
// soundtrack-looking folders found inside game installs (<library>\steamapps\common\<Game>\...).
// Covering folders are collapsed, so no returned folder sits inside another. Empty when Steam
// is not installed. If cancelled, callers must discard the partial result.
std::vector<std::wstring> FindSteamMusicFolders(const std::atomic<bool>* cancel = nullptr);

// The soundtrack folders under one Steam library root (an arbitrary '<library>' path holding a
// steamapps folder). Same rules as above; testable on a synthetic library.
std::vector<std::wstring> FindSteamMusicFoldersIn(const std::wstring& libraryPath,
                                                   const std::atomic<bool>* cancel = nullptr);

// The library paths in a libraryfolders.vdf document: the "path" values of the current
// format, and the plain path values of the old one. Escapes such as \\ are decoded.
std::vector<std::wstring> ParseSteamLibraryFolders(const std::string& vdf);
