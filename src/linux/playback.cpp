// Playback on Linux: what plays next (Now Playing, shuffle, repeat), the engine's gapless
// hand-off (NeedNext -> SetNext), each track's ReplayGain and the equalizer, and play and skip
// counts. Playing from the Library or an auto-playlist copies what is shown into Now Playing
// first, as on Windows.
#include "app.h"
#include "radio_ui.h"
#include "ui.h"

#include "cue.h"
#include "game_music.h"
#include "peaks.h"
#include "playlog.h"
#include "points.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace lx {

namespace {

// What the engine holds under each token: the Now Playing index and track, and the quick loudness
// estimate standing in for its ReplayGain until it is measured.
struct TokenInfo {
    int index;
    TrackId id;
    int following = -1;  // next surviving occurrence when this item was removed
    bool estimated = false;
    double estimate = 0;
    int albumNext = -2;
    int radioNext = -2;
    TrackId radioNextId = kNone;
    Repeat nextRepeat = Repeat::Off;
    bool queued = false;  // played from the playback queue; `index` is then the list position it broke into
    bool popped = false;  // ... and has left the queue
};
std::unordered_map<unsigned long long, TokenInfo> g_tokens;
unsigned long long g_nextToken = 1;
std::deque<TrackId> g_radioHistory;

int NowPlayingTab() {
    for (size_t i = 0; i < g.playlists.size(); ++i)
        if (g.playlists[i].kind == PlaylistKind::NowPlaying) return (int)i;
    return -1;
}

std::vector<TrackId>& Queue() {
    static std::vector<TrackId> none;
    int np = NowPlayingTab();
    return np >= 0 ? g.playlists[np].items : none;
}

unsigned long long NewToken(int index, TrackId id) {
    // Forget tokens the engine can no longer refer to: everything before the audible one.
    if (g_tokens.size() > 64) std::erase_if(g_tokens, [](const auto& t) { return t.first < g.token; });
    unsigned long long token = g_nextToken++;
    g_tokens[token] = {index, id};
    return token;
}

// A track from the playback queue: `anchor` is where in Now Playing playback picks up once the queue is empty.
unsigned long long NewQueuedToken(int anchor, TrackId id) {
    unsigned long long token = NewToken(anchor, id);
    g_tokens[token].queued = true;
    return token;
}

double EqPeak() {
    unsigned rate = g.engine.Rate();
    return replaygain::EqPeakDb(g.settings.eq, rate ? rate : 48000);
}

PlayItem MakeItem(unsigned long long token, TrackId id) {
    PlayItem item{token, g_lib.Path(id), GainFor(id), g_lib[id].Duration()};
    const Track& t = g_lib[id];
    item.estimate = g.settings.rg.on && g.settings.rgEstimate && t.rgTrackGain == kNoGain && t.rgAlbumGain == kNoGain;
    if (IsSubtrack(item.path) && !IsGameMusicFile(item.path)) {  // a CUE track: its slice of the file
        double start = 0, end = 0;
        if (SubtrackRange(item.path, start, end)) {
            item.start = start;
            item.end = end;
            item.duration = end > 0 ? end - start : item.duration;
        } else {
            item.start = -1;
        }
    }
    PlaybackSection section = SectionOf(id);
    if (section.on && section.end - section.start > 0.05 && item.start >= 0 &&
        (item.duration <= 0 || section.end <= item.duration)) {
        item.loopStart = section.start;
        item.loopEnd = section.end;
    }
    return item;
}

void Reshuffle(int first) {
    if (g.settings.shuffle == kShuffleAlbums) {
        ShuffleAlbums(g.shuffleOrder, Queue(), g.rng, first);
        g.shufflePos = first >= 0 ? 0 : -1;
        return;
    }
    int n = (int)Queue().size();
    if (g.settings.skipAwareShuffle) {
        g.shuffleOrder.clear();
        g.shuffleOrder.reserve(n);
        for (int i = 0; i < n; ++i) if (i != first) g.shuffleOrder.push_back((uint32_t)i);
        ShuffleByListening(g.shuffleOrder, Queue(), g.rng);
        std::reverse(g.shuffleOrder.begin(), g.shuffleOrder.end());
        if (first >= 0 && first < n) g.shuffleOrder.insert(g.shuffleOrder.begin(), (uint32_t)first);
        g.shufflePos = first >= 0 && first < n ? 0 : -1;
        return;
    }
    g.shuffleOrder.resize(n);
    for (int i = 0; i < n; ++i) g.shuffleOrder[i] = i;
    std::shuffle(g.shuffleOrder.begin(), g.shuffleOrder.end(), g.rng);
    if (first >= 0) {
        auto it = std::find(g.shuffleOrder.begin(), g.shuffleOrder.end(), first);
        if (it != g.shuffleOrder.end()) std::iter_swap(g.shuffleOrder.begin(), it);
    }
    g.shufflePos = first >= 0 ? 0 : -1;
}

// The queue index after `index` (or -1 at the end), following shuffle and repeat. `manual` is a
// press of Next: Repeat One moves on then, as on Windows.
int After(int index, bool manual) {
    int n = (int)Queue().size();
    if (n == 0) return -1;
    if (g.settings.repeat == Repeat::One && !manual) return index;
    if (g.settings.shuffle) {
        if ((int)g.shuffleOrder.size() != n) Reshuffle(index);
        auto it = std::find(g.shuffleOrder.begin(), g.shuffleOrder.end(), index);
        size_t pos = it == g.shuffleOrder.end() ? 0 : (size_t)(it - g.shuffleOrder.begin()) + 1;
        if (pos < g.shuffleOrder.size()) return g.shuffleOrder[pos];
        if (g.settings.repeat != Repeat::Off) {
            Reshuffle(-1);
            return g.shuffleOrder[0];
        }
        return -1;
    }
    if (index + 1 < n) return index + 1;
    return g.settings.repeat != Repeat::Off ? 0 : -1;
}

// Album repeat may build a new pass. Keep the answer on its request token so gapless lookahead,
// a refreshed answer and manual Next all agree even after the old index moved in the new pass.
int Following(unsigned long long token, bool manual) {
    auto it = g_tokens.find(token);
    if (it == g_tokens.end()) return -1;
    auto& info = it->second;
    if (info.index < 0 && !g.settings.shuffle && !g.settings.radio) {
        if (info.following >= 0 && info.following < (int)Queue().size()) return info.following;
        return !Queue().empty() && g.settings.repeat != Repeat::Off ? 0 : -1;
    }
    if (g.settings.radio && !(g.settings.repeat == Repeat::One && !manual)) {
        auto& items = Queue();
        if (info.radioNext >= 0 && info.radioNext < (int)items.size() && items[info.radioNext] == info.radioNextId)
            return info.radioNext;
        auto weights = ListeningWeights(items, info.id);
        double total = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i] == info.id || !g_lib.Contains(items[i]) || g_lib[items[i]].Failed()) weights[i] = 0;
            auto recent = std::min<size_t>(5, g_radioHistory.size());
            if (std::find(g_radioHistory.rbegin(), g_radioHistory.rbegin() + recent, items[i]) != g_radioHistory.rbegin() + recent)
                weights[i] *= 0.15;
            total += weights[i];
        }
        int pick = -1;
        if (total > 0) {
            double draw = std::generate_canonical<double, 53>(g.rng) * total;
            for (size_t i = 0; i < items.size(); ++i)
                if (weights[i] > 0 && (draw -= weights[i]) < 0) { pick = (int)i; break; }
        } else if (info.index >= 0 && info.index < (int)items.size() &&
                   items[info.index] == info.id && g_lib.Contains(info.id) && !g_lib[info.id].Failed()) {
            pick = info.index;
        }
        info.radioNext = pick;
        info.radioNextId = pick >= 0 ? items[pick] : kNone;
        return pick;
    }
    if (g.settings.shuffle != kShuffleAlbums || (g.settings.repeat == Repeat::One && !manual))
        return After(info.index, manual);
    if (info.albumNext == -2 || info.nextRepeat != g.settings.repeat || g.shuffleOrder.empty()) {
        info.albumNext = After(info.index, manual);
        info.nextRepeat = g.settings.repeat;
    }
    return info.albumNext;
}

