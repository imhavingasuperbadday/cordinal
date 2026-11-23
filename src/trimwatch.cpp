// TrimWatch (trimwatch.h): when the memory figure has stayed above its floor long enough to trim again.
#include "trimwatch.h"

#include <algorithm>

bool TrimWatch::Sample(size_t bytes, uint64_t now) {
    if (!bytes || now - trimmedAt < kSettle) return false;
    if (trigger && now - trimmedAt >= kJudge) {
        gap = bytes + kSlack >= trigger ? std::min(gap * 2, kMaxGap) : kFirstGap;
        trigger = 0;
    }
    if (bytes <= floor) {
        floor = bytes;
        over = 0;
        return false;
    }
    if (bytes <= floor + kSlack) {
        over = 0;
        return false;
    }
    if (++over < 2) return false;
    const bool large = bytes > floor + kLarge;
    if (now - trimmedAt < (large ? kLargeGap : gap)) return false;
    trigger = bytes;
    return true;
}
