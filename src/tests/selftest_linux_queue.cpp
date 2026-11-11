// Linux queue editor regression checks, as on Windows (selftest_ui.cpp, TestQueueEditor): your
// tracks stay ahead of Choose by taste's picks, edits keep that order, picks follow the shuffle
// order without repeats and go when taste stops choosing, and the panel's keys and drags edit the
// queue. Runs with --check-layout against a library of its own; nothing is played or saved.
#ifndef _WIN32
#include "../linux/ui_internal.h"
#include <cstdio>

namespace lx {
int CheckLinuxQueue() {
    int failed = 0;
    auto check = [&](bool ok, const char* message) {
        printf("%s %s\n", ok ? "ok  " : "FAIL", message);
        fflush(stdout);
        failed += !ok;
    };
    Library savedLib = std::move(g_lib);
    g_lib = Library();
    const auto savedSettings = g.settings;
    const auto savedPlaylists = g.playlists;
    const auto savedQueue = g.queue;
    const int savedActive = g.active, savedPos = g.queuePos;
    const TrackId savedPlaying = g.playing;
    const auto savedToken = g.token;
    const auto savedOrder = g.shuffleOrder;
    std::vector<TrackId> ids;
    for (int i = 0; i < 7; ++i) ids.push_back(g_lib.Add(L"/checks/queue-" + std::to_wstring(i) + L".flac"));
    int np = EnsureNowPlayingTab();
    g.playlists[np].items = ids;
    g.queue.clear();
    g.queuePicks = 0;
    g.pickIndex.clear();
    g.pickFrom = -1;
    g.settings.repeat = Repeat::Off;
    g.settings.radio = false;
    g.settings.shuffle = kShuffleTracks;
    g.shuffleOrder.clear();
    g.queuePos = 0;
    g.playing = ids[0];
    g.token = 0;  // nothing reaches the engine
    g.settings.taste = true;
    auto distinct = [&] {
        std::vector<TrackId> q(g.queue.begin(), g.queue.end());
        std::sort(q.begin(), q.end());
        return std::adjacent_find(q.begin(), q.end()) == q.end() && !std::count(q.begin(), q.end(), ids[0]);
    };

    RefreshNext();
    check(g.queuePicks == 5 && g.queue.size() == 5 && g.pickIndex.size() == 5 && distinct(),
          "queue: Choose by taste in track shuffle picks five tracks from the order");
    bool atPlace = true;
    for (size_t k = 0; k < g.pickIndex.size(); ++k) atPlace &= ids[g.pickIndex[k]] == g.queue[k];
    check(atPlace, "queue: each pick plays at its own Now Playing position");
    TrackId mine = g_lib.Add(L"/checks/queue-mine.flac");
    QueueTracks({mine}, false);
    check(g.queue.front() == mine && g.queuePicks == 5, "queue: Add to queue goes ahead of the picks");
    TrackId pick = g.queue[1];
    MoveInQueue({1}, 0);
    check(g.queue.front() == pick && QueuedByYou() == 2 && g.queuePicks == 5 && distinct(),
          "queue: a pick moved among yours becomes yours and the next in the order replaces it");
    int at = MoveInQueue({0}, (int)g.queue.size());
    check(at == 1 && g.queue[1] == pick && QueuedByYou() == 2, "queue: your track never moves below the picks");
    RemoveFromQueue({(int)g.queue.size() - 1});
    check(g.queuePicks == 4 && g.pickIndex.size() == 4 && QueuedByYou() == 2,
          "queue: a removed pick isn't replaced once the order is done");
    MoveInQueue({2}, 2, true);
    check(QueuedByYou() == 3 && g.queuePicks == 3 && g.pickIndex.size() == 3, "queue: Keep makes a pick yours");
    g.settings.repeat = Repeat::One;
    RefreshNext();
    check(g.queuePicks == 0 && g.queue.size() == 3, "queue: Repeat track drops the picks and keeps yours");
    g.settings.repeat = Repeat::Off;
    g.settings.taste = false;
    RefreshNext();
    check(g.queuePicks == 0, "queue: no picks while taste is off");

    // The panel: a click and Delete remove a track, a drag moves one.
    g.settings.shuffle = kShuffleOff;
    g.queue = {ids[1], ids[2], ids[3], ids[4]};
    L.upNext = {700, 100, 300, 500};
    auto rowY = [](int i) { return 100 + S(40) + S(44) * i + S(22); };
    QueueMouseDown(760, rowY(0), SDL_BUTTON_LEFT, 1, SDL_KMOD_NONE);
    QueueMouseUp();
    SDL_KeyboardEvent del{};
    del.key = SDLK_DELETE;
    QueueKey(del);
    check(g.queue == std::deque<TrackId>{ids[2], ids[3], ids[4]}, "queue: Delete in the panel removes the selected track");
    QueueMouseDown(760, rowY(2), SDL_BUTTON_LEFT, 1, SDL_KMOD_NONE);
    QueueMouseMove(760, rowY(1));
    QueueMouseMove(760, rowY(0) - S(10));
    QueueMouseUp();
    check(g.queue == std::deque<TrackId>{ids[4], ids[2], ids[3]}, "queue: dragging the last queued track to the top moves it there");
    QueueMouseDown(10, 10, SDL_BUTTON_LEFT, 1, SDL_KMOD_NONE);  // elsewhere: the keyboard leaves the panel
    check(!QueueFocused(), "queue: a click outside the panel takes the keyboard back");

    g.queue = savedQueue;
    g.queuePicks = 0;
    g.pickIndex.clear();
    g.playlists = savedPlaylists;
    g.active = savedActive;
    g.queuePos = savedPos;
    g.playing = savedPlaying;
    g.token = savedToken;
    g.shuffleOrder = savedOrder;
    g.settings = savedSettings;
    g_lib = std::move(savedLib);
    L.upNext = {};
    return failed;
}
}  // namespace lx
#endif
