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
// The https address the image was downloaded from, when cordinal fetched it from a provider: Discord can show
// that address itself, so the status shows the same picture as the player. "" for an image the user supplied
// from a file, for a choice made by a build that did not record addresses, or when the track has none.
std::wstring SourceUrlFor(const std::wstring& trackPath);
// Records the address an image was uploaded to, for the tracks that already use it: the Discord status shows it.
// `imagePath` is what ImageFor returned. Ignored unless the address is https and the file is one of ours.
void RememberImageUrl(const std::wstring& imagePath, const std::string& url);
// Gives every track in `trackPaths` the image, which must be a complete JPEG or PNG
// (online::InspectImage). `sourceUrl` is where the image was downloaded from, when it was; it is recorded so the
// Discord status can point at it, and is ignored unless it is an https address. False with `error` when the
// image isn't usable or couldn't be saved; nothing changes then.
bool Set(const std::vector<std::wstring>& trackPaths, const std::string& image, std::wstring& error,
         const std::string& sourceUrl = {});
// Returns the tracks to their own covers; the number that had a custom one.
size_t Clear(const std::vector<std::wstring>& trackPaths);
// How many of `trackPaths` have a custom cover.
size_t Count(const std::vector<std::wstring>& trackPaths);

}  // namespace customcover
