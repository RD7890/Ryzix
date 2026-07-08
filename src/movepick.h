/*
 * Ryzix Chess Engine
 * movepick.h — Move ordering and Static Exchange Evaluation.
 *
 * Move ordering priority (high → low):
 *   TT move > good captures (SEE ≥ 0, MVV-LVA) > promotions >
 *   killers > counter-move > quiet history > bad captures (SEE < 0)
 *
 * Modelled after Stockfish's movepick.h.
 */
#pragma once
#include "position.h"
#include "movegen.h"

namespace Ryzix {

// Forward declaration of SearchState (defined in search.h)
struct SearchState;

// Static Exchange Evaluation.
// Estimates material gain/loss of a capture sequence on square 'to'.
int see(const Board& b, int to, Piece target, int from, Piece moving);

// Score for a single move (used for sorting).
int moveScore(const Board& b, Move m, Move ttMove,
              const SearchState& ss, Move prevMove);

// Sort all moves in ml by descending score (insertion sort — fast for N≤40).
void sortMoves(MoveList& ml, const Board& b, Move ttMove,
               const SearchState& ss, Move prevMove);

} // namespace Ryzix
