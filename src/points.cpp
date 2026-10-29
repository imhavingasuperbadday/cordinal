// The points rules and ledger: what each listen, play, milestone, album run and library addition
// earns, plus the day and streak bookkeeping. All amounts are centipoints; points.h holds the API
// and the storage format (library.cdb). Everything runs on the UI thread.
#include "points.h"

#include "playlog.h"
#include "sections.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_set>

namespace {

constexpr size_t kRecentLimit = 500;
constexpr int64_t kStreakSeconds = 600;  // a day counts once this much was listened
constexpr uint32_t kYearSeconds = 365 * 86400u;

PointsState g_state;
bool g_initialized = false;

// Album keys (album + album artist, lowercase) with at least one collected track, as 64-bit
// hashes: the same albums as the old lowercased strings, without a heap node and string each.
std::unordered_set<uint64_t> g_albums;
bool g_albumsBuilt = false;

// The album run in progress. It lives in memory only: a restart starts a new run.
struct AlbumRun {
    uint64_t key = 0;   // AlbumKey of the run's album
    uint32_t last = 0;  // disc << 16 | track number of the last track in the run
    uint32_t length = 0, inLibrary = 0, albumTotal = 0;
    bool full = false;
};
AlbumRun g_run;

const struct {
    int level;
    const wchar_t* name;
} kRanks[] = {{1, L"Egg"},
              {3, L"Hatchling"},
              {6, L"Nestling"},
              {10, L"Fledgling"},
              {15, L"Songbird"},
              {20, L"Redbird"},
              {27, L"Crested Redbird"},
              {35, L"Northern Cardinal"},
              {45, L"Cardinal of Song"},
              {60, L"Cardinal of the Choir"},
              {80, L"Cardinal Elector"},
              {100, L"Archcardinal"}};

const wchar_t* const kKindNames[] = {L"Listening", L"Play",          L"Discovery", L"Milestone", L"Rediscovery",
                                     L"Album run", L"Full album",    L"Collection", L"New album", L"Back pay"};
static_assert(std::size(kKindNames) == (size_t)PointsKind::BackPay + 1);

std::wstring Lower(std::wstring s) {
    if (!s.empty())
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING, s.c_str(), (int)s.size(), s.data(),
                      (int)s.size(), nullptr, nullptr, 0);
    return s;
}

// Album and album artist, case-insensitively, as one 64-bit hash (FNV-1a over the lowercased
// strings, with a separator so "a"|"bc" and "ab"|"c" differ). 0 for tracks without an album:
// they never form an album run or a new album. A hash collision is astronomically unlikely.
uint64_t AlbumKey(const Track& t) {
    if (!t.album) return 0;
    std::wstring album = Lower(g_lib.Str(t.album));
    std::wstring artist = Lower(g_lib.Str(t.albumArtist ? t.albumArtist : t.artist));
    uint64_t h = 1469598103934665603ull;
    for (wchar_t c : album) h = (h ^ (uint64_t)c) * 1099511628211ull;
    h = (h ^ 0x1full) * 1099511628211ull;  // separator: album then artist
    for (wchar_t c : artist) h = (h ^ (uint64_t)c) * 1099511628211ull;
    return h ? h : 1;  // 0 is reserved for "no album"
}

// Points for the n-th counted play of a track (devotion), in centipoints.
uint64_t PlayCenti(uint32_t n) { return (uint64_t)(5 + (int)std::floor(4.0 * std::log2((double)std::max<uint32_t>(n, 1)))) * 100; }

// Milestone bonuses, in centipoints, for reaching exactly n counted plays.
uint64_t MilestoneCenti(uint32_t n) {
    switch (n) {
        case 10: return 2000;
        case 25: return 5000;
        case 50: return 10000;
        case 100: return 25000;
        case 250: return 50000;
        case 500: return 100000;
        default: break;
    }
    return n >= 1000 && n % 1000 == 0 ? 250000 : 0;
}

// Counted plays of one track since local midnight. The log is chronological, so the walk stops at
// the first entry from an earlier day.
uint32_t CountedPlaysToday(uint32_t now, TrackId id) {
    int64_t today = LocalDay(now);
    uint32_t n = 0;
    const auto& log = PlayLog();
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
        if (LocalDay(it->time) != today) break;
        if (it->id == id && (it->flags & PlayEvent::kCounted)) ++n;
    }
    return n;
}

// Tracks of one album that are in the library, used to tell when a run has played a whole album.
uint32_t CountAlbumTracks(uint64_t key) {
    uint32_t n = 0;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id)
        if (g_lib[id].InLibrary() && AlbumKey(g_lib[id]) == key) ++n;
    return n;
}

