/*
 * Ryzix Chess Engine
 * uci.h — Universal Chess Interface protocol.
 *
 * Implements the full UCI protocol: uci, isready, ucinewgame,
 * position, go, stop, quit, setoption.
 */
#pragma once

namespace Ryzix {

// Enter the UCI command loop (blocks until "quit").
void uciLoop();

} // namespace Ryzix