// What plays after `token`: the playback queue's head, else the list from where the queue broke into it.
struct Step {
    TrackId id = kNone;
    int index = -1;       // the Now Playing position, or for a queued track the one it broke into
    bool queued = false;
};

Step StepAfter(unsigned long long token, bool manual) {
    auto it = g_tokens.find(token);
    if (it == g_tokens.end()) return {};
    const TokenInfo& info = it->second;
    if (info.queued && g.settings.repeat == Repeat::One && !manual) return {info.id, info.index, true};
    // A queued track still at the head of the queue is the one playing.
    size_t from = info.queued && !info.popped && !g.queue.empty() && g.queue.front() == info.id ? 1 : 0;
    for (size_t i = from; i < g.queue.size(); ++i)
        if (g_lib.Contains(g.queue[i])) return {g.queue[i], info.index, true};
    int next = Following(token, manual);
    auto& items = Queue();
    return next >= 0 && next < (int)items.size() ? Step{items[next], next, false} : Step{};
}

// Tells the engine what follows `token`, for its gapless hand-off.
void AnswerNext(unsigned long long token, bool failed) {
    auto it = g_tokens.find(token);
    Step step;
    if (it != g_tokens.end() && !(g.stopAfterCurrent && (token == g.token || !g.token))) step = StepAfter(token, false);
    // Don't loop forever over a list where nothing plays.
    if (failed && it != g_tokens.end() && step.id == it->second.id && step.index == it->second.index && step.queued == it->second.queued)
        step = {};
    if (step.id == kNone) {
        g.engine.SetNext(token, nullptr);
        PrefetchWaveform(kNone);
        return;
    }
    unsigned long long next = step.queued ? NewQueuedToken(step.index, step.id) : NewToken(step.index, step.id);
    PlayItem item = MakeItem(next, step.id);
    g.engine.SetNext(token, &item);
    PrefetchWaveform(step.id);
}

