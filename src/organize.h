// Auto-organize: moves and renames files into a folder layout built from their tags, e.g.
//   <root>\Album Artist\Album\01 Title.flac
// Every run writes an undo log, so the last organize can be reversed.
#pragma once
#include "library.h"

#include <string>
#include <vector>

// MusicBee-style title format: $if2 supplies a fallback for an empty field and [ ] drops its
// contents when the fields inside are all empty.
constexpr wchar_t kDefaultOrganizePattern[] =
    L"$if2(%album artist%,Unknown artist)\\$if2(%album%,Unknown album)\\"
    L"[$ifgreater(%totaldiscs%,1,%discnumber%-,)][$num(%tracknumber%,2) ]%title%";

struct OrganizeMove {
    TrackId id = 0;
    std::wstring from, to;
    std::wstring skip;  // why it won't be moved (empty = it will)
};

// Where each track would go. Tracks already in place, or whose name would collide with another
// file, get a reason in `skip` (collisions get " (2)" added instead when that's free).
std::vector<OrganizeMove> PlanOrganize(const std::vector<TrackId>& ids, const std::wstring& root, const std::wstring& pattern);

// The folders and file name (no extension) `pattern` gives a track, each a safe path component.
// Never empty: a pattern that gives nothing falls back to the file's current name.
std::vector<std::wstring> PatternParts(TrackId id, const std::wstring& pattern);

// A file name component made safe for Windows (no reserved characters or names, no trailing
// dots/spaces, at most `maxLen` characters).
std::wstring SafePathComponent(const std::wstring& s, size_t maxLen = 120);

// Everything a run reports back: the tracks whose files moved, the files that failed with a
// reason, and the undo log that was written (or "").
struct OrganizeResult {
    std::vector<std::pair<TrackId, std::wstring>> moved;  // track -> its new path
    std::vector<std::pair<std::wstring, std::wstring>> failed;  // file -> error
    std::wstring log;  // the undo log written
};

// Moves the planned files (with their folder's cover images when a whole folder moves to one
// place), removes folders that end up empty (never one in `keep`), and writes an undo log in
// `logDir`. Runs on any thread; the library itself isn't touched (the caller renames tracks).
OrganizeResult ExecuteOrganize(const std::vector<OrganizeMove>& plan, const std::wstring& logDir,
                               const std::vector<std::wstring>& keep);

// Reverses the newest undo log in `logDir`: (from, to) pairs that were moved back.
std::vector<std::pair<std::wstring, std::wstring>> UndoLastOrganize(const std::wstring& logDir,
                                                                    const std::vector<std::wstring>& keep, std::wstring& error);
