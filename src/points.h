// cordinal points: a score for collecting music and, above all, for listening to it again and
// again. Amounts are stored in centipoints (1/100 point) so that nothing drifts by rounding; the
// UI shows whole points. Everything here runs on the UI thread.
//
// Times are UnixNow() seconds; days and streaks are in the user's local time zone.
#pragma once
#include "library.h"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class PointsKind : uint8_t {
    Listening, Play, Discovery, Milestone, Rediscovery, AlbumRun, FullAlbum, Collection, NewAlbum, BackPay
};

// One ledger entry: a UnixNow() time, its kind, the track (or UINT32_MAX) and its centipoints.
struct PointsEvent {
    uint32_t time;
    PointsKind kind;
    TrackId id;  // UINT32_MAX when none
    uint64_t centi;
};

// The result of one award call: the centipoints total, the level before/after (for the level-up
// flash) and the itemized events.
struct PointsAward {
    uint64_t centi = 0;  // total of the events
    int levelBefore = 0, levelAfter = 0;
    std::vector<PointsEvent> events;
};

uint64_t PointsTotal();  // centipoints
int PointsLevel(uint64_t centi);
uint64_t PointsThreshold(int level);  // centipoints at which `level` starts
const wchar_t* PointsRank(int level);  // the rank title for a level
int PointsNextRankLevel(int level);    // 0 if there is no higher rank
int PointsStreak(uint32_t now);        // consecutive counting days up to today or yesterday
double PointsMultiplier(uint32_t now);  // 1 + 0.05 * min(streak - 1, 10)
const std::deque<PointsEvent>& PointsRecent();                  // newest last, at most 500
const std::vector<std::pair<uint32_t, uint64_t>>& PointsByDay();  // (local day, centi), sorted by day
const wchar_t* PointsKindName(PointsKind k);

// Call LogPlay for the listen first: the same-day fatigue rule counts the log. `previousLastPlayed`
// is the track's lastPlayed from before this listen. Only logged listens (counted or skipped)
// earn anything.
PointsAward AwardListen(TrackId id, double seconds, bool counted, bool skip, uint32_t previousLastPlayed);
// Tracks that just became part of the library. Each track is awarded once, ever.
PointsAward AwardLibraryAdds(const std::vector<TrackId>& ids);
// Once per library file that has no points section yet: the points its existing stats are worth.
PointsAward GrantBackPayIfNeeded();

// The same, with the time given (for tests).
PointsAward AwardListenAt(uint32_t now, TrackId id, double seconds, bool counted, bool skip, uint32_t previousLastPlayed);
PointsAward AwardLibraryAddsAt(uint32_t now, const std::vector<TrackId>& ids);
PointsAward GrantBackPayAt(uint32_t now);

// library.cdb support (librarydb.cpp). Points are saved only after back pay has been granted.
struct PointsState {
    uint64_t total = 0;
    int64_t streakDay = -1000000, dayDay = -1000000;  // local days; the last counting day, the day being counted
    uint32_t streakLength = 0, daySeconds = 0;        // consecutive counting days; listened seconds of dayDay
    std::deque<PointsEvent> recent;
    std::vector<std::pair<uint32_t, uint64_t>> byDay;
};
bool PointsInitialized();
const PointsState& CurrentPointsState();
void SetPointsState(PointsState&& state);
void ResetPoints();
// Events of tracks that aren't saved are dropped. Ids are written as saved indices.
std::string EncodePoints(const std::vector<uint32_t>& newId);
std::string EncodePoints(const PointsState& state, const std::vector<uint32_t>& newId);
bool DecodePoints(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId, PointsState& out);

void RemapPoints(const std::vector<TrackId>& mapping);