// The queued track that has been playing leaves the queue once the engine asks what follows it (decoding
// comes before it is heard, and what follows must see the rest of the queue).
void LeaveQueue(TokenInfo& info) {
    if (info.queued && !info.popped && !g.queue.empty() && g.queue.front() == info.id) {
        g.queue.pop_front();
        info.popped = true;
        g.redraw = true;
    }
}

int Before(int index) {
    int n = (int)Queue().size();
    if (n == 0) return -1;
    if (g.settings.shuffle && !g.shuffleOrder.empty()) {
        auto it = std::find(g.shuffleOrder.begin(), g.shuffleOrder.end(), index);
        if (it != g.shuffleOrder.end() && it != g.shuffleOrder.begin()) return *(it - 1);
        return index;
    }
    return index > 0 ? index - 1 : (g.settings.repeat != Repeat::Off ? n - 1 : 0);
}

void TrackListening() {
    if (g.playing == kNone || g.engine.Paused()) return;
    double pos = g.engine.Position();
    double step = pos - g.lastPosition;
    if (step > 0 && step < 1.5) g.listened += step;  // a seek doesn't count as listening
    g.lastPosition = pos;
}

void LeaveTrack(bool manual) {
    if (g.playing == kNone) return;
    if (manual) TrackListening();
    if (g.settings.radio && g.listened > 0) {
        g_radioHistory.push_back(g.playing);
        if (g_radioHistory.size() > 256) g_radioHistory.pop_front();
    }
    RecordListening(g.playing, g.listened, manual);
    g.listened = 0;
}

}  // namespace

PlaybackSection SectionOf(TrackId id) {
    if (!g_lib.Contains(id)) return {};
    const Track& t = g_lib[id];
    auto key = PeaksKeyName(g_lib.Path(id), t.fileSize, t.fileTime);
    auto found = g.settings.sections.find(key);
    return found == g.settings.sections.end() ? PlaybackSection{} : found->second;
}

void SetSectionFor(TrackId id, const PlaybackSection& section) {
    if (!g_lib.Contains(id)) return;
    const Track& t = g_lib[id];
    auto key = PeaksKeyName(g_lib.Path(id), t.fileSize, t.fileTime);
    if (std::isfinite(section.start) && std::isfinite(section.end) && section.start >= 0 &&
        section.end <= 1e9 && section.end - section.start > 0.05)
        g.settings.sections[key] = section;
    else g.settings.sections.erase(key);
    SaveSettings();
}

// Local playback and the paired browser use the same counting, history and points rules.
void RecordListening(TrackId id, double seconds, bool manual, uint32_t when) {
    if (!g_lib.Contains(id)) return;
    Track& track = g_lib[id];
    bool counted = seconds >= std::clamp(track.Duration() * 0.5, 1.0, 240.0);
    bool skip = !counted && manual && seconds >= 0.5;
    if (!counted && !skip) return;
    uint32_t previous = track.lastPlayed;
    uint32_t now = when ? std::min(when, UnixNow()) : UnixNow();
    if (counted) {
        ++track.playCount;
        ScrobbleCounted(id, now);
        if (track.playCount == 100 || track.playCount == 250 || track.playCount == 500 || track.playCount % 1000 == 0) {
            ShowMessage(std::to_wstring(track.playCount) + L"th play: " + TrackTitle(id) + L".");
            PlaySfx(Sfx::Milestone);
        }
        if (!track.firstPlayed || now < track.firstPlayed) track.firstPlayed = now;
        track.lastPlayed = std::max(track.lastPlayed, now);
    } else {
        ++track.skipCount;
    }
    LogPlay(id, now, seconds, counted, skip);
    AnnouncePoints(AwardListen(id, seconds, counted, skip, previous));
    g_lib.Touch();
    g.libraryDirty = true;
    g.redraw = true;
}

bool IsPaused() { return g.engine.Paused(); }

// What comes after the playing track changed (the queue did): answer the engine's hand-off again.
void RefreshNext() {
    if (g.playing == kNone || !g.token) return;
    // Queue entries are removed when decoding reaches them, before they are audible. Replacing
    // that buffered audio here would silently lose an entry that the listener still expects.
    for (const auto& [token, info] : g_tokens)
        if (token > g.token && info.queued && info.popped) return;
    AnswerNext(g.token, false);
}

