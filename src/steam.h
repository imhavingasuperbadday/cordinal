// Finds the music folders of Steam games (steamapps\music in every Steam library), so the
// library can offer the soundtracks people already have on disk. Paths use the host platform's
// separators and comparisons.
#pragma once
#include <atomic>
#include <string>
#include <vector>

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
