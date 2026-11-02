// Covers chosen in cordinal for single tracks or whole albums, shown in place of a track's own cover
// (embedded or beside it) without touching the music files. Each image is kept once in the data
// folder's covers folder, named by its content, and covers.txt maps track paths to it; both are
// replaced through a temporary file, so a failed write leaves the earlier choice in place.
// Restoring a track's original cover removes its line and any image no track uses any more.
// Shared by the Windows and Linux front ends. Lookups take a lock and may run on any thread (the
// art workers ask for every cover they decode); changes come from the UI thread.
#pragma once
#include <string>
#include <vector>

namespace customcover {

// Loads `dataDir`/covers/covers.txt; call once before the art workers start.
void Start(const std::wstring& dataDir);
// The image file chosen for the track at `trackPath` (as the library names it), or "" for none.
std::wstring ImageFor(const std::wstring& trackPath);
// Gives every track in `trackPaths` the image, which must be a complete JPEG or PNG
// (online::InspectImage). False with `error` when it isn't or couldn't be saved; nothing changes then.
bool Set(const std::vector<std::wstring>& trackPaths, const std::string& image, std::wstring& error);
// Returns the tracks to their own covers; the number that had a custom one.
size_t Clear(const std::vector<std::wstring>& trackPaths);
// How many of `trackPaths` have a custom cover.
size_t Count(const std::vector<std::wstring>& trackPaths);

}  // namespace customcover