// Play next (the front of the queue) or Add to queue (its end): played before the playlist continues.
void QueueTracks(const std::vector<TrackId>& ids, bool next) {
    if (ids.empty()) return;
    if (next) g.queue.insert(g.queue.begin(), ids.begin(), ids.end());
    else g.queue.insert(g.queue.end(), ids.begin(), ids.end());
    PlaySfx(next ? Sfx::PlayNext : Sfx::QueueAdd, (int)g.queue.size());
    ShowMessage((next ? L"Playing next: " : L"Queued: ") + std::to_wstring(ids.size()) + (ids.size() == 1 ? L" track" : L" tracks") +
                L" (" + std::to_wstring(g.queue.size()) + L" in the queue).");
    g.redraw = true;
    if (g.playing == kNone) Next();
    else RefreshNext();
}

void ClearQueue() {
    if (g.queue.empty()) return;
    PlaySfx(Sfx::QueueClear);
    // A queued track playing now stays in it until the engine has moved on (see LeaveQueue).
    auto it = g_tokens.find(g.token);
    TrackId playing = it != g_tokens.end() && it->second.queued && !it->second.popped && !g.queue.empty() && g.queue.front() == it->second.id
                          ? it->second.id : kNone;
    g.queue.clear();
    if (playing != kNone) it->second.popped = true;
    RefreshNext();
    g.redraw = true;
}

int QueuePosition(TrackId id) {
    for (size_t i = 0; i < g.queue.size(); ++i)
        if (g.queue[i] == id) return (int)i + 1;
    return 0;
}

std::wstring TrackTitle(TrackId id) {
    const Track& t = g_lib[id];
    return t.title ? g_lib.Str(t.title) : StemOf(g_lib.Str(t.file));
}

std::wstring TrackArtist(TrackId id) {
    const Track& t = g_lib[id];
    return g_lib.Str(t.artist ? t.artist : t.albumArtist);
}

void StartRadio(const radio::Station& station) {
    Stop();
    unsigned long long token = g_nextToken++;
    g.token = token;
    RadioBegin(station, token);
}

void PlayFromQueue(int index, double start, bool paused) {
    auto& q = Queue();
    if (index < 0 || index >= (int)q.size()) return;
    RadioCancelPlayback();
    LeaveTrack(true);
    g.resumeTrack = kNone;
    g.resumeIndex = -1;
    g.resumePosition = 0;
    unsigned long long token = NewToken(index, q[index]);
    PrepareWaveform(q[index]);
    g.engine.Play(MakeItem(token, q[index]), start, paused);
    g.engine.SetPaused(paused);
    if (g.settings.shuffle && (g.shuffleOrder.size() != q.size() || g.shufflePos < 0)) Reshuffle(index);
    // The display follows at once; TrackStarted confirms it when the engine reaches it.
    g.queuePos = index;
    g.playing = q[index];
    g.token = token;
    g.listened = 0;
    g.lastPosition = start;
    g.redraw = true;
    MprisChanged();
}

bool Resume() {
    TrackId id = g.resumeTrack;
    if (!g.settings.resumeOn || g.playing != kNone || !g_lib.Contains(id)) return false;
    auto& queue = Queue();
    int index = g.resumeIndex;
    if (index < 0 || index >= (int)queue.size() || queue[index] != id) {
        auto it = std::find(queue.begin(), queue.end(), id);
        index = it == queue.end() ? -1 : (int)(it - queue.begin());
    }
    if (index < 0) {
        queue.assign(1, id);
        index = 0;
        g.shuffleOrder.clear();
        g.libraryDirty = true;
        if (g.active == NowPlayingTab()) RebuildRows();
    }
    double position = g.resumePosition;
    double duration = g_lib[id].Duration();
    if (position <= 1 || position >= duration - 2) position = 0;
    PlayFromQueue(index, position);
    return true;
}

// Reopens the last session's track as Preferences asks: loaded and paused where it stopped, or
// playing from there. Both choices need Resume where you left off on. Runs at startup, and when
// those choices change while the stopped track is still waiting, so a change shows what it does.
void ReopenLastTrack() {
    if (!g.settings.resumeOn || g.playing != kNone || !(g.settings.resumeInSong || g.settings.resumePlay)) return;
    TrackId id = g.resumeTrack;
    if (!g_lib.Contains(id)) return;
    auto& queue = Queue();
    int index = g.resumeIndex;
    if (index < 0 || index >= (int)queue.size() || queue[index] != id) {
        auto it = std::find(queue.begin(), queue.end(), id);
        index = it == queue.end() ? -1 : (int)(it - queue.begin());
    }
    double position = g.resumePosition;
    double duration = g_lib[id].Duration();
    if (position <= 1 || position >= duration - 2) position = 0;
    if (index < 0) {
        queue.assign(1, id);
        index = 0;
        g.shuffleOrder.clear();
        g.libraryDirty = true;
        if (g.active == NowPlayingTab()) RebuildRows();
    }
    PlayFromQueue(index, position, !g.settings.resumePlay);
}

