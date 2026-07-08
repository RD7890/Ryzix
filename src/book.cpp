/*
 * Ryzix Chess Engine
 * book.cpp — Opening book.
 *
 * Personality:
 *   White: London System (d4 → Nf3 → Bf4 → e3 → Bd3 → c3 → 0-0)
 *   Black vs 1.e4: Caro-Kann Defence
 *   Black vs 1.d4: solid QGD setup (d5 → e6 → Nf6)
 *   Black vs other: d5
 *
 * Initialisation: bookInit() replays each move sequence on a scratch
 * Board and records (Zobrist hash → UCI move) in an unordered_map.
 * The hash already encodes side-to-move, so White and Black entries
 * coexist without conflict.
 */
#include "book.h"
#include "position.h"
#include <unordered_map>
#include <sstream>
#include <cstring>

namespace Ryzix {

// ── Book storage ─────────────────────────────────────────────────
static std::unordered_map<uint64_t, std::string> gBookMap;

// ── Opening lines ────────────────────────────────────────────────
// Each entry: { "space-separated moves to reach position", "book reply" }
// "" means the starting position.
static const struct { const char* seq; const char* mv; } BOOK_LINES[] = {

    // ═══════════════════════════════════════════════
    //  LONDON SYSTEM — White's personality
    // ═══════════════════════════════════════════════

    // ── Move 1: always d4 ──────────────────────────
    {"", "d2d4"},

    // ── Move 2: Nf3 (regardless of Black reply) ────
    {"d2d4 d7d5", "g1f3"},
    {"d2d4 g8f6", "g1f3"},
    {"d2d4 e7e6", "g1f3"},
    {"d2d4 c7c5", "g1f3"},
    {"d2d4 g7g6", "g1f3"},
    {"d2d4 f7f5", "g1f3"},
    {"d2d4 b7b6", "g1f3"},
    {"d2d4 c7c6", "g1f3"},
    {"d2d4 b8c6", "g1f3"},
    {"d2d4 a7a5", "g1f3"},
    {"d2d4 a7a6", "g1f3"},
    {"d2d4 b7b5", "g1f3"},
    {"d2d4 h7h6", "g1f3"},
    {"d2d4 h7h5", "g1f3"},

    // ── Move 3: Bf4 (London bishop) ────────────────
    // After 1.d4 d5 2.Nf3 ...
    {"d2d4 d7d5 g1f3 g8f6", "c1f4"},
    {"d2d4 d7d5 g1f3 e7e6", "c1f4"},
    {"d2d4 d7d5 g1f3 c7c5", "c1f4"},
    {"d2d4 d7d5 g1f3 c7c6", "c1f4"},
    {"d2d4 d7d5 g1f3 b8c6", "c1f4"},
    {"d2d4 d7d5 g1f3 b7b6", "c1f4"},
    {"d2d4 d7d5 g1f3 a7a6", "c1f4"},
    {"d2d4 d7d5 g1f3 a7a5", "c1f4"},
    {"d2d4 d7d5 g1f3 f7f5", "c1f4"},
    {"d2d4 d7d5 g1f3 g7g6", "c1f4"},
    {"d2d4 d7d5 g1f3 h7h6", "c1f4"},
    // If Black plays Bf5 early against the London, play e3
    {"d2d4 d7d5 g1f3 c8f5", "e2e3"},
    // After 1.d4 Nf6 2.Nf3 ...
    {"d2d4 g8f6 g1f3 d7d5", "c1f4"},
    {"d2d4 g8f6 g1f3 e7e6", "c1f4"},
    {"d2d4 g8f6 g1f3 g7g6", "c1f4"},
    {"d2d4 g8f6 g1f3 c7c5", "c1f4"},
    {"d2d4 g8f6 g1f3 b7b6", "c1f4"},
    {"d2d4 g8f6 g1f3 c7c6", "c1f4"},
    // After 1.d4 e6 2.Nf3 ...
    {"d2d4 e7e6 g1f3 g8f6", "c1f4"},
    {"d2d4 e7e6 g1f3 d7d5", "c1f4"},
    {"d2d4 e7e6 g1f3 c7c5", "c1f4"},
    {"d2d4 e7e6 g1f3 b7b6", "c1f4"},
    // After 1.d4 c5 2.Nf3 ...
    {"d2d4 c7c5 g1f3 g8f6", "c1f4"},
    {"d2d4 c7c5 g1f3 e7e6", "c1f4"},
    {"d2d4 c7c5 g1f3 d7d5", "c1f4"},
    // After 1.d4 g6 2.Nf3 ...
    {"d2d4 g7g6 g1f3 g8f6", "c1f4"},
    {"d2d4 g7g6 g1f3 d7d5", "c1f4"},

    // ── Move 4: e3 ─────────────────────────────────
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6", "e2e3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 c7c5", "e2e3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 c7c6", "e2e3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 b7b6", "e2e3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 g7g6", "e2e3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 b8d7", "e2e3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 f8d6", "e2e3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 a7a6", "e2e3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 h7h6", "e2e3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 g8f6", "e2e3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 c7c5", "e2e3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 b8d7", "e2e3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 f8d6", "e2e3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 c7c6", "e2e3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 b7b6", "e2e3"},
    {"d2d4 d7d5 g1f3 c7c5 c1f4 g8f6", "e2e3"},
    {"d2d4 d7d5 g1f3 c7c5 c1f4 e7e6", "e2e3"},
    {"d2d4 d7d5 g1f3 c7c5 c1f4 b8c6", "e2e3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 e7e6", "e2e3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 c7c5", "e2e3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 c7c6", "e2e3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 b8d7", "e2e3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 f8f5", "e2e3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 a7a6", "e2e3"},
    {"d2d4 g8f6 g1f3 e7e6 c1f4 d7d5", "e2e3"},
    {"d2d4 g8f6 g1f3 e7e6 c1f4 c7c5", "e2e3"},
    {"d2d4 g8f6 g1f3 e7e6 c1f4 b7b6", "e2e3"},
    {"d2d4 g8f6 g1f3 c7c5 c1f4 e7e6", "e2e3"},
    {"d2d4 g8f6 g1f3 g7g6 c1f4 d7d5", "e2e3"},
    {"d2d4 e7e6 g1f3 g8f6 c1f4 d7d5", "e2e3"},
    {"d2d4 e7e6 g1f3 g8f6 c1f4 c7c5", "e2e3"},
    {"d2d4 e7e6 g1f3 d7d5 c1f4 g8f6", "e2e3"},
    {"d2d4 e7e6 g1f3 d7d5 c1f4 c7c5", "e2e3"},

    // ── Move 5: Bd3 ────────────────────────────────
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 f8d6", "f1d3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 c7c5", "f1d3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 b8d7", "f1d3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 c7c6", "f1d3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 b8c6", "f1d3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 a7a6", "f1d3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 h7h6", "f1d3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 c7c5 e2e3 b8c6", "f1d3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 c7c5 e2e3 e7e6", "f1d3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 c7c5 e2e3 d5d4", "f1d3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 c7c6 e2e3 e7e6", "f1d3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 c7c6 e2e3 b8d7", "f1d3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 g8f6 e2e3 f8d6", "f1d3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 g8f6 e2e3 c7c5", "f1d3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 g8f6 e2e3 b8d7", "f1d3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 e7e6 e2e3 f8d6", "f1d3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 e7e6 e2e3 c7c5", "f1d3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 e7e6 e2e3 b8d7", "f1d3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 e7e6 e2e3 c7c6", "f1d3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 c7c6 e2e3 e7e6", "f1d3"},
    {"d2d4 g8f6 g1f3 d7d5 c1f4 b8d7 e2e3 e7e6", "f1d3"},

    // ── Move 6: c3 (solidify London center) ────────
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 f8d6 f1d3 e8g8", "c2c3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 f8d6 f1d3 c7c5", "c2c3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 f8d6 f1d3 b8d7", "c2c3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 b8d7 f1d3 f8d6", "c2c3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 b8d7 f1d3 e8g8", "c2c3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 c7c5 f1d3 b8c6", "c2c3"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 c7c5 f1d3 b8d7", "c2c3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 g8f6 e2e3 f8d6 f1d3 e8g8", "c2c3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 g8f6 e2e3 f8d6 f1d3 c7c5", "c2c3"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 g8f6 e2e3 b8d7 f1d3 e8g8", "c2c3"},

    // ── Move 7: castle ─────────────────────────────
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 f8d6 f1d3 e8g8 c2c3 b8d7",  "e1g1"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 f8d6 f1d3 e8g8 c2c3 c7c5",  "e1g1"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 f8d6 f1d3 e8g8 c2c3 b7b6",  "e1g1"},
    {"d2d4 d7d5 g1f3 g8f6 c1f4 e7e6 e2e3 b8d7 f1d3 f8d6 c2c3 e8g8",  "e1g1"},
    {"d2d4 d7d5 g1f3 e7e6 c1f4 g8f6 e2e3 f8d6 f1d3 e8g8 c2c3 b8d7",  "e1g1"},

    // ═══════════════════════════════════════════════
    //  CARO-KANN — Black's personality vs 1.e4
    // ═══════════════════════════════════════════════

    // Move 1: c6 (Caro-Kann)
    {"e2e4", "c7c6"},

    // Move 2: d5
    {"e2e4 c7c6 d2d4", "d7d5"},
    {"e2e4 c7c6 d2d3", "d7d5"},
    {"e2e4 c7c6 g1f3", "d7d5"},
    {"e2e4 c7c6 b1c3", "d7d5"},
    {"e2e4 c7c6 f2f4", "d7d5"},

    // Main lines after 1.e4 c6 2.d4 d5
    // Advance variation (3.e5): Bf5
    {"e2e4 c7c6 d2d4 d7d5 e4e5", "c8f5"},
    // Classical (3.Nd2): dxe4
    {"e2e4 c7c6 d2d4 d7d5 b1d2", "d5e4"},
    // Main line (3.Nc3): dxe4
    {"e2e4 c7c6 d2d4 d7d5 b1c3", "d5e4"},
    // Exchange (3.exd5): cxd5
    {"e2e4 c7c6 d2d4 d7d5 e4d5", "c6d5"},
    // Fantasy (3.f3): dxe4
    {"e2e4 c7c6 d2d4 d7d5 f2f3", "d5e4"},
    // Two Knights (3.Nc3 with Nf3): Bg4
    {"e2e4 c7c6 d2d4 d7d5 b1c3 g8f6", "c8g4"},

    // After 3.Nd2 dxe4 4.Nxe4: Bf5 (Classical Caro-Kann)
    {"e2e4 c7c6 d2d4 d7d5 b1d2 d5e4 d2e4", "c8f5"},
    // After 3.Nc3 dxe4 4.Nxe4: Bf5
    {"e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4", "c8f5"},

    // After 4.Ne4 Bf5 5.Ng3: Bg6
    {"e2e4 c7c6 d2d4 d7d5 b1d2 d5e4 d2e4 c8f5 e4g3", "f5g6"},
    {"e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5 e4g3", "f5g6"},

    // After 4.Ne4 Bf5 5.Ng5: Nf6
    {"e2e4 c7c6 d2d4 d7d5 b1d2 d5e4 d2e4 c8f5 e4g5", "g8f6"},
    {"e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5 e4g5", "g8f6"},

    // After Ng3 Bg6 6.h4: h6
    {"e2e4 c7c6 d2d4 d7d5 b1d2 d5e4 d2e4 c8f5 e4g3 f5g6 h2h4", "h7h6"},
    {"e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5 e4g3 f5g6 h2h4", "h7h6"},

    // After Bg6 h4 h6 7.Nf3: Nd7
    {"e2e4 c7c6 d2d4 d7d5 b1d2 d5e4 d2e4 c8f5 e4g3 f5g6 h2h4 h7h6 g1f3", "b8d7"},
    {"e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5 e4g3 f5g6 h2h4 h7h6 g1f3", "b8d7"},

    // Advance variation follow-ups
    {"e2e4 c7c6 d2d4 d7d5 e4e5 c8f5 g1f3", "e7e6"},
    {"e2e4 c7c6 d2d4 d7d5 e4e5 c8f5 b1c3", "e7e6"},
    {"e2e4 c7c6 d2d4 d7d5 e4e5 c8f5 c2c4", "e7e6"},
    {"e2e4 c7c6 d2d4 d7d5 e4e5 c8f5 g1f3 e7e6 f1e2", "c7c5"},
    {"e2e4 c7c6 d2d4 d7d5 e4e5 c8f5 g1f3 e7e6 c1e3", "g8e7"},
    {"e2e4 c7c6 d2d4 d7d5 e4e5 c8f5 g1f3 e7e6 f1d3", "f5d3"},
    {"e2e4 c7c6 d2d4 d7d5 e4e5 c8f5 g1f3 e7e6 f1e2 c7c5 c2c3", "b8c6"},

    // Exchange + follow-ups
    {"e2e4 c7c6 d2d4 d7d5 e4d5 c6d5 c2c4", "g8f6"},
    {"e2e4 c7c6 d2d4 d7d5 e4d5 c6d5 g1f3", "g8f6"},
    {"e2e4 c7c6 d2d4 d7d5 e4d5 c6d5 b1c3", "g8f6"},
    {"e2e4 c7c6 d2d4 d7d5 e4d5 c6d5 c2c4 g8f6 b1c3", "e7e6"},

    // ═══════════════════════════════════════════════
    //  vs 1.d4 — QGD / solid d5 setup
    // ═══════════════════════════════════════════════

    // Move 1: d5
    {"d2d4", "d7d5"},

    // vs QGD main: d5 e6 Nf6
    {"d2d4 d7d5 c2c4", "e7e6"},
    {"d2d4 d7d5 g1f3", "g8f6"},
    {"d2d4 d7d5 c1f4", "g8f6"},
    {"d2d4 d7d5 e2e3", "g8f6"},
    {"d2d4 d7d5 b1c3", "g8f6"},
    {"d2d4 d7d5 e2e4", "d5e4"},
    {"d2d4 d7d5 g2g3", "g8f6"},
    {"d2d4 d7d5 c2c3", "g8f6"},

    // After 2.c4 e6
    {"d2d4 d7d5 c2c4 e7e6 b1c3", "g8f6"},
    {"d2d4 d7d5 c2c4 e7e6 g1f3", "g8f6"},
    {"d2d4 d7d5 c2c4 e7e6 b1d2", "g8f6"},
    {"d2d4 d7d5 c2c4 e7e6 c4d5", "e6d5"},
    {"d2d4 d7d5 c2c4 e7e6 e2e3", "g8f6"},

    // After 2.c4 e6 3.Nc3 Nf6: Be7 (solid QGD)
    {"d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5", "f8e7"},
    {"d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 g1f3", "f8e7"},
    {"d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 e2e3", "f8e7"},

    // After 2.c4 e6 3.Nf3 Nf6 4.Nc3: Be7
    {"d2d4 d7d5 c2c4 e7e6 g1f3 g8f6 b1c3", "f8e7"},
    {"d2d4 d7d5 c2c4 e7e6 g1f3 g8f6 c1g5", "f8e7"},
    {"d2d4 d7d5 c2c4 e7e6 g1f3 g8f6 e2e3", "f8b4"},

    // After QGD: castle
    {"d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5 f8e7 g1f3", "e8g8"},
    {"d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 g1f3 f8e7 c1g5", "e8g8"},
    {"d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 g1f3 f8e7 e2e3", "e8g8"},

    // ═══════════════════════════════════════════════
    //  vs other first moves as Black
    // ═══════════════════════════════════════════════
    {"g1f3", "d7d5"},
    {"c2c4", "e7e6"},
    {"f2f4", "d7d5"},
    {"g2g3", "d7d5"},
    {"b2b3", "d7d5"},
    {"b2b4", "e7e5"},
    {"a2a4", "d7d5"},
    {"h2h4", "d7d5"},
    {"e2e3", "d7d5"},
    {"d2d3", "e7e5"},
    {"c2c3", "e7e5"},
    {"b1c3", "e7e5"},

    // vs 1.Nf3: d5, then Nf6 after various White moves
    {"g1f3 d7d5 d2d4", "g8f6"},
    {"g1f3 d7d5 c2c4", "e7e6"},
    {"g1f3 d7d5 g2g3", "g8f6"},

    // vs 1.c4 e6: continue with d5 or Nf6
    {"c2c4 e7e6 d2d4", "d7d5"},
    {"c2c4 e7e6 g1f3", "d7d5"},
    {"c2c4 e7e6 b1c3", "d7d5"},
};

// ── bookInit ─────────────────────────────────────────────────────
void bookInit() {
    gBookMap.clear();
    constexpr int N = sizeof(BOOK_LINES) / sizeof(BOOK_LINES[0]);

    for (int i = 0; i < N; i++) {
        Board tmp;
        tmp.setFen(START_FEN);
        tmp.gameHashCount = 0;

        // Replay the move sequence to reach the target position
        const std::string seq(BOOK_LINES[i].seq);
        if (!seq.empty()) {
            std::istringstream ss(seq);
            std::string mv;
            while (ss >> mv) {
                Move m = tmp.fromUci(mv);
                if (m.isNull()) break;
                tmp.gameHashes[tmp.gameHashCount++] = tmp.hash;
                if (!tmp.makeMove(m)) break;  // illegal move in sequence
            }
        }
        // Record position hash → book reply (first entry wins)
        if (gBookMap.find(tmp.hash) == gBookMap.end()) {
            gBookMap[tmp.hash] = BOOK_LINES[i].mv;
        }
    }
}

// ── bookProbe ────────────────────────────────────────────────────
std::string bookProbe(const Board& b) {
    auto it = gBookMap.find(b.hash);
    if (it == gBookMap.end()) return "";
    return it->second;
}

} // namespace Ryzix