// Builds the set of albums that already have a collected track, for the new-album bonus.
void EnsureAlbums() {
    if (g_albumsBuilt) return;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        const Track& t = g_lib[id];
        if (t.flags & Track::kCollected) {
            uint64_t key = AlbumKey(t);
            if (key) g_albums.insert(key);
        }
    }
    g_albumsBuilt = true;
}

// Adds to the award. Consecutive events of one kind and track are merged.
void AddEvent(PointsAward& a, uint32_t time, PointsKind kind, TrackId id, uint64_t centi) {
    if (!centi) return;
    a.centi += centi;
    if (!a.events.empty() && a.events.back().kind == kind && a.events.back().id == id) {
        a.events.back().centi += centi;
        return;
    }
    a.events.push_back({time, kind, id, centi});
}

// Adds to the per-day ledger, keeping it sorted by day (the newest day is the common case).
void AddByDay(uint32_t day, uint64_t centi) {
    auto& byDay = g_state.byDay;
    if (byDay.empty() || byDay.back().first < day) {
        byDay.emplace_back(day, centi);
        return;
    }
    auto it = std::lower_bound(byDay.begin(), byDay.end(), day,
                               [](const std::pair<uint32_t, uint64_t>& x, uint32_t d) { return x.first < d; });
    if (it != byDay.end() && it->first == day) it->second += centi;
    else byDay.insert(it, {day, centi});
}

// Books an award into the total, the day, and the recent list.
void Commit(PointsAward& a, uint32_t now) {
    g_state.total += a.centi;
    if (a.centi) AddByDay((uint32_t)std::max<int64_t>(LocalDay(now), 0), a.centi);
    for (const PointsEvent& e : a.events) {
        g_state.recent.push_back(e);
        if (g_state.recent.size() > kRecentLimit) g_state.recent.pop_front();
    }
    a.levelAfter = PointsLevel(g_state.total);
}

// A fresh award whose levelBefore already reflects the current total.
PointsAward NewAward() {
    PointsAward a;
    a.levelBefore = a.levelAfter = PointsLevel(g_state.total);
    return a;
}

// Streak multiplier for a streak of `streak` counting days.
double Multiplier(int streak) { return 1.0 + 0.05 * std::clamp(streak - 1, 0, 10); }

// Accumulates the day's listened seconds and extends the streak once its first 10 minutes are met.
void UpdateStreak(uint32_t now, double seconds) {
    int64_t day = LocalDay(now);
    if (g_state.dayDay != day) {
        g_state.dayDay = day;
        g_state.daySeconds = 0;
    }
    g_state.daySeconds = (uint32_t)std::min<double>(g_state.daySeconds + seconds, UINT32_MAX);
    if (g_state.daySeconds >= kStreakSeconds && g_state.streakDay != day) {
        g_state.streakLength = g_state.streakDay == day - 1 ? g_state.streakLength + 1 : 1;
        g_state.streakDay = day;
    }
}

// Album runs: four or more tracks of one album in ascending order, back to back. Any other
// logged listen ends the run.
void AdvanceAlbumRun(PointsAward& a, uint32_t now, TrackId id, const Track& t) {
    uint64_t key = AlbumKey(t);
    uint32_t order = ((uint32_t)t.disc << 16) | t.trackNo;
    bool continues = key && g_run.length && key == g_run.key && order > g_run.last;
    if (!continues) {
        g_run = AlbumRun{};
        if (!key) return;
        g_run.key = key;
    }
    ++g_run.length;
    g_run.last = order;
    if (t.InLibrary()) ++g_run.inLibrary;
    if (g_run.length == 4) {
        g_run.albumTotal = CountAlbumTracks(g_run.key);
        AddEvent(a, now, PointsKind::AlbumRun, id, 3000);
    } else if (g_run.length > 4) {
        AddEvent(a, now, PointsKind::AlbumRun, id, 500);
    }
    if (g_run.length >= 4 && !g_run.full && g_run.albumTotal > 0 && g_run.inLibrary == g_run.albumTotal) {
        g_run.full = true;
        AddEvent(a, now, PointsKind::FullAlbum, id, 5000);
    }
}

}  // namespace

uint64_t PointsTotal() { return g_state.total; }

// Level curve: level L starts at 50 * (L - 1)^2.5 whole points, stored in centipoints.
uint64_t PointsThreshold(int level) {
    if (level <= 1) return 0;
    return (uint64_t)std::llround(50.0 * std::pow(level - 1, 2.5)) * 100;
}