// Now Playing can be closed while playback is stopped. Whatever needs it makes it again, after the
// Library; the closed one comes back with Reopen closed playlist.
int EnsureNowPlayingTab() {
    if (int np = NowPlayingTab(); np >= 0) return np;
    int at = 1;
    for (size_t i = 0; i < g.playlists.size(); ++i)
        if (g.playlists[i].kind == PlaylistKind::Library) {
            at = (int)i + 1;
            break;
        }
    g.playlists.insert(g.playlists.begin() + at, Playlist{L"Now Playing", {}, PlaylistKind::NowPlaying});
    if (g.active >= at) ++g.active;
    g.libraryDirty = true;
    return at;
}

void PlayRow(int row) {
    if (row < 0 || row >= (int)g.rows.size()) return;
    int np = EnsureNowPlayingTab();
    if (g.active != np) {
        // What is shown becomes Now Playing, so changing a filter mid-album doesn't change what plays.
        g.playlists[np].items = g.rows;
        g.libraryDirty = true;
        g.shuffleOrder.clear();
    }
    int index = g.active == np && row < (int)g.rowItems.size() ? (int)g.rowItems[row] : row;
    PlayFromQueue(index);
}

// What a view plays (an album, the Albums & Tracks view) becomes Now Playing, from `index`.
void PlayTracks(const std::vector<TrackId>& tracks, int index) {
    if (tracks.empty()) return;
    int np = EnsureNowPlayingTab();
    g.playlists[np].items = tracks;
    g.libraryDirty = true;
    g.shuffleOrder.clear();
    if (g.active == np) RebuildRows();
    PlayFromQueue(std::clamp(index, 0, (int)tracks.size() - 1));
}

void TogglePause() {
    if (RadioPlaying()) {
        g.engine.SetPaused(!g.engine.Paused()); g.redraw = true; MprisChanged(); return;
    }
    if (g.playing == kNone) {
        if (InternetRadio().active) { RadioPlay(InternetRadio().selected); return; }
        bool selected = std::any_of(g.selected.begin(), g.selected.end(), [](char value) { return value != 0; });
        if (!selected && Resume()) return;
        if (selected && g.focus >= 0) PlayRow(g.focus);
        else if (!Queue().empty()) PlayFromQueue(std::clamp(g.queuePos, 0, (int)Queue().size() - 1));
        else if (!g.rows.empty()) PlayRow(std::max(g.focus, 0));
        return;
    }
    g.engine.SetPaused(!g.engine.Paused());
    g.redraw = true;
    MprisChanged();
}

void Stop() {
    RadioCancelPlayback();
    CancelPeaks();
    if (g.settings.resumeOn && g.playing != kNone && g_lib.Contains(g.playing)) {
        g.resumeTrack = g.playing;
        g.resumeIndex = g.queuePos;
        g.resumePosition = g.engine.Position();
    }
    LeaveTrack(true);
    g.engine.Stop();
    g.playing = kNone;
    g.listened = 0;
    g.redraw = true;
    MprisChanged();
    SaveSettings();
    FlushDeferredTags();
}

ReplayGainOptions GainOptions() {
    auto options = g.settings.rg;
    options.shuffleTracks = g.settings.shuffle == kShuffleTracks;
    return options;
}

float GainFor(TrackId id) {
    return replaygain::Gain(g_lib[id], GainOptions(), g.settings.rg.on ? EqPeak() : 0);
}

float GainForEstimate(TrackId id, double lufs) {
    return replaygain::GainForEstimate(g_lib[id], lufs, GainOptions(), g.settings.rg.on ? EqPeak() : 0);
}

// `includePlaying` is false for background measurements: the track being heard keeps its level
// until it ends instead of jumping mid-song.
void RefreshGains(bool includePlaying) {
    g.engine.SetLimiter(g.settings.rg.on, g.settings.rg.headroom);
    for (auto& [token, info] : g_tokens)
        if ((includePlaying || token != g.token) && info.id < g_lib.Count())
            g.engine.SetGain(token, info.estimated ? GainForEstimate(info.id, info.estimate) : GainFor(info.id));
    g.redraw = true;
}

// Loop points apply to tracks opened from now on; the effects at once.
void ApplyEffects() {
    g.engine.SetReverb(g.settings.reverb);
    g.engine.SetSmartVolume(g.settings.loudnessComp, g.settings.nightMode);
    g.engine.SetLoops(g.settings.loops, 8.0);
}

