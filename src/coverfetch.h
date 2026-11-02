// The library side of the cover fetcher, shared by the Windows and Linux front ends: which albums a
// selection or the library holds, which of them have no cover, the batch search that proposes one
// for each, and saving the chosen covers. The front ends own the windows and the worker threads;
// the album lists are built on the UI thread and everything slow here runs on those workers.
#pragma once
#include "library.h"
#include "metadata_fetch.h"
#include "tagwriter.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace coverfetch {

// One album as the search sees it: its tracks, its folder and what to ask for.
struct Album {
    std::vector<TrackId> tracks;
    std::wstring dir;         // the folder of its first track (the real file for CUE subtracks)
    std::wstring firstPath;   // the real file of its first track, for reading embedded art
    std::vector<std::wstring> paths;  // its tracks' paths as the library names them, for custom covers
    std::string artist, album;
    std::wstring Label() const;  // "Artist - Album"
};

// UI thread. The albums of `ids`, by folder and album name, in their order. An album without an
// album tag takes its folder's name, so a manual search still has something to ask for.
std::vector<Album> AlbumsOf(const std::vector<TrackId>& ids);
// UI thread. Every album of the library with an album tag, for Fetch missing covers.
std::vector<Album> LibraryAlbums();
// Worker. True when the album already shows a cover: one chosen in cordinal, a folder file or a
// picture in its first track.
bool AlbumHasCover(const Album& album);

struct Proposal {
    Album album;
    online::CoverFound cover;
    bool confident = false;  // starts checked in the review
};
struct BatchResult {
    std::vector<Proposal> proposals;  // confident ones first, then by score
    size_t albums = 0, hadCover = 0, notFound = 0;
    bool cancelled = false;
    bool limited = false;  // stopped at kMaxProposals; the rest wait for the next run
};
// Each proposal holds its downloaded image, so one review is kept to a size memory can afford.
constexpr size_t kMaxProposals = 200;
// Worker. Searches every album that has no cover yet and proposes the best result for each.
// `progress` is called with (albums done, albums in all).
void RunBatch(const std::vector<Album>& albums, online::Cancel& cancel, const online::ImageDecoder& decode,
              int thumbSize, const std::function<void(size_t, size_t)>& progress, BatchResult& out);

// What the user confirmed: an album and the image to give it.
struct CoverChoice {
    Album album;
    std::string image;
};
// Only the checked proposals become choices; nothing else is ever written.
std::vector<CoverChoice> CheckedChoices(const std::vector<Proposal>& proposals, const std::vector<bool>& checked);

struct FolderResult {
    bool custom = false;  // SaveCustom's: shown in cordinal, not written in the folder
    size_t written = 0, kept = 0, failed = 0;
    std::vector<std::wstring> errors;
    std::vector<TrackId> changed;  // the tracks whose folder gained a cover
};
// Worker. cover.jpg in each album's folder, atomically; an existing cover stays unless `overwrite`.
FolderResult SaveToFolders(const std::vector<CoverChoice>& choices, bool overwrite);
// Worker. Each cover becomes its album's tracks' custom cover (customcover.h); no music file or
// folder changes, and any cover the tracks had is only hidden.
FolderResult SaveCustom(const std::vector<CoverChoice>& choices);
// UI thread. The tag edits that embed each cover in its album's files, for one undoable tag write.
// Tracks whose format can't hold a picture are counted in `unsupported` and left alone.
std::vector<std::pair<TrackId, TagEdit>> EmbedEdits(const std::vector<CoverChoice>& choices, size_t& unsupported);

}  // namespace coverfetch
