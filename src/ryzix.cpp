/*
 * Ryzix Chess Engine v1.0
 * ~200 ELO UCI chess engine for Android ARM64
 *
 * UCI protocol and architecture patterns inspired by Stockfish
 * (https://github.com/official-stockfish/Stockfish)
 *
 * Compile (native):
 *   clang++ -O2 -std=c++17 src/ryzix.cpp -o ryzix -lm
 *
 * Compile (Android ARM64):
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang++ \
 *     -O2 -DNDEBUG -std=c++17 -static-libstdc++ -lm src/ryzix.cpp -o ryzix
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace Ryzix {

// ================================================================
// Types & Constants
// ================================================================

enum Color : int { WHITE = 0, BLACK = 1, NO_COLOR = 2 };
inline Color operator~(Color c) { return Color(c ^ 1); }

enum PieceType : int {
    NO_PIECE_TYPE = 0,
    PAWN = 1, KNIGHT = 2, BISHOP = 3,
    ROOK = 4, QUEEN  = 5, KING   = 6
};

// Piece encoding:
//   0     = EMPTY
//   1–6   = white (WP WN WB WR WQ WK)
//   7–12  = black (BP BN BB BR BQ BK)
enum Piece : int {
    EMPTY = 0,
    WP=1, WN=2, WB=3, WR=4, WQ=5, WK=6,
    BP=7, BN=8, BB=9, BR=10,BQ=11,BK=12
};

inline Color     colorOf(Piece p) {
    return p == EMPTY ? NO_COLOR : (p <= 6 ? WHITE : BLACK);
}
inline PieceType typeOf(Piece p) {
    return p == EMPTY ? NO_PIECE_TYPE : PieceType((p - 1) % 6 + 1);
}

// Centipawn values — indexed by Piece enum (0-12)
static constexpr int PIECE_VAL[13] = {
    0,
    100, 320, 330, 500, 900, 20000,   // white
    100, 320, 330, 500, 900, 20000    // black
};

// Squares: A1=0 … H8=63  (rank-major: sq = rank*8 + file)
enum Square : int {
    A1=0,B1,C1,D1,E1,F1,G1,H1,
    A2,B2,C2,D2,E2,F2,G2,H2,
    A3,B3,C3,D3,E3,F3,G3,H3,
    A4,B4,C4,D4,E4,F4,G4,H4,
    A5,B5,C5,D5,E5,F5,G5,H5,
    A6,B6,C6,D6,E6,F6,G6,H6,
    A7,B7,C7,D7,E7,F7,G7,H7,
    A8,B8,C8,D8,E8,F8,G8,H8,
    NO_SQ = 64
};

inline int  rankOf(int s)            { return s >> 3; }
inline int  fileOf(int s)            { return s & 7;  }
inline int  mkSq(int r, int f)       { return (r << 3) | f; }
inline bool onBoard(int s)           { return (unsigned)s < 64u; }

// ================================================================
// Move  (16-bit)
//   bits  0– 5  from-square
//   bits  6–11  to-square
//   bits 12–13  move-type   0=normal 1=castle 2=en-passant 3=promotion
//   bits 14–15  promo-piece 0=N 1=B 2=R 3=Q
// ================================================================

struct Move {
    uint16_t data = 0;

    Move() = default;
    Move(int from, int to, int mtype = 0, int promo = 0)
        : data(uint16_t(from | (to << 6) | (mtype << 12) | (promo << 14))) {}

    int  from()  const { return  data & 0x3F;        }
    int  to()    const { return (data >>  6) & 0x3F; }
    int  mtype() const { return (data >> 12) & 0x3;  }
    int  promo() const { return (data >> 14) & 0x3;  }
    bool isNull()const { return data == 0;            }
    bool operator==(const Move& o) const { return data == o.data; }
};

static const Move NULL_MOVE{};

// ================================================================
// Board
// ================================================================

struct UndoInfo {
    int   epSquare;
    int   castling;   // bit0=WK-side, bit1=WQ-side, bit2=BK-side, bit3=BQ-side
    int   halfMove;
    Piece captured;
};

struct Board {
    Piece    sq[64];
    Color    side;
    int      epSquare;
    int      castling;
    int      halfMove;
    int      fullMove;
    int      ply;
    UndoInfo history[512];

    void reset() {
        std::memset(sq, 0, sizeof(sq));
        side = WHITE; epSquare = NO_SQ;
        castling = halfMove = ply = 0;
        fullMove = 1;
    }

    void setFen(const std::string& fen);
    std::string toUci(Move m) const;
    Move        fromUci(const std::string& s) const;
    bool        makeMove(Move m);    // false → illegal (leaves own king in check)
    void        unmakeMove(Move m);
    bool        inCheck()  const;
    bool        isAttacked(int sq, Color by) const;
    int         kingSquare(Color c) const;
};

// ----------------------------------------------------------------
// FEN parsing
// ----------------------------------------------------------------
void Board::setFen(const std::string& fen) {
    reset();
    std::istringstream ss(fen);
    std::string pieces, turn, castle, ep, hm, fm;
    ss >> pieces >> turn >> castle >> ep;

    int rank = 7, file = 0;
    for (char c : pieces) {
        if (c == '/') { rank--; file = 0; continue; }
        if (c >= '1' && c <= '8') { file += c - '0'; continue; }
        int s = mkSq(rank, file);
        switch (c) {
            case 'P': sq[s]=WP; break; case 'N': sq[s]=WN; break;
            case 'B': sq[s]=WB; break; case 'R': sq[s]=WR; break;
            case 'Q': sq[s]=WQ; break; case 'K': sq[s]=WK; break;
            case 'p': sq[s]=BP; break; case 'n': sq[s]=BN; break;
            case 'b': sq[s]=BB; break; case 'r': sq[s]=BR; break;
            case 'q': sq[s]=BQ; break; case 'k': sq[s]=BK; break;
            default: break;
        }
        file++;
    }

    side = (turn == "w") ? WHITE : BLACK;
    castling = 0;
    for (char c : castle) {
        if (c == 'K') castling |= 1;
        if (c == 'Q') castling |= 2;
        if (c == 'k') castling |= 4;
        if (c == 'q') castling |= 8;
    }
    epSquare = NO_SQ;
    if (ep.size() == 2 && ep[0] >= 'a' && ep[0] <= 'h' && (ep[1]=='3'||ep[1]=='6'))
        epSquare = mkSq(ep[1] - '1', ep[0] - 'a');

    if (ss >> hm) halfMove = std::stoi(hm);
    if (ss >> fm) fullMove = std::stoi(fm);
}

// ----------------------------------------------------------------
// UCI move formatting
// ----------------------------------------------------------------
std::string Board::toUci(Move m) const {
    if (m.isNull()) return "0000";
    std::string s;
    s += char('a' + fileOf(m.from()));
    s += char('1' + rankOf(m.from()));
    s += char('a' + fileOf(m.to()));
    s += char('1' + rankOf(m.to()));
    if (m.mtype() == 3) {
        const char* pc = "nbrq";
        s += pc[m.promo()];
    }
    return s;
}

Move Board::fromUci(const std::string& s) const {
    if (s.size() < 4) return NULL_MOVE;
    int from = mkSq(s[1]-'1', s[0]-'a');
    int to   = mkSq(s[3]-'1', s[2]-'a');
    if (!onBoard(from) || !onBoard(to)) return NULL_MOVE;
    if (s.size() >= 5) {
        int promo = 0;
        switch (s[4]) {
            case 'n': promo=0; break; case 'b': promo=1; break;
            case 'r': promo=2; break; case 'q': promo=3; break;
            default: break;
        }
        return Move(from, to, 3, promo);
    }
    return Move(from, to);
}

// ----------------------------------------------------------------
// King square lookup
// ----------------------------------------------------------------
int Board::kingSquare(Color c) const {
    Piece k = (c == WHITE) ? WK : BK;
    for (int i = 0; i < 64; i++)
        if (sq[i] == k) return i;
    return NO_SQ;
}

// ----------------------------------------------------------------
// Attack detection
//   Checks whether square 's' is attacked by any piece of color 'by'.
//   Pattern adapted from Stockfish's isAttacked() / generate_*_attacks().
// ----------------------------------------------------------------
bool Board::isAttacked(int s, Color by) const {
    // ── Pawn attacks ──────────────────────────────────────────
    if (by == WHITE) {
        if (rankOf(s) > 0) {
            if (fileOf(s) > 0 && sq[s-9] == WP) return true;
            if (fileOf(s) < 7 && sq[s-7] == WP) return true;
        }
    } else {
        if (rankOf(s) < 7) {
            if (fileOf(s) > 0 && sq[s+7] == BP) return true;
            if (fileOf(s) < 7 && sq[s+9] == BP) return true;
        }
    }

    // ── Knight attacks ────────────────────────────────────────
    Piece kn = (by == WHITE) ? WN : BN;
    static constexpr int KD[8] = {-17,-15,-10,-6,6,10,15,17};
    for (int d : KD) {
        int t = s + d;
        if (!onBoard(t)) continue;
        if (std::abs(fileOf(t)-fileOf(s)) > 2) continue;
        if (std::abs(rankOf(t)-rankOf(s)) > 2) continue;
        if (sq[t] == kn) return true;
    }

    // ── Diagonal sliders (bishop + queen) ────────────────────
    Piece bi = (by==WHITE) ? WB : BB;
    Piece qu = (by==WHITE) ? WQ : BQ;
    static constexpr int DD[4] = {-9,-7,7,9};
    for (int d : DD) {
        int cur = s;
        while (true) {
            int t = cur + d;
            if (!onBoard(t)) break;
            // Anti-wrap: diagonal step must change both rank and file by 1
            if (std::abs(fileOf(t)-fileOf(cur)) != 1) break;
            if (sq[t] == bi || sq[t] == qu) return true;
            if (sq[t] != EMPTY) break;
            cur = t;
        }
    }

    // ── Straight sliders (rook + queen) ──────────────────────
    Piece ro = (by==WHITE) ? WR : BR;
    int r0 = rankOf(s), f0 = fileOf(s);
    // left
    for (int f=f0-1; f>=0; f--) { Piece p=sq[mkSq(r0,f)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }
    // right
    for (int f=f0+1; f<8;  f++) { Piece p=sq[mkSq(r0,f)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }
    // down
    for (int r=r0-1; r>=0; r--) { Piece p=sq[mkSq(r,f0)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }
    // up
    for (int r=r0+1; r<8;  r++) { Piece p=sq[mkSq(r,f0)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }

    // ── King attacks ──────────────────────────────────────────
    Piece ki = (by==WHITE) ? WK : BK;
    for (int dr=-1; dr<=1; dr++) for (int df=-1; df<=1; df++) {
        if (!dr && !df) continue;
        int r=r0+dr, f=f0+df;
        if (r>=0&&r<8&&f>=0&&f<8 && sq[mkSq(r,f)]==ki) return true;
    }
    return false;
}

bool Board::inCheck() const {
    int ks = kingSquare(side);
    return ks != NO_SQ && isAttacked(ks, ~side);
}

// ----------------------------------------------------------------
// Make move
//   Returns false and reverts if the move leaves our king in check.
// ----------------------------------------------------------------
bool Board::makeMove(Move m) {
    UndoInfo& u = history[ply];
    u.epSquare = epSquare;
    u.castling = castling;
    u.halfMove = halfMove;
    u.captured = EMPTY;

    int   from  = m.from(), to = m.to(), mt = m.mtype();
    Piece moving = sq[from], target = sq[to];
    u.captured = target;

    sq[to] = moving; sq[from] = EMPTY;

    // En-passant capture
    if (mt == 2) {
        int capSq = to + (side == WHITE ? -8 : 8);
        u.captured = sq[capSq];
        sq[capSq]  = EMPTY;
        sq[to]     = moving; // pawn moves to ep square
    }

    // Castling — also move the rook
    if (mt == 1) {
        if      (to == G1) { sq[H1]=EMPTY; sq[F1]=WR; }
        else if (to == C1) { sq[A1]=EMPTY; sq[D1]=WR; }
        else if (to == G8) { sq[H8]=EMPTY; sq[F8]=BR; }
        else if (to == C8) { sq[A8]=EMPTY; sq[D8]=BR; }
    }

    // Promotion — replace pawn with promoted piece
    if (mt == 3) {
        static const Piece WP4[4] = {WN,WB,WR,WQ};
        static const Piece BP4[4] = {BN,BB,BR,BQ};
        sq[to] = (side == WHITE) ? WP4[m.promo()] : BP4[m.promo()];
    }

    // Update en-passant target square
    epSquare = NO_SQ;
    if (typeOf(moving) == PAWN && std::abs(to-from) == 16)
        epSquare = (from + to) / 2;

    // Update castling rights
    if (moving == WK) castling &= ~3;
    if (moving == BK) castling &= ~12;
    if (from==A1||to==A1) castling &= ~2;
    if (from==H1||to==H1) castling &= ~1;
    if (from==A8||to==A8) castling &= ~8;
    if (from==H8||to==H8) castling &= ~4;

    // Fifty-move clock
    halfMove = (typeOf(moving)==PAWN || target!=EMPTY) ? 0 : halfMove+1;

    side = ~side;
    if (side == WHITE) fullMove++;
    ply++;

    // Legality: the side that just moved must not be in check
    Color movedSide = ~side;
    if (isAttacked(kingSquare(movedSide), side)) {
        unmakeMove(m);
        return false;
    }
    return true;
}

// ----------------------------------------------------------------
// Unmake move
// ----------------------------------------------------------------
void Board::unmakeMove(Move m) {
    ply--;
    side = ~side;
    if (side == BLACK) fullMove--;

    UndoInfo& u = history[ply];
    epSquare = u.epSquare;
    castling = u.castling;
    halfMove = u.halfMove;

    int   from = m.from(), to = m.to(), mt = m.mtype();
    Piece moved = sq[to];

    // Restore pawn if it was a promotion
    if (mt == 3) moved = (side == WHITE) ? WP : BP;

    sq[from] = moved;
    sq[to]   = u.captured;

    // Restore en-passant captured pawn
    if (mt == 2) {
        int capSq  = to + (side == WHITE ? -8 : 8);
        sq[capSq]  = (side == WHITE) ? BP : WP;
        sq[to]     = EMPTY;
    }

    // Restore castling rook
    if (mt == 1) {
        if      (to==G1) { sq[F1]=EMPTY; sq[H1]=WR; }
        else if (to==C1) { sq[D1]=EMPTY; sq[A1]=WR; }
        else if (to==G8) { sq[F8]=EMPTY; sq[H8]=BR; }
        else if (to==C8) { sq[D8]=EMPTY; sq[A8]=BR; }
    }
}

// ================================================================
// Move Generation
//   Pseudo-legal move list; legality checked in makeMove().
//   Structure mirrors Stockfish's generate_pawn_moves /
//   generate_moves template pattern.
// ================================================================

struct MoveList {
    Move moves[256];
    int  count = 0;
    void push(int from, int to, int mt=0, int promo=0) {
        moves[count++] = Move(from, to, mt, promo);
    }
};

// Slider move generator: walks in each direction until blocked or board edge.
static void genSliders(const Board& b, MoveList& ml, int s,
                       const int* dirs, int ndirs) {
    Color us = b.side;
    for (int i = 0; i < ndirs; i++) {
        int d = dirs[i], cur = s;
        while (true) {
            int t = cur + d;
            if (!onBoard(t)) break;
            // Anti-wrap for diagonal moves
            if ((d==9||d==-9||d==7||d==-7) && std::abs(fileOf(t)-fileOf(cur))!=1) break;
            // Anti-wrap for horizontal moves
            if (d == 1  && fileOf(t)==0) break;
            if (d == -1 && fileOf(t)==7) break;
            if (b.sq[t] == EMPTY) {
                ml.push(s, t);
            } else {
                if (colorOf(b.sq[t]) != us) ml.push(s, t);
                break;
            }
            cur = t;
        }
    }
}

static void generatePseudoMoves(const Board& b, MoveList& ml) {
    Color us = b.side, them = ~us;
    static constexpr int DIAG[4]     = {-9,-7, 7, 9};
    static constexpr int STRAIGHT[4] = {-8,-1, 1, 8};
    static constexpr int QUEEN_D[8]  = {-9,-8,-7,-1, 1, 7, 8, 9};
    static constexpr int KNIGHT_D[8] = {-17,-15,-10,-6,6,10,15,17};

    int pushDir   = (us == WHITE) ?  8 : -8;
    int startRank = (us == WHITE) ?  1  :  6;
    int promRank  = (us == WHITE) ?  7  :  0;
    Piece myPawn  = (us == WHITE) ? WP  : BP;

    for (int s = 0; s < 64; s++) {
        Piece p = b.sq[s];
        if (p == EMPTY || colorOf(p) != us) continue;

        switch (typeOf(p)) {

        case PAWN: {
            int r = rankOf(s), f = fileOf(s);
            int fwd = s + pushDir;
            // Forward push (quiet)
            if (onBoard(fwd) && b.sq[fwd] == EMPTY) {
                if (rankOf(fwd) == promRank) {
                    for (int pr=0; pr<4; pr++) ml.push(s, fwd, 3, pr);
                } else {
                    ml.push(s, fwd);
                    // Double push from start rank
                    if (r == startRank) {
                        int fwd2 = fwd + pushDir;
                        if (b.sq[fwd2] == EMPTY) ml.push(s, fwd2);
                    }
                }
            }
            // Diagonal captures (including en-passant)
            for (int df : {-1, 1}) {
                if (f+df < 0 || f+df > 7) continue;
                int t = fwd + df;
                if (!onBoard(t)) continue;
                bool isCapture = (b.sq[t] != EMPTY && colorOf(b.sq[t]) == them);
                bool isEP      = (t == b.epSquare);
                if (isCapture || isEP) {
                    if (rankOf(t) == promRank) {
                        for (int pr=0; pr<4; pr++) ml.push(s, t, 3, pr);
                    } else {
                        ml.push(s, t, isEP ? 2 : 0);
                    }
                }
            }
            break;
        }

        case KNIGHT:
            for (int d : KNIGHT_D) {
                int t = s + d;
                if (!onBoard(t)) continue;
                if (std::abs(fileOf(t)-fileOf(s))>2) continue;
                if (std::abs(rankOf(t)-rankOf(s))>2) continue;
                if (b.sq[t]==EMPTY || colorOf(b.sq[t])!=us) ml.push(s, t);
            }
            break;

        case BISHOP: genSliders(b, ml, s, DIAG,     4); break;
        case ROOK:   genSliders(b, ml, s, STRAIGHT, 4); break;
        case QUEEN:  genSliders(b, ml, s, QUEEN_D,  8); break;

        case KING: {
            int r0=rankOf(s), f0=fileOf(s);
            for (int dr=-1; dr<=1; dr++) for (int df=-1; df<=1; df++) {
                if (!dr && !df) continue;
                int r2=r0+dr, f2=f0+df;
                if (r2<0||r2>7||f2<0||f2>7) continue;
                int t = mkSq(r2, f2);
                if (b.sq[t]==EMPTY || colorOf(b.sq[t])!=us) ml.push(s, t);
            }
            // King-side castling
            if (us==WHITE && s==E1) {
                if ((b.castling&1) && b.sq[F1]==EMPTY && b.sq[G1]==EMPTY
                    && !b.isAttacked(E1,BLACK)
                    && !b.isAttacked(F1,BLACK)
                    && !b.isAttacked(G1,BLACK))
                    ml.push(E1, G1, 1);
                if ((b.castling&2) && b.sq[D1]==EMPTY && b.sq[C1]==EMPTY && b.sq[B1]==EMPTY
                    && !b.isAttacked(E1,BLACK)
                    && !b.isAttacked(D1,BLACK)
                    && !b.isAttacked(C1,BLACK))
                    ml.push(E1, C1, 1);
            } else if (us==BLACK && s==E8) {
                if ((b.castling&4) && b.sq[F8]==EMPTY && b.sq[G8]==EMPTY
                    && !b.isAttacked(E8,WHITE)
                    && !b.isAttacked(F8,WHITE)
                    && !b.isAttacked(G8,WHITE))
                    ml.push(E8, G8, 1);
                if ((b.castling&8) && b.sq[D8]==EMPTY && b.sq[C8]==EMPTY && b.sq[B8]==EMPTY
                    && !b.isAttacked(E8,WHITE)
                    && !b.isAttacked(D8,WHITE)
                    && !b.isAttacked(C8,WHITE))
                    ml.push(E8, C8, 1);
            }
            break;
        }
        default: break;
        }
    }
}

// Filter pseudo-legal → legal by test-making each move
static std::vector<Move> legalMoves(Board& b) {
    MoveList ml;
    generatePseudoMoves(b, ml);
    std::vector<Move> legal;
    legal.reserve(ml.count);
    for (int i = 0; i < ml.count; i++) {
        if (b.makeMove(ml.moves[i])) {
            legal.push_back(ml.moves[i]);
            b.unmakeMove(ml.moves[i]);
        }
    }
    return legal;
}

// ================================================================
// Evaluation — simple material count + basic pawn PST
//   Returns score in centipawns from the side-to-move's perspective.
//   Inspired by Stockfish evaluate.cpp's material blending approach.
// ================================================================

static int evaluate(const Board& b) {
    // Pawn piece-square table (White perspective, A1=0 orientation)
    static constexpr int PST_PAWN[64] = {
         0,  0,  0,  0,  0,  0,  0,  0,   // rank 1
         5, 10, 10,-20,-20, 10, 10,  5,   // rank 2
         5, -5,-10,  0,  0,-10, -5,  5,   // rank 3
         0,  0,  0, 20, 20,  0,  0,  0,   // rank 4
         5,  5, 10, 25, 25, 10,  5,  5,   // rank 5
        10, 10, 20, 30, 30, 20, 10, 10,   // rank 6
        50, 50, 50, 50, 50, 50, 50, 50,   // rank 7
         0,  0,  0,  0,  0,  0,  0,  0    // rank 8
    };

    int score = 0;
    for (int s = 0; s < 64; s++) {
        Piece p = b.sq[s];
        if (p == EMPTY) continue;
        int v = PIECE_VAL[p];
        if (typeOf(p) == PAWN) {
            // PST index: white uses sq directly; black mirrors vertically
            int pstIdx = (colorOf(p)==WHITE) ? s : (56 - rankOf(s)*8 + fileOf(s));
            v += PST_PAWN[pstIdx] / 5;
        }
        if (colorOf(p) == WHITE) score += v;
        else                     score -= v;
    }
    return (b.side == WHITE) ? score : -score;
}

// ================================================================
// Search — ~200 ELO
//
//   Design: depth-1 material-biased move ordering with ±2000 cp
//   random noise.  High noise means even blunder-free positions are
//   often mishandled, targeting beginner-level (~200 ELO) strength.
//
//   Outputs N PV lines for MultiPV compatibility with A1Chess.
// ================================================================

static std::mt19937 RNG(
    uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()));

static int noise(int lo, int hi) {
    return std::uniform_int_distribution<int>(lo, hi)(RNG);
}

// Crude move score for ordering (no deep analysis)
static int moveScore(const Board& b, Move m) {
    int s = 0;
    Piece cap = b.sq[m.to()];
    if (cap != EMPTY)    s += PIECE_VAL[cap] * 8;     // reward captures
    if (m.mtype() == 3)  s += 800 * (m.promo() + 1);  // reward promotions
    return s;
}

struct PVLine {
    Move             best;
    int              score;     // centipawns, side-to-move perspective
    std::vector<Move> pv;
};

// Compute up to 'multiPV' independent PV lines.
static std::vector<PVLine> search(Board& b, int multiPV) {
    auto moves = legalMoves(b);
    if (moves.empty()) return {};

    struct SM { Move m; int score; };
    std::vector<SM> scored;
    scored.reserve(moves.size());

    for (auto& m : moves) {
        // Add ±2000 cp noise — this is what limits strength to ~200 ELO
        int s = moveScore(b, m) + noise(-2000, 2000);
        scored.push_back({m, s});
    }
    std::sort(scored.begin(), scored.end(),
              [](const SM& a, const SM& b){ return a.score > b.score; });

    int n = std::min((int)scored.size(), multiPV);
    std::vector<PVLine> result;
    result.reserve(n);

    for (int i = 0; i < n; i++) {
        Move m = scored[i].m;
        if (!b.makeMove(m)) continue;   // shouldn't happen — already legal

        // 1-ply eval (opponent's perspective → negate for us)
        int score = -evaluate(b);

        // Build a 2-move PV: our move + a "reasonable" opponent reply
        std::vector<Move> pv = {m};
        auto replies = legalMoves(b);
        if (!replies.empty()) {
            // Pick best opponent reply by heuristic (with small noise)
            Move bestReply = replies[0];
            int  bestRS    = INT_MIN;
            for (auto& r : replies) {
                int rs = moveScore(b, r) + noise(-200, 200);
                if (rs > bestRS) { bestRS = rs; bestReply = r; }
            }
            pv.push_back(bestReply);
        }
        b.unmakeMove(m);
        result.push_back({m, score, pv});
    }

    // Sort final results by score (best first)
    std::sort(result.begin(), result.end(),
              [](const PVLine& a, const PVLine& b){ return a.score > b.score; });
    return result;
}

// ================================================================
// UCI Loop
//   Implements the Universal Chess Interface protocol.
//   Compatible with A1Chess Android (uses same UCI command set
//   as Stockfish: uci / isready / position / go / stop / quit).
//   Reference: https://backscattering.de/chess/uci/
// ================================================================

static const std::string START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

static void uciLoop() {
    Board board;
    board.setFen(START_FEN);
    int multiPV = 3;

    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string cmd;
        ss >> cmd;

        // ── uci ───────────────────────────────────────────────
        if (cmd == "uci") {
            std::cout
                << "id name Ryzix 1.0\n"
                << "id author RD7890\n"
                << "option name MultiPV type spin default 3 min 1 max 10\n"
                << "option name Skill Level type spin default 0 min 0 max 20\n"
                << "option name Threads type spin default 1 min 1 max 1\n"
                << "uciok\n" << std::flush;
        }

        // ── isready ───────────────────────────────────────────
        else if (cmd == "isready") {
            std::cout << "readyok\n" << std::flush;
        }

        // ── ucinewgame ────────────────────────────────────────
        else if (cmd == "ucinewgame") {
            board.setFen(START_FEN);
        }

        // ── setoption ─────────────────────────────────────────
        //   A1Chess sends:
        //     setoption name MultiPV value 3
        //     setoption name Skill Level value N
        //     setoption name Threads value 1
        else if (cmd == "setoption") {
            std::string rest;
            std::getline(ss, rest);
            auto npos = rest.find("name ");
            auto vpos = rest.find(" value ");
            if (npos != std::string::npos) {
                std::string name  = (vpos != std::string::npos)
                                    ? rest.substr(npos+5, vpos-npos-5)
                                    : rest.substr(npos+5);
                std::string value = (vpos != std::string::npos)
                                    ? rest.substr(vpos+7) : "";
                // trim whitespace
                while (!name.empty()  && name.front()==' ')  name.erase(0,1);
                while (!name.empty()  && name.back()==' ')   name.pop_back();
                while (!value.empty() && value.front()==' ') value.erase(0,1);
                while (!value.empty() && value.back()==' ')  value.pop_back();

                if (name == "MultiPV" && !value.empty())
                    multiPV = std::max(1, std::stoi(value));
                // "Skill Level" and "Threads" accepted silently
            }
        }

        // ── position ──────────────────────────────────────────
        //   position startpos [moves ...]
        //   position fen <fen> [moves ...]
        else if (cmd == "position") {
            std::string type;
            ss >> type;

            if (type == "fen") {
                std::string rest;
                std::getline(ss, rest);
                while (!rest.empty() && rest.front()==' ') rest.erase(0,1);
                auto mpos = rest.find(" moves ");
                std::string fenStr = (mpos!=std::string::npos)
                                     ? rest.substr(0, mpos) : rest;
                if (!fenStr.empty()) board.setFen(fenStr);
                else                 board.setFen(START_FEN);
                if (mpos != std::string::npos) {
                    std::istringstream ms(rest.substr(mpos+7));
                    std::string mv;
                    while (ms >> mv) {
                        Move m = board.fromUci(mv);
                        if (!m.isNull()) board.makeMove(m);
                    }
                }
            } else {   // startpos
                board.setFen(START_FEN);
                std::string tok;
                if ((ss >> tok) && tok == "moves") {
                    std::string mv;
                    while (ss >> mv) {
                        Move m = board.fromUci(mv);
                        if (!m.isNull()) board.makeMove(m);
                    }
                }
            }
        }

        // ── go ────────────────────────────────────────────────
        //   Ryzix is synchronous (instant reply).
        //   Outputs N multipv info lines then bestmove.
        //   A1Chess parses:  info ... multipv N score cp X pv <moves>
        else if (cmd == "go") {
            // movetime / wtime / btime parsed but ignored — always instant
            auto pvLines = search(board, multiPV);

            if (pvLines.empty()) {
                // No legal moves: checkmate or stalemate
                std::cout << "bestmove (none)\n" << std::flush;
                continue;
            }

            for (int i = 0; i < (int)pvLines.size(); i++) {
                auto& pv = pvLines[i];
                std::cout << "info depth 1 seldepth 1"
                          << " multipv " << (i+1)
                          << " score cp " << pv.score
                          << " nodes 100 nps 100000 time 1 pv";
                for (auto& mv : pv.pv)
                    std::cout << ' ' << board.toUci(mv);
                std::cout << '\n';
            }
            std::cout << "bestmove " << board.toUci(pvLines[0].best) << "\n"
                      << std::flush;
        }

        // ── stop ──────────────────────────────────────────────
        //   A1Chess sends stop before isready when restarting analysis.
        //   Since we are synchronous there is nothing to interrupt.
        else if (cmd == "stop") {
            // nothing to do
        }

        // ── quit ──────────────────────────────────────────────
        else if (cmd == "quit") {
            break;
        }
    }
}

} // namespace Ryzix

int main() {
    Ryzix::uciLoop();
    return 0;
}
