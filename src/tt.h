/*
 * Ryzix Chess Engine
 * tt.h — Transposition Table.
 *
 * 4M entries (~48 MB). Each entry stores:
 *   key (64-bit Zobrist), best move, score, depth, bound type, age.
 * Replacement policy: prefer same position, deeper search, or new age.
 */
#pragma once
#include "types.h"

namespace Ryzix {

enum Bound : uint8_t {
    BOUND_NONE  = 0,
    BOUND_UPPER = 1,   // alpha — score is an upper bound
    BOUND_LOWER = 2,   // beta  — score is a lower bound (fail-high)
    BOUND_EXACT = 3    // PV node — exact score
};

struct TTEntry {
    uint64_t key   = 0;
    uint16_t move  = 0;
    int16_t  score = 0;
    int8_t   depth = -1;
    uint8_t  bound = BOUND_NONE;
    int8_t   age   = 0;
};

extern int8_t TT_AGE;

void     ttClear();
TTEntry* ttProbe(uint64_t hash);
void     ttStore(uint64_t hash, Move m, int score, int depth, Bound bound, int ply);
int      ttScore(int rawScore, int ply);

} // namespace Ryzix
