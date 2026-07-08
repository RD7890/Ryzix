/*
 * Ryzix Chess Engine
 * evaluate.h — Classical hand-crafted evaluation. NO NNUE.
 *
 * Features:
 *   PeSTO piece-square tables (MG + EG, phase-tapered)
 *   Mobility (pseudo-legal move count per piece type)
 *   Pawn structure (doubled, isolated, passed pawns)
 *   Knight outpost bonuses
 *   Bishop pair bonus
 *   Rook on open/semi-open file, rook on 7th rank
 *   King safety (pawn shield, attacker weights, open files)
 *   Tempo bonus
 */
#pragma once
#include "position.h"

namespace Ryzix {

// Returns evaluation in centipawns from the side-to-move's perspective.
int evaluate(const Board& b);

} // namespace Ryzix
