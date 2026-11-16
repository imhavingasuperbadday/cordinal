// Find duplicates (Library menu), shared by both front ends: which library tracks are copies of one
// song, which copy to keep, and moving the history of the copies that go to the one that stays.
//
// Two kinds of group. Identical audio: the same decoded sound (Track::audioHash, a FLAC or .cord MD5
// or the hash measuring computes for every other format), whatever the tags or file names. Same song:
// the same artist and title within a second of each other's length, in different encodes. Within a
// group the copies are ordered best first: lossless before lossy, then the higher bitrate, the fuller
// tags and the longer history, so the first is the one suggested to keep.
//
// Clean-up is three steps so nothing slow runs on the UI thread: Plan picks, for each copy to remove,
// the copy that keeps its history; a worker sends the files to the Recycle Bin (convert.h RecycleFile);
// then Merge, on the UI thread, carries the recycled copies' plays, skips, dates, ratings, play log
// entries and playlist places over to the kept copies. Undo reverses it after the files come back.
// Tracks are found again by path at each step, so a library compaction in between does no harm.
#pragma once
#include "library.h"

#include <string>
#include <vector>

namespace duplicates {

struct Group {
    bool identical = false;      // the same decoded audio; otherwise the same song in different files
    std::vector<TrackId> tracks; // best first: tracks[0] is the copy suggested to keep
};

// A library or Inbox track whose file is present and whole (a cue sheet's part shares its file).
bool Eligible(const Library& library, TrackId id);
// Eligible tracks with no audio hash yet. Measuring them (ReplayGain's scan) gives them one.
std::vector<TrackId> Unmeasured(const Library& library);
// True when `a` is the better copy to keep.
bool Better(const Library& library, TrackId a, TrackId b);
// Identical-audio groups first, then same-song groups; each group's copies best first. A same-song
// group lists only the best copy of any identical group it touches, so no track is in two groups.
std::vector<Group> Find(const Library& library);
// "FLAC, 44.1 kHz" or "MP3, 320 kb/s": what tells the copies of a group apart.
std::wstring FormatText(const Track& t);

// One copy to remove and the copy that inherits its history.
struct Move {
    std::wstring extra, keep;  // paths
};
// For each group, the unchecked copy listed first keeps the history of the checked ones. A group whose
// copies are all checked is left alone (`whole` counts them); nothing is planned for a playing track.
std::vector<Move> Plan(const Library& library, const std::vector<Group>& groups,
                       const std::vector<std::vector<bool>>& remove, TrackId playing, size_t* whole = nullptr);

// What Merge changed, enough to take it back.
// The history a clean-up moves: a track's statistics.
struct History {
    uint32_t playCount = 0, skipCount = 0, lastPlayed = 0, firstPlayed = 0, added = 0;
    uint8_t rating = 0;
    uint16_t flags = 0;
};
struct Merged {
    std::wstring extra, keep;
    History extraBefore, keepBefore, keepAfter;
};
struct PlaylistChange {
    uint64_t identity = 0;
    std::vector<TrackId> before, after;
};
struct EventChange {
    size_t index = 0;   // in the play log
    uint32_t time = 0;  // the event's, to recognize it again
    size_t merged = 0;  // whose event it was (Cleanup::merged)
};
struct Cleanup {
    std::vector<Merged> merged;
    std::vector<PlaylistChange> playlists;
    std::vector<EventChange> events;
    bool Empty() const { return merged.empty(); }
};

// UI thread, once the files of `moves` with recycled[i] set are in the Recycle Bin: each such copy's
// plays, skips, dates and rating join its kept copy's, its play log entries and its playlist places
// become the kept copy's (a static playlist that already holds the kept copy drops the extra entry,
// as one undoable playlist edit), and the copy is marked missing.
Cleanup Merge(Library& library, std::vector<Playlist>& playlists, const std::vector<Move>& moves,
              const std::vector<bool>& recycled);
// UI thread, once the files of `cleanup.merged` with restored[i] set are back: their history leaves the
// kept copies again, and playlists still as Merge left them return to how they were. Returns the
// tracks whose tags should be read again (restored files); `skipped` counts playlists changed since.
std::vector<TrackId> Undo(Library& library, std::vector<Playlist>& playlists, const Cleanup& cleanup,
                          const std::vector<bool>& restored, size_t* skipped = nullptr);

}  // namespace duplicates
