/*
 * Ryzix Chess Engine
 * types.h — Core types, enums, constants, and Move struct.
 *
 * Architecture inspired by Stockfish's modular design.
 * No NNUE — pure classical hand-crafted evaluation.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>

namespace Ryzix {

// ── Colors ───────────────────────────────────────────────────────
enum Color : int { WHITE = 0, BLACK = 1, NO_COLOR = 2 };
inline Color operator~(Color c) { return Color(c ^ 1); }

// ── Piece types ──────────────────────────────────────────────────
enum PieceType : int {
    NO_PIECE_TYPE = 0,
    PAWN = 1, KNIGHT = 2, BISHOP = 3,
    ROOK = 4, QUEEN  = 5, KING   = 6
};

// ── Pieces ───────────────────────────────────────────────────────
enum Piece : int {
    EMPTY = 0,
    WP=1,  WN=2,  WB=3,  WR=4,  WQ=5,  WK=6,
    BP=7,  BN=8,  BB=9,  BR=10, BQ=11, BK=12
};

inline Color     colorOf(Piece p) { return p == EMPTY ? NO_COLOR : (p <= 6 ? WHITE : BLACK); }
inline PieceType typeOf (Piece p) { return p == EMPTY ? NO_PIECE_TYPE : PieceType((p-1)%6+1); }

// ── Squares ──────────────────────────────────────────────────────
enum Square : int {
    A1=0, B1, C1, D1, E1, F1, G1, H1,
    A2,   B2, C2, D2, E2, F2, G2, H2,
    A3,   B3, C3, D3, E3, F3, G3, H3,
    A4,   B4, C4, D4, E4, F4, G4, H4,
    A5,   B5, C5, D5, E5, F5, G5, H5,
    A6,   B6, C6, D6, E6, F6, G6, H6,
    A7,   B7, C7, D7, E7, F7, G7, H7,
    A8,   B8, C8, D8, E8, F8, G8, H8,
    NO_SQ = 64
};

inline int  rankOf(int s)      { return s >> 3; }
inline int  fileOf(int s)      { return s & 7; }
inline int  mkSq(int r, int f) { return (r << 3) | f; }
inline bool onBoard(int s)     { return static_cast<unsigned>(s) < 64u; }

// ── Constants ────────────────────────────────────────────────────
static constexpr int INF        = 32000;
static constexpr int MATE_SCORE = 31000;
static constexpr int MATE_BOUND = 30000;
static constexpr int MAX_PLY    = 128;

// ── Material values (SEE + basic eval) ───────────────────────────
static constexpr int SEE_VAL[7] = { 0, 100, 300, 300, 500, 900, 20000 };

// ── Move (16-bit packed) ─────────────────────────────────────────
// bits  0-5  : from square
// bits  6-11 : to square
// bits 12-13 : move type  (0=normal, 1=castle, 2=en-passant, 3=promotion)
// bits 14-15 : promo piece (0=N, 1=B, 2=R, 3=Q)
struct Move {
    uint16_t data = 0;

    Move() = default;
    Move(int from, int to, int mtype = 0, int promo = 0)
        : data(static_cast<uint16_t>(from | (to << 6) | (mtype << 12) | (promo << 14))) {}

    int  from()   const { return  data        & 0x3F; }
    int  to()     const { return (data >>  6) & 0x3F; }
    int  mtype()  const { return (data >> 12) & 0x03; }
    int  promo()  const { return (data >> 14) & 0x03; }
    bool isNull() const { return data == 0; }

    bool operator==(const Move& o) const { return data == o.data; }
    bool operator!=(const Move& o) const { return data != o.data; }

    static Move fromRaw(uint16_t d) { Move m; m.data = d; return m; }
};

static constexpr Move NULL_MOVE{};

// ── Zobrist keys (defined in position.cpp) ───────────────────────
extern uint64_t ZPIECE[13][64];
extern uint64_t ZSIDE;
extern uint64_t ZEP[8];
extern uint64_t ZCASTLE[16];

void initZobrist();

} // namespace Ryzix
