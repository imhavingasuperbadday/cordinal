// When to give memory back to the system again, shared by both builds (app_memory.cpp on Windows,
// src/linux/memory.cpp on Linux). After a trim, pages come back only when something touches them:
// what the window and playback use within a second or two, then one-off work that stays resident and
// is never touched again. In a normal Windows window that was most of Task Manager's figure:
// startup's unpacked code and its scan (12.7 MB, against 3.4 MB once trimmed), each track change's
// library save (about 2 MB), and, about two minutes after a start, another process reading every
// page of this one (most likely antivirus), which doubled the figure for good.
// TrimWatch is fed that figure every few seconds, keeps the smallest settled one as its floor and
// asks for another trim when the figure stays above it. Each trim is judged half a minute later:
// still down, and the next may come as soon; back up, and those pages are in use, so the wait
// doubles and a set that really grew costs a few trims, not one a minute.
#pragma once
#include <cstddef>
#include <cstdint>

struct TrimWatch {
    static constexpr uint64_t kSettle = 4000;      // playback's pages are back within this of a trim
    static constexpr uint64_t kJudge = 30000;      // a trim that helped is still down this long after
    static constexpr size_t kSlack = 512 * 1024;   // growth that is noise: a passing allocation
    static constexpr size_t kLarge = 4 << 20;      // growth no playing player makes: trim promptly
    static constexpr uint64_t kLargeGap = 10000, kFirstGap = 30000, kMaxGap = 16 * 60000;
    uint64_t trimmedAt = 0, gap = kFirstGap;
    size_t floor = SIZE_MAX;
    size_t trigger = 0;  // the figure the last trim was for, until it is judged
    int over = 0;        // samples in a row above the floor's slack; one alone is a passing allocation
    void Trimmed(uint64_t now) { trimmedAt = now, floor = SIZE_MAX, over = 0; }
    bool Sample(size_t bytes, uint64_t now);  // true: trim now, then call Trimmed
};
