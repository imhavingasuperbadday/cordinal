// The play log implementation: recording events, the local-calendar helpers, the statistics the
// year-in-review view and auto-playlist rules read, and the library.cdb section codec.
#include "sort.h"
#include "playlog.h"

#include "sections.h"

#include <algorithm>
#include <ctime>
#include <unordered_map>

namespace {

std::vector<PlayEvent> g_log;
bool modelDirty = true;
struct ListeningCounts { uint32_t plays = 0, earlySkips = 0; };
std::unordered_map<TrackId, ListeningCounts> listeningCounts;
std::unordered_map<uint64_t, uint32_t> nextCounts;

// Civil date to days since 1970-01-01 (proleptic Gregorian calendar).
int64_t DaysFromCivil(int64_t y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct LocalFields {
    int64_t day;
    int hour, weekday, year, month, mday;  // weekday 0 = Monday
};

// Splits a time into local calendar fields once, for callers that need several of them.
LocalFields LocalFieldsOf(uint32_t time) {
    tm t{};
    __time64_t seconds = time;
    if (_localtime64_s(&t, &seconds) != 0) t = tm{};
    return {DaysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday), t.tm_hour, (t.tm_wday + 6) % 7, t.tm_year + 1900,
            t.tm_mon + 1, t.tm_mday};
}

// The tracks of one artist or album: the best-played track stands for the group.
struct Group {
    TrackId track = UINT32_MAX;
    uint32_t plays = 0, seconds = 0, bestPlays = 0;
    void Add(TrackId id, uint32_t p, uint32_t s) {
        plays += p;
        seconds += s;
        if (track == UINT32_MAX || p > bestPlays) {
            track = id;
            bestPlays = p;
        }
    }
};

// Sorts most-played first (ties by seconds, then by track for determinism) and truncates to topN.
void KeepTop(std::vector<TopEntry>& entries, size_t topN) {
    auto compare = [](const TopEntry& x, const TopEntry& y) {
        if (x.plays != y.plays) return x.plays > y.plays;
        if (x.seconds != y.seconds) return x.seconds > y.seconds;
        return x.track < y.track;
    };
    if (entries.size() > topN) {
        if (topN) std::partial_sort(entries.begin(), entries.begin() + (ptrdiff_t)topN, entries.end(), compare);
        entries.resize(topN);
    } else SortRecords(entries.begin(), entries.end(), compare);
}

}  // namespace

int64_t LocalDay(uint32_t time) { return LocalFieldsOf(time).day; }
int LocalHour(uint32_t time) { return LocalFieldsOf(time).hour; }
int LocalWeekday(uint32_t time) { return LocalFieldsOf(time).weekday; }

void LogPlay(TrackId id, uint32_t time, double seconds, bool counted, bool skip) {
    if (!counted && !skip) return;
    modelDirty = true;
    g_log.push_back({id, time,
        (uint16_t)(seconds >= 0 ? std::min(seconds, 65535.0) : 0.0),  // NaN fails the test and is 0
        (uint8_t)((counted ? PlayEvent::kCounted : 0) | (skip ? PlayEvent::kSkip : 0))});
}

const std::vector<PlayEvent>& PlayLog() { return g_log; }

void SetPlayLog(std::vector<PlayEvent>&& events) { g_log = std::move(events); modelDirty = true; }

