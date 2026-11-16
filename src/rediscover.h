// Songs and albums worth hearing again: ones played a lot that have gone quiet for longer than their
// own play history suggests. Portable; the Windows popup (app_spots.cpp) and Linux panel show them.
#pragma once
#include "library.h"
#include <functional>

struct Rediscovery {
    std::vector<TrackId> tracks;  // one song, or an album's tracks in disc and track order
    bool album = false;
    uint32_t lastPlayed = 0;      // the newest play among them, which a snooze is tied to
    double plays = 0;             // per track, for an album: what sets its gap
};

// How long a song or album played `plays` times (per track) may go unheard before it is offered:
// the more it was played, the sooner it is missed, from about ten months down to two weeks.
double RediscoverGapDays(double plays);
// Up to `albums` albums and `tracks` songs past their gap at `now`, the most overdue first. A song in
// a listed album is not listed again. `hidden` skips a song (or the album whose newest play it is).
std::vector<Rediscovery> FindRediscoveries(const Library& lib, uint32_t now, size_t albums, size_t tracks,
                                           const std::function<bool(TrackId)>& hidden = {});