void ApplyEq() {
    g.engine.SetEq(g.settings.eq);
    if (g.settings.rg.on) RefreshGains();
}

// The next tracks in play order, for measuring their ReplayGain before they are heard.
std::vector<TrackId> UpcomingTracks() {
    // A walk of its own rather than After(), which reshuffles when it runs off the end.
    std::vector<TrackId> ids;
    auto& q = Queue();
    int n = (int)q.size();
    if (!n) return ids;
    bool shuffled = g.settings.shuffle && (int)g.shuffleOrder.size() == n;
    int pos = g.queuePos;
    if (shuffled) {
        auto it = std::find(g.shuffleOrder.begin(), g.shuffleOrder.end(), g.queuePos);
        pos = it == g.shuffleOrder.end() ? -1 : (int)(it - g.shuffleOrder.begin());
    }
    for (int i = 1; i <= 8 && i < n; ++i) {
        int at = pos + i;
        if (at >= n) {
            if (g.settings.repeat == Repeat::Off) break;
            at -= n;
        }
        ids.push_back(q[shuffled ? g.shuffleOrder[at] : at]);
    }
    return ids;
}

void PlayQueued(TrackId id, int anchor) {
    RadioCancelPlayback();
    LeaveTrack(true);
    g.resumeTrack = kNone;
    g.resumeIndex = -1;
    g.resumePosition = 0;
    unsigned long long token = NewQueuedToken(anchor, id);
    PrepareWaveform(id);
    g.engine.Play(MakeItem(token, id));
    g.engine.SetPaused(false);
    g.queuePos = anchor;
    g.playing = id;
    g.token = token;
    g.listened = 0;
    g.lastPosition = 0;
    g.redraw = true;
    MprisChanged();
}

void Next() {
    if (RadioPlaying()) return;
    auto it = g_tokens.find(g.token);
    if (it != g_tokens.end()) LeaveQueue(it->second);  // skipping the queued track playing now takes it out
    int anchor = it != g_tokens.end() ? it->second.index : g.queuePos;
    while (!g.queue.empty() && !g_lib.Contains(g.queue.front())) g.queue.pop_front();
    if (!g.queue.empty()) {
        TrackId id = g.queue.front();
        g.queue.pop_front();
        PlayQueued(id, anchor);
        g_tokens[g.token].popped = true;
        return;
    }
    if (Queue().empty()) {
        if (it != g_tokens.end() && it->second.queued) Stop();
        return;
    }
    int next = g.token ? Following(g.token, true) : After(g.queuePos, true);
    if (next < 0) Stop();
    else PlayFromQueue(next);
}

void Previous() {
    if (RadioPlaying()) return;
    if (Queue().empty() && g.queue.empty()) return;
    if (g.playing != kNone && g.engine.Position() > 3) {
        SeekTo(0);
        return;
    }
    if (auto it = g_tokens.find(g.token); it != g_tokens.end() && it->second.queued) {  // back to where the queue broke in
        if (it->second.index >= 0 && it->second.index < (int)Queue().size()) PlayFromQueue(it->second.index);
        else SeekTo(0);
        return;
    }
    if (Queue().empty()) return;
    if (g.settings.radio && !g_radioHistory.empty()) {
        TrackId previous = g_radioHistory.back();
        g_radioHistory.pop_back();
        auto it = std::find(Queue().begin(), Queue().end(), previous);
        if (it != Queue().end()) {
            PlayFromQueue((int)(it - Queue().begin()));
            return;
        }
    }
    PlayFromQueue(Before(std::max(g.queuePos, 0)));
}

void SeekTo(double seconds) {
    if (RadioPlaying()) return;
    if (g.playing == kNone) return;
    TrackListening();
    PlayItem item = MakeItem(g.token, g.playing);
    if (item.loopEnd > item.loopStart)
        seconds = std::clamp(seconds, item.loopStart, std::max(item.loopStart, item.loopEnd - 0.001));
    g.engine.Seek(std::max(0.0, seconds), &item);
    g.redraw = true;
    MprisChanged(true);
}

void SetVolume(float volume) {
    g.settings.volume = std::clamp(volume, 0.0f, 1.0f);
    // A perceptual curve, as the slider means loudness rather than amplitude.
    float v = g.settings.volume;
    g.engine.SetVolume(v * v);
    g.redraw = true;
    MprisChanged();
}

void SetShuffle(int mode) {
    if (mode && mode != g.settings.shuffle) PlaySfx(Sfx::Dice);
    g.settings.shuffle = mode;
    if (g.settings.rg.mode == ReplayGainMode::PlaybackOrder) RefreshGains(false);
    for (auto& [token, info] : g_tokens) info.albumNext = -2;
    g.shuffleOrder.clear();
    if (g.settings.shuffle && g.queuePos >= 0) Reshuffle(g.queuePos);
    RefreshNext();
    SaveSettings();
    g.redraw = true;
    MprisChanged();
}

