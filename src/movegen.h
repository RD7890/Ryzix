/*
 * Ryzix Chess Engine
 * movegen.h — Pseudo-legal and legal move generation.
 *
 * Modelled after Stockfish's movegen.h.
 * MoveList holds up to 256 moves per position (more than sufficient).
 */
#pragma once
#include "position.h"
#include <vector>

namespace Ryzix {

// ── Move list (stack-allocated for speed) ────────────────────────
struct MoveList {
    Move moves[256];
    int  count = 0;

    void push(int from, int to, int mt = 0, int promo = 0) {
        moves[count++] = Move(from, to, mt, promo);
    }
};

// Generate all pseudo-legal moves for the side to move.
void generatePseudoMoves(const Board& b, MoveList& ml);

// Return only legal moves (filters pseudo-legal for king safety).
std::vector<Move> legalMoves(Board& b);

} // namespace Ryzix