std::vector<double> ListeningWeights(const std::vector<TrackId>& candidates, TrackId from) {
    if (modelDirty) {
        listeningCounts.clear();
        nextCounts.clear();
        // Bound memory and let older habits age out. Adjacent counted listens only: an early
        // skip breaks a transition, as does a break of more than two minutes between tracks.
        const PlayEvent* previous = nullptr;
        size_t start = g_log.size() > 10000 ? g_log.size() - 10000 : 0;
        for (size_t i = start; i < g_log.size(); ++i) {
            const auto& e = g_log[i];
            if (e.id == UINT32_MAX) { previous = nullptr; continue; }
            auto& counts = listeningCounts[e.id];
            bool counted = (e.flags & PlayEvent::kCounted) != 0;
            counts.plays += counted;
            counts.earlySkips += !counted && (e.flags & PlayEvent::kSkip) && e.seconds < 30;
            int64_t began = (int64_t)e.time - e.seconds;
            if (counted && previous && previous->id != e.id && began >= (int64_t)previous->time - 5 &&
                began <= (int64_t)previous->time + 120 && e.time >= previous->time)
                ++nextCounts[((uint64_t)previous->id << 32) | e.id];
            previous = counted ? &e : nullptr;
        }
        modelDirty = false;
    }
    std::vector<double> weights;
    weights.reserve(candidates.size());
    for (TrackId id : candidates) {
        double weight = 1;
        auto counts = listeningCounts.find(id);
        if (counts != listeningCounts.end()) {
            const auto& c = counts->second;
            weight = std::max(0.05, (c.plays + 2.0) / (c.plays + 2.0 + 4.0 * c.earlySkips));
        }
        if (from != UINT32_MAX) {
            auto next = nextCounts.find(((uint64_t)from << 32) | id);
            if (next != nextCounts.end()) weight *= 1 + 8.0 * next->second;
        }
        weights.push_back(weight);
    }
    return weights;
}

// `from`/`to` bound the counted period ([from, to)); the first-ever-play map is built over the
// whole log, so discoveries stay correct when the period does not start at the beginning.
PeriodStats ComputeStats(uint32_t from, uint32_t to, size_t topN) {
    PeriodStats s;
    struct Agg {
        uint32_t plays = 0, seconds = 0;
    };
    std::unordered_map<TrackId, uint32_t> firstPlay;
    std::unordered_map<TrackId, Agg> tracks;
    std::unordered_map<int64_t, uint32_t> daySeconds;
    for (const PlayEvent& e : g_log) {
        if (e.flags & PlayEvent::kCounted) {
            auto [it, added] = firstPlay.emplace(e.id, e.time);
            if (!added && e.time < it->second) it->second = e.time;
        }
        if (e.time < from || e.time >= to) continue;
        Agg& a = tracks[e.id];
        a.seconds += e.seconds;
        s.seconds += e.seconds;
        LocalFields f = LocalFieldsOf(e.time);
        daySeconds[f.day] += e.seconds;
        if (e.flags & PlayEvent::kCounted) {
            ++a.plays;
            ++s.plays;
            ++s.byHour[f.hour];
            ++s.byWeekday[f.weekday];
        }
        if (e.flags & PlayEvent::kSkip) ++s.skips;
    }
    // Discoveries: tracks whose first counted play ever falls in the period.
    for (const auto& [id, when] : firstPlay)
        if (when >= from && when < to) ++s.discoveries;

    std::unordered_map<StringPool::Handle, Group> artists;
    std::unordered_map<uint64_t, Group> albums;  // album handle << 32 | album artist handle
    for (const auto& [id, a] : tracks) {
        if (!a.plays) continue;
        ++s.distinctTracks;
        const Track& t = g_lib[id];
        s.topTracks.push_back({id, t.title, a.plays, a.seconds});
        artists[t.artist].Add(id, a.plays, a.seconds);
        uint64_t albumKey = ((uint64_t)t.album << 32) | (t.albumArtist ? t.albumArtist : t.artist);
        albums[albumKey].Add(id, a.plays, a.seconds);
    }
    for (const auto& [handle, g] : artists) s.topArtists.push_back({g.track, handle, g.plays, g.seconds});
    for (const auto& [key, g] : albums) s.topAlbums.push_back({g.track, (StringPool::Handle)(key >> 32), g.plays, g.seconds});
    KeepTop(s.topTracks, topN);
    KeepTop(s.topArtists, topN);
    KeepTop(s.topAlbums, topN);

    // Streaks and the busiest day, over the local days with any listening.
    std::vector<int64_t> days;
    days.reserve(daySeconds.size());
    for (const auto& [day, secs] : daySeconds) {
        days.push_back(day);
        if (secs > s.busiestDaySeconds || (secs == s.busiestDaySeconds && (uint32_t)day < s.busiestDay)) {
            s.busiestDaySeconds = secs;
            s.busiestDay = (uint32_t)day;
        }
    }
    SortRecords(days.begin(), days.end());
    uint32_t run = 0;
    for (size_t i = 0; i < days.size(); ++i) {
        run = i && days[i] == days[i - 1] + 1 ? run + 1 : 1;
        s.longestStreak = std::max(s.longestStreak, run);
    }
    return s;
}

