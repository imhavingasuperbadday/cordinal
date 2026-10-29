// Playback control, playback order, now-playing strip and status bar; also the queue, the token
// bookkeeping that ties a play request to its playlist slot, and the listening statistics that
// count plays and skips. Runs on the UI thread: the engine's WM_ENGINE messages arrive through
// OnEngineEvent, and UI ticks call UpdateStatus/WaveformTick from MainTick.
#include "app.h"
#include "cue.h"
#include "game_music.h"
#include "playlog.h"

namespace app {

// The last requested seek preview: held by UpdateStatus until the engine agrees (RequestSeek).
static double s_seekTo = -1;
static ULONGLONG s_seekUntil = 0;

// The preview belongs to the track it was asked for: a new track starts at its own position, and
// holding the old target there would show its first half-second at the wrong spot.
static void CancelSeekPreview() {
    s_seekTo = -1;
    s_seekUntil = 0;
}

PlayItem MakeItem(unsigned long long token, TrackId id) {
    PlayItem item{token, g_lib.Path(id), RgGainFor(id), g_lib[id].Duration()};
    item.estimate = g.rgOn && g.rgEstimate && g_lib[id].rgTrackGain == kNoGain && g_lib[id].rgAlbumGain == kNoGain;
    // A song of a game-music file (NSF, GBS...) is chosen by the decoder, not by a range.
    if (IsSubtrack(item.path) && !IsGameMusicFile(item.path)) {
        // A CUE track plays its slice of the file. If the sheet is gone, start -1 makes the engine
        // report an error rather than play the whole file.
        double start = 0, end = 0;
        if (SubtrackRange(item.path, start, end)) {
            item.start = start;
            item.end = end;
            item.duration = end > 0 ? end - start : item.duration;
        } else {
            item.start = -1;
        }
    }
    // Section marks use the track's own timeline, including for CUE slices. Keep that timeline
    // intact so waveform marks, lyrics, seeking and resume all use the same positions.
    Region region = RegionOf(id);
    if (region.on && region.start >= 0 && region.end - region.start > 0.05 && item.start >= 0 &&
        (item.duration <= 0 || region.end <= item.duration)) {
        item.loopStart = region.start;
        item.loopEnd = region.end;
    }
    return item;
}

// Issues a fresh token for `id` in playlist `pl`. Tokens are the engine's handle on a track; the
// map keeps the playlist slot for the answer to SetNext, and is pruned of entries too old to be
// referenced (the current and decoding tokens are always kept).
unsigned long long NewToken(TrackId id, int pl, uint32_t index) {
    EnsurePlaylistIdentities();
    unsigned long long token = g.nextToken++;
    g.tokens[token] = {id, pl};
    g.tokens[token].playlistIdentity = pl >= 0 && pl < (int)g.playlists.size() ? g.playlists[pl].identity : 0;
    g.tokens[token].index = index == UINT32_MAX ? (uint32_t)IndexIn(pl, id) : index;
    if (g.tokens.size() > 64)  // forget tokens that can no longer be referenced
        for (auto it = g.tokens.begin(); it != g.tokens.end();)
            // resumeToken is looked up again when its TrackStarted arrives; dropping it here
            // would leave playback running with no track attached.
            it = (it->first + 32 < token && it->first != g.playingToken && it->first != g.decodingToken &&
                  it->first != g.resumeToken)
                     ? g.tokens.erase(it)
                     : std::next(it);
    return token;
}

// Where `token` sits in `playlist` now, or -1 if the playlist changed under it. An anchored token
// (one made for the queue) tracks the slot it was queued after, so edits to the playlist don't
// derail it.
int TokenIndex(unsigned long long token, int playlist) {
    if (playlist < 0 || playlist >= (int)g.playlists.size()) return -1;
    auto it = g.tokens.find(token);
    if (it == g.tokens.end() || it->second.pl != playlist) return -1;
    const auto& info = it->second;
    const auto& items = g.playlists[playlist].items;
    uint32_t index = info.anchor != kNone ? info.anchorIndex : info.index;
    TrackId id = info.Position();
    return index < items.size() && items[index] == id ? (int)index : -1;
}

// Playlist entries were removed/reordered: `mapping` gives each old slot its new one (UINT32_MAX
// when gone). Tokens for that playlist are re-pointed; one whose track vanished gets a `following`
// slot, so the queue can still continue sensibly.
void RemapOccurrences(int playlist, const std::vector<uint32_t>& mapping) {
    for (auto& [token, info] : g.tokens) {
        if (info.pl != playlist) continue;
        uint32_t old = info.anchor != kNone ? info.anchorIndex : info.index;
        if (old == UINT32_MAX) old = info.following;
        info.index = info.index < mapping.size() ? mapping[info.index] : UINT32_MAX;
        info.anchorIndex = info.anchorIndex < mapping.size() ? mapping[info.anchorIndex] : UINT32_MAX;
        info.radioNext = info.radioNext < mapping.size() ? mapping[info.radioNext] : UINT32_MAX;
        info.following = UINT32_MAX;
        if (TokenIndex(token, playlist) < 0 && old != UINT32_MAX) {
            for (size_t i = old; i < mapping.size(); ++i)
                if (mapping[i] != UINT32_MAX) { info.following = mapping[i]; break; }
            if (info.following == UINT32_MAX)
                info.following = (uint32_t)std::count_if(mapping.begin(), mapping.end(), [](uint32_t i) { return i != UINT32_MAX; });
        }
    }
    g.shuffleQueue.clear();
    g.shuffleStarted = false;
}

// Index of the track after `fromToken` in its playlist, honoring the playback order.
int ComputeNext(unsigned long long fromToken, bool manual, int& plOut) {
    int pl = g.active;
    auto it = g.tokens.find(fromToken);
    if (it != g.tokens.end()) {
        if (it->second.pl < 0 || it->second.pl >= (int)g.playlists.size()) return -1;
        pl = it->second.pl;
    }
    plOut = pl;
    auto& items = g.playlists[pl].items;
    int n = (int)items.size();
    if (n == 0) return -1;
    int cur = TokenIndex(fromToken, pl);
    if (cur < 0 && it != g.tokens.end() && it->second.following != UINT32_MAX && !g.shuffle && !g.radio) {
        uint32_t next = it->second.following;
        return next < items.size() ? (int)next : g.repeat == Repeat::Off ? -1 : 0;
    }
    if (g.repeat == Repeat::One && !manual && cur >= 0) return cur;
    if (g.radio) {
        // Cache by request token: gapless lookahead and repeated NeedNext must offer the same
        // track. Picks stay within the source playlist, and the explicit queue still comes first.
        if (it != g.tokens.end() && it->second.radioNext < items.size()) return (int)it->second.radioNext;
        TrackId from = it != g.tokens.end() ? it->second.id : kNone;
        auto weights = ListeningWeights(items, from);
        double total = 0;
        for (int i = 0; i < n; ++i) {
            if (items[i] == from || items[i] >= g_lib.Count() || g_lib[items[i]].Failed()) weights[i] = 0;
            // A short memory stops two strong transitions from trapping radio in a tiny loop.
            if (std::find(g.history.rbegin(), g.history.rbegin() + std::min<size_t>(5, g.history.size()), items[i]) !=
                g.history.rbegin() + std::min<size_t>(5, g.history.size())) weights[i] *= 0.15;
            total += weights[i];
        }
        int pick = -1;
        if (total > 0) {
            double draw = std::generate_canonical<double, 53>(g.rng) * total;
            for (int i = 0; i < n; ++i)
                if (weights[i] > 0 && (draw -= weights[i]) < 0) { pick = i; break; }
        } else if (cur >= 0 && items[cur] < g_lib.Count() && !g_lib[items[cur]].Failed()) pick = cur;
        if (it != g.tokens.end()) it->second.radioNext = (uint32_t)pick;
        return pick;
    }
    if (!g.shuffle) {
        if (cur + 1 < n) return cur + 1;
        return g.repeat != Repeat::Off ? 0 : -1;
    }
    // Keep positions until they play, so re-answering offers the same occurrence.
    if (g.shufflePl != pl) {
        g.shuffleQueue.clear();
        g.shufflePl = pl;
        g.shuffleStarted = false;
    }
    if (!g.shuffleStarted || (fromToken != g.shuffleToken && (it == g.tokens.end() || !it->second.queued))) {
        auto occurrence = std::find(g.shuffleQueue.begin(), g.shuffleQueue.end(), (uint32_t)cur);
        if (g.shuffle == kShuffleAlbums && g.shuffleStarted) {
            // A manual jump continues from that album's position in this pass. If it already
            // played, start afresh there so the rest of that album still stays in sequence.
            if (occurrence != g.shuffleQueue.end()) g.shuffleQueue.erase(occurrence, g.shuffleQueue.end());
            else { g.shuffleQueue.clear(); g.shuffleStarted = false; }
        } else if (occurrence != g.shuffleQueue.end()) g.shuffleQueue.erase(occurrence);
        g.shuffleToken = fromToken;
    }
    for (int pass = 0; pass < 2; ++pass) {
        while (!g.shuffleQueue.empty()) {
            uint32_t k = g.shuffleQueue.back();
            if (k < items.size()) return (int)k;
            g.shuffleQueue.pop_back();  // removed from the playlist since the pass began
        }
        if (g.shuffleStarted && g.repeat == Repeat::Off) return -1;
        if (g.shuffle == kShuffleAlbums) {
            ShuffleAlbums(g.shuffleQueue, items, g.rng, g.shuffleStarted ? -1 : cur);
            if (!g.shuffleStarted && cur >= 0)
                g.shuffleQueue.erase(g.shuffleQueue.begin(),
                    std::find(g.shuffleQueue.begin(), g.shuffleQueue.end(), (uint32_t)cur) + 1);
            std::reverse(g.shuffleQueue.begin(), g.shuffleQueue.end());
        } else {
            for (uint32_t index = 0; index < items.size(); ++index)
                if ((int)index != cur) g.shuffleQueue.push_back(index);
            if (g.skipAwareShuffle) ShuffleByListening(g.shuffleQueue, items, g.rng);
            else std::shuffle(g.shuffleQueue.begin(), g.shuffleQueue.end(), g.rng);
        }
        g.shuffleStarted = true;
    }
    return cur >= 0 ? cur : 0;  // a playlist of one track, repeating
}

// The next few tracks likely to play, soonest first: the queue, then the playing playlist in
// playback order (the shuffle pass's next positions). For measuring ReplayGain ahead of time;
// it changes nothing, unlike ComputeNext.
std::vector<TrackId> UpcomingTracks() {
    constexpr size_t kAhead = 3;
    std::vector<TrackId> out;
    for (TrackId id : g.queue) {
        if (out.size() >= kAhead) return out;
        out.push_back(id);
    }
    const int pl = g.playingPl;
    if (pl < 0 || pl >= (int)g.playlists.size()) return out;
    const auto& items = g.playlists[pl].items;
    if (g.radio) {
        auto token = g.tokens.find(g.playingToken);
        if (token != g.tokens.end() && token->second.radioNext < items.size())
            out.push_back(items[token->second.radioNext]);
        return out;
    }
    if (g.shuffle) {
        for (auto it = g.shuffleQueue.rbegin(); it != g.shuffleQueue.rend() && out.size() < kAhead; ++it)
            if (*it < items.size() && items[*it] != g.playing) out.push_back(items[*it]);
    } else {
        int cur = TokenIndex(g.playingToken, pl);
        for (int k = cur + 1; cur >= 0 && k < (int)items.size() && out.size() < kAhead; ++k) out.push_back(items[k]);
    }
    return out;
}

// The token for playing the head of the queue after `fromToken`: it keeps the playlist and the
// position the playlist continues from once the queue is done.
unsigned long long QueuedToken(unsigned long long fromToken) {
    TrackId id = g.queue.front();
    auto it = g.tokens.find(fromToken);
    int pl = it != g.tokens.end() ? it->second.pl : g.playingPl >= 0 ? g.playingPl : g.active;
    TrackId anchor = it != g.tokens.end() ? it->second.Position() : kNone;
    uint32_t anchorIndex = (uint32_t)TokenIndex(fromToken, pl);
    uint32_t following = it != g.tokens.end() ? it->second.following : UINT32_MAX;
    unsigned long long token = NewToken(id, pl);
    g.tokens[token].anchor = anchor;
    g.tokens[token].anchorIndex = anchorIndex;
    g.tokens[token].following = following;
    g.tokens[token].queued = true;
    return token;
}

// Answers the engine's NeedNext for `token`: the head of the queue if there is one, else the
// playlist's next track per the current order. Queued tokens are remembered so a re-answer (after
// the order or playlist changed) computes the same thing.
void AnswerNext(unsigned long long token) {
    if (g.stopAfterCurrent && (token == g.playingToken || g.playingToken == 0)) {
        g.engine.SetNext(token, nullptr);
        return;
    }
    // The queue comes first. It's only popped when the track starts, so re-answering (after the
    // order or playlist changed) offers the same head again.
    if (!g.queue.empty()) {
        unsigned long long next = QueuedToken(token);
        PlayItem item = MakeItem(next, g.queue.front());
        g.engine.SetNext(token, &item);
        return;
    }
    int pl = 0;
    int k = ComputeNext(token, false, pl);
    if (k < 0) {
        g.engine.SetNext(token, nullptr);
        return;
    }
    TrackId id = g.playlists[pl].items[k];
    PlayItem item = MakeItem(NewToken(id, pl, (uint32_t)k), id);
    g.engine.SetNext(token, &item);
}

// Re-answers the pending "what comes next" after the order or playlist changed.
void RefreshNext() {
    if (RadioPlaying()) return;
    if (g.decodingToken && (g.decodingToken == g.playingToken || g.playingToken == 0)) AnswerNext(g.decodingToken);
}

// Playing from a view copies what it shows into Now Playing (created next to the Library tab),
// so later changes to the view never change what plays next. Returns the Now Playing index
// and moves `index` to the same track there.
int SnapshotToNowPlaying(int pl, int& index) {
    std::vector<TrackId> items;
    const Playlist& src = g.playlists[pl];
    if (pl == g.active && !g.filter.empty()) {
        // What's on screen: the search narrows the snapshot too.
        TrackId id = src.items[index];
        for (uint32_t i : g.view) items.push_back(src.items[i]);
        index = (int)(std::find(items.begin(), items.end(), id) - items.begin());
    } else {
        items = src.items;
    }
    int np = FindPlaylist(PlaylistKind::NowPlaying);
    if (np < 0) {
        np = 1;
        InsertPlaylist(np, Playlist{L"Now Playing", {}, PlaylistKind::NowPlaying});
    }
    g.playlists[np].items = std::move(items);
    g.playlists[np].position = {};
    g.shuffleQueue.clear();
    g.shuffleStarted = false;
    g.dirty = true;
    if (g.active == np) RebuildView();
    return np;
}

// ---------------------------------------------------------------------------
// Queue

void QueueTracks(const std::vector<TrackId>& ids, bool next) {
    if (ids.empty()) return;
    if (next) g.queue.insert(g.queue.begin(), ids.begin(), ids.end());
    else g.queue.insert(g.queue.end(), ids.begin(), ids.end());
    PlaySfx(next ? Sfx::PlayNext : Sfx::QueueAdd, (int)g.queue.size());
    RefreshNext();  // what comes after the current track changed
    InvalidateRect(g.list, nullptr, FALSE);
    UpdateStatus();
    ShowMessage((next ? L"Playing next: " : L"Queued: ") + std::to_wstring(ids.size()) +
                (ids.size() == 1 ? L" track" : L" tracks") + L" (" + std::to_wstring(g.queue.size()) + L" in the queue).");
    if (g.playing == kNone) Next();
}

void ClearQueue() {
    if (!g.queue.empty()) PlaySfx(Sfx::QueueClear);
    g.queue.clear();
    RefreshNext();
    InvalidateRect(g.list, nullptr, FALSE);
    UpdateStatus();
}

int QueuePosition(TrackId id) {
    for (size_t i = 0; i < g.queue.size(); ++i)
        if (g.queue[i] == id) return (int)i + 1;
    return 0;
}

// Plays `items` (an album, a selection) from `index`, through Now Playing.
void PlayList(const std::vector<TrackId>& items, int index) {
    if (items.empty()) return;
    int np = FindPlaylist(PlaylistKind::NowPlaying);
    if (np < 0) {
        np = 1;
        InsertPlaylist(np, Playlist{L"Now Playing", {}, PlaylistKind::NowPlaying});
    }
    g.playlists[np].items = items;
    g.playlists[np].position = {};  // a new playback snapshot has its own navigation state
    g.shuffleQueue.clear();
    g.shuffleStarted = false;
    g.dirty = true;
    if (g.active == np) RebuildView();
    PlayTrack(np, std::clamp(index, 0, (int)items.size() - 1), true);
}

// Starts `index` of playlist `pl`. A view is snapshotted into Now Playing first, so later edits to
// the view can't change what plays. `remember` pushes the outgoing track onto the history that
// Previous uses in Shuffle.
void PlayTrack(int pl, int index, bool remember) {
    if (pl < 0 || pl >= (int)g.playlists.size()) return;
    if (index < 0 || index >= (int)g.playlists[pl].items.size()) return;
    RadioCancelPlayback();
    if (g.playlists[pl].IsView()) pl = SnapshotToNowPlaying(pl, index);
    auto& items = g.playlists[pl].items;
    FinishListening(true);
    if (remember && g.playing != kNone) {
        g.history.push_back(g.playing);
        if (g.history.size() > 256) g.history.erase(g.history.begin());
    }
    TrackId id = items[index];
    g_lib[id].flags &= ~Track::kFailed;
    unsigned long long token = NewToken(id, pl, (uint32_t)index);
    g.firstValidToken = token;
    g.consecutiveErrors = 0;
    g.decodingToken = token;
    g.engine.Play(MakeItem(token, id));
}

// A header row plays its album from the top.
void PlayRow(int row) {
    FlushSearch();
    if (int pos = ViewPosNear(row); pos >= 0) PlayTrack(g.active, (int)g.view[pos], true);
}

void PlayFocusedOrFirst() {
    FlushSearch();
    if (CanvasActive()) {
        LibViewPlaySelection();
        return;
    }
    int row = ListView_GetNextItem(g.list, -1, LVNI_FOCUSED);
    if (row < 0) row = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
    PlayRow(row < 0 ? 0 : row);
}

void Stop() {
    RadioCancelPlayback();
    if (g.playing != kNone && g.resumeOn) {
        g.resumeTrack = g.playing;
        g.resumePl = g.playingPl;
        g.resumePos = g.engine.Position();
    }
    FinishListening(true);
    g.engine.Stop();
    CancelSeekPreview();
    g.firstValidToken = g.nextToken;
    g.tokens.clear();
    g.compactPending = true;
    g.seeking = false;
    g.playing = kNone;
    g.playingToken = 0;
    g.decodingToken = 0;
    g.playingPl = -1;
    UpdateNowPlaying();
    UpdateStatus();
    InvalidateRect(g.list, nullptr, FALSE);
    LibViewInvalidate();
}

void Next() {
    if (RadioPlaying()) return;
    if (!g.queue.empty()) {
        FinishListening(true);
        unsigned long long token = QueuedToken(g.playingToken);
        g.firstValidToken = token;
        g.decodingToken = token;
        g.engine.Play(MakeItem(token, g.queue.front()));
        return;
    }
    int pl = 0;
    int k = ComputeNext(g.playingToken, true, pl);
    if (k >= 0) PlayTrack(pl, k, true);
}

// In Shuffle, Previous returns to the track actually played before this one (the history), since
// the shuffled order has no "before". Otherwise it steps back one, wrapping when Repeat is on.
void Previous() {
    if (RadioPlaying()) return;
    if ((g.shuffle || g.radio) && !g.history.empty()) {
        TrackId t = g.history.back();
        g.history.pop_back();
        for (int pl : {g.playingPl, g.active}) {
            int k = IndexIn(pl, t);
            if (k >= 0) {
                PlayTrack(pl, k, false);
                return;
            }
        }
    }
    int pl = g.playingPl >= 0 ? g.playingPl : g.active;
    int n = (int)g.playlists[pl].items.size();
    if (n == 0) return;
    int cur = TokenIndex(g.playingToken, pl);
    if (cur < 0) cur = IndexIn(pl, g.playing);
    bool wrap = g.repeat != Repeat::Off;
    PlayTrack(pl, cur <= 0 ? (wrap ? n - 1 : 0) : cur - 1, false);
}

// Plays the track that was stopped from where it was: in its playlist if it's still there,
// else on its own.
bool Resume() {
    TrackId id = g.resumeTrack;
    if (!g.resumeOn || g.playing != kNone || id == kNone || id >= g_lib.Count()) return false;
    double pos = g.resumePos;
    int pl = g.resumePl, k = IndexIn(pl, id);
    if (k < 0) {
        pl = FindPlaylist(PlaylistKind::NowPlaying);
        k = IndexIn(pl, id);
    }
    if (k >= 0) PlayTrack(pl, k, true);
    else PlayList({id}, 0);
    g.resumeToken = pos > 1 && pos < g_lib[id].Duration() - 2 ? g.decodingToken : 0;
    g.resumePos = pos;
    return true;
}

// Stopped with nothing selected, Play continues where playback stopped.
static bool ResumeInsteadOfSelection() {
    return g.playing == kNone && SelectedTracks().empty() && Resume();
}

void PlayButton() {
    if (RadioPlaying()) { TogglePause(); return; }
    if (g.playing != kNone) {
        if (g.engine.Paused()) g.engine.SetPaused(false);
        else g.engine.Seek(0);
    } else if (RadioActive()) {
        RadioPlaySelected();
    } else if (!ResumeInsteadOfSelection()) {
        PlayFocusedOrFirst();
    }
    InvalidateRect(g.list, nullptr, FALSE);
    LibViewInvalidate();
}

void TogglePause() {
    if (RadioPlaying()) {
        g.engine.SetPaused(!g.engine.Paused());
        UpdateStatus();
        return;
    }
    if (g.playing == kNone) {
        if (RadioActive()) { RadioPlaySelected(); return; }
        if (!ResumeInsteadOfSelection()) PlayFocusedOrFirst();
        return;
    }
    g.engine.SetPaused(!g.engine.Paused());
    InvalidateRect(g.list, nullptr, FALSE);
    LibViewInvalidate();
    UpdateStatus();
}

// The slider is 0..100; the engine gets the square of it, which tracks perceived loudness better
// than a linear ramp would. Zero remembers the last audible level so unmuting can restore it.
void SetVolumePercent(int percent) {
    int previous = g.volumePercent, audible = g.lastAudibleVolume;
    g.volumePercent = std::clamp(percent, 0, 100);
    if (g.volumePercent > 0) g.lastAudibleVolume = g.volumePercent;
    if (previous != g.volumePercent) IniWrite(L"Player", L"Volume", std::to_wstring(g.volumePercent));
    if (audible != g.lastAudibleVolume) IniWrite(L"Player", L"LastAudibleVolume", std::to_wstring(g.lastAudibleVolume));
    double v = g.volumePercent / 100.0;
    g.engine.SetVolume((float)(v * v));  // perceptual curve
    SendMessageW(g.volume, TBM_SETPOS, TRUE, g.volumePercent);
    SetTextIfChanged(g.speaker, g.volumePercent == 0 ? L"\U0001F507" : L"\U0001F50A");
}

void ToggleMute() {
    SetVolumePercent(g.volumePercent == 0 ? g.lastAudibleVolume : 0);
}

void ApplyEffects() {
    g.engine.SetReverb(g.reverb);
    g.engine.SetSmartVolume(g.loudnessComp, g.nightMode);
}

// Neither is saved: a podcast's 1.5x tempo shouldn't still be on for tomorrow's album.
void SetSpeed(double speed) {
    g.speed = speed;
    g.engine.SetSpeed(speed);
    ShowMessage(speed == 1 ? L"Normal speed." : L"Playing at " + RateText() + L" (the pitch follows).");
    UpdateStatus();
}

void SetTempo(double tempo) {
    g.tempo = tempo;
    g.engine.SetTempo(tempo);
    ShowMessage(tempo == 1 ? L"Normal tempo." : L"Playing at " + RateText() + L" (the pitch stays).");
    UpdateStatus();
}

std::wstring RateText() {
    if (g.speed == 1 && g.tempo == 1) return L"";
    wchar_t buf[48];
    if (g.speed != 1 && g.tempo != 1) swprintf_s(buf, L"%gx speed, %gx tempo", g.speed, g.tempo);
    else swprintf_s(buf, L"%gx %ls", g.speed != 1 ? g.speed : g.tempo, g.speed != 1 ? L"speed" : L"tempo");
    return buf;
}

void RevealPlayingTrack() {
    if (g.playing == kNone) return;
    if (IndexIn(g.playingPl, g.playing) < 0) {
        ShowMessage(L"The playing track is no longer in its playlist.");
        return;
    }
    if (g.active != g.playingPl) ActivatePlaylist(g.playingPl);
    auto findRow = [] {
        for (int pos = 0; pos < (int)g.view.size(); ++pos)
            if (Active().items[g.view[pos]] == g.playing &&
                (!g.playingToken || (int)g.view[pos] == TokenIndex(g.playingToken, g.playingPl))) return RowOfViewPos(pos);
        return -1;
    };
    if (findRow() < 0) { SetWindowTextW(g.search, L""); FlushSearch(); }
    int row = findRow();
    if (row < 0) return;
    ClearSelection();
    ListView_SetItemState(g.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g.list, row, FALSE);
    SetFocus(g.list);
}

// Sound effects follow the volume slider, well below the music, and quieter still while it
// plays. Exclusive output has the device to itself, so they stay silent there.
void PlaySfx(Sfx sound, int arg, bool evenWhenOff) {
    if ((!g.soundEffects && !evenWhenOff) || g.exclusive) return;
    bool ducked = g.playing != kNone && !g.engine.Paused();
    double v = g.volumePercent / 100.0;
    if (PlayPackThemeSound(sound, (float)(0.3 * v * v * (ducked ? 0.6 : 1)))) return;
    if (ThemeInfo(g.themeMode).gameSounds && PlayGameSound(sound, (float)(0.3 * v * v * (ducked ? 0.6 : 1)))) return;
    sfx::Play(sound, arg, ducked, (float)(0.4 * v * v * (ducked ? 0.6 : 1)));
}

// The engine may not know a length (some streams); fall back to the library's value.
double CurrentDuration() {
    if (g.playing == kNone) return 0;
    double d = g.engine.Duration();
    return d > 0 ? d : g_lib[g.playing].Duration();
}

// The engine handles a seek on its own thread, so Position() still reports the old spot for a
// few milliseconds afterwards. UpdateStatus would take that for the truth and snap the seek bar
// back there for a tick, which reads as the click "not taking". The bar is held at the requested
// position until the engine agrees, or half a second passes, so a seek that never lands can't
// pin the bar away from where playback actually is.
void RequestSeek(double seconds) {
    if (RadioPlaying()) return;
    if (g.playing != kNone) {
        PlayItem item = MakeItem(g.playingToken, g.playing);
        if (item.loopEnd > item.loopStart)
            seconds = std::clamp(seconds, item.loopStart, std::max(item.loopStart, item.loopEnd - 0.001));
        g.engine.Seek(seconds, &item);
    } else g.engine.Seek(seconds);
    s_seekTo = seconds;
    s_seekUntil = GetTickCount64() + 500;
}

// Use the pending target for repeated key presses while the engine is still seeking.
void SeekBy(double seconds) {
    if (g.playing == kNone) return;
    double pos = g.engine.Position();
    if (s_seekTo >= 0 && GetTickCount64() < s_seekUntil && std::fabs(pos - s_seekTo) > 0.35) pos = s_seekTo;
    double target = std::max(0.0, pos + seconds);
    double duration = CurrentDuration();
    if (duration > 0) target = std::min(target, duration);
    RequestSeek(target);
    UpdateStatus();
    if (g.seek) InvalidateRect(g.seek, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Now playing, status bar

void UpdateNowPlaying() {
    if (RadioPlaying()) {
        SetTextIfChanged(g.npTitle, RadioStationName());
        std::wstring title = RadioSongTitle();
        SetTextIfChanged(g.npSub, title.empty() ? L"Live radio" : title);
        SetTextIfChanged(g.hwnd, RadioStationName() + L" - cordinal");
    } else if (g.playing != kNone) {
        auto lookup = Lookup(g.playing, true);
        SetTextIfChanged(g.npTitle, FormatTitle(g.fmtTitle, lookup));
        std::wstring sub = FormatTitle(g.fmtSub, lookup), chapter = CurrentChapterText();
        if (!chapter.empty()) sub += L"  \x00B7  " + chapter;
        SetTextIfChanged(g.npSub, sub);
        SetTextIfChanged(g.hwnd, ApolloMode() ? ApolloWindowTitle() : FormatTitle(g.fmtWindow, lookup));
    } else if (g.resumeOn && g.resumeTrack != kNone && g.resumeTrack < g_lib.Count()) {
        // Stopped: offer to continue where playback stopped.
        auto lookup = Lookup(g.resumeTrack, false);
        SetTextIfChanged(g.npTitle, L"Continue: " + FormatTitle(L"%title%", lookup));
        std::wstring sub = FormatTitle(L"[%artist%]", lookup);
        if (std::wstring album = FormatTitle(L"[%album%]", lookup); !album.empty()) sub += (sub.empty() ? L"" : L"  \x00B7  ") + album;
        sub += (sub.empty() ? L"" : L"  \x00B7  ") + FormatDuration(g.resumePos) + L" / " + FormatTitle(L"[%length%]", lookup);
        if (g.layout.hints) sub += L"   \x25B6 Click or press Space to continue";
        SetTextIfChanged(g.npSub, sub);
        SetTextIfChanged(g.hwnd, kAppName);
    } else {
        SetTextIfChanged(g.npTitle, kAppName);
        // How to start is a hint; what to do about an empty library or playlist always shows.
        const wchar_t* hint = g.layout.hints ? L"Double-click a track to start playing." : L"";
        if (Active().items.empty())
            hint = Active().kind == PlaylistKind::Library && g.libraryFolders.empty()
                       ? L"Your library is empty: add your music folder with Library \x25B8 Library folders to keep it "
                         L"up to date, or drop folders here to add what they hold now."
                   : Active().Editable() ? L"Drop music here, or press Ctrl+O to add files."
                                         : L"Nothing here yet.";
        SetTextIfChanged(g.npSub, hint);
        SetTextIfChanged(g.hwnd, kAppName);
    }
    LoadArtFor(RadioPlaying() ? kNone : g.playing != kNone ? g.playing : g.resumeOn ? g.resumeTrack : kNone);
    PlatformTrackChanged();
    WaveformTrackChanged();
    RegionTrackChanged();
    LyricsTrackChanged();
    MiniPlayerTick();
    PanelsTick();
}

void UpdateStatus() {
    static bool showedPause = false;  // the play button's glyph: redraw only when it changes
    bool showPause = (g.playing != kNone || RadioPlaying()) && !g.engine.Paused();
    SetTextIfChanged(g.buttons[1], showPause ? L"Pause" : L"Play");
    if (showPause != showedPause && g.buttons[1]) InvalidateRect(g.buttons[1], nullptr, FALSE);
    showedPause = showPause;
    UpdateTaskbar();
    AnimatePointsChip();
    MiniPlayerTick();
    PlatformTick();
    FoobarTick();
    double pos = g.engine.Position(), dur = CurrentDuration();
    if (s_seekTo >= 0) {
        if (std::fabs(pos - s_seekTo) <= 0.35 || GetTickCount64() >= s_seekUntil) s_seekTo = -1;
        else pos = s_seekTo;  // the seek is still queued: show where it is heading
    }
    bool active = g.playing != kNone;
    if (active) {
        if (!g.seeking) {
            const LPARAM target = dur > 0 ? (LPARAM)(pos / dur * 1000) : 0;
            if (SendMessageW(g.seek, TBM_GETPOS, 0, 0) != target) SendMessageW(g.seek, TBM_SETPOS, FALSE, target);
        }
        if (!g.seeking) ShowPlaybackTime(pos, dur);
    } else if (RadioPlaying()) {
        if (SendMessageW(g.seek, TBM_GETPOS, 0, 0)) SendMessageW(g.seek, TBM_SETPOS, FALSE, 0);
        SetTextIfChanged(g.elapsed, L"LIVE");
        SetTextIfChanged(g.time, L"");
    } else {
        if (SendMessageW(g.seek, TBM_GETPOS, 0, 0)) SendMessageW(g.seek, TBM_SETPOS, FALSE, 0);
        SetTextIfChanged(g.elapsed, L"");
        SetTextIfChanged(g.time, L"");
    }
    const bool seekEnabled = active && dur > 0;
    if (!!IsWindowEnabled(g.seek) != seekEnabled) EnableWindow(g.seek, seekEnabled);
    WaveformTick();  // the waveform's color follows play/pause and the warnings
    if (ApolloMode()) {  // Apollo's status window and two-part status bar
        ApolloTick();
        ApolloStatus();
        return;
    }

    if (GetTickCount64() >= g.messageUntil) {
        std::wstring text;
        if (RadioPlaying()) text = g.engine.Paused() ? L"Radio paused." : L"Live radio: " + RadioStationName();
        else if (FoobarMode())  // foobar2000's: the playing file's format and time
            text = active ? (g.engine.Paused() ? L"Paused | " : L"") +
                                FormatTitle(L"[%codec%][ | %bitrate% kbps][ | %samplerate% Hz][ | %channels%] | %playback_time%[ / %length%]",
                                            Lookup(g.playing, true))
                          : L"Playback stopped.";
        else if (active)
            text = (g.engine.Paused() ? L"Paused | " : L"") + FormatTitle(g.fmtStatus, Lookup(g.playing, true));
        else
            text = L"Stopped.";
        if (g.stopAfterCurrent) text += L"  (stop after current)";
        if (!g.queue.empty()) text += L"  \x2022  " + std::to_wstring(g.queue.size()) + L" queued";
        if (active && !RateText().empty()) text += L"  \x2022  " + RateText();
        SetStatusPart(0, text);
    }

    std::wstring dsp;
    std::vector<std::wstring> statusItems;
    if (BackupBusy()) {
        dsp = BackupStatus();
    } else if (g.importRunning) {
        dsp = L"Adding audio files...";
    } else if (!g.tagPending.empty()) {
        dsp = L"Reading metadata: " + std::to_wstring(g.tagPending.size()) + L" remaining";
    } else if (OrganizeBusy()) {
        dsp = L"Organizing files...";
    } else if (TagWriteBusy()) {
        dsp = TagWriteStatus();
    } else if (!HelperToolRunning().empty()) {
        dsp = HelperToolStatus();
    } else if (g.rgRunning && !g.rgAuto) {
        int done = g.rgDone.load(), total = g.rgTotal.load();
        dsp = L"Scanning ReplayGain " + std::to_wstring(done) + L"/" + std::to_wstring(total);
        if (done > 0 && total > done && g.rgStart) {
            double elapsed = (GetTickCount64() - g.rgStart) / 1000.0;
            if (elapsed > 0.1) dsp += L"  \x2022  " + FormatDuration((total - done) * elapsed / done) + L" left";
        }
    } else {
        std::wstring rg = L"RG: ";
        if (!g.rgOn) {
            rg += L"off";
        } else if (active && g.playing < g_lib.Count()) {
            const Track& t = g_lib[g.playing];
            wchar_t buf[32];
            auto token = g.tokens.find(g.playingToken);
            const bool estimated = token != g.tokens.end() && token->second.estimated;
            const float gain = g.engine.PlayingGain();
            swprintf_s(buf, L"%+.1f dB%ls", 20 * std::log10(std::max(gain, 1e-6f)), estimated ? L" (estimate)" : L"");
            rg += t.rgTrackGain == kNoGain && t.rgAlbumGain == kNoGain && !estimated ? L"not measured yet" : buf;
        } else {
            rg += L"on";
        }
        if (g.rgRunning && g.rgAuto)
            rg += L"  \x2022  measuring " + std::to_wstring(g.rgDone.load()) + L"/" + std::to_wstring(g.rgTotal.load());
        // Output: shared or exclusive, the device's rate and format, and whether it's bit-perfect. A
        // track a helper program decodes says so, and claims no bit-perfect path: cordinal hasn't
        // checked that program's decoding as it checks its own.
        Engine::OutputInfo o = g.engine.Output();
        const std::wstring helper = active ? HelperDecoding(g.playing) : L"";
        wchar_t out[96];
        if (!o.ready)
            swprintf_s(out, L"%ls (idle)", g.exclusive ? L"Exclusive" : L"Shared");
        else if (o.exclusive)
            swprintf_s(out, L"Exclusive %g kHz %d-bit%ls%ls", o.rate / 1000.0, o.bits, o.floatFormat ? L" float" : L"",
                       active && o.bitPerfect && helper.empty() ? L" \x2022 bit-perfect" : L"");
        else
            swprintf_s(out, L"%ls %g kHz", g.exclusive ? L"Shared (exclusive unavailable)" : L"Shared", o.rate / 1000.0);
        // Only shown when the layout asks for them; work in progress above always shows.
        if (g.layout.statusDetails) {
            statusItems = {rg, g.eq.crossfeed ? (g.eq.enabled ? L"EQ + crossfeed" : L"Crossfeed") :
                              g.eq.enabled ? L"EQ on" : L"EQ off", out + (helper.empty() ? L"" : L" \x2022 decoded by " + helper)};
            dsp = statusItems[0] + L"  |  " + statusItems[1] + L"  |  " + statusItems[2];
        }
    }
    if (statusItems != g.statusItems) {
        g.statusItems = statusItems;
        g.statusParts[1].clear();  // redraw even if the joined text happens to match
    }
    SetStatusPart(1, dsp);

    if (g.statsDirty) {
        g.statsDirty = false;
        auto selected = SelectedTracks();
        double total = 0;
        auto& items = Active().items;
        std::wstring text;
        if (!selected.empty()) {
            for (TrackId id : selected) total += g_lib[id].Duration();
            text = std::to_wstring(selected.size()) + L" selected  \x2022  " + FormatTotalDuration(total);
        } else {
            for (uint32_t i : g.view) total += g_lib[items[i]].Duration();
            text = std::to_wstring(g.view.size()) + (g.filter.empty() ? L"" : L" of " + std::to_wstring(items.size())) +
                   L" tracks  \x2022  " + FormatTotalDuration(total);
        }
        SetStatusPart(2, text);
    }
    if (RadioActive()) SetStatusPart(2, std::to_wstring(RadioCount()) + L" stations");
}

// ---------------------------------------------------------------------------
// The status bar's middle part: each item is a button. ReplayGain switches on and off,
// EQ opens the equalizer, and the output switches between shared and exclusive.

void DrawStatusItems(HDC dc, RECT r) {
    HWND tips = g.tips;
    for (int i = 0; i < 3; ++i) g.statusItemRects[i] = {};
    int x = r.left;
    for (int i = 0; i < (int)g.statusItems.size() && i < 3; ++i) {
        const std::wstring& s = g.statusItems[i];
        SIZE ext{};
        GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &ext);
        RECT tr{x, r.top, std::min<int>(x + ext.cx, r.right), r.bottom};
        DrawTextW(dc, s.c_str(), (int)s.size(), &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        g.statusItemRects[i] = {tr.left - S(3), r.top - S(2), tr.right + S(3), r.bottom + S(2)};
        if (tips) {
            TTTOOLINFOW ti{sizeof(ti)};
            ti.hwnd = g.status;
            ti.uId = 100 + i;
            ti.rect = g.statusItemRects[i];
            SendMessageW(tips, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
        x = tr.right;
        if (i + 1 < (int)g.statusItems.size()) {
            RECT sep{x, r.top, r.right, r.bottom};
            DrawTextW(dc, L"  |  ", -1, &sep, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            GetTextExtentPoint32W(dc, L"  |  ", 5, &ext);
            x += ext.cx;
        }
        if (x >= r.right) break;
    }
}

void ClickStatusItem(int item) {
    switch (item) {
        case 0:
            OnCommand(IDM_RG_ENABLE, 0, nullptr);
            break;
        case 1: ShowEqualizer(); break;
        case 2: OnCommand(g.exclusive ? IDM_OUTPUT_SHARED : IDM_OUTPUT_EXCLUSIVE, 0, nullptr); break;
    }
    UpdateStatus();
}

static int StatusItemAt(POINT pt) {
    for (int i = 0; i < (int)g.statusItems.size() && i < 3; ++i)
        if (PtInRect(&g.statusItemRects[i], pt)) return i;
    return -1;
}

LRESULT CALLBACK StatusSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // the paint below covers the whole bar
        case WM_PAINT: {
            // Painted off screen through the control's own paint (its parts are owner drawn),
            // then the light 3D size grip that comctl32 adds for a resizable parent is covered:
            // the window's corner resizes it anyway.
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1, (int)rc.right), std::max(1, (int)rc.bottom));
            HGDIOBJ oldBmp = SelectObject(mem, bmp);
            FillSolid(mem, rc, SurfaceColor());
            DefSubclassProc(h, WM_PAINT, (WPARAM)mem, 0);
            RECT last{};
            int parts = (int)SendMessageW(h, SB_GETPARTS, 0, 0);
            if (parts > 0 && SendMessageW(h, SB_GETRECT, parts - 1, (LPARAM)&last) && last.right < rc.right)
                FillSolid(mem, {last.right - S(2), rc.top, rc.right, rc.bottom}, SurfaceColor());
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (StatusItemAt(pt) >= 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONUP: {
            int item = StatusItemAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (item >= 0) {
                ClickStatusItem(item);
                return 0;
            }
            break;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, StatusSubclass, 0);
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void ShowPlaybackTime(double pos, double duration) {
    SetTextIfChanged(g.elapsed, FormatDuration(pos));
    SetTextIfChanged(g.time, g.timeRemaining ? L"-" + FormatDuration(std::max(0.0, duration - pos)) : FormatDuration(duration));
}

// The now playing cover, filling its square; without art, the album's own tile (the title's for a
// single), or a plain note before anything has played.
void PaintArt(HDC dc) {
    RECT r = g.artRect;
    if (!g.art.Empty()) {
        DrawCoverArt(dc, r, g.art);
    } else {
        TrackId id = g.playing != kNone ? g.playing : g.resumeOn ? g.resumeTrack : kNone;
        std::wstring name;
        if (id != kNone && id < g_lib.Count()) {
            name = FormatTitle(L"[%album%]", Lookup(id, false));
            if (name.empty()) name = FormatTitle(L"[%title%]", Lookup(id, false));
        }
        DrawCoverPlaceholder(dc, r, name);
    }
    DrawCoverFrame(dc, r);
}

// ---------------------------------------------------------------------------
// Play statistics. A track counts as played once it has actually been heard for half its
// length (at most 4 minutes, the scrobbling rule); leaving it earlier by hand is a skip.

void TrackListening() {
    if (g.playing == kNone || g.engine.Paused()) return;
    double pos = g.engine.Position();
    if (g.statTrack != g.playing) {
        g.statTrack = g.playing;
        g.listened = 0;
        g.lastPos = pos;
        return;
    }
    double delta = pos - g.lastPos;
    if (delta > 0 && delta < 1.5) g.listened += delta;  // seeks jump further and don't count
    g.lastPos = pos;
}

void FinishListening(bool manual) {
    if (g.statTrack == kNone) return;
    // Credit the last partial tick; after a natural transition the position already belongs
    // to the next track, so only a manual change reads it.
    if (manual) TrackListening();
    TrackId id = g.statTrack;
    g.statTrack = kNone;
    RecordListening(id, g.listened, manual);
    g.listened = 0;
}

// Both the audio engine and paired browsers commit on the UI thread through the same rules.
void RecordListening(TrackId id, double seconds, bool manual, uint32_t when) {
    if (id >= g_lib.Count()) return;
    Track& t = g_lib[id];
    double needed = std::clamp(t.Duration() * 0.5, 1.0, 240.0);
    bool counted = seconds >= needed;
    bool skip = !counted && manual && seconds >= 0.5;
    if (!counted && !skip) return;
    uint32_t previousLastPlayed = t.lastPlayed;
    uint32_t now = when ? std::min(when, UnixNow()) : UnixNow();
    if (counted) {
        ++t.playCount;
        if (!t.firstPlayed || now < t.firstPlayed) t.firstPlayed = now;
        t.lastPlayed = std::max(t.lastPlayed, now);  // an older offline play doesn't move it back
        if (t.playCount == 100 || t.playCount == 250 || t.playCount == 500 || t.playCount % 1000 == 0) {
            std::wstring title = g_lib.Str(t.title);
            ShowMessage(std::to_wstring(t.playCount) + L"th play: " + (title.empty() ? g_lib.Str(t.file) : title) + L".");
            PlaySfx(Sfx::Milestone);
        }
    } else {
        ++t.skipCount;
    }
    LogPlay(id, now, seconds, counted, skip);  // before the award: its fatigue rule reads the log
    AnnouncePoints(AwardListen(id, seconds, counted, skip, previousLastPlayed));
    if (counted) PlatformCountedPlay(id, now);
    g_lib.Touch();
    g.dirty = true;
}

// A WM_ENGINE event from the engine, on the UI thread. Events for tokens issued before the last
// Stop are stale and dropped. TrackStarted is where `g.playing` is updated; Ended clears it.
void OnEngineEvent(EngineEvent e, unsigned long long token) {
    if (RadioEngineEvent(e, token)) return;
    std::wstring problem;
    if (e == EngineEvent::Error || e == EngineEvent::Damaged) problem = g.engine.TakeError(token);
    if (token && token < g.firstValidToken) return;
    auto it = g.tokens.find(token);
    // A queued track leaves the queue once the engine starts on it (decoding comes before it's
    // heard, and asking what follows it must already see the rest of the queue).
    if ((e == EngineEvent::NeedNext || e == EngineEvent::Error) && it != g.tokens.end() && it->second.queued &&
        !g.queue.empty() && g.queue.front() == it->second.id) {
        g.queue.pop_front();
        it->second.queued = false;
        InvalidateRect(g.list, nullptr, FALSE);
    }
    switch (e) {
        case EngineEvent::NeedNext:
            g.decodingToken = token;
            AnswerNext(token);
            break;
        case EngineEvent::Error: {
            std::wstring msg = std::move(problem);
            if (it != g.tokens.end()) g_lib[it->second.id].flags |= Track::kFailed;
            ShowMessage(msg.empty() ? L"Couldn't play the file." : msg);
            InvalidateRect(g.list, nullptr, FALSE);
            LibViewInvalidate();
            int total = it != g.tokens.end() && it->second.pl >= 0 ? (int)g.playlists[it->second.pl].items.size() : 1;
            g.decodingToken = token;
            int pl = 0;
            int k = ++g.consecutiveErrors >= std::clamp(total, 1, 50) ? -1 : ComputeNext(token, true, pl);
            if (k >= 0) {
                TrackId id = g.playlists[pl].items[k];
                PlayItem item = MakeItem(NewToken(id, pl, (uint32_t)k), id);
                g.engine.SetNext(token, &item);
            } else {
                g.engine.SetNext(token, nullptr);
            }
            break;
        }
        case EngineEvent::Damaged: {
            if (it == g.tokens.end() || it->second.id >= g_lib.Count()) break;
            Track& t = g_lib[it->second.id];
            t.flags |= Track::kFailed;
            ShowMessage(g_lib.Str(t.title ? t.title : t.file) + L": " +
                        (problem.empty() ? L"Playback failed. The audio is damaged or unreadable." : problem));
            InvalidateRect(g.list, nullptr, FALSE);
            LibViewInvalidate();
            break;
        }
        case EngineEvent::TrackStarted:
            if (it == g.tokens.end()) break;
            if (token == g.playingToken) break;  // a seek/reopened section keeps its listening session
            CancelSeekPreview();  // a previewed seek belongs to the track that is leaving
            if (g.radio && g.playing != kNone && g.statTrack == g.playing) {
                g.history.push_back(g.playing);
                if (g.history.size() > 256) g.history.erase(g.history.begin());
            }
            FinishListening(false);
            g.consecutiveErrors = 0;
            g.playingToken = token;
            g.playing = it->second.id;
            g.playingPl = it->second.pl;
            if (token == g.resumeToken) g.engine.Seek(g.resumePos);
            g.resumeToken = 0;
            g.resumeTrack = kNone;
            UpdateNowPlaying();
            UpdateStatus();
            // Once a session: the properties line keeps saying it for each file.
            if (static bool told = false; !told && IgnoresWavPackCorrection(g_lib.Path(g.playing))) {
                told = true;
                ShowMessage(L"Playing the lossy part of a hybrid WavPack file: its .wvc correction file isn't "
                            L"used yet.");
            }
            InvalidateRect(g.list, nullptr, FALSE);
            LibViewInvalidate();
            InvalidateRect(g.tabs, nullptr, FALSE);
            MaybeAutoMeasure(true);  // the next tracks' albums before anything else
            break;
        case EngineEvent::Estimated: {
            // Applied once per token, as a slow glide, and kept until the track ends: the scan's
            // real measurement takes over the next time it plays.
            const double lufs = g.engine.TakeEstimate(token);
            if (it == g.tokens.end() || it->second.estimated || !g.rgOn || !std::isfinite(lufs)) break;
            it->second.estimated = true;
            it->second.estimate = lufs;
            g.engine.SetGain(token, RgGainForEstimate(it->second.id, lufs), true);
            if (token == g.playingToken) UpdateStatus();
            break;
        }
        case EngineEvent::Ended:
            g.resumeTrack = kNone;  // the playlist is over; nothing to continue
            CancelSeekPreview();
            FinishListening(false);
            g.playing = kNone;
            g.playingToken = 0;
            g.playingPl = -1;
            g.stopAfterCurrent = false;
            UpdateNowPlaying();
            UpdateStatus();
            InvalidateRect(g.list, nullptr, FALSE);
            LibViewInvalidate();
            break;
    }
}

}  // namespace app
