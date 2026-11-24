// Checks of library sync through a shared folder (syncfolder.h): two simulated PCs with their music in
// different places merge plays, listens, ratings, hearts, playlists and places in songs through real log
// files, then a damaged log line, a track that arrives later, a crash between publishing and saving, and a
// profile copied to a new PC. The Windows self-test (selftest_sync.cpp, suite "sync") and the Linux core
// test (core_test.cpp, "sync") both run them, so the Win32 file calls are checked on both platforms.
#pragma once
#include "library.h"
#include "playlog.h"
#include "syncfolder.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace sync_checks {

using Results = std::vector<std::pair<bool, std::string>>;

struct Pc {
    syncfolder::Sync sync;
    Library lib;
    std::vector<Playlist> playlists;
    std::vector<std::pair<TrackId, double>> spots;
    std::vector<PlayEvent> log;
    std::wstring root;

    Pc(const std::wstring& scratch, const wchar_t* name, const std::wstring& shared, const std::wstring& music) {
        sync.folder = shared;
        sync.stateFile = scratch + kPathSep + name + L"-state.txt";
        sync.machine = name;
        sync.fingerprint = ToUtf8(name);
        sync.roots = {music};
        root = music;
    }
    std::wstring PathOf(const wchar_t* file) const {
        return root + kPathSep + L"Artist" + kPathSep + L"Album" + kPathSep + file;
    }
    TrackId Add(const wchar_t* file, const char* title, uint32_t plays) {
        TrackId id = lib.Add(PathOf(file));
        Track& t = lib[id];
        t.title = lib.Intern(title);
        t.artist = lib.Intern("Artist");
        t.album = lib.Intern("Album");
        t.durationMs = 200000;
        t.flags |= Track::kLoaded | Track::kInLibrary;
        t.playCount = plays;
        if (plays) t.firstPlayed = 1000, t.lastPlayed = 2000;
        return id;
    }
    TrackId Find(const wchar_t* file) const { return lib.Find(PathOf(file)); }
    // A pass as the player runs it: merge, log the other PCs' listens, save, then the bookkeeping.
    syncfolder::Changes Pass(bool save = true) {
        syncfolder::Changes c;
        sync.RunNow({lib, playlists, spots, log}, c);
        for (const PlayEvent& e : c.listens) log.push_back(e);
        if (save) sync.LibrarySaved();
        return c;
    }
    void Listen(TrackId id) {
        ++lib[id].playCount;
        lib[id].lastPlayed = UnixNow();
        log.push_back({id, UnixNow(), 180, PlayEvent::kCounted});
    }
};