void SetSkipAwareShuffle(bool on) {
    if (g.settings.skipAwareShuffle == on) return;
    g.settings.skipAwareShuffle = on;
    if (g.settings.shuffle == kShuffleTracks) {
        g.shuffleOrder.clear();
        if (g.queuePos >= 0) Reshuffle(g.queuePos);
        RefreshNext();
    }
    SaveSettings();
    g.redraw = true;
}

void SetRadio(bool on) {
    if (g.settings.radio == on) return;
    g.settings.radio = on;
    for (auto& [token, info] : g_tokens) info.radioNext = -2;
    RefreshNext();
    SaveSettings();
    ShowMessage(on ? L"Radio is on." : L"Radio is off.");
}

void SetResumeOn(bool on) {
    if (g.settings.resumeOn == on) return;
    g.settings.resumeOn = on;
    if (!on) {
        g.resumeTrack = kNone;
        g.resumeIndex = -1;
        g.resumePosition = 0;
    }
    SaveSettings();
    g.redraw = true;
}

void RemapNowPlaying(const std::vector<size_t>& oldToNew) {
    auto nextSurvivor = [&](size_t start) {
        for (size_t i = start; i < oldToNew.size(); ++i)
            if (oldToNew[i] != SIZE_MAX) return (int)oldToNew[i];
        return -1;
    };
    for (auto& [token, info] : g_tokens) {
        if (info.index >= 0 && (size_t)info.index < oldToNew.size()) {
            size_t old = (size_t)info.index;
            info.following = nextSurvivor(old + 1);
            info.index = oldToNew[old] == SIZE_MAX ? -1 : (int)oldToNew[old];
        } else if (info.following >= 0) {
            info.following = nextSurvivor((size_t)info.following);
        }
        info.albumNext = -2;
        info.radioNext = -2;
    }
    if (g.queuePos >= 0 && (size_t)g.queuePos < oldToNew.size())
        g.queuePos = oldToNew[g.queuePos] == SIZE_MAX ? -1 : (int)oldToNew[g.queuePos];
    if (g.resumeIndex >= 0 && (size_t)g.resumeIndex < oldToNew.size())
        g.resumeIndex = oldToNew[g.resumeIndex] == SIZE_MAX ? -1 : (int)oldToNew[g.resumeIndex];
    g.shuffleOrder.clear();
    if (g.playing != kNone && g.token) {
        TrackListening();
        RefreshNext();
    }
}

void ToggleShuffle() { SetShuffle((g.settings.shuffle + 1) % kShuffleCount); }

void SetRepeat(Repeat repeat) {
    g.settings.repeat = repeat;
    RefreshNext();
    SaveSettings();
    g.redraw = true;
    MprisChanged();
}

void CycleRepeat() { SetRepeat((Repeat)(((int)g.settings.repeat + 1) % 3)); }

void SetExclusive(bool exclusive) {
    g.settings.exclusive = exclusive;
    sfx::SetSuppressed(exclusive);  // lets go of the device before exclusive output asks for it
    g.engine.SetExclusive(exclusive);
    SaveSettings();
    ShowMessage(exclusive ? L"Exclusive output: each track at its own rate, bit-perfect at 100% volume with EQ, "
                            L"crossfeed and ReplayGain off."
                          : L"Shared output.");
    g.redraw = true;
}

void SetSpeed(double speed) {
    g.speed = std::isfinite(speed) ? std::clamp(speed, 0.25, 4.0) : 1.0;
    g.engine.SetSpeed(g.speed);
    wchar_t text[96];
    swprintf(text, std::size(text), g.speed == 1 ? L"Normal speed." : L"Playing at %gx speed (the pitch follows).", g.speed);
    ShowMessage(text);
    MprisChanged();
}

void SetTempo(double tempo) {
    g.tempo = std::isfinite(tempo) ? std::clamp(tempo, 0.25, 4.0) : 1.0;
    g.engine.SetTempo(g.tempo);
    wchar_t text[96];
    swprintf(text, std::size(text), g.tempo == 1 ? L"Normal tempo." : L"Playing at %gx tempo (the pitch stays).", g.tempo);
    ShowMessage(text);
    MprisChanged();
}

void ToggleStopAfterCurrent() {
    g.stopAfterCurrent = !g.stopAfterCurrent;
    if (g.stopAfterCurrent && g.playing != kNone && g.token) {
        // Decoded-ahead queue entries haven't been heard yet. Put them back before dropping the
        // buffered next tracks so stopping here doesn't consume the listener's queue.
        std::vector<unsigned long long> ahead;
        for (const auto& [token, info] : g_tokens)
            if (token > g.token && info.queued && info.popped) ahead.push_back(token);
        std::sort(ahead.rbegin(), ahead.rend());
        for (auto token : ahead) {
            auto& info = g_tokens[token];
            g.queue.push_front(info.id);
            info.popped = false;
        }
        AnswerNext(g.token, false);
    } else RefreshNext();
    ShowMessage(g.stopAfterCurrent ? L"Playback will stop after the current track." : L"Stop after current is off.");
}