std::vector<TrackId> PlayedOnThisDay(uint32_t now) {
    LocalFields today = LocalFieldsOf(now);
    std::unordered_map<TrackId, uint32_t> counts;
    for (const PlayEvent& e : g_log) {
        if (!(e.flags & PlayEvent::kCounted) || e.time >= now) continue;
        LocalFields f = LocalFieldsOf(e.time);
        if (f.month == today.month && f.mday == today.mday && f.year < today.year) ++counts[e.id];
    }
    std::vector<std::pair<uint32_t, TrackId>> ranked;
    for (const auto& [id, n] : counts) ranked.emplace_back(n, id);
    SortObjects(ranked.begin(), ranked.end(), [](const auto& x, const auto& y) {
        return x.first != y.first ? x.first > y.first : x.second < y.second;
    });
    std::vector<TrackId> out;
    out.reserve(ranked.size());
    for (const auto& r : ranked) out.push_back(r.second);
    return out;
}

// Layout: varint count, then per event: varint saved track index, zigzag delta of the time
// (from the previous kept event), varint seconds, varint flags.
std::string EncodePlayLog(const std::vector<uint32_t>& newId) { return EncodePlayLog(g_log, newId); }

std::string EncodePlayLog(const std::vector<PlayEvent>& events, const std::vector<uint32_t>& newId) {
    std::string out;
    ByteWriter w(out);
    auto kept = [&](const PlayEvent& e) { return e.id < newId.size() && newId[e.id] != UINT32_MAX; };
    size_t count = 0;
    for (const PlayEvent& e : events) count += kept(e);
    w.U(count);
    int64_t previous = 0;
    for (const PlayEvent& e : events) {
        if (!kept(e)) continue;
        w.U(newId[e.id]);
        w.I((int64_t)e.time - previous);
        previous = e.time;
        w.U(e.seconds);
        w.U(e.flags);
    }
    return out;
}

// Rebuilds the log, re-accumulating the time deltas and rejecting any field out of range.
bool DecodePlayLog(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId,
                   std::vector<PlayEvent>& out) {
    ByteReader r(data);
    uint64_t count = r.U(data.size() / 4);  // an event takes at least four bytes
    if (!r.ok || (count && !trackCount)) return false;
    std::vector<PlayEvent> events;
    events.reserve((size_t)count);
    int64_t time = 0;
    for (uint64_t i = 0; i < count && r.ok; ++i) {
        uint64_t index = r.U(trackCount - 1);
        time += r.I();
        uint64_t seconds = r.U(65535);
        uint64_t flags = r.U(PlayEvent::kCounted | PlayEvent::kSkip);
        if (time < 0 || time > UINT32_MAX || !flags) r.ok = false;
        if (!r.ok) break;
        TrackId id = savedToId ? (*savedToId)[(size_t)index] : (TrackId)index;
        events.push_back({id, (uint32_t)time, (uint16_t)seconds, (uint8_t)flags});
    }
    if (!r.ok || !r.Done()) return false;
    out = std::move(events);
    return true;
}

// Rewrites every event's track id through `mapping` (from Library::Compact); dropped tracks
// become UINT32_MAX and are skipped on the next save.
void RemapPlayLog(const std::vector<TrackId>& mapping) {
    modelDirty = true;
    for (auto& event : g_log) event.id = event.id < mapping.size() ? mapping[event.id] : UINT32_MAX;
}