// `scratch` is emptied first and removed at the end.
inline Results All(const std::wstring& scratch) {
    Results out;
    auto check = [&](bool ok, const char* what) { out.emplace_back(ok, what); };
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(fs::path(scratch), ec);
    const std::wstring shared = scratch + kPathSep + L"shared";
    fs::create_directories(fs::path(shared), ec);
    if (ec) {
        check(false, "sync scratch folder");
        return out;
    }
#ifdef _WIN32
    const std::wstring musicA = L"C:\\SyncTest\\Music", musicB = L"D:\\Elsewhere\\Songs";
#else
    const std::wstring musicA = L"/srv/synctest/Music", musicB = L"/home/elsewhere/Songs";
#endif
    Pc a(scratch, L"PC-A", shared, musicA), b(scratch, L"PC-B", shared, musicB);
    const TrackId a1 = a.Add(L"01 One.flac", "One", 5), a2 = a.Add(L"02 Two.flac", "Two", 0),
                  a3 = a.Add(L"03 Three.flac", "Three", 0);
    a.lib[a1].rating = 80;
    a.playlists.push_back(Playlist{L"Road trip", {a1, a2}});
    const TrackId b1 = b.Add(L"01 One.flac", "One", 5), b2 = b.Add(L"02 Two.flac", "Two", 3),
                  b3 = b.Add(L"03 Three.flac", "Three", 0);
    b.lib.SetFavorite(b2, true);
    b.playlists.push_back(Playlist{L"Road trip", {b3}});  // the same name, made separately

    // Joining: counts merge by the highest, ratings and hearts fill gaps, nobody's playlist is replaced.
    a.Pass();
    b.Pass();
    a.Pass();
    check(a.lib[a1].playCount == 5 && b.lib[b1].playCount == 5, "a copied count isn't doubled");
    check(a.lib[a2].playCount == 3 && b.lib[b2].playCount == 3, "a count only one PC had arrives");
    check(b.lib[b1].rating == 80 && a.lib.Favorite(a2), "rating and heart fill the gaps");
    check(a.playlists[0].items.size() == 2 && b.playlists[0].items == std::vector<TrackId>{b3},
          "two playlists of one name each stay");

    // Plays after joining add up, and A's listens land in B's play log for the year in review.
    a.Listen(a1);
    a.Listen(a1);
    b.lib[b1].playCount += 1;  // an imported or hand-edited count: no listen to go with it
    a.Pass();
    const syncfolder::Changes heard = b.Pass();
    a.Pass();
    check(a.lib[a1].playCount == 8 && b.lib[b1].playCount == 8, "plays from both PCs add up");
    check(heard.listens.size() == 2 && b.log.size() == 2 && b.log[0].id == b1, "listens arrive with their times");
    check(b.lib[b1].lastPlayed >= UnixNow() - 60, "last played follows the newest listen");
    a.Pass();
    b.Pass();
    check(a.lib[a1].playCount == 8 && b.lib[b1].playCount == 8 && b.log.size() == 2,
          "passes with nothing new change nothing");

    // Newest change wins: ratings, hearts, playlists, closing a playlist, places in songs.
    b.lib[b1].rating = 100;
    a.lib.SetFavorite(a2, false);
    b.Pass();
    a.Pass();
    b.Pass();
    check(a.lib[a1].rating == 100 && !b.lib.Favorite(b2), "a rating and a heart changed later win");
    a.playlists[0].items = {a2};
    a.spots = {{a3, 42.0}};
    a.Pass();
    const syncfolder::Changes edited = b.Pass();
    check(b.playlists[0].items == std::vector<TrackId>{b2} && !b.playlists[0].history.undo.empty(),
          "an edited playlist replaces the other PC's copy, undoably");
    check(edited.playlistsEdited == std::vector<int>{0}, "the edit is reported");
    check(b.spots.size() == 1 && b.spots[0].first == b3 && std::abs(b.spots[0].second - 42) < 0.01,
          "a place in a song arrives");
    a.playlists.push_back(Playlist{L"Gym", {a3}, PlaylistKind::Static});
    a.playlists.erase(a.playlists.begin());
    a.Pass();
    const syncfolder::Changes closed = b.Pass();
    check(closed.playlistsRemoved == std::vector<std::wstring>{L"Road trip"} && closed.playlistsAdded == 1 &&
              b.playlists.back().name == L"Gym" && b.playlists.back().items == std::vector<TrackId>{b3},
          "a closed playlist closes and a new one arrives");
    b.playlists.erase(b.playlists.begin());  // as the player closes it
    b.Pass();

    // A track B doesn't have yet waits until it arrives; a damaged line is skipped.
    const TrackId a4 = a.Add(L"04 Four.flac", "Four", 0);
    a.lib[a4].playCount = 2;
    a.lib[a4].firstPlayed = UnixNow() + 1;  // played since joining
    a.Pass();
    bool cut = false;
    for (const auto& entry : fs::directory_iterator(fs::path(shared), ec))
        if (entry.path().filename().string().starts_with("PC-A-")) {
            std::ofstream(entry.path(), std::ios::binary | std::ios::app) << "R\t123\t100\t0\tbroken";  // no newline
            cut = true;
        }
    check(cut, "A's log is in the folder");
    b.Pass();
    const bool waited = !b.lib.Contains(b.Find(L"04 Four.flac"));
    const TrackId b4 = b.Add(L"04 Four.flac", "Four", 0);
    a.lib[a3].rating = 60;
    a.Pass();
    b.Pass();
    check(waited && b.lib[b4].playCount == 2, "a track that arrives later gets its plays");
    check(b.lib[b3].rating == 60 && b.lib[b1].rating == 100, "a damaged line is skipped and the log carries on");

    // A crash after publishing but before saving: the next start merges this PC's own lines again.
    const fs::path saved = fs::path(scratch) / "saved.txt";
    fs::copy_file(fs::path(a.sync.stateFile), saved, fs::copy_options::overwrite_existing, ec);
    a.lib[a3].rating = 20;
    a.Listen(a3);
    a.Pass(false);  // published, then the process died before the library and bookkeeping were saved
    fs::copy_file(saved, fs::path(a.sync.stateFile), fs::copy_options::overwrite_existing, ec);
    Pc restarted(scratch, L"PC-A", shared, musicA);
    for (TrackId id = 0; id < (TrackId)a.lib.Count(); ++id) restarted.lib.ImportTrack(a.lib, id);
    const TrackId r1 = restarted.Find(L"01 One.flac"), r3 = restarted.Find(L"03 Three.flac");
    restarted.lib[r3].rating = 60;  // as last saved
    restarted.lib[r3].playCount -= 1;
    restarted.Pass();
    b.Pass();
    check(restarted.lib[r3].rating == 20 && restarted.lib[r3].playCount == 1 && restarted.log.size() == 1,
          "this PC's unsaved changes come back from its own log");
    check(b.lib[b3].rating == 20 && b.lib[b3].playCount == 1, "and reach the other PC once");

    // A copy of A's profile on a third PC carries on from what A had read, without counting A twice.
    fs::copy_file(fs::path(restarted.sync.stateFile), fs::path(scratch) / "PC-C-state.txt",
                  fs::copy_options::overwrite_existing, ec);
    Pc c(scratch, L"PC-C", shared, musicA);
    for (TrackId id = 0; id < (TrackId)restarted.lib.Count(); ++id) c.lib.ImportTrack(restarted.lib, id);
    const TrackId c1 = c.Find(L"01 One.flac");
    c.Pass();
    c.Listen(c1);
    c.Pass();
    restarted.Pass();
    b.Pass();
    check(c.lib[c1].playCount == 9 && restarted.lib[r1].playCount == 9 && b.lib[b1].playCount == 9,
          "a copied profile starts its own log without counting twice");

    fs::remove_all(fs::path(scratch), ec);
    return out;
}

}  // namespace sync_checks
