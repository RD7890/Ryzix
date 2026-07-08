/*
 * Ryzix Chess Engine
 * timeman.cpp — Time management implementation.
 *
 * Converts the UCI time-control parameters into a soft and hard
 * time limit for the search.
 *
 * Soft limit: the search may complete the current depth and stop.
 * Hard limit: the search must abort immediately.
 */
#include "timeman.h"
#include <algorithm>

namespace Ryzix {

TimeControl calcTime(int myTime, int myInc, int movestogo,
                     int movetime, bool isInfinite)
{
    TimeControl tc;

    if (isInfinite || movetime <= 0 && myTime <= 0) {
        // Infinite search: give 1 hour budget
        tc.softLimitMs = tc.hardLimitMs = 3'600'000;
        return tc;
    }

    if (movetime > 0) {
        // Fixed time per move
        tc.softLimitMs = tc.hardLimitMs = std::max(10, movetime - 30);
        return tc;
    }

    if (myTime <= 0) myTime = 5000;

    int mtg = (movestogo > 0) ? movestogo : 25;

    // Base allocation: time/movestogo + most of the increment
    int base = myTime / mtg + myInc * 3 / 4;

    // Soft: aim to finish inside this budget
    tc.softLimitMs = std::max(50, std::min(base, myTime / 3));

    // Hard: absolute ceiling — never exceed 1/4 of remaining or 3× soft
    tc.hardLimitMs = std::max(tc.softLimitMs,
                              std::min(myTime / 4, base * 3));

    // Low-time safety net
    if (myTime < 1000) {
        tc.softLimitMs = std::max(50, myTime / 20);
        tc.hardLimitMs = tc.softLimitMs * 2;
    }

    return tc;
}

} // namespace Ryzix