int PointsLevel(uint64_t centi) {
    int level = 1 + (int)std::floor(std::pow(centi / 5000.0, 0.4));
    while (level > 1 && PointsThreshold(level) > centi) --level;
    while (PointsThreshold(level + 1) <= centi) ++level;
    return level;
}

const wchar_t* PointsRank(int level) {
    const wchar_t* name = kRanks[0].name;
    for (const auto& r : kRanks)
        if (level >= r.level) name = r.name;
    return name;
}

int PointsNextRankLevel(int level) {
    for (const auto& r : kRanks)
        if (r.level > level) return r.level;
    return 0;
}

int PointsStreak(uint32_t now) {
    int64_t today = LocalDay(now);
    if (g_state.streakDay == today || g_state.streakDay == today - 1) return (int)g_state.streakLength;
    return 0;
}

double PointsMultiplier(uint32_t now) { return Multiplier(PointsStreak(now)); }

const std::deque<PointsEvent>& PointsRecent() { return g_state.recent; }

const std::vector<std::pair<uint32_t, uint64_t>>& PointsByDay() { return g_state.byDay; }

const wchar_t* PointsKindName(PointsKind k) { return kKindNames[(int)k]; }

// Awards one logged listen (counted or skipped) and books it. The caller logs the play first.
PointsAward AwardListenAt(uint32_t now, TrackId id, double seconds, bool counted, bool skip, uint32_t previousLastPlayed) {
    PointsAward a = NewAward();
    if ((!counted && !skip) || id >= g_lib.Count()) return a;
    seconds = seconds > 0 ? seconds : 0;
    // The streak as it stood before this listen.
    double mult = PointsMultiplier(now);
    if (!counted) g_run = AlbumRun{};  // a skipped track is a track in between
    AddEvent(a, now, PointsKind::Listening, id, (uint64_t)std::llround(seconds / 60.0 * 200.0 * mult));
    if (counted) {
        Track& t = g_lib[id];
        uint32_t n = t.playCount;
        uint64_t play = PlayCenti(n);
        if (CountedPlaysToday(now, id) > 10) play /= 2;  // from the 11th play of the day
        AddEvent(a, now, PointsKind::Play, id, (uint64_t)std::llround((double)play * mult));
        if (n == 1) AddEvent(a, now, PointsKind::Discovery, id, 1000);
        AddEvent(a, now, PointsKind::Milestone, id, MilestoneCenti(n));
        if (previousLastPlayed && now >= previousLastPlayed && now - previousLastPlayed >= kYearSeconds)
            AddEvent(a, now, PointsKind::Rediscovery, id, 1500);
        AdvanceAlbumRun(a, now, id, t);
    }
    UpdateStreak(now, seconds);
    Commit(a, now);
    return a;
}

PointsAward AwardListen(TrackId id, double seconds, bool counted, bool skip, uint32_t previousLastPlayed) {
    return AwardListenAt(UnixNow(), id, seconds, counted, skip, previousLastPlayed);
}

// Awards the tracks that just entered the library; each track pays once, ever (the kCollected flag).
PointsAward AwardLibraryAddsAt(uint32_t now, const std::vector<TrackId>& ids) {
    PointsAward a = NewAward();
    EnsureAlbums();
    for (TrackId id : ids) {
        if (id >= g_lib.Count()) continue;
        Track& t = g_lib[id];
        if (!t.InLibrary() || (t.flags & Track::kCollected)) continue;
        t.flags |= Track::kCollected;
        AddEvent(a, now, PointsKind::Collection, id, 200);
        uint64_t key = AlbumKey(t);
        if (key && g_albums.insert(key).second) AddEvent(a, now, PointsKind::NewAlbum, id, 2000);
    }
    Commit(a, now);
    return a;
}

PointsAward AwardLibraryAdds(const std::vector<TrackId>& ids) { return AwardLibraryAddsAt(UnixNow(), ids); }

// One-time back pay for a library that predates points: what its existing play counts and
// durations would have earned, without multipliers, fatigue or album/streak bonuses.
PointsAward GrantBackPayAt(uint32_t now) {
    PointsAward a = NewAward();
    if (g_initialized) return a;
    EnsureAlbums();
    uint64_t sum = 0;
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        Track& t = g_lib[id];
        if (!t.InLibrary() || (t.flags & Track::kCollected)) continue;
        t.flags |= Track::kCollected;
        sum += 200;
        uint64_t key = AlbumKey(t);
        if (key && g_albums.insert(key).second) sum += 2000;
    }
    for (TrackId id = 0; id < (TrackId)g_lib.Count(); ++id) {
        const Track& t = g_lib[id];
        if (!t.playCount) continue;
        // Every play as it would have been counted, without multipliers or fatigue.
        for (uint32_t k = 1; k <= t.playCount; ++k) sum += PlayCenti(k) + MilestoneCenti(k);
        sum += 1000;  // discovery
        sum += (uint64_t)std::llround((double)t.playCount * t.Duration() * 200.0 / 60.0);
    }
    AddEvent(a, now, PointsKind::BackPay, UINT32_MAX, sum);
    g_initialized = true;
    Commit(a, now);
    return a;
}

