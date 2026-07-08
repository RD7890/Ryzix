/*
 * Ryzix Chess Engine
 * timeman.h — Time management.
 *
 * Converts UCI time controls (wtime/btime/inc/movestogo/movetime/infinite)
 * into a soft limit (target) and hard limit (maximum) in milliseconds.
 * Modelled after Stockfish's time manager.
 */
#pragma once
#include "types.h"

namespace Ryzix {

struct TimeControl {
    int softLimitMs;  // search aims to finish by here
    int hardLimitMs;  // search must abort by here
};

// Compute time allocation for the current position.
// myTime: remaining time for the side to move (ms)
// myInc:  increment per move (ms)
// movestogo: moves until next time control (0 = sudden death)
// movetime: fixed time per move (-1 = not specified)
// isInfinite: true if "go infinite"
TimeControl calcTime(int myTime, int myInc, int movestogo,
                     int movetime, bool isInfinite);

} // namespace Ryzix
