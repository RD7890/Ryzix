/*
 * Ryzix Chess Engine
 * tt.cpp — Transposition table implementation.
 *
 * 4M entries × 12 bytes ≈ 48 MB.
 * One entry per hash bucket (direct mapping).
 * Replacement policy: prefer same key (deeper search),
 * different key (stale), or exact bound.
 */
#include "tt.h"
#include <cstring>

namespace Ryzix {

static constexpr size_t TT_SIZE = 1u << 22;   // 4 194 304 entries
static TTEntry TT[TT_SIZE];
int8_t TT_AGE = 0;

void ttClear() {
    std::memset(TT, 0, sizeof(TT));
    TT_AGE = 0;
}

TTEntry* ttProbe(uint64_t hash) {
    return &TT[hash & (TT_SIZE - 1)];
}

void ttStore(uint64_t hash, Move m, int score, int depth, Bound bound, int ply) {
    TTEntry& e = TT[hash & (TT_SIZE - 1)];

    // Adjust mate scores for ply distance
    if (score >= MATE_BOUND)  score += ply;
    if (score <= -MATE_BOUND) score -= ply;

    // Replace if: different key, same key but deeper, stale age, or exact bound
    bool replace = (e.key != hash)
                || (e.depth <= depth)
                || (e.age  != TT_AGE)
                || (bound  == BOUND_EXACT);
    if (!replace) return;

    e.key   = hash;
    if (!m.isNull() || e.key != hash) e.move = m.data;
    e.score = static_cast<int16_t>(score);
    e.depth = static_cast<int8_t>(depth);
    e.bound = static_cast<uint8_t>(bound);
    e.age   = TT_AGE;
}

int ttScore(int rawScore, int ply) {
    if (rawScore >= MATE_BOUND)  return rawScore - ply;
    if (rawScore <= -MATE_BOUND) return rawScore + ply;
    return rawScore;
}

} // namespace Ryzix
