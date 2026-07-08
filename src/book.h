/*
 * Ryzix Chess Engine
 * book.h — Opening book.
 *
 * Ryzix's personality:
 *   White: London System (d4, Nf3, Bf4, e3, Bd3, c3)
 *   Black: Caro-Kann Defence vs 1.e4; solid d5/e6/Nf6 vs 1.d4
 *
 * Must be initialised after initZobrist() via bookInit().
 */
#pragma once
#include "position.h"
#include <string>

namespace Ryzix {

// Populate the book hash map. Must be called after initZobrist().
void bookInit();

// Probe the opening book for the current position.
// Returns a UCI move string (e.g. "d2d4") or "" if out of book.
std::string bookProbe(const Board& b);

} // namespace Ryzix