PointsAward GrantBackPayIfNeeded() { return GrantBackPayAt(UnixNow()); }

bool PointsInitialized() { return g_initialized; }

void SetPointsState(PointsState&& state) {
    g_state = std::move(state);
    g_initialized = true;
    g_albums.clear();
    g_albumsBuilt = false;
    g_run = AlbumRun{};
}

void ResetPoints() {
    g_state = PointsState{};
    g_initialized = false;
    g_albums.clear();
    g_albumsBuilt = false;
    g_run = AlbumRun{};
}

// Layout: varint payload version (1), total, streak and day state, the days (varint delta,
// centipoints), then the recent events (kind, time delta, track index + 1 or 0, centipoints).
const PointsState& CurrentPointsState() { return g_state; }

std::string EncodePoints(const std::vector<uint32_t>& newId) { return EncodePoints(g_state, newId); }

std::string EncodePoints(const PointsState& state, const std::vector<uint32_t>& newId) {
    std::string out;
    ByteWriter w(out);
    w.U(1);
    w.U(state.total);
    w.I(state.streakDay);
    w.U(state.streakLength);
    w.I(state.dayDay);
    w.U(state.daySeconds);
    w.U(state.byDay.size());
    int64_t previousDay = 0;
    for (const auto& [day, centi] : state.byDay) {
        w.I((int64_t)day - previousDay);
        previousDay = day;
        w.U(centi);
    }
    std::vector<const PointsEvent*> kept;
    for (const PointsEvent& e : state.recent)
        if (e.id == UINT32_MAX || (e.id < newId.size() && newId[e.id] != UINT32_MAX)) kept.push_back(&e);
    w.U(kept.size());
    int64_t previousTime = 0;
    for (const PointsEvent* e : kept) {
        w.U((uint64_t)e->kind);
        w.I((int64_t)e->time - previousTime);
        previousTime = e->time;
        w.U(e->id == UINT32_MAX ? 0 : newId[e->id] + 1);
        w.U(e->centi);
    }
    return out;
}

// Inverse of EncodePoints. Track ids are saved-file indices; savedToId maps them back to this
// library. Rejects malformed or truncated data.
bool DecodePoints(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId, PointsState& out) {
    ByteReader r(data);
    if (r.U(1) != 1) return false;
    PointsState s;
    s.total = r.U();
    s.streakDay = r.I();
    s.streakLength = r.Unsigned<uint32_t>();
    s.dayDay = r.I();
    s.daySeconds = r.Unsigned<uint32_t>();
    uint64_t days = r.U(data.size() / 2);
    int64_t day = 0;
    for (uint64_t i = 0; i < days && r.ok; ++i) {
        int64_t previous = day;
        day += r.I();
        uint64_t centi = r.U();
        if (day < 0 || day > UINT32_MAX || (i && day <= previous)) r.ok = false;
        if (r.ok) s.byDay.emplace_back((uint32_t)day, centi);
    }
    uint64_t events = r.U(kRecentLimit);
    int64_t time = 0;
    for (uint64_t i = 0; i < events && r.ok; ++i) {
        uint64_t kind = r.U((uint64_t)PointsKind::BackPay);
        time += r.I();
        uint64_t index = r.U(trackCount);  // 0 = no track, else saved index + 1
        uint64_t centi = r.U();
        if (time < 0 || time > UINT32_MAX) r.ok = false;
        if (!r.ok) break;
        TrackId id = UINT32_MAX;
        if (index) id = savedToId ? (*savedToId)[(size_t)index - 1] : (TrackId)(index - 1);
        s.recent.push_back({(uint32_t)time, (PointsKind)kind, id, centi});
    }
    if (!r.ok || !r.Done()) return false;
    out = std::move(s);
    return true;
}

// After library tracks are removed or reordered: drops events of tracks that are gone and rebuilds
// the album set, which depends on the library contents.
void RemapPoints(const std::vector<TrackId>& mapping) {
    for (auto& event : g_state.recent) event.id = event.id < mapping.size() ? mapping[event.id] : UINT32_MAX;
    g_albums.clear();
    g_albumsBuilt = false;
}