void OnEngineEvent(EngineEvent e, unsigned long long token) {
    if (RadioEngineEvent(e, token)) return;
    if (RadioPlaying()) {
        if (e == EngineEvent::Error || e == EngineEvent::Damaged) g.engine.TakeError(token);
        return;
    }
    auto it = g_tokens.find(token);
    int index = it == g_tokens.end() ? -1 : it->second.index;
    auto& q = Queue();
    switch (e) {
        case EngineEvent::NeedNext:
        case EngineEvent::Error: {
            if (e == EngineEvent::Error) {
                std::wstring error = g.engine.TakeError(token);
                if (it != g_tokens.end() && g_lib.Contains(it->second.id)) g_lib[it->second.id].flags |= Track::kFailed;
                if (!error.empty()) ShowMessage(error);
            }
            if (it != g_tokens.end()) LeaveQueue(it->second);
            AnswerNext(token, e == EngineEvent::Error);
            break;
        }
        case EngineEvent::TrackStarted:
            if (g.playing == kNone) break;  // an event already queued before Stop must not resume playback
            if (it != g_tokens.end() && it->second.queued) {  // the queue's head: the list stays where it was
                if (token < g.token) break;
                bool newTrack = token != g.token;
                if (newTrack) LeaveTrack(false);
                g.queuePos = it->second.index;
                g.playing = it->second.id;
                g.token = token;
                if (newTrack) g.listened = 0;
                g.lastPosition = g.engine.Position();
                ScrobbleTrackStarted(token, g.playing);
            } else if (index >= 0 && index < (int)q.size()) {
                if (token < g.token) break;  // an older start may arrive after a manual change
                bool newTrack = token != g.token;
                if (newTrack) LeaveTrack(false);
                g.queuePos = index;
                g.playing = q[index];
                g.token = token;
                if (g.settings.shuffle && !g.shuffleOrder.empty()) {
                    auto pos = std::find(g.shuffleOrder.begin(), g.shuffleOrder.end(), index);
                    g.shufflePos = pos == g.shuffleOrder.end() ? -1 : (int)(pos - g.shuffleOrder.begin());
                }
                if (newTrack) {
                    g.listened = 0;
                }
                g.lastPosition = g.engine.Position();
                ScrobbleTrackStarted(token, g.playing);
            }
            g.redraw = true;
            MprisChanged();
            FlushDeferredTags();
            MaybeAutoMeasure(true);
            break;
        case EngineEvent::Damaged: {
            std::wstring msg = g.engine.TakeError(token);
            if (it != g_tokens.end() && it->second.id < g_lib.Count()) {
                g_lib[it->second.id].flags |= Track::kFailed;
                ShowMessage(TrackTitle(it->second.id) + L": " +
                            (msg.empty() ? L"Playback failed. The audio is damaged or unreadable." : msg));
                g.redraw = true;
            }
            break;
        }
        case EngineEvent::Ended:
            LeaveTrack(false);
            g.playing = kNone;
            g.stopAfterCurrent = false;
            g.listened = 0;
            g.redraw = true;
            MprisChanged();
            FlushDeferredTags();
            break;
        case EngineEvent::Estimated: {
            // The first seconds' loudness, until the track is measured: a slow level change.
            double lufs = g.engine.TakeEstimate(token);
            if (it == g_tokens.end() || it->second.estimated || !g.settings.rg.on || !std::isfinite(lufs)) break;
            it->second.estimated = true;
            it->second.estimate = lufs;
            g.engine.SetGain(token, GainForEstimate(it->second.id, lufs), true);
            g.redraw = true;
            break;
        }
    }
}

// Called a few times a second to follow output changes and count only the time actually heard.
void PlaybackTick() {
    // The output settles after a track starts (exclusive mode reopens at the track's rate).
    static unsigned lastRate = 0;
    static int lastState = -1;
    Engine::OutputInfo o = g.engine.Output();
    int state = o.exclusive * 2 + o.bitPerfect;
    if (o.rate != lastRate || state != lastState) {
        // The EQ's peak boost depends on the rate, and with it the room ReplayGain leaves.
        if (o.rate != lastRate && g.settings.rg.on && g.settings.eq.enabled) RefreshGains();
        lastRate = o.rate;
        lastState = state;
        MprisChanged();
        g.redraw = true;
    }
    TrackListening();
}

}  // namespace lx
