// Library sync between PCs through a folder the user already syncs (Syncthing, Nextcloud, Dropbox, a USB
// stick): no server and no account. Each PC appends what changed on it to its own small log in that
// folder and only reads the others' logs, so no two PCs ever write the same file and a sync tool never
// has a conflict to resolve. library.cdb itself never goes near the folder.
//
// What travels: plays and skips (with each listen, so the year in review counts both PCs), ratings,
// hearts, static and auto playlists, and the places songs were left partway. A track is found by its
// path under a library folder, then by its audio hash, then by artist, album, title and length, so a
// Windows PC and a Linux laptop with the same music in different places agree.
//
// The merge rules give the same result whatever order the logs arrive in:
// - Plays and skips are counters. Each PC adds what it counted; the counts a PC had when it joined are
//   merged by taking the highest, so a library copied from another PC isn't counted twice.
// - Ratings, hearts, playlists (by name) and places in songs: the newest change wins. What a PC already
//   had when it joined only fills gaps, so joining never overwrites anything.
//
// Format (docs/sync.md): UTF-8 lines, tab-separated, each ending in a checksum. A line cut short by a
// sync in progress or a full disk is skipped until it's whole; unknown kinds are skipped for later
// versions. This PC's bookkeeping (what it read from whom, what it last published) lives in the profile
// folder and is written only after library.cdb is saved, so a crash never applies or publishes twice.
//
// Threading: Tick runs on the UI thread and only starts a worker that reads the folder; the merge
// itself runs back on the UI thread, which owns the library.
#pragma once
#include "library.h"
#include "playlog.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace syncfolder {

// The player's live data a pass reads and merges into, all owned by the UI thread.
struct Live {
    Library& library;
    std::vector<Playlist>& playlists;
    std::vector<std::pair<TrackId, double>>& spots;  // songs left partway, newest first (Come back)
    const std::vector<PlayEvent>& playLog;
};

// What a merge changed here, for the player to apply and show.
struct Changes {
    size_t tracks = 0;                 // tracks whose plays, skips, rating or heart changed
    std::vector<PlayEvent> listens;    // other PCs' listens: add each with LogPlay
    std::vector<TrackId> created;      // tracks a synced playlist named that weren't here yet: read their tags
    std::vector<int> playlistsEdited;  // playlists whose items or rules were replaced (undoably)
    size_t playlistsAdded = 0;         // new playlists, appended at the end of `playlists`
    std::vector<std::wstring> playlistsRemoved;  // closed on another PC: close them here too (reopenable)
    bool spots = false;
    bool saveNow = false;  // the library should be saved now; call LibrarySaved once it is
    bool Any() const {
        return tracks || !listens.empty() || !playlistsEdited.empty() || playlistsAdded ||
               !playlistsRemoved.empty() || spots;
    }
    std::wstring Summary() const;  // "Synced 3 tracks and 1 playlist from your other PCs."
};

struct PeerStatus {
    std::wstring name;   // the other PC's name
    uint32_t lastSeen;   // UnixNow() of its newest change read here, 0 = none yet
};
struct Status {
    bool on = false, busy = false;
    uint32_t lastPass = 0;      // UnixNow() of the last finished pass
    std::wstring problem;       // why the last pass failed, empty when it worked
    std::vector<PeerStatus> peers;
};

struct State;
struct Scan;

class Sync {
public:
    // Set by the player before each Tick; an empty folder turns sync off.
    std::wstring folder;
    std::wstring stateFile;         // in the profile folder, never the shared one
    std::wstring machine;           // this PC's name as the others list it
    std::string fingerprint;        // this PC and profile; a copied profile on another PC differs
    std::vector<std::wstring> roots;  // library folders: paths under them travel relative

    Sync();
    ~Sync();
    // Starts a pass when one is due (about once a minute, or at once with `now`) and merges a finished
    // one. Returns true when `changes` needs the player's attention.
    bool Tick(Live live, Changes& changes, bool now = false);
    // At exit, after the last listen is recorded: waits for a running pass, then runs one in place so
    // everything this PC did is published. The player then saves the library and calls LibrarySaved.
    void Finish(Live live, Changes& changes);
    // One whole pass in place (Finish and the tests). False with `problem` set when the folder failed.
    bool RunNow(Live live, Changes& changes);
    // After library.cdb was saved: writes the bookkeeping that matches it.
    void LibrarySaved();
    Status CurrentStatus() const;

private:
    std::unique_ptr<State> pending_;   // merged and waiting for the library save
    std::thread worker_;
    std::atomic<bool> done_{false};
    std::unique_ptr<Scan> result_;
    std::wstring activeFolder_;
    uint64_t lastStart_ = 0;           // UnixNow() of the last started pass
    uint64_t digest_ = 0;              // the stats, playlists and spots when the last pass started
    bool merged_ = false;              // a merge ran this session
    size_t listenMark_ = SIZE_MAX;     // play log entries before it are published or from other PCs
    size_t pendingCount_ = 0;          // library size when waiting lines were last tried
    // The folder's log files and sizes after the last pass, as a digest: an unchanged folder needs no
    // further reading. 0 = unknown.
    uint64_t seen_ = 0;
    Status status_;
    bool Start(Live live, bool force);
    bool Merge(Live live, Scan& scan, Changes& changes);
    void Join();
};

// Exposed for the tests.
uint64_t LocalDigest(const Live& live);
}  // namespace syncfolder
