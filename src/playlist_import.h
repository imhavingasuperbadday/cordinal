// Importing a library someone already has: Spotify's account data export (YourLibrary.json's
// saved tracks and albums, Playlist1.json's playlists of any size and privacy) and the CSV
// playlists every moving service writes (Exportify, TuneMyMusic, Soundiiz). Everything here is
// pure: parsing, matching rows against the library the UI thread already owns, and turning a
// finished web-save run back into an ordered list of files. The UI layer reads the file, calls
// SplitByLibrary, feeds the missing tracks through RunWebFetch, then rebuilds each list's exact
// order with AssembleFiles - so a playlist imported today is the same playlist on both platforms,
// keeps its order, and skips every song the library already has.
#pragma once
#include "webfetch.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace import {

// One row of an imported list, before the library is consulted.
struct ImportTrack {
    std::string artist, title, album;  // UTF-8, "" when the row does not say
    std::string uri;                  // "spotify:track:<id>" or an open.spotify.com track link
    unsigned durationMs = 0;          // 0 when missing: the match then goes by name alone
};

// One list read out of an export or a CSV. `skipped` counts the rows that named nothing the
// library or the queue could use: removed Spotify tracks, local files, podcast episodes, blanks.
// `favorites` marks Spotify's saved tracks (Liked Songs): the import hearts what it brings in.
struct ImportList {
    std::string title;
    std::vector<ImportTrack> tracks;
    unsigned skipped = 0;
    bool favorites = false;
};

// Spotify's "Download your data" export. YourLibrary.json holds the saved tracks ("Liked
// Songs", however many) and the saved albums; the albums come back as open.spotify.com links
// because only the site's own page names their tracks. Playlist1.json holds every playlist,
// private ones included, each as one ImportList. False when the text is neither file.
bool ParseYourLibrary(std::string_view json, ImportList& liked, std::vector<std::string>& albumLinks);
bool ParsePlaylist1(std::string_view json, std::vector<ImportList>& lists);

// A CSV playlist: a header row naming the columns (Track Name, Artist Name(s), Album Name,
// Duration (ms), Track URI; quoted commas, UTF-8 with a BOM, missing durations and URIs, and
// CR, LF or CRLF rows are all tolerated). A Playlist Name column splits one file into several
// lists; rows without one belong to `fallbackTitle`. False when the header names no track column.
bool ParseCsv(std::string_view text, const std::string& fallbackTitle, std::vector<ImportList>& lists);

// True when pasted text is itself a Spotify export or a CSV list rather than links, so the
// paste box can import what was pasted without a file.
bool LooksLikeImport(std::string_view text);

// Parses import text: a Spotify export, or a CSV list titled `fallbackTitle` (a file's stem, or
// a fixed name for pasted text). `error` is one line of words for a status bar. False leaves the
// outputs empty.
bool ParseImportText(std::string_view text, const std::wstring& fallbackTitle,
                     std::vector<ImportList>& lists, std::vector<std::string>& albumLinks,
                     std::string& error);
// Reads a .json / .csv file and parses what it holds.
bool ParseImportFile(const std::wstring& path, std::vector<ImportList>& lists,
                     std::vector<std::string>& albumLinks, std::string& error);

// A file name the importers take (.json / .csv), for the file dialogs and dropped files.
bool IsImportFileName(const std::wstring& path);

// The library's own answer to "do I already have this song?": (artist, title) -> a file the
// user owns, with names normalized (case and spacing ignored, the first of several artists
// accepted, so "A, B" finds A's own file). The UI thread captures it in one pass; every lookup
// after that is pure, so workers and tests never touch the live library.
class OwnedIndex {
public:
    void Add(std::string_view artist, std::string_view title, const std::wstring& path);
    bool Find(std::string_view artist, std::string_view title, std::wstring& path) const;
    size_t Size() const { return byName_.size(); }

private:
    std::map<std::pair<std::string, std::string>, std::wstring> byName_;
};

// One row of a list's plan: the track is already owned (its file), or the queue saves it as
// `missing`, the row's index into the plan list's WebTracks.
struct ImportSlot {
    int missing = -1;
    std::wstring owned;
};

// What one imported list will do: the tracks the queue must save, in list order, and the slots
// that rebuild the list's exact order once the run is through.
struct ImportPlanList {
    std::string title;
    std::vector<WebTrack> missing;  // Spotify-style rows the queue matches on YouTube
    std::vector<ImportSlot> slots;  // the list's rows, in exact order
    bool favorites = false;         // Spotify's saved tracks: heart what the import brings in
    size_t OwnedCount() const;
};
struct ImportPlan {
    std::vector<ImportPlanList> lists;
    std::vector<std::string> albumLinks;  // saved Spotify albums, resolved by the queue itself
    size_t TrackCount() const;            // every row the import names
    size_t OwnedCount() const;            // rows the library already had
    size_t MissingCount() const;          // rows the queue will save
};

// Splits the parsed lists against the library: owned rows keep their file, the rest become
// WebTracks the queue matches on YouTube - the id from the row's uri when there is one, else a
// stable name key, so the saved/failed record remembers what a re-import should skip.
void SplitByLibrary(const std::vector<ImportList>& lists, const OwnedIndex& owned, ImportPlan& plan);

// Identity slots for a link-resolved playlist: row i of the range is item first + i.
std::vector<ImportSlot> RangeSlots(size_t count);

// The ordered files one saved list is made of: a slot's own file, or the file the queue saved
// for its missing track, or the copy the library turns out to have of a match that failed.
// Slots with nothing to show are skipped, and nothing else changes order.
std::vector<std::wstring> AssembleFiles(const std::vector<WebFetchItem>& items, size_t first,
                                        const std::vector<ImportSlot>& slots, const OwnedIndex& owned);

}  // namespace import
