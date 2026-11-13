// Checks of the track list's wind (wind.h) without a window: silence starts nothing, music starts
// ribbons and a note attack a gust, a ribbon's path flows instead of drawing a straight line, it melts
// into the list's edges rather than vanishing at them, a loud moment swells the whole wind, the count
// stays capped and the geometry stays finite, and a pause lets every ribbon drift off so the players can
// stop their timers. The Windows self-test (selftest_dsp.cpp, suite "equalizer") and the Linux core test
// (core_test.cpp, "waveform") both run them.
#pragma once
#include "wind.h"

#include <cmath>

// 0 when every check passes, otherwise the number of the first that failed.
inline int CheckWind() {
    const float silence[3] = {}, quiet[3] = {1e-4f, 5e-5f, 1e-5f}, loud[3] = {0.2f, 0.1f, 0.05f};
    const float dt = 1.0f / 30;
    Wind wind(42);
    for (int i = 0; i < 90; ++i) wind.Step(silence, true, dt);
    if (wind.Active()) return 1;  // three seconds of silence
    for (int i = 0; i < 90; ++i) wind.Step(quiet, true, dt);
    size_t steady = wind.Streaks().size();
    if (!steady) return 2;  // a steady level, quiet or not, still moves the wind
    wind.Step(loud, true, dt);
    if (wind.Spawned() < 3) return 3;  // an attack sends a gust of at least three
    for (int i = 0; i < 300; ++i) wind.Step(i % 8 ? quiet : loud, true, dt);
    if (wind.Streaks().size() > Wind::kMaxStreaks) return 4;
    float bend = 0;  // how far a grown path strays from the line between its ends
    float lx[Wind::kPoints], ly[Wind::kPoints], rx[Wind::kPoints], ry[Wind::kPoints];
    for (const WindStreak& s : wind.Streaks()) {
        if (s.count < 2 || s.count > Wind::kPoints) return 5;
        float fade = Wind::Fade(s);
        if (!(fade >= 0 && fade <= 1)) return 5;
        if (s.count < 3) continue;                   // a newborn's two points sit on the origin
        if (!(s.x[s.count - 1] > s.x[0])) return 5;  // the path runs rightwards, tail to head
        if (Wind::Ribbon(s, 500, 400, 1.5f, 2.0f, lx, ly, rx, ry) != s.count) return 6;
        for (int i = 0; i < s.count; ++i) {
            float px = s.x[i] * 500, py = s.y[i] * 400;
            if (!std::isfinite(lx[i]) || !std::isfinite(ly[i]) || !std::isfinite(rx[i]) || !std::isfinite(ry[i]))
                return 6;
            if (std::fabs(lx[i] - px) > 9 || std::fabs(ly[i] - py) > 9) return 6;  // edges stay by the path
            if (std::fabs(rx[i] - px) > 9 || std::fabs(ry[i] - py) > 9) return 6;
        }
        float dx = s.x[s.count - 1] - s.x[0], dy = s.y[s.count - 1] - s.y[0], len = std::hypot(dx, dy);
        if (len < 0.08f) continue;  // only a grown ribbon has room to show its curve
        for (int i = 1; i + 1 < s.count; ++i) {
            float px = s.x[i] - s.x[0], py = s.y[i] - s.y[0];
            bend = std::max(bend, std::fabs(px * dy - py * dx) / len);
        }
    }
    if (bend < 0.004f) return 7;  // a steady beat still draws ribbons that curl, not straight lines
    // A ribbon melts into the right edge: it stays while its tail is still inside, so the part that
    // shows fades out instead of vanishing with its head.
    Wind edge(11);
    bool melting = false;
    for (int i = 0; i < 30 * 12 && !melting; ++i) {
        edge.Step(i % 5 ? quiet : loud, true, dt);
        for (const WindStreak& s : edge.Streaks())
            if (s.headX > 1.03f && s.x[0] < 0.95f && Wind::Fade(s) > 0.3f) melting = true;
    }
    if (!melting) return 10;
    float mid = Wind::EdgeFade(0.95f, 0.5f);
    if (Wind::EdgeFade(0.5f, 0.5f) != 1 || Wind::EdgeFade(1.02f, 0.5f) != 0 || Wind::EdgeFade(0.5f, -0.02f) != 0 ||
        !(mid > 0.05f && mid < 0.95f))
        return 11;
    for (int i = 0; i < 30 * 5; ++i) wind.Step(loud, false, dt);  // paused: the loud audio is held
    if (wind.Active()) return 8;
    // The music drives the wind itself, not only its ribbons: a loud moment swells it, and it settles.
    Wind swell(21);
    for (int i = 0; i < 300; ++i) swell.Step(quiet, true, dt);
    float resting = swell.Swell();
    const float burst[3] = {2.0f, 1.0f, 0.5f};  // ten times the quiet level
    for (int i = 0; i < 6; ++i) swell.Step(burst, true, dt);
    if (!(swell.Swell() > resting + 0.05f)) return 12;
    for (int i = 0; i < 120; ++i) swell.Step(quiet, true, dt);
    if (!(swell.Swell() < resting + 0.02f)) return 13;
    Wind a(7), b(7);
    for (int i = 0; i < 60; ++i) {
        a.Step(i % 6 ? quiet : loud, true, dt);
        b.Step(i % 6 ? quiet : loud, true, dt);
    }
    if (a.Streaks().size() != b.Streaks().size()) return 9;
    if (a.Active() && (a.Streaks()[0].headY != b.Streaks()[0].headY || a.Streaks()[0].count != b.Streaks()[0].count))
        return 9;
    return 0;
}
