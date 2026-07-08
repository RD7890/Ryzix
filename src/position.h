/*
 * Ryzix Chess Engine
 * position.h — Board state representation.
 *
 * Modelled after Stockfish's Position class.
 * Stores piece placement, side to move, castling rights,
 * en-passant square, half/full move counters, Zobrist hash,
 * and per-ply undo information.
 */
#pragma once
#include "types.h"
#include <string>

namespace Ryzix {

// ── Undo information saved before each make-move ─────────────────
struct UndoInfo {
    int      epSquare;
    int      castling;
    int      halfMove;
    Piece    captured;
    uint64_t hash;
};

// ── Board ────────────────────────────────────────────────────────
struct Board {
    Piece    sq[64];          // piece on each square
    Color    side;            // side to move
    int      epSquare;        // en-passant target square (or NO_SQ)
    int      castling;        // 4-bit: 1=WK, 2=WQ, 4=BK, 8=BQ
    int      halfMove;        // half-move clock (50-move rule)
    int      fullMove;        // full move number
    int      ply;             // search ply (incremented by makeMove)
    uint64_t hash;            // incremental Zobrist hash

    // Undo stack for search (extra headroom past MAX_PLY for game history)
    UndoInfo history[MAX_PLY * 2 + 16];

    // Game-level hash history for repetition detection
    uint64_t gameHashes[1024];
    int      gameHashCount;

    // ── Lifecycle ─────────────────────────────────────────────────
    void reset();
    void computeHash();
    void setFen(const std::string& fen);

    // ── Move encoding / decoding ──────────────────────────────────
    std::string toUci(Move m) const;
    Move        fromUci(const std::string& s) const;

    // ── Move make / unmake ────────────────────────────────────────
    bool makeMove(Move m);          // returns false if leaves king in check
    void unmakeMove(Move m);

    // ── Queries ───────────────────────────────────────────────────
    bool inCheck() const;
    bool isAttacked(int sq, Color by) const;
    int  kingSquare(Color c) const;
    bool isRepetition(int searchPly) const;
};

// Starting position FEN
extern const std::string START_FEN;

} // namespace Ryzix
