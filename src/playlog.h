// The play log: one record per finished listen (a counted play or a skip), stored in library.cdb.
// The year-in-review statistics and the play-count auto-playlist rules are computed from it.
//
// Days, hours and weekdays are in the user's local time zone. Times are UnixNow() seconds.
//
// The log is a UI-thread global, like the library: events are appended as playback ends and the
// whole log is rebuilt from library.cdb on load.
#pragma once
#include "library.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct PlayEvent {  // one finished listen (12 bytes)
    TrackId id;
    uint32_t time;     // UnixNow() when it ended
    uint16_t seconds;  // really listened (seeks excluded), clamped
    uint8_t flags;     // kCounted (counted as a play) | kSkip
    enum : uint8_t { kCounted = 1, kSkip = 2 };
};
static_assert(sizeof(PlayEvent) == 12, "keep play events compact");

struct TopEntry {
    TrackId track;  // tracks: the track; artists and albums: a track of the group
    StringPool::Handle key;  // tracks: title; artists: artist; albums: album
    uint32_t plays;          // counted plays
    uint32_t seconds;        // listened seconds (counted plays and skips)
};

struct PeriodStats {
    uint32_t plays = 0, skips = 0, distinctTracks = 0, discoveries = 0;  // discoveries: first-ever counted play in the period
    uint64_t seconds = 0;                                                  // listened seconds, counted plays and skips
    std::vector<TopEntry> topTracks, topArtists, topAlbums;  // most plays first, at most topN each
    uint32_t byHour[24] = {}, byWeekday[7] = {};             // counted plays; weekday 0 = Monday
    uint32_t longestStreak = 0;                              // consecutive local days with listening
    uint32_t busiestDay = 0, busiestDaySeconds = 0;          // local day as days since 1970
};

// Local calendar helpers. A day is the number of days since 1970-01-01 in local time.
int64_t LocalDay(uint32_t time);
int LocalHour(uint32_t time);
int LocalWeekday(uint32_t time);  // 0 = Monday

void LogPlay(TrackId id, uint32_t time, double seconds, bool counted, bool skip);
const std::vector<PlayEvent>& PlayLog();
void SetPlayLog(std::vector<PlayEvent>&& events);
void RemapPlayLog(const std::vector<TrackId>& mapping);

// True when `e` began within two minutes of `prior` ending, continuing the same sitting. A start is
// the end time minus the seconds heard, so a long pause can make a listen look later than it was.
bool Follows(const PlayEvent& prior, const PlayEvent& e);
// Left by hand within 30 seconds without counting as a play.
bool EarlySkip(const PlayEvent& e);

// Where a listen sat in its sitting. Plain shuffle drops songs into these moments at random, so a
// song's skips can be compared with the usual rate for its moments: a skip in a run of skips is
// someone hunting for a mood, and says less about the song than one right after a song they heard.
// Left is a skip after which nothing else began within ten minutes: the listener stopped, closed
// the player or shut down, which says nothing about the song. The newest skip stays Left until
// something follows it.
enum class ListenMoment : uint8_t { First, AfterSkip, AfterListen, Left };
// One moment per event from `start` to the end of the log.
std::vector<ListenMoment> ListenMoments(size_t start);
// The early-skip rate of a kind of moment, drawn toward `overall` while it has few listens.
double MomentSkipRate(double skips, double listens, double overall);
// What an early skip or a counted play is worth where early skips happen at `rate` against
// `overall`: a surprising outcome counts for more than a plain one, an expected one for less,
// between a quarter and two and a half of a plain count.
double SkipWorth(double overall, double rate);
double PlayWorth(double overall, double rate);

// Smoothed early-skip weights; radio also rewards counted next-track transitions in a session.
// Skips and plays are judged against their moments, and a skip that ended the listening is not
// held against its song. Unknown tracks retain a nonzero chance. The model is rebuilt lazily after
// log changes.
std::vector<double> ListeningWeights(const std::vector<TrackId>& candidates, TrackId from = UINT32_MAX);
// Statistics for the events with `from` <= time < `to`.
PeriodStats ComputeStats(uint32_t from, uint32_t to, size_t topN);
// Counted plays on today's month and day in earlier years, most-played first, distinct.
std::vector<TrackId> PlayedOnThisDay(uint32_t now);

// library.cdb support. Encoding skips events whose track isn't saved (newId = UINT32_MAX) and
// writes the rest with their saved index. Decoding checks every field; `savedToId` maps saved
// indices to track ids (nullptr = identity).
std::string EncodePlayLog(const std::vector<uint32_t>& newId);
std::string EncodePlayLog(const std::vector<PlayEvent>& events, const std::vector<uint32_t>& newId);
bool DecodePlayLog(std::string_view data, uint64_t trackCount, const std::vector<TrackId>* savedToId,
                   std::vector<PlayEvent>& out);
