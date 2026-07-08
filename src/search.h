/*
 * Ryzix Chess Engine
 * search.h — Iterative-deepening PVS (Principal Variation Search).
 *
 * Search features:
 *   Iterative deepening with aspiration windows
 *   Check extension  (+1 ply)
 *   Singular extensions (SE)
 *   Null-move pruning (R = 3 + depth/3)
 *   ProbCut
 *   Late-Move Reductions (LMR, log-table)
 *   Internal Iterative Reduction (IIR)
 *   Futility pruning (depth ≤ 8)
 *   Reverse Futility Pruning (RFP)
 *   SEE pruning for quiet moves
 *   Quiescence search with delta pruning
 *   Repetition detection / 50-move rule
 *   Killer moves (2 per ply)
 *   Counter-move table
 *   Quiet + capture history heuristics
 *
 * Modelled after Stockfish's search.h.
 */
#pragma once
#include "position.h"
#include "tt.h"
#include <chrono>
#include <vector>

namespace Ryzix {

// ── Search state (per search instance) ───────────────────────────
struct SearchState {
    Move      killers[MAX_PLY][2];
    int       history[13][64];           // quiet move history [piece][to]
    int       captureHistory[13][64][7]; // capture history [piece][to][captType]
    int       counterMove[13][64];       // counter-move table [piece][to]
    int       staticEval[MAX_PLY];       // static eval at each search ply
    long long nodes;
    int       ply;
    bool      stop;

    std::chrono::steady_clock::time_point startTime;
    int timeLimitMs;     // soft limit
    int maxTimeLimitMs;  // hard limit

    void reset();

    static int clamp_hist(int v, int limit = 16384) {
        return v < -limit ? -limit : (v > limit ? limit : v);
    }

    // Soft time check: used in the outer iterative-deepening loop.
    bool timeUp();
    // Hard time check: used inside alphaBeta to prevent overrun.
    bool innerTimeUp();
};

// ── PV line (one search result) ──────────────────────────────────
struct PVLine {
    Move best;
    int  score;
    int  depth;
};

// ── LMR reduction table (precomputed log formula) ─────────────────
extern int LMR_TABLE[MAX_PLY][64];
void initLMR();

// ── Root search ──────────────────────────────────────────────────
// Runs iterative-deepening PVS with Multi-PV support.
// Returns up to multiPV PV lines sorted by score descending.
std::vector<PVLine> rootSearch(Board& b,
                               int timeLimitMs,
                               int maxTimeLimitMs,
                               int multiPV,
                               int maxDepthLimit = 64);

} // namespace Ryzix
