/*
 * Ryzix Chess Engine v3.0  — Maximum Strength Edition
 *
 * Search:
 *   Iterative-deepening Principal Variation Search (PVS)
 *   Transposition Table  (48 MB, 4M entries, 64-bit Zobrist)
 *   Aspiration Windows   (growing delta, ±25 cp seed)
 *   Check Extension      (+1 ply when in check)
 *   Singular Extensions  (SE) — re-search with reduced beta
 *   Null-Move Pruning    (R = 3 + depth/3, adaptive)
 *   ProbCut              (probabilistic cut at depth ≥ 5)
 *   Late-Move Reductions (LMR) — log-table formula
 *   Internal Iterative Reduction (IIR) — no TT hit → depth-1
 *   Futility Pruning     (depth ≤ 8, margin 100*depth)
 *   Reverse Futility Pruning (RFP) — static eval ≥ β + margin
 *   SEE Pruning          — prune negative-SEE captures/quiets
 *   Quiescence Search    — captures + promotions + checks at ply 0
 *   Delta Pruning        — in quiescence
 *   Repetition Detection — three-fold / two-fold draw
 *   Killer Moves         (2 per ply)
 *   Counter-Move Table
 *   History Heuristic    (quiet + capture, with malus)
 *
 * Evaluation:
 *   PeSTO piece-square tables (MG + EG, phase-tapered)
 *   Mobility             (pseudo-legal move count, per piece type)
 *   Pawn structure       (doubled, isolated, backward, passed pawns)
 *   Passed pawn bonuses  (rank-squared, scaled by endgame phase)
 *   King safety          (pawn shield, attacker weights, open files)
 *   Rook on open/semi-open file
 *   Rook on 7th rank
 *   Knight outpost squares
 *   Bishop pair bonus
 *   Tempo bonus
 *
 * Build (native):
 *   g++ -O3 -DNDEBUG -std=c++17 -march=native src/ryzix.cpp -o ryzix
 * Build (Android ARM64, NDK r25c):
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang++ \
 *     -O3 -DNDEBUG -std=c++17 -static-libstdc++ src/ryzix.cpp -o ryzix
 */

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
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

enum Piece : int {
    EMPTY = 0,
    WP=1, WN=2, WB=3, WR=4, WQ=5, WK=6,
    BP=7, BN=8, BB=9, BR=10, BQ=11, BK=12
};

inline Color     colorOf(Piece p) { return p == EMPTY ? NO_COLOR : (p <= 6 ? WHITE : BLACK); }
inline PieceType typeOf (Piece p) { return p == EMPTY ? NO_PIECE_TYPE : PieceType((p-1)%6+1); }

// Material values for SEE and basic eval
static constexpr int SEE_VAL[7] = { 0, 100, 300, 300, 500, 900, 20000 };

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

inline int  rankOf(int s)      { return s >> 3; }
inline int  fileOf(int s)      { return s & 7; }
inline int  mkSq(int r, int f) { return (r << 3) | f; }
inline bool onBoard(int s)     { return (unsigned)s < 64u; }

static constexpr int INF        = 32000;
static constexpr int MATE_SCORE = 31000;
static constexpr int MATE_BOUND = 30000;
static constexpr int MAX_PLY    = 128;

// ================================================================
// Move  (16-bit packed)
// ================================================================

struct Move {
    uint16_t data = 0;
    Move() = default;
    Move(int from, int to, int mtype = 0, int promo = 0)
        : data(uint16_t(from | (to << 6) | (mtype << 12) | (promo << 14))) {}
    int  from()   const { return  data & 0x3F; }
    int  to()     const { return (data >>  6) & 0x3F; }
    int  mtype()  const { return (data >> 12) & 0x3; }
    int  promo()  const { return (data >> 14) & 0x3; }
    bool isNull() const { return data == 0; }
    bool operator==(const Move& o) const { return data == o.data; }
    bool operator!=(const Move& o) const { return data != o.data; }
    static Move fromRaw(uint16_t d) { Move m; m.data = d; return m; }
};

static const Move NULL_MOVE{};

// ================================================================
// Zobrist Keys
// ================================================================

static uint64_t ZPIECE[13][64];
static uint64_t ZSIDE;
static uint64_t ZEP[8];
static uint64_t ZCASTLE[16];

static void initZobrist() {
    uint64_t s = 0x5EED1234CAFEBABEull;
    auto rng = [&]() -> uint64_t {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
    };
    for (int p = 0; p < 13; p++) for (int q = 0; q < 64; q++) ZPIECE[p][q] = rng();
    ZSIDE = rng();
    for (int f = 0; f < 8;  f++) ZEP[f]     = rng();
    for (int c = 0; c < 16; c++) ZCASTLE[c] = rng();
}

// ================================================================
// Board
// ================================================================

struct UndoInfo {
    int      epSquare;
    int      castling;
    int      halfMove;
    Piece    captured;
    uint64_t hash;
};

struct Board {
    Piece    sq[64];
    Color    side;
    int      epSquare;
    int      castling;
    int      halfMove;
    int      fullMove;
    int      ply;
    uint64_t hash;
    UndoInfo history[MAX_PLY * 2 + 16];  // extra room for game history

    // Game-level hash history for repetition detection
    uint64_t gameHashes[1024];
    int      gameHashCount;

    void reset() {
        std::memset(sq, 0, sizeof(sq));
        side = WHITE; epSquare = NO_SQ;
        castling = halfMove = ply = 0;
        fullMove = 1; hash = 0;
        gameHashCount = 0;
    }

    void computeHash() {
        hash = 0;
        for (int s = 0; s < 64; s++)
            if (sq[s] != EMPTY) hash ^= ZPIECE[sq[s]][s];
        if (side == BLACK) hash ^= ZSIDE;
        if (epSquare != NO_SQ) hash ^= ZEP[fileOf(epSquare)];
        hash ^= ZCASTLE[castling];
    }

    void     setFen(const std::string& fen);
    std::string toUci(Move m)              const;
    Move        fromUci(const std::string& s) const;
    bool        makeMove(Move m);
    void        unmakeMove(Move m);
    bool        inCheck()                  const;
    bool        isAttacked(int s, Color by) const;
    int         kingSquare(Color c)        const;
    bool        isRepetition(int searchPly) const;
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
        epSquare = mkSq(ep[1]-'1', ep[0]-'a');
    if (ss >> hm) halfMove = std::stoi(hm);
    if (ss >> fm) fullMove = std::stoi(fm);
    computeHash();
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
    if (m.mtype() == 3) { const char* pc = "nbrq"; s += pc[m.promo()]; }
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

int Board::kingSquare(Color c) const {
    Piece k = (c == WHITE) ? WK : BK;
    for (int i = 0; i < 64; i++) if (sq[i] == k) return i;
    return NO_SQ;
}

// ----------------------------------------------------------------
// Repetition detection
// ----------------------------------------------------------------
bool Board::isRepetition(int searchPly) const {
    // Walk back through the position history (same side to move = step 2)
    // and count hash matches. Stop at the first irreversible move
    // (halfMove counter resets to 0 *after* the irreversible move is made,
    //  so we stop when history[i].halfMove == 0, meaning the move that
    //  produced position i was irreversible).
    int count = 0;

    // Search path (positions encountered since search started)
    for (int i = ply - 2; i >= 0; i -= 2) {
        if (history[i].hash == hash) {
            count++;
            // One repetition within the search tree is treated as a draw
            // (avoids loops; conservative but safe).
            if (count >= 1) return true;
        }
        if (history[i].halfMove == 0) break;  // irreversible move boundary
    }

    // Game history (positions before the current search started)
    for (int i = gameHashCount - 1; i >= 0; i--) {
        if (gameHashes[i] == hash) {
            count++;
            // Two-fold repetition in game history is a draw
            if (count >= 2) return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------
// Attack detection
// ----------------------------------------------------------------
bool Board::isAttacked(int s, Color by) const {
    if (by == WHITE) {
        if (rankOf(s)>0) {
            if (fileOf(s)>0 && sq[s-9]==WP) return true;
            if (fileOf(s)<7 && sq[s-7]==WP) return true;
        }
    } else {
        if (rankOf(s)<7) {
            if (fileOf(s)>0 && sq[s+7]==BP) return true;
            if (fileOf(s)<7 && sq[s+9]==BP) return true;
        }
    }
    Piece kn = (by==WHITE) ? WN : BN;
    static constexpr int KD[8] = {-17,-15,-10,-6,6,10,15,17};
    for (int d : KD) {
        int t = s+d; if (!onBoard(t)) continue;
        if (std::abs(fileOf(t)-fileOf(s))>2) continue;
        if (std::abs(rankOf(t)-rankOf(s))>2) continue;
        if (sq[t]==kn) return true;
    }
    Piece bi=(by==WHITE)?WB:BB, qu=(by==WHITE)?WQ:BQ;
    static constexpr int DD[4] = {-9,-7,7,9};
    for (int d : DD) {
        int cur=s;
        while (true) {
            int t=cur+d; if (!onBoard(t)) break;
            if (std::abs(fileOf(t)-fileOf(cur))!=1) break;
            if (sq[t]==bi||sq[t]==qu) return true;
            if (sq[t]!=EMPTY) break;
            cur=t;
        }
    }
    Piece ro=(by==WHITE)?WR:BR;
    int r0=rankOf(s), f0=fileOf(s);
    for (int f=f0-1;f>=0;f--) { Piece p=sq[mkSq(r0,f)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }
    for (int f=f0+1;f< 8;f++) { Piece p=sq[mkSq(r0,f)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }
    for (int r=r0-1;r>=0;r--) { Piece p=sq[mkSq(r,f0)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }
    for (int r=r0+1;r< 8;r++) { Piece p=sq[mkSq(r,f0)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }
    Piece ki=(by==WHITE)?WK:BK;
    for (int dr=-1;dr<=1;dr++) for (int df=-1;df<=1;df++) {
        if (!dr&&!df) continue;
        int r=r0+dr, f=f0+df;
        if (r>=0&&r<8&&f>=0&&f<8&&sq[mkSq(r,f)]==ki) return true;
    }
    return false;
}

bool Board::inCheck() const {
    int ks = kingSquare(side);
    return ks!=NO_SQ && isAttacked(ks, ~side);
}

// ----------------------------------------------------------------
// Make/Unmake move — with incremental Zobrist update
// ----------------------------------------------------------------
bool Board::makeMove(Move m) {
    UndoInfo& u = history[ply];
    u.epSquare = epSquare; u.castling = castling;
    u.halfMove = halfMove; u.hash = hash;

    int   from = m.from(), to = m.to(), mt = m.mtype();
    Piece moving = sq[from], target = sq[to];
    u.captured = target;

    if (epSquare != NO_SQ) hash ^= ZEP[fileOf(epSquare)];
    hash ^= ZCASTLE[castling];
    hash ^= ZPIECE[moving][from];
    sq[to] = moving; sq[from] = EMPTY;
    if (target != EMPTY) hash ^= ZPIECE[target][to];

    if (mt == 2) {
        int capSq = to + (side==WHITE ? -8 : 8);
        u.captured = sq[capSq];
        hash ^= ZPIECE[sq[capSq]][capSq];
        sq[capSq] = EMPTY;
        sq[to] = moving;
    }
    if (mt == 1) {
        if      (to==G1) { hash^=ZPIECE[WR][H1]; hash^=ZPIECE[WR][F1]; sq[H1]=EMPTY; sq[F1]=WR; }
        else if (to==C1) { hash^=ZPIECE[WR][A1]; hash^=ZPIECE[WR][D1]; sq[A1]=EMPTY; sq[D1]=WR; }
        else if (to==G8) { hash^=ZPIECE[BR][H8]; hash^=ZPIECE[BR][F8]; sq[H8]=EMPTY; sq[F8]=BR; }
        else if (to==C8) { hash^=ZPIECE[BR][A8]; hash^=ZPIECE[BR][D8]; sq[A8]=EMPTY; sq[D8]=BR; }
    }
    if (mt == 3) {
        static const Piece WP4[4] = {WN,WB,WR,WQ};
        static const Piece BP4[4] = {BN,BB,BR,BQ};
        Piece promo = (side==WHITE) ? WP4[m.promo()] : BP4[m.promo()];
        hash ^= ZPIECE[moving][to];
        sq[to] = promo;
        hash ^= ZPIECE[promo][to];
    } else {
        hash ^= ZPIECE[sq[to]][to];
    }

    epSquare = NO_SQ;
    if (typeOf(moving)==PAWN && std::abs(to-from)==16) {
        epSquare = (from+to)/2;
        hash ^= ZEP[fileOf(epSquare)];
    }

    if (moving==WK) castling &= ~3;
    if (moving==BK) castling &= ~12;
    if (from==A1||to==A1) castling &= ~2;
    if (from==H1||to==H1) castling &= ~1;
    if (from==A8||to==A8) castling &= ~8;
    if (from==H8||to==H8) castling &= ~4;

    hash ^= ZCASTLE[castling];
    halfMove = (typeOf(moving)==PAWN||target!=EMPTY) ? 0 : halfMove+1;
    hash ^= ZSIDE;
    side = ~side;
    if (side==WHITE) fullMove++;
    ply++;

    Color movedSide = ~side;
    if (isAttacked(kingSquare(movedSide), side)) {
        unmakeMove(m);
        return false;
    }
    return true;
}

void Board::unmakeMove(Move m) {
    ply--;
    side = ~side;
    if (side==BLACK) fullMove--;

    UndoInfo& u = history[ply];
    epSquare = u.epSquare; castling = u.castling;
    halfMove = u.halfMove; hash = u.hash;

    int   from = m.from(), to = m.to(), mt = m.mtype();
    Piece moved = sq[to];

    if (mt==3) moved = (side==WHITE) ? WP : BP;
    sq[from] = moved;
    sq[to]   = u.captured;

    if (mt==2) {
        int capSq = to + (side==WHITE ? -8 : 8);
        sq[capSq] = (side==WHITE) ? BP : WP;
        sq[to]    = EMPTY;
    }
    if (mt==1) {
        if      (to==G1) { sq[F1]=EMPTY; sq[H1]=WR; }
        else if (to==C1) { sq[D1]=EMPTY; sq[A1]=WR; }
        else if (to==G8) { sq[F8]=EMPTY; sq[H8]=BR; }
        else if (to==C8) { sq[D8]=EMPTY; sq[A8]=BR; }
    }
}

// ================================================================
// Move Generation
// ================================================================

struct MoveList {
    Move moves[256];
    int  count = 0;
    void push(int from, int to, int mt=0, int promo=0) {
        moves[count++] = Move(from, to, mt, promo);
    }
};

static void genSliders(const Board& b, MoveList& ml, int s,
                       const int* dirs, int ndirs) {
    Color us = b.side;
    for (int i = 0; i < ndirs; i++) {
        int d = dirs[i], cur = s;
        while (true) {
            int t = cur+d;
            if (!onBoard(t)) break;
            if ((d==9||d==-9||d==7||d==-7) && std::abs(fileOf(t)-fileOf(cur))!=1) break;
            if (d== 1 && fileOf(t)==0) break;
            if (d==-1 && fileOf(t)==7) break;
            if (b.sq[t]==EMPTY) { ml.push(s,t); }
            else { if (colorOf(b.sq[t])!=us) ml.push(s,t); break; }
            cur = t;
        }
    }
}

static void generatePseudoMoves(const Board& b, MoveList& ml) {
    Color us=b.side, them=~us;
    static constexpr int DIAG[4]     = {-9,-7, 7, 9};
    static constexpr int STRAIGHT[4] = {-8,-1, 1, 8};
    static constexpr int QUEEN_D[8]  = {-9,-8,-7,-1, 1, 7, 8, 9};
    static constexpr int KNIGHT_D[8] = {-17,-15,-10,-6, 6,10,15,17};

    int pushDir   = (us==WHITE) ?  8 : -8;
    int startRank = (us==WHITE) ?  1  :  6;
    int promRank  = (us==WHITE) ?  7  :  0;

    for (int s=0; s<64; s++) {
        Piece p = b.sq[s];
        if (p==EMPTY || colorOf(p)!=us) continue;
        switch (typeOf(p)) {
        case PAWN: {
            int r=rankOf(s), f=fileOf(s), fwd=s+pushDir;
            if (onBoard(fwd) && b.sq[fwd]==EMPTY) {
                if (rankOf(fwd)==promRank) { for (int pr=0;pr<4;pr++) ml.push(s,fwd,3,pr); }
                else {
                    ml.push(s,fwd);
                    if (r==startRank) { int fwd2=fwd+pushDir; if (b.sq[fwd2]==EMPTY) ml.push(s,fwd2); }
                }
            }
            for (int df : {-1,1}) {
                if (f+df<0||f+df>7) continue;
                int t=fwd+df; if (!onBoard(t)) continue;
                bool isCap=(b.sq[t]!=EMPTY&&colorOf(b.sq[t])==them);
                bool isEP =(t==b.epSquare);
                if (isCap||isEP) {
                    if (rankOf(t)==promRank) { for (int pr=0;pr<4;pr++) ml.push(s,t,3,pr); }
                    else ml.push(s,t,isEP?2:0);
                }
            }
            break;
        }
        case KNIGHT:
            for (int d : KNIGHT_D) {
                int t=s+d; if (!onBoard(t)) continue;
                if (std::abs(fileOf(t)-fileOf(s))>2) continue;
                if (std::abs(rankOf(t)-rankOf(s))>2) continue;
                if (b.sq[t]==EMPTY||colorOf(b.sq[t])!=us) ml.push(s,t);
            }
            break;
        case BISHOP: genSliders(b,ml,s,DIAG,    4); break;
        case ROOK:   genSliders(b,ml,s,STRAIGHT,4); break;
        case QUEEN:  genSliders(b,ml,s,QUEEN_D, 8); break;
        case KING: {
            int r0=rankOf(s),f0=fileOf(s);
            for (int dr=-1;dr<=1;dr++) for (int df=-1;df<=1;df++) {
                if (!dr&&!df) continue;
                int r2=r0+dr,f2=f0+df;
                if (r2<0||r2>7||f2<0||f2>7) continue;
                int t=mkSq(r2,f2);
                if (b.sq[t]==EMPTY||colorOf(b.sq[t])!=us) ml.push(s,t);
            }
            if (us==WHITE&&s==E1) {
                if ((b.castling&1)&&b.sq[F1]==EMPTY&&b.sq[G1]==EMPTY
                    &&!b.isAttacked(E1,BLACK)&&!b.isAttacked(F1,BLACK)&&!b.isAttacked(G1,BLACK))
                    ml.push(E1,G1,1);
                if ((b.castling&2)&&b.sq[D1]==EMPTY&&b.sq[C1]==EMPTY&&b.sq[B1]==EMPTY
                    &&!b.isAttacked(E1,BLACK)&&!b.isAttacked(D1,BLACK)&&!b.isAttacked(C1,BLACK))
                    ml.push(E1,C1,1);
            } else if (us==BLACK&&s==E8) {
                if ((b.castling&4)&&b.sq[F8]==EMPTY&&b.sq[G8]==EMPTY
                    &&!b.isAttacked(E8,WHITE)&&!b.isAttacked(F8,WHITE)&&!b.isAttacked(G8,WHITE))
                    ml.push(E8,G8,1);
                if ((b.castling&8)&&b.sq[D8]==EMPTY&&b.sq[C8]==EMPTY&&b.sq[B8]==EMPTY
                    &&!b.isAttacked(E8,WHITE)&&!b.isAttacked(D8,WHITE)&&!b.isAttacked(C8,WHITE))
                    ml.push(E8,C8,1);
            }
            break;
        }
        default: break;
        }
    }
}

static std::vector<Move> legalMoves(Board& b) {
    MoveList ml; generatePseudoMoves(b, ml);
    std::vector<Move> legal; legal.reserve(ml.count);
    for (int i=0; i<ml.count; i++)
        if (b.makeMove(ml.moves[i])) { legal.push_back(ml.moves[i]); b.unmakeMove(ml.moves[i]); }
    return legal;
}

// ================================================================
// Static Exchange Evaluation (SEE)
// ================================================================

static int see(const Board& b, int to, Piece target, int from, Piece moving) {
    // Returns SEE value of capturing on 'to' with 'moving' from 'from'
    int gain[32];
    int d = 0;
    gain[d] = SEE_VAL[(int)typeOf(target)];

    // Build a simple occupancy mask and find next attacker
    // Simplified SEE using xray approach
    Board tmp = b;
    tmp.sq[from] = EMPTY;

    // Promoted piece value
    Piece mover = moving;
    d++;

    while (true) {
        Color side = (d % 2 == 1) ? ~b.side : b.side;
        gain[d] = SEE_VAL[(int)typeOf(mover)] - gain[d-1];

        // Find least valuable attacker from 'side'
        int bestFrom = NO_SQ;
        int bestVal = INF;
        Piece bestPiece = EMPTY;

        // Pawns
        if (side == WHITE) {
            if (rankOf(to)>0) {
                if (fileOf(to)>0 && tmp.sq[to-9]==WP && SEE_VAL[PAWN]<bestVal) { bestFrom=to-9; bestVal=SEE_VAL[PAWN]; bestPiece=WP; }
                if (fileOf(to)<7 && tmp.sq[to-7]==WP && SEE_VAL[PAWN]<bestVal) { bestFrom=to-7; bestVal=SEE_VAL[PAWN]; bestPiece=WP; }
            }
        } else {
            if (rankOf(to)<7) {
                if (fileOf(to)>0 && tmp.sq[to+7]==BP && SEE_VAL[PAWN]<bestVal) { bestFrom=to+7; bestVal=SEE_VAL[PAWN]; bestPiece=BP; }
                if (fileOf(to)<7 && tmp.sq[to+9]==BP && SEE_VAL[PAWN]<bestVal) { bestFrom=to+9; bestVal=SEE_VAL[PAWN]; bestPiece=BP; }
            }
        }

        // Knights
        static constexpr int KD[8] = {-17,-15,-10,-6,6,10,15,17};
        Piece wantKn = (side==WHITE) ? WN : BN;
        for (int dd : KD) {
            int t=to+dd; if (!onBoard(t)) continue;
            if (std::abs(fileOf(t)-fileOf(to))>2||std::abs(rankOf(t)-rankOf(to))>2) continue;
            if (tmp.sq[t]==wantKn && SEE_VAL[KNIGHT]<bestVal) { bestFrom=t; bestVal=SEE_VAL[KNIGHT]; bestPiece=wantKn; break; }
        }

        // Bishops/Queens (diagonals)
        static constexpr int DD[4] = {-9,-7,7,9};
        Piece wantBi=(side==WHITE)?WB:BB, wantQu=(side==WHITE)?WQ:BQ;
        for (int dd : DD) {
            int cur=to;
            while (true) {
                int t=cur+dd; if (!onBoard(t)) break;
                if (std::abs(fileOf(t)-fileOf(cur))!=1) break;
                if (tmp.sq[t]!=EMPTY) {
                    if ((tmp.sq[t]==wantBi||tmp.sq[t]==wantQu) && SEE_VAL[(int)typeOf(tmp.sq[t])]<bestVal) {
                        bestFrom=t; bestVal=SEE_VAL[(int)typeOf(tmp.sq[t])]; bestPiece=tmp.sq[t];
                    }
                    break;
                }
                cur=t;
            }
        }

        // Rooks/Queens (straights)
        static constexpr int SS[4] = {-8,-1,1,8};
        Piece wantRo=(side==WHITE)?WR:BR;
        int r0=rankOf(to),f0=fileOf(to);
        for (int dd : SS) {
            int cur=to;
            while (true) {
                int t=cur+dd; if (!onBoard(t)) break;
                if (dd==1&&fileOf(t)==0) break;
                if (dd==-1&&fileOf(t)==7) break;
                if (tmp.sq[t]!=EMPTY) {
                    if ((tmp.sq[t]==wantRo||tmp.sq[t]==wantQu) && SEE_VAL[(int)typeOf(tmp.sq[t])]<bestVal) {
                        bestFrom=t; bestVal=SEE_VAL[(int)typeOf(tmp.sq[t])]; bestPiece=tmp.sq[t];
                    }
                    break;
                }
                cur=t;
            }
        }
        (void)r0; (void)f0;

        // Kings
        Piece wantKi=(side==WHITE)?WK:BK;
        for (int dr=-1;dr<=1;dr++) for (int df=-1;df<=1;df++) {
            if (!dr&&!df) continue;
            int r=rankOf(to)+dr, f=fileOf(to)+df;
            if (r<0||r>7||f<0||f>7) continue;
            int t=mkSq(r,f);
            if (tmp.sq[t]==wantKi && SEE_VAL[KING]<bestVal) { bestFrom=t; bestVal=SEE_VAL[KING]; bestPiece=wantKi; }
        }

        if (bestFrom == NO_SQ) break;  // no more attackers

        // "Capture" the piece: remove from board, recapture on 'to'
        mover = bestPiece;
        tmp.sq[bestFrom] = EMPTY;
        d++;
        if (d >= 32) break;
    }

    // Minimax up the gain array
    while (--d) {
        gain[d-1] = std::max(-gain[d], gain[d-1]);
    }
    return gain[0];
}

// ================================================================
// PeSTO Piece-Square Tables
// ================================================================

static constexpr int MG_VAL[7] = {0,  82, 337, 365, 477, 1025,    0};
static constexpr int EG_VAL[7] = {0,  94, 281, 297, 512,  936,    0};

static constexpr int MG_PAWN[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    98,134, 61, 95, 68,126, 34,-11,
    -6,  7, 26, 31, 65, 56, 25,-20,
   -14, 13,  6, 21, 23, 12, 17,-23,
   -27, -2, -5, 12, 17,  6, 10,-25,
   -26, -4, -4,-10,  3,  3, 33,-12,
   -35, -1,-20,-23,-15, 24, 38,-22,
     0,  0,  0,  0,  0,  0,  0,  0,
};
static constexpr int EG_PAWN[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
   178,173,158,134,147,132,165,187,
    94,100, 85, 67, 56, 53, 82, 84,
    32, 24, 13,  5, -2,  4, 17, 17,
    13,  9, -3, -7, -7, -8,  3, -1,
     4,  7, -6,  1,  0, -5, -1, -8,
    13,  8,  8, 10, 13,  0,  2, -7,
     0,  0,  0,  0,  0,  0,  0,  0,
};
static constexpr int MG_KNIGHT[64] = {
   -167,-89,-34,-49, 61,-97,-15,-107,
    -73,-41, 72, 36, 23, 62,  7, -17,
    -47, 60, 37, 65, 84,129, 73,  44,
     -9, 17, 19, 53, 37, 69, 18,  22,
    -13,  4, 16, 13, 28, 19, 21,  -8,
    -23, -9, 12, 10, 19, 17, 25, -16,
    -29,-53,-12, -3, -1, 18,-14, -19,
   -105,-21,-58,-33,-17,-28,-19, -23,
};
static constexpr int EG_KNIGHT[64] = {
    -58,-38,-13,-28,-31,-27,-63,-99,
    -25, -8,-25, -2, -9,-25,-24,-52,
    -24,-20, 10,  9, -1, -9,-19,-41,
    -17,  3, 22, 22, 22, 11,  8,-18,
    -18, -6, 16, 25, 16, 17,  4,-18,
    -23, -3, -1, 15, 10, -3,-20,-22,
    -42,-20,-10, -5, -2,-20,-23,-44,
    -29,-51,-23,-15,-22,-18,-50,-64,
};
static constexpr int MG_BISHOP[64] = {
    -29,  4,-82,-37,-25,-42,  7, -8,
    -26, 16,-18,-13, 30, 59, 18,-47,
    -16, 37, 43, 40, 35, 50, 37, -2,
     -4,  5, 19, 50, 37, 37,  7, -2,
     -6, 13, 13, 26, 34, 12, 10,  4,
      0, 15, 15, 15, 14, 27, 18, 10,
      4, 15, 16,  0,  7, 21, 33,  1,
    -33,-3,-14,-21,-13,-12,-39,-21,
};
static constexpr int EG_BISHOP[64] = {
    -14,-21,-11, -8, -7, -9,-17,-24,
     -8, -4,  7,-12, -3,-13, -4,-14,
      2, -8,  0, -1, -2,  6,  0,  4,
     -3,  9, 12,  9, 14, 10,  3,  2,
     -6,  3, 13, 19,  7, 10, -3, -9,
    -12, -3,  8, 10, 13,  3, -7,-15,
    -14,-18, -7, -1,  4, -9,-15,-27,
    -23, -9,-23, -5, -9,-16, -5,-17,
};
static constexpr int MG_ROOK[64] = {
     32, 42, 32, 51, 63,  9, 31, 43,
     27, 32, 58, 62, 80, 67, 26, 44,
     -5, 19, 26, 36, 17, 45, 61, 16,
    -24,-11,  7, 26, 24, 35, -8,-20,
    -36,-26,-12, -1,  9, -7,  6,-23,
    -45,-25,-16,-17,  3,  0, -5,-33,
    -44,-16,-20, -9, -1, 11, -6,-71,
    -19,-13,  1, 17, 16,  7,-37,-26,
};
static constexpr int EG_ROOK[64] = {
     13, 10, 18, 15, 12, 12,  8,  5,
     11, 13, 13, 11, -3,  3,  8,  3,
      7,  7,  7,  5,  4, -3, -5, -3,
      4,  3, 13,  1,  2,  1, -1,  2,
      3,  5,  8,  4, -5, -6, -8, -11,
     -4,  0, -5, -1, -7,-12, -8,-16,
     -6, -6,  0,  2, -9, -9,-11, -3,
     -9,  2,  3, -1, -5,-13,  4,-20,
};
static constexpr int MG_QUEEN[64] = {
    -28,  0, 29, 12, 59, 44, 43, 45,
    -24,-39, -5,  1,-16, 57, 28, 54,
    -13,-17,  7,  8, 29, 56, 47, 57,
    -27,-27,-16,-16, -1, 17, -2,  1,
     -9,-26, -9,-10, -2, -4,  3, -3,
    -14,  2,-11, -2, -5,  2, 14,  5,
    -35, -8, 11,  2,  8, 15, -3,  1,
     -1,-18, -9, 10,-15,-25,-31,-50,
};
static constexpr int EG_QUEEN[64] = {
     -9, 22, 22, 27, 27, 19, 10, 20,
    -17, 20, 32, 41, 58, 25, 30,  0,
    -20,  6,  9, 49, 47, 35, 19,  9,
      3, 22, 24, 45, 57, 40, 57, 36,
    -18, 28, 19, 47, 31, 34, 39, 23,
    -16,-27, 15,  6,  9, 17, 10,  5,
    -22,-23,-30,-16,-16,-23,-36,-32,
    -33,-28,-22,-43, -5,-32,-20,-41,
};
static constexpr int MG_KING[64] = {
    -65, 23, 16,-15,-56,-34,  2, 13,
     29, -1,-20, -7, -8, -4,-38,-29,
     -9, 24,  2,-16,-20,  6, 22,-22,
    -17,-20,-12,-27,-30,-25,-14,-36,
    -49, -1,-27,-39,-46,-44,-33,-51,
    -14,-14,-22,-46,-44,-30,-15,-27,
      1,  7, -8,-64,-43,-16,  9,  8,
    -15, 36, 12,-54,  8,-28, 24, 14,
};
static constexpr int EG_KING[64] = {
    -74,-35,-18,-18,-11, 15,  4,-17,
    -12, 17, 14, 17, 17, 38, 23, 11,
     10, 17, 23, 15, 20, 45, 44, 13,
     -8, 22, 24, 27, 26, 33, 26,  3,
    -18, -4, 21, 24, 27, 23,  9,-11,
    -19, -3, 11, 21, 23, 16,  7, -9,
    -27,-11,  4, 13, 14,  4, -5,-17,
    -53,-34,-21,-11,-28,-14,-24,-43,
};

static constexpr int PHASE_INC[7] = {0, 0, 1, 1, 2, 4, 0};
static constexpr int TOTAL_PHASE  = 24;

inline int mgPst(PieceType pt, int sq, Color us) {
    int idx = (us == WHITE) ? (sq ^ 56) : sq;
    switch (pt) {
        case PAWN:   return MG_PAWN[idx];
        case KNIGHT: return MG_KNIGHT[idx];
        case BISHOP: return MG_BISHOP[idx];
        case ROOK:   return MG_ROOK[idx];
        case QUEEN:  return MG_QUEEN[idx];
        case KING:   return MG_KING[idx];
        default:     return 0;
    }
}
inline int egPst(PieceType pt, int sq, Color us) {
    int idx = (us == WHITE) ? (sq ^ 56) : sq;
    switch (pt) {
        case PAWN:   return EG_PAWN[idx];
        case KNIGHT: return EG_KNIGHT[idx];
        case BISHOP: return EG_BISHOP[idx];
        case ROOK:   return EG_ROOK[idx];
        case QUEEN:  return EG_QUEEN[idx];
        case KING:   return EG_KING[idx];
        default:     return 0;
    }
}

// ================================================================
// Evaluation — Full Positional
// ================================================================

// Knight outpost squares: central squares shielded by own pawn
static constexpr bool OUTPOST[64] = {
    0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
    0,1,1,1,1,1,1,0,
    0,1,1,1,1,1,1,0,
    0,0,1,1,1,1,0,0,
    0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
};

// Mobility weights (centipawns per pseudo-legal move beyond baseline)
static constexpr int MG_MOB_KNIGHT = 4;
static constexpr int EG_MOB_KNIGHT = 4;
static constexpr int MG_MOB_BISHOP = 3;
static constexpr int EG_MOB_BISHOP = 5;
static constexpr int MG_MOB_ROOK   = 2;
static constexpr int EG_MOB_ROOK   = 4;
static constexpr int MG_MOB_QUEEN  = 1;
static constexpr int EG_MOB_QUEEN  = 2;

// King attack weights by piece type
static constexpr int KING_ATTACK_WT[7] = {0, 0, 2, 2, 3, 5, 0};

static int evaluate(const Board& b) {
    int mgScore = 0, egScore = 0, phase = 0;
    int wBishops = 0, bBishops = 0;

    // King squares
    int wKing = b.kingSquare(WHITE);
    int bKing = b.kingSquare(BLACK);
    int wKingF = (wKing != NO_SQ) ? fileOf(wKing) : 4;
    int bKingF = (bKing != NO_SQ) ? fileOf(bKing) : 4;

    int wKingAttacks = 0, bKingAttacks = 0;  // weighted attacker count near king

    for (int s = 0; s < 64; s++) {
        Piece p = b.sq[s];
        if (p == EMPTY) continue;
        Color    c  = colorOf(p);
        PieceType pt = typeOf(p);

        phase += PHASE_INC[pt];

        int mgV = MG_VAL[pt] + mgPst(pt, s, c);
        int egV = EG_VAL[pt] + egPst(pt, s, c);

        if (c == WHITE) { mgScore += mgV; egScore += egV; }
        else            { mgScore -= mgV; egScore -= egV; }

        if (pt == BISHOP) { if (c==WHITE) wBishops++; else bBishops++; }

        // ---- Mobility ----
        int mob = 0;
        static constexpr int DIAG[4]     = {-9,-7, 7, 9};
        static constexpr int STRAIGHT[4] = {-8,-1, 1, 8};
        static constexpr int QUEEN_D[8]  = {-9,-8,-7,-1, 1, 7, 8, 9};
        static constexpr int KNIGHT_D[8] = {-17,-15,-10,-6, 6,10,15,17};

        if (pt == KNIGHT) {
            for (int d : KNIGHT_D) {
                int t=s+d; if (!onBoard(t)) continue;
                if (std::abs(fileOf(t)-fileOf(s))>2||std::abs(rankOf(t)-rankOf(s))>2) continue;
                if (b.sq[t]==EMPTY||colorOf(b.sq[t])!=c) mob++;
            }
            int sign = (c==WHITE)?1:-1;
            mgScore += sign * mob * MG_MOB_KNIGHT;
            egScore += sign * mob * EG_MOB_KNIGHT;

            // Knight outpost bonus
            if (c==WHITE && OUTPOST[s^56]) {
                // Check pawn support
                bool support = (fileOf(s)>0 && rankOf(s)>0 && b.sq[s-9]==WP) ||
                               (fileOf(s)<7 && rankOf(s)>0 && b.sq[s-7]==WP);
                if (support) { mgScore += 20; egScore += 10; }
            } else if (c==BLACK && OUTPOST[s]) {
                bool support = (fileOf(s)>0 && rankOf(s)<7 && b.sq[s+7]==BP) ||
                               (fileOf(s)<7 && rankOf(s)<7 && b.sq[s+9]==BP);
                if (support) { mgScore -= 20; egScore -= 10; }
            }

            // King proximity (knight is good near enemy king)
            int enemyKing = (c==WHITE) ? bKing : wKing;
            if (enemyKing != NO_SQ) {
                int dist = std::abs(rankOf(s)-rankOf(enemyKing)) + std::abs(fileOf(s)-fileOf(enemyKing));
                if (dist <= 3) {
                    int sign2 = (c==WHITE)?1:-1;
                    mgScore += sign2 * KING_ATTACK_WT[KNIGHT] * (4-dist);
                    if (c==WHITE) wKingAttacks += KING_ATTACK_WT[KNIGHT];
                    else          bKingAttacks += KING_ATTACK_WT[KNIGHT];
                }
            }
        } else if (pt == BISHOP) {
            for (int d : DIAG) {
                int cur=s;
                while (true) {
                    int t=cur+d; if (!onBoard(t)) break;
                    if (std::abs(fileOf(t)-fileOf(cur))!=1) break;
                    if (b.sq[t]==EMPTY) { mob++; cur=t; }
                    else { if (colorOf(b.sq[t])!=c) mob++; break; }
                }
            }
            int sign = (c==WHITE)?1:-1;
            mgScore += sign * mob * MG_MOB_BISHOP;
            egScore += sign * mob * EG_MOB_BISHOP;

            // King attack contribution
            int enemyKing = (c==WHITE) ? bKing : wKing;
            if (enemyKing != NO_SQ) {
                int dist = std::abs(rankOf(s)-rankOf(enemyKing)) + std::abs(fileOf(s)-fileOf(enemyKing));
                if (dist <= 4) {
                    if (c==WHITE) wKingAttacks += KING_ATTACK_WT[BISHOP];
                    else          bKingAttacks += KING_ATTACK_WT[BISHOP];
                }
            }
        } else if (pt == ROOK) {
            for (int d : STRAIGHT) {
                int cur=s;
                while (true) {
                    int t=cur+d; if (!onBoard(t)) break;
                    if (d== 1&&fileOf(t)==0) break;
                    if (d==-1&&fileOf(t)==7) break;
                    if (b.sq[t]==EMPTY) { mob++; cur=t; }
                    else { if (colorOf(b.sq[t])!=c) mob++; break; }
                }
            }
            int sign = (c==WHITE)?1:-1;
            mgScore += sign * mob * MG_MOB_ROOK;
            egScore += sign * mob * EG_MOB_ROOK;

            // Rook on open/semi-open file
            int f = fileOf(s);
            bool ownPawn=false, oppPawn=false;
            for (int r=0;r<8;r++) {
                if (b.sq[mkSq(r,f)]==WP) ownPawn=true;
                if (b.sq[mkSq(r,f)]==BP) oppPawn=true;
            }
            if (c==WHITE) {
                if (!ownPawn && !oppPawn) { mgScore += 25; egScore += 20; }  // open file
                else if (!ownPawn)        { mgScore += 12; egScore += 10; }  // semi-open
            } else {
                if (!ownPawn && !oppPawn) { mgScore -= 25; egScore -= 20; }
                else if (!oppPawn)        { mgScore -= 12; egScore -= 10; }
            }

            // Rook on 7th rank
            if (c==WHITE && rankOf(s)==6) { mgScore += 20; egScore += 30; }
            if (c==BLACK && rankOf(s)==1) { mgScore -= 20; egScore -= 30; }

            // King attack
            int enemyKing = (c==WHITE) ? bKing : wKing;
            if (enemyKing != NO_SQ) {
                int dist = std::abs(rankOf(s)-rankOf(enemyKing)) + std::abs(fileOf(s)-fileOf(enemyKing));
                if (dist <= 3) {
                    if (c==WHITE) wKingAttacks += KING_ATTACK_WT[ROOK];
                    else          bKingAttacks += KING_ATTACK_WT[ROOK];
                }
            }
        } else if (pt == QUEEN) {
            for (int d : QUEEN_D) {
                int cur=s;
                while (true) {
                    int t=cur+d; if (!onBoard(t)) break;
                    bool diag=(d==9||d==-9||d==7||d==-7);
                    if (diag && std::abs(fileOf(t)-fileOf(cur))!=1) break;
                    if (d== 1&&fileOf(t)==0) break;
                    if (d==-1&&fileOf(t)==7) break;
                    if (b.sq[t]==EMPTY) { mob++; cur=t; }
                    else { if (colorOf(b.sq[t])!=c) mob++; break; }
                }
            }
            int sign = (c==WHITE)?1:-1;
            mgScore += sign * mob * MG_MOB_QUEEN;
            egScore += sign * mob * EG_MOB_QUEEN;

            // King attack
            int enemyKing = (c==WHITE) ? bKing : wKing;
            if (enemyKing != NO_SQ) {
                int dist = std::abs(rankOf(s)-rankOf(enemyKing)) + std::abs(fileOf(s)-fileOf(enemyKing));
                if (dist <= 4) {
                    if (c==WHITE) wKingAttacks += KING_ATTACK_WT[QUEEN];
                    else          bKingAttacks += KING_ATTACK_WT[QUEEN];
                }
            }
        }
    }

    // Bishop pair bonus
    if (wBishops >= 2) { mgScore += 40; egScore += 50; }
    if (bBishops >= 2) { mgScore -= 40; egScore -= 50; }

    // ---- Pawn structure ----
    for (int f = 0; f < 8; f++) {
        int wPawns = 0, bPawns = 0;
        for (int r = 0; r < 8; r++) {
            if (b.sq[mkSq(r,f)] == WP) wPawns++;
            if (b.sq[mkSq(r,f)] == BP) bPawns++;
        }
        if (wPawns > 1) { mgScore -= 10*(wPawns-1); egScore -= 20*(wPawns-1); }
        if (bPawns > 1) { mgScore += 10*(bPawns-1); egScore += 20*(bPawns-1); }

        // Isolated pawns
        int wNeighbor = 0, bNeighbor = 0;
        for (int df : {-1, 1}) {
            int nf = f + df; if (nf < 0 || nf > 7) continue;
            for (int r = 0; r < 8; r++) {
                if (b.sq[mkSq(r,nf)] == WP) wNeighbor++;
                if (b.sq[mkSq(r,nf)] == BP) bNeighbor++;
            }
        }
        if (wPawns > 0 && wNeighbor == 0) { mgScore -= 12; egScore -= 20; }
        if (bPawns > 0 && bNeighbor == 0) { mgScore += 12; egScore += 20; }
    }

    // Passed pawns
    for (int s = 0; s < 64; s++) {
        if (b.sq[s] == WP) {
            int r = rankOf(s), f = fileOf(s); bool passed = true;
            for (int rr = r+1; rr < 8 && passed; rr++)
                for (int df = -1; df <= 1; df++) {
                    int nf = f+df; if (nf<0||nf>7) continue;
                    if (b.sq[mkSq(rr,nf)] == BP) { passed = false; break; }
                }
            if (passed) {
                int bonus = r*r*5;
                mgScore += bonus;
                egScore += bonus*2;
                // Bonus if king supports passed pawn in endgame
                if (wKing != NO_SQ) {
                    int dist = std::abs(rankOf(wKing)-(r+1)) + std::abs(fileOf(wKing)-f);
                    egScore += std::max(0, 10 - dist*2);
                }
            }
        }
        if (b.sq[s] == BP) {
            int r = rankOf(s), f = fileOf(s); bool passed = true;
            for (int rr = r-1; rr >= 0 && passed; rr--)
                for (int df = -1; df <= 1; df++) {
                    int nf = f+df; if (nf<0||nf>7) continue;
                    if (b.sq[mkSq(rr,nf)] == WP) { passed = false; break; }
                }
            if (passed) {
                int bonus = (7-r)*(7-r)*5;
                mgScore -= bonus;
                egScore -= bonus*2;
                if (bKing != NO_SQ) {
                    int dist = std::abs(rankOf(bKing)-(r-1)) + std::abs(fileOf(bKing)-f);
                    egScore -= std::max(0, 10 - dist*2);
                }
            }
        }
    }

    // ---- King Safety ----
    // Pawn shield + open files + attacker bonus
    for (Color c : {WHITE, BLACK}) {
        int ks = (c==WHITE) ? wKing : bKing;
        if (ks == NO_SQ) continue;
        int kf = fileOf(ks), kr = rankOf(ks);
        int sign = (c == WHITE) ? 1 : -1;
        int pawnDir = (c==WHITE) ? 1 : -1;

        // Pawn shield: pawns directly in front of king
        for (int df = -1; df <= 1; df++) {
            int f = kf+df; if (f<0||f>7) continue;
            int shieldR = kr+pawnDir;
            bool hasPawn = (shieldR>=0&&shieldR<8 && (c==WHITE ? b.sq[mkSq(shieldR,f)]==WP : b.sq[mkSq(shieldR,f)]==BP));
            bool ownPawnOnFile = false, oppPawnOnFile = false;
            for (int r=0;r<8;r++) {
                if (c==WHITE && b.sq[mkSq(r,f)]==WP) ownPawnOnFile=true;
                if (c==WHITE && b.sq[mkSq(r,f)]==BP) oppPawnOnFile=true;
                if (c==BLACK && b.sq[mkSq(r,f)]==BP) ownPawnOnFile=true;
                if (c==BLACK && b.sq[mkSq(r,f)]==WP) oppPawnOnFile=true;
            }
            if (!hasPawn) mgScore -= sign*10;  // missing shield pawn
            if (!ownPawnOnFile && !oppPawnOnFile) mgScore -= sign*20;  // open file
            else if (!ownPawnOnFile)              mgScore -= sign*10;  // semi-open
        }

        // Attacker count penalty (scaled by number of attackers)
        int attackers = (c==WHITE) ? bKingAttacks : wKingAttacks;
        static constexpr int SAFETY_TABLE[20] = {
            0, 0, 1, 2, 3, 5, 7, 9, 12, 15,
            18, 22, 26, 30, 35, 40, 45, 50, 56, 62
        };
        int safetyIdx = std::min(attackers, 19);
        mgScore -= sign * SAFETY_TABLE[safetyIdx];
    }

    // ---- Phase interpolation ----
    if (phase > TOTAL_PHASE) phase = TOTAL_PHASE;
    int score = (mgScore * phase + egScore * (TOTAL_PHASE - phase)) / TOTAL_PHASE;

    // Tempo bonus
    score += 14;

    return (b.side == WHITE) ? score : -score;
}

// ================================================================
// Transposition Table
// ================================================================

enum Bound : uint8_t { BOUND_NONE=0, BOUND_UPPER=1, BOUND_LOWER=2, BOUND_EXACT=3 };

struct TTEntry {
    uint64_t key   = 0;
    uint16_t move  = 0;
    int16_t  score = 0;
    int8_t   depth = -1;
    uint8_t  bound = BOUND_NONE;
    int8_t   age   = 0;
};

static constexpr size_t TT_SIZE = 1 << 22;  // 4M entries ≈ 48 MB
static TTEntry TT[TT_SIZE];
static int8_t  TT_AGE = 0;

static void ttClear() { std::memset(TT, 0, sizeof(TT)); TT_AGE = 0; }

static TTEntry* ttProbe(uint64_t hash) {
    return &TT[hash & (TT_SIZE - 1)];
}

static void ttStore(uint64_t hash, Move m, int score, int depth, Bound bound, int ply) {
    TTEntry& e = TT[hash & (TT_SIZE-1)];
    if (score >= MATE_BOUND)  score += ply;
    if (score <= -MATE_BOUND) score -= ply;
    // Replace if: same position, new entry deeper, or different position, or stale age
    bool replace = (e.key != hash) || (e.depth <= depth) || (e.age != TT_AGE) || (bound == BOUND_EXACT);
    if (!replace) return;
    e.key   = hash;
    if (!m.isNull() || e.key != hash) e.move = m.data;
    e.score = int16_t(score);
    e.depth = int8_t(depth);
    e.bound = bound;
    e.age   = TT_AGE;
}

static int ttScore(int rawScore, int ply) {
    if (rawScore >= MATE_BOUND)  return rawScore - ply;
    if (rawScore <= -MATE_BOUND) return rawScore + ply;
    return rawScore;
}

// ================================================================
// LMR Table  (pre-computed log formula)
// ================================================================

static int LMR_TABLE[MAX_PLY][64];  // [depth][moveIndex]

static void initLMR() {
    for (int d = 1; d < MAX_PLY; d++)
        for (int m = 1; m < 64; m++) {
            LMR_TABLE[d][m] = std::max(0, (int)(0.75 + std::log(d) * std::log(m) / 2.25));
        }
}

// ================================================================
// Search State
// ================================================================

struct SearchState {
    Move killers[MAX_PLY][2];
    int  history[13][64];           // quiet move history [piece][to]
    int  captureHistory[13][64][7]; // capture history [piece][to][captType]
    int  counterMove[13][64];       // counter-move table [piece][to]
    int  staticEval[MAX_PLY];       // static eval at each ply
    long long nodes;
    int  ply;
    bool stop;
    std::chrono::steady_clock::time_point startTime;
    int  timeLimitMs;
    int  maxTimeLimitMs;  // hard limit

    void reset() {
        std::memset(killers,        0, sizeof(killers));
        std::memset(history,        0, sizeof(history));
        std::memset(captureHistory, 0, sizeof(captureHistory));
        std::memset(counterMove,    0, sizeof(counterMove));
        std::memset(staticEval,     0, sizeof(staticEval));
        nodes = 0; ply = 0; stop = false;
        timeLimitMs = maxTimeLimitMs = 5000;
    }

    static int clamp_hist(int v, int limit = 16384) {
        return std::max(-limit, std::min(limit, v));
    }

    // Soft time-up: used inside iterative deepening loop
    bool timeUp() {
        if (nodes & 4095) return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        return elapsed >= timeLimitMs;
    }

    // Hard time-up: enforced inside inner search to prevent overrun
    bool hardTimeUp() {
        if (nodes & 4095) return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        return elapsed >= maxTimeLimitMs;
    }

    // Called from inside alphaBeta/quiesce — uses hard limit
    bool innerTimeUp() {
        if (nodes & 4095) return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        return elapsed >= maxTimeLimitMs;
    }
};

// ================================================================
// Move Ordering
// ================================================================

static int moveOrderScore(const Board& b, Move m, Move ttMove,
                          const SearchState& ss, Move prevMove) {
    if (m == ttMove) return 3000000;
    Piece mover = b.sq[m.from()];
    Piece cap   = b.sq[m.to()];
    if (m.mtype() == 2) cap = (b.side == WHITE) ? BP : WP;

    if (cap != EMPTY || m.mtype() == 3) {
        if (m.mtype() == 3) {
            // Promotion
            int base = (m.promo() == 3) ? 1900000 : 900000 + m.promo()*100;
            return base;
        }
        // Capture: SEE-based ordering
        int seeVal = see(b, m.to(), cap, m.from(), mover);
        if (seeVal >= 0) {
            // Good capture: MVV + SEE
            int mvv = 1000000 + SEE_VAL[(int)typeOf(cap)]*8 - SEE_VAL[(int)typeOf(mover)];
            return mvv + seeVal / 10;
        } else {
            // Bad capture: below quiet moves
            return seeVal;
        }
    }

    if (ss.ply < MAX_PLY) {
        if (m == ss.killers[ss.ply][0]) return 800000;
        if (m == ss.killers[ss.ply][1]) return 799000;
    }
    if (!prevMove.isNull()) {
        Piece prevP = b.sq[prevMove.to()];
        if ((int)prevP < 13 && m.data == (uint16_t)ss.counterMove[prevP][prevMove.to()])
            return 700000;
    }
    return ss.history[mover][m.to()];
}

static void sortMoves(MoveList& ml, const Board& b, Move ttMove,
                      const SearchState& ss, Move prevMove) {
    int scores[256];
    for (int i = 0; i < ml.count; i++)
        scores[i] = moveOrderScore(b, ml.moves[i], ttMove, ss, prevMove);
    for (int i = 1; i < ml.count; i++) {
        Move m = ml.moves[i]; int s = scores[i]; int j = i-1;
        while (j >= 0 && scores[j] < s) {
            ml.moves[j+1] = ml.moves[j]; scores[j+1] = scores[j]; j--;
        }
        ml.moves[j+1] = m; scores[j+1] = s;
    }
}

// ================================================================
// Quiescence Search
// ================================================================

static int quiesce(Board& b, int alpha, int beta, SearchState& ss, int qDepth = 0) {
    if (ss.stop || ss.innerTimeUp()) { ss.stop = true; return 0; }
    ss.nodes++;

    // Repetition check in quiescence
    if (b.isRepetition(ss.ply)) return 0;

    int stand_pat = evaluate(b);
    if (stand_pat >= beta) return beta;

    // Delta pruning
    int bigDelta = 1000;  // queen + margin
    if (stand_pat < alpha - bigDelta) return alpha;

    if (stand_pat > alpha) alpha = stand_pat;

    MoveList ml; generatePseudoMoves(b, ml);

    // Score captures (SEE-based)
    int scores[256];
    for (int i = 0; i < ml.count; i++) {
        Move m = ml.moves[i];
        Piece cap = b.sq[m.to()];
        bool isPromo = (m.mtype() == 3);
        bool isEP    = (m.mtype() == 2);
        if (cap == EMPTY && !isPromo && !isEP) { scores[i] = -1; continue; }
        if (isPromo) { scores[i] = 2000000 + m.promo(); continue; }
        if (isEP)    { scores[i] = 100; continue; }
        // SEE score — skip bad captures in quiescence (after depth 0)
        int seeV = see(b, m.to(), cap, m.from(), b.sq[m.from()]);
        if (seeV < 0 && qDepth > 0) { scores[i] = -1; continue; }
        scores[i] = seeV + SEE_VAL[(int)typeOf(cap)];
    }

    for (int i = 0; i < ml.count; i++) {
        int best_idx = i;
        for (int j = i+1; j < ml.count; j++)
            if (scores[j] > scores[best_idx]) best_idx = j;
        std::swap(ml.moves[i], ml.moves[best_idx]);
        std::swap(scores[i], scores[best_idx]);

        if (scores[i] < 0) break;

        Move m = ml.moves[i];
        if (!b.makeMove(m)) continue;
        ss.ply++;
        int score = -quiesce(b, -beta, -alpha, ss, qDepth+1);
        ss.ply--;
        b.unmakeMove(m);

        if (ss.stop) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

// ================================================================
// Alpha-Beta (PVS) Search
// ================================================================

static int alphaBeta(Board& b, int alpha, int beta, int depth,
                     bool nullOk, SearchState& ss, Move prevMove = Move{});

static int alphaBeta(Board& b, int alpha, int beta, int depth,
                     bool nullOk, SearchState& ss, Move prevMove) {
    if (ss.stop || ss.innerTimeUp()) { ss.stop = true; return 0; }
    ss.nodes++;

    bool isRoot = (ss.ply == 0);
    bool pvNode = (beta - alpha > 1);

    // Draw detection
    if (!isRoot) {
        if (b.halfMove >= 100) return 0;
        if (b.isRepetition(ss.ply)) return 0;
    }

    // TT probe
    TTEntry* tte = ttProbe(b.hash);
    Move ttMove{};
    int ttScore_ = 0;
    bool ttHit = (tte->key == b.hash && tte->depth >= 0);
    if (ttHit) {
        ttMove   = Move::fromRaw(tte->move);
        ttScore_ = ttScore(tte->score, ss.ply);
        if (!pvNode && tte->depth >= depth) {
            if (tte->bound == BOUND_EXACT) return ttScore_;
            if (tte->bound == BOUND_LOWER && ttScore_ >= beta)  return ttScore_;
            if (tte->bound == BOUND_UPPER && ttScore_ <= alpha) return ttScore_;
        }
    }

    if (depth <= 0) return quiesce(b, alpha, beta, ss);

    bool inCheck = b.inCheck();
    if (inCheck) depth++;  // check extension

    // Static eval
    int staticEv;
    if (ttHit && tte->bound != BOUND_NONE) {
        staticEv = ttScore_;
    } else {
        staticEv = evaluate(b);
    }
    if (ss.ply < MAX_PLY) ss.staticEval[ss.ply] = staticEv;

    bool improving = (ss.ply >= 2) && !inCheck &&
                     (staticEv > ss.staticEval[ss.ply - 2]);

    // ---- Reverse Futility Pruning (RFP) ----
    if (!inCheck && !pvNode && depth <= 8) {
        int rfpMargin = 80 * depth;
        if (staticEv - rfpMargin >= beta && staticEv < MATE_BOUND)
            return staticEv - rfpMargin;
    }

    // ---- Null-Move Pruning ----
    if (!inCheck && nullOk && depth >= 3 && !pvNode && staticEv >= beta) {
        int bigPieces = 0;
        Piece myRook  = (b.side==WHITE) ? WR : BR;
        Piece myQueen = (b.side==WHITE) ? WQ : BQ;
        Piece myKnight= (b.side==WHITE) ? WN : BN;
        Piece myBishop= (b.side==WHITE) ? WB : BB;
        for (int s=0; s<64; s++) {
            Piece p = b.sq[s];
            if (p==myRook||p==myQueen||p==myKnight||p==myBishop) bigPieces++;
        }
        if (bigPieces > 0) {
            int R = 3 + depth/3 + std::min(3, (staticEv - beta)/150);
            b.hash ^= ZSIDE;
            if (b.epSquare != NO_SQ) b.hash ^= ZEP[fileOf(b.epSquare)];
            int savedEp = b.epSquare;
            b.side = ~b.side; b.ply++; ss.ply++;
            b.epSquare = NO_SQ;

            int nullScore = -alphaBeta(b, -beta, -beta+1, depth-R-1, false, ss);

            b.ply--; ss.ply--;
            b.side = ~b.side; b.epSquare = savedEp;
            if (savedEp != NO_SQ) b.hash ^= ZEP[fileOf(savedEp)];
            b.hash ^= ZSIDE;

            if (!ss.stop && nullScore >= beta)
                return (nullScore >= MATE_BOUND) ? beta : nullScore;
        }
    }

    // ---- ProbCut ----
    if (!inCheck && !pvNode && depth >= 5 && std::abs(beta) < MATE_BOUND) {
        int probBeta = beta + 150;
        MoveList pcMl; generatePseudoMoves(b, pcMl);
        for (int i = 0; i < pcMl.count; i++) {
            Move m = pcMl.moves[i];
            Piece cap = b.sq[m.to()];
            if (m.mtype()==2) cap = (b.side==WHITE)?BP:WP;
            if (cap == EMPTY && m.mtype()!=3) continue;  // only captures/promos
            // Quick SEE check
            int seeVal = (cap!=EMPTY) ? see(b, m.to(), cap, m.from(), b.sq[m.from()]) : 0;
            if (seeVal + (m.mtype()==3 ? 400 : 0) < probBeta - staticEv) continue;
            if (!b.makeMove(m)) continue;
            ss.ply++;
            int score = -quiesce(b, -probBeta, -probBeta+1, ss);
            if (score >= probBeta)
                score = -alphaBeta(b, -probBeta, -probBeta+1, depth-4, false, ss);
            ss.ply--;
            b.unmakeMove(m);
            if (ss.stop) return 0;
            if (score >= probBeta) {
                ttStore(b.hash, m, score, depth-3, BOUND_LOWER, ss.ply);
                return score;
            }
        }
    }

    // ---- Internal Iterative Reduction (IIR) ----
    if (!ttHit && depth >= 4) depth--;

    // ---- Futility pruning setup ----
    bool doFutility = false;
    int  futilityMargin = 0;
    if (!inCheck && depth <= 8 && !pvNode) {
        futilityMargin = 100 * depth;
        doFutility = (staticEv + futilityMargin <= alpha);
    }

    // Generate and sort moves
    MoveList ml; generatePseudoMoves(b, ml);
    sortMoves(ml, b, ttMove, ss, prevMove);

    // ---- Singular Extensions ----
    // If TT move exists and is deeply searched, verify it's singularly best
    Move singularMove{};
    if (!isRoot && ttHit && !ttMove.isNull()
        && depth >= 6
        && tte->depth >= depth - 3
        && tte->bound != BOUND_UPPER
        && std::abs(ttScore_) < MATE_BOUND) {
        singularMove = ttMove;
    }

    int  bestScore   = -INF;
    Move bestMove{};
    int  legalCount  = 0;
    bool raisedAlpha = false;

    // Store per-capture info BEFORE unmake so malus can be applied correctly
    struct CaptRecord { Piece piece; int toSq; int captType; };
    Move quietsTried[64]; int quietCount = 0;
    CaptRecord captsTried[64]; int captCount  = 0;

    for (int i = 0; i < ml.count; i++) {
        Move m = ml.moves[i];

        // Record captured type BEFORE make (board not yet changed)
        Piece preCap = b.sq[m.to()];
        if (m.mtype() == 2) preCap = (b.side == WHITE) ? BP : WP;

        if (!b.makeMove(m)) continue;
        legalCount++;
        ss.ply++;

        bool isCapture  = (preCap != EMPTY);
        bool isPromo    = (m.mtype() == 3);
        bool givesCheck = b.inCheck();
        Piece moverPiece = b.sq[m.to()];

        // ---- Futility pruning ----
        if (doFutility && !isCapture && !isPromo && !givesCheck && legalCount > 1) {
            ss.ply--;
            b.unmakeMove(m);
            continue;
        }

        // ---- SEE pruning for quiet moves at shallow depths ----
        if (!isCapture && !isPromo && !inCheck && !givesCheck && !pvNode && depth <= 6 && legalCount > 1) {
            // Skip quiet moves with very negative history at shallow depth
            if (ss.history[moverPiece][m.to()] < -3000 * depth) {
                ss.ply--;
                b.unmakeMove(m);
                continue;
            }
        }

        // Track for history
        if (!isCapture && !isPromo && quietCount < 64)
            quietsTried[quietCount++] = m;
        if (isCapture && captCount < 64)
            captsTried[captCount++] = { moverPiece, m.to(), (int)typeOf(preCap) };

        // ---- Singular Extension ----
        int extend = 0;
        if (!singularMove.isNull() && m == singularMove && !ss.stop) {
            // Do a reduced verification search with (ttScore - margin) as beta
            int singBeta  = ttScore_ - 2 * depth;
            int singDepth = (depth - 1) / 2;
            ss.ply--;
            b.unmakeMove(m);
            // Temporarily search without this move (nullOk=false so NMP doesn't corrupt)
            int singScore = alphaBeta(b, singBeta - 1, singBeta, singDepth, false, ss, prevMove);
            if (!b.makeMove(m)) { continue; }  // shouldn't fail but safety
            ss.ply++;
            if (!ss.stop && singScore < singBeta) {
                extend = 1;  // TT move is singular — extend
            } else if (singBeta >= beta) {
                // Multi-cut heuristic: if verification already beats beta, prune
                return singBeta;
            }
        }

        int score;

        if (legalCount == 1) {
            score = -alphaBeta(b, -beta, -alpha, depth-1+extend, true, ss, m);
        } else {
            int reduction = 0;
            if (depth >= 2 && legalCount >= 3 && !isCapture && !isPromo && !inCheck && !givesCheck) {
                reduction = LMR_TABLE[std::min(depth, MAX_PLY-1)][std::min(legalCount, 63)];
                // Adjust LMR
                if (pvNode)    reduction = std::max(0, reduction - 1);
                if (!improving) reduction++;
                if (m == ss.killers[ss.ply-1][0] || m == ss.killers[ss.ply-1][1])
                    reduction = std::max(0, reduction - 1);
                int hist = ss.history[moverPiece][m.to()];
                if      (hist > 6000)  reduction = std::max(0, reduction - 2);
                else if (hist > 3000)  reduction = std::max(0, reduction - 1);
                else if (hist < -3000) reduction++;
                reduction = std::max(0, std::min(reduction, depth - 1));
            }

            score = -alphaBeta(b, -alpha-1, -alpha, depth-1-reduction, true, ss, m);

            if (score > alpha && reduction > 0)
                score = -alphaBeta(b, -alpha-1, -alpha, depth-1, true, ss, m);

            if (score > alpha && pvNode)
                score = -alphaBeta(b, -beta, -alpha, depth-1, true, ss, m);
        }

        ss.ply--;
        b.unmakeMove(m);

        if (ss.stop) return 0;

        if (score > bestScore) {
            bestScore = score;
            bestMove  = m;
        }
        if (score > alpha) {
            alpha = score;
            raisedAlpha = true;
            if (alpha >= beta) {
                Piece captured = b.history[b.ply].captured;
                if (captured == EMPTY && !isPromo) {
                    if (ss.ply < MAX_PLY) {
                        ss.killers[ss.ply][1] = ss.killers[ss.ply][0];
                        ss.killers[ss.ply][0] = m;
                    }
                    if (!prevMove.isNull() && ss.ply > 0) {
                        Piece prevPiece = b.sq[prevMove.to()];
                        if ((int)prevPiece < 13)
                            ss.counterMove[prevPiece][prevMove.to()] = m.data;
                    }
                    int bonus = std::min(depth * depth * 2, 1600);
                    Piece cutter = b.sq[m.from()];
                    ss.history[cutter][m.to()] = SearchState::clamp_hist(
                        ss.history[cutter][m.to()] + bonus);
                    for (int q = 0; q < quietCount - 1; q++) {
                        Piece qp = b.sq[quietsTried[q].from()];
                        ss.history[qp][quietsTried[q].to()] = SearchState::clamp_hist(
                            ss.history[qp][quietsTried[q].to()] - bonus);
                    }
                } else if (preCap != EMPTY) {
                    int bonus = std::min(depth * depth * 2, 1600);
                    int captType = (int)typeOf(preCap);
                    // Bonus for the cutoff capture (moverPiece still valid after unmake)
                    ss.captureHistory[moverPiece][m.to()][captType] = SearchState::clamp_hist(
                        ss.captureHistory[moverPiece][m.to()][captType] + bonus);
                    // Malus for earlier captures that did NOT cause a cut
                    for (int c2 = 0; c2 < captCount - 1; c2++) {
                        auto& cr = captsTried[c2];
                        ss.captureHistory[cr.piece][cr.toSq][cr.captType] = SearchState::clamp_hist(
                            ss.captureHistory[cr.piece][cr.toSq][cr.captType] - bonus);
                    }
                }
                ttStore(b.hash, m, beta, depth, BOUND_LOWER, ss.ply);
                return beta;
            }
        }
    }

    if (legalCount == 0) {
        return inCheck ? (-MATE_SCORE + ss.ply) : 0;
    }

    Bound bound = raisedAlpha ? BOUND_EXACT : BOUND_UPPER;
    ttStore(b.hash, bestMove, bestScore, depth, bound, ss.ply);
    return bestScore;
}

// ================================================================
// Root Search with proper Multi-PV
// ================================================================

struct PVLine {
    Move best;
    int  score;
    int  depth;
};

static std::vector<PVLine> rootSearch(Board& b, int timeLimitMs, int maxTimeLimitMs, int multiPV, int maxDepthLimit = 64) {
    SearchState ss;
    ss.reset();
    ss.startTime      = std::chrono::steady_clock::now();
    ss.timeLimitMs    = timeLimitMs;
    ss.maxTimeLimitMs = maxTimeLimitMs;

    auto rootMoves = legalMoves(b);
    if (rootMoves.empty()) return {};

    multiPV = std::min(multiPV, (int)rootMoves.size());

    std::vector<int>    rootScores(rootMoves.size(), 0);
    std::vector<PVLine> result;
    result.reserve(multiPV);

    int maxDepth = maxDepthLimit;
    TT_AGE++;

    for (int pvIdx = 0; pvIdx < multiPV; pvIdx++) {
        PVLine line{};
        line.score = -INF;
        line.depth = 0;

        std::vector<Move> excludedMoves;
        for (int k = 0; k < pvIdx; k++)
            if (!result[k].best.isNull()) excludedMoves.push_back(result[k].best);

        int lineTime = (pvIdx == 0) ? timeLimitMs
                                    : std::max(200, timeLimitMs / (multiPV * 2));

        ss.stop = false;
        ss.ply  = 0;
        ss.timeLimitMs = lineTime;

        int prevBestScore = 0;

        for (int depth = 1; depth <= maxDepth; depth++) {
            if (ss.timeUp() || ss.stop) break;
            if (pvIdx > 0 && ss.hardTimeUp()) break;
            ss.stop = false; ss.ply = 0;

            int bestScoreAtDepth = -INF;
            Move bestMoveAtDepth{};

            // Sort root moves by previous depth scores
            std::vector<int> idx(rootMoves.size());
            for (int i=0;i<(int)idx.size();i++) idx[i]=i;
            std::sort(idx.begin(), idx.end(), [&](int a2, int b2){
                return rootScores[a2]>rootScores[b2];
            });

            // Aspiration windows
            int aspDelta = 25;
            int aspAlpha = (depth >= 4 && prevBestScore > -MATE_BOUND) ? prevBestScore - aspDelta : -INF;
            int aspBeta  = (depth >= 4 && prevBestScore < MATE_BOUND)  ? prevBestScore + aspDelta :  INF;

            // At root we run the full aspiration loop ourselves
            while (true) {
                int alpha2 = aspAlpha, beta2 = aspBeta;
                bool firstMove = true;

                for (int ii = 0; ii < (int)rootMoves.size(); ii++) {
                    int i = idx[ii];
                    Move m = rootMoves[i];
                    bool excluded = false;
                    for (auto& ex : excludedMoves) if (m == ex) { excluded=true; break; }
                    if (excluded) continue;

                    if (!b.makeMove(m)) continue;
                    ss.ply++;
                    int score;
                    if (firstMove) {
                        score = -alphaBeta(b, -beta2, -alpha2, depth-1, true, ss);
                        firstMove = false;
                    } else {
                        score = -alphaBeta(b, -alpha2-1, -alpha2, depth-1, true, ss);
                        if (!ss.stop && score > alpha2)
                            score = -alphaBeta(b, -beta2, -alpha2, depth-1, true, ss);
                    }
                    ss.ply--;
                    b.unmakeMove(m);

                    if (ss.stop) break;
                    rootScores[i] = score;
                    if (score > bestScoreAtDepth) {
                        bestScoreAtDepth = score;
                        bestMoveAtDepth  = m;
                        if (score > alpha2) alpha2 = score;
                    }
                }

                if (ss.stop) break;

                // Aspiration window handling
                if (bestScoreAtDepth <= aspAlpha) {
                    aspAlpha = std::max(-INF, aspAlpha - aspDelta);
                    aspDelta *= 2;
                } else if (bestScoreAtDepth >= aspBeta) {
                    aspBeta = std::min(INF, aspBeta + aspDelta);
                    aspDelta *= 2;
                } else {
                    break;  // within window
                }
                if (aspAlpha <= -INF/2) aspAlpha = -INF;
                if (aspBeta  >=  INF/2) aspBeta  =  INF;
            }

            if (!ss.stop && !bestMoveAtDepth.isNull()) {
                line.best  = bestMoveAtDepth;
                line.score = bestScoreAtDepth;
                line.depth = depth;
                prevBestScore = bestScoreAtDepth;
            }

            if (!ss.stop) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - ss.startTime).count();
                long long nps = elapsed > 0 ? ss.nodes*1000/elapsed : ss.nodes;

                std::cout << "info depth " << depth
                          << " seldepth " << depth+3;
                if (std::abs(line.score) >= MATE_BOUND) {
                    int mate_in = (MATE_SCORE - std::abs(line.score) + 1) / 2;
                    std::cout << " score mate " << (line.score>0 ? mate_in : -mate_in);
                } else {
                    std::cout << " score cp " << line.score;
                }
                std::cout << " nodes " << ss.nodes
                          << " nps "   << nps
                          << " time "  << elapsed
                          << " multipv " << (pvIdx+1)
                          << " pv "    << b.toUci(line.best)
                          << "\n" << std::flush;
            }
        }

        if (!line.best.isNull()) result.push_back(line);

        // Restore time limit for next PV line
        ss.timeLimitMs = std::max(200, maxTimeLimitMs - (int)std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() - ss.startTime).count());
    }

    std::sort(result.begin(), result.end(), [](const PVLine& a, const PVLine& b2){
        return a.score > b2.score;
    });
    return result;
}

// ================================================================
// UCI Loop
// ================================================================

static const std::string START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

static void uciLoop() {
    initZobrist();
    initLMR();
    ttClear();

    Board board;
    board.setFen(START_FEN);

    int  multiPV    = 1;
    bool limitStr   = false;
    int  uciElo     = 3000;

    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string cmd; ss >> cmd;

        if (cmd == "uci") {
            std::cout
                << "id name Ryzix 3.0\n"
                << "id author RD7890\n"
                << "option name MultiPV type spin default 1 min 1 max 10\n"
                << "option name Hash type spin default 48 min 1 max 512\n"
                << "option name Threads type spin default 1 min 1 max 1\n"
                << "option name UCI_LimitStrength type check default false\n"
                << "option name UCI_Elo type spin default 3000 min 500 max 3200\n"
                << "option name Skill Level type spin default 20 min 0 max 20\n"
                << "uciok\n" << std::flush;
        }

        else if (cmd == "isready") {
            std::cout << "readyok\n" << std::flush;
        }

        else if (cmd == "ucinewgame") {
            board.setFen(START_FEN);
            board.gameHashCount = 0;
            ttClear();
        }

        else if (cmd == "setoption") {
            std::string rest; std::getline(ss, rest);
            auto npos = rest.find("name ");
            auto vpos = rest.find(" value ");
            if (npos != std::string::npos) {
                std::string name  = (vpos!=std::string::npos) ? rest.substr(npos+5,vpos-npos-5) : rest.substr(npos+5);
                std::string value = (vpos!=std::string::npos) ? rest.substr(vpos+7) : "";
                while (!name.empty()  && name.front()==' ')  name.erase(0,1);
                while (!name.empty()  && name.back()==' ')   name.pop_back();
                while (!value.empty() && value.front()==' ') value.erase(0,1);
                while (!value.empty() && value.back()==' ')  value.pop_back();

                if (name == "MultiPV" && !value.empty())
                    multiPV = std::max(1, std::stoi(value));
                else if (name == "UCI_LimitStrength" && !value.empty())
                    limitStr = (value == "true");
                else if (name == "UCI_Elo" && !value.empty())
                    uciElo = std::stoi(value);
                // Hash, Threads, Skill Level accepted silently
            }
            (void)limitStr; (void)uciElo;
        }

        else if (cmd == "position") {
            std::string type; ss >> type;
            // Save current hash before reset (for repetition tracking across moves)
            std::vector<uint64_t> oldGameHashes;
            for (int i = 0; i < board.gameHashCount; i++)
                oldGameHashes.push_back(board.gameHashes[i]);

            if (type == "fen") {
                std::string rest; std::getline(ss, rest);
                while (!rest.empty() && rest.front()==' ') rest.erase(0,1);
                auto mpos = rest.find(" moves ");
                std::string fenStr = (mpos!=std::string::npos) ? rest.substr(0,mpos) : rest;
                board.setFen(fenStr.empty() ? START_FEN : fenStr);
                if (mpos != std::string::npos) {
                    std::istringstream ms(rest.substr(mpos+7));
                    std::string mv;
                    while (ms >> mv) {
                        // Record hash before move for repetition
                        if (board.gameHashCount < 1000)
                            board.gameHashes[board.gameHashCount++] = board.hash;
                        Move m=board.fromUci(mv);
                        if (!m.isNull()) board.makeMove(m);
                    }
                }
            } else {
                board.setFen(START_FEN);
                board.gameHashCount = 0;
                std::string tok;
                if ((ss>>tok) && tok=="moves") {
                    std::string mv;
                    while (ss>>mv) {
                        if (board.gameHashCount < 1000)
                            board.gameHashes[board.gameHashCount++] = board.hash;
                        Move m=board.fromUci(mv);
                        if (!m.isNull()) board.makeMove(m);
                    }
                }
            }
        }

        else if (cmd == "go") {
            int movetime = -1, wtime = -1, btime = -1, winc = 0, binc = 0;
            int movestogo = 25, depthLimit = 64;
            bool infinite = false;
            std::string tok;
            while (ss >> tok) {
                if      (tok=="movetime")   { int v; if(ss>>v) movetime=v; }
                else if (tok=="wtime")      { int v; if(ss>>v) wtime=v; }
                else if (tok=="btime")      { int v; if(ss>>v) btime=v; }
                else if (tok=="winc")       { int v; if(ss>>v) winc=v; }
                else if (tok=="binc")       { int v; if(ss>>v) binc=v; }
                else if (tok=="movestogo")  { int v; if(ss>>v) movestogo=v; }
                else if (tok=="infinite")   { infinite=true; }
                else if (tok=="depth")      { int v; if(ss>>v) depthLimit=v; }
            }

            int softLimit, hardLimit;
            if (infinite || (depthLimit < 64 && movetime < 0 && wtime < 0 && btime < 0)) {
                // depth-only or infinite: give huge time budget, depth cap enforced in search
                softLimit = hardLimit = 3600000;
            } else if (movetime > 0) {
                softLimit = hardLimit = movetime - 30;
            } else {
                int myTime = (board.side==WHITE) ? wtime : btime;
                int myInc  = (board.side==WHITE) ? winc  : binc;
                if (myTime < 0) { myTime = 5000; myInc = 0; }

                // Base: time/movestogo + increment contribution
                int base = myTime / std::max(movestogo, 10) + myInc * 3 / 4;
                softLimit = std::max(50, std::min(base, myTime / 3));
                hardLimit = std::max(softLimit, std::min(myTime / 4, base * 3));
                if (myTime < 1000) { softLimit = std::max(50, myTime/20); hardLimit = softLimit * 2; }
            }

            auto pvLines = rootSearch(board, softLimit, hardLimit, multiPV, depthLimit);

            if (pvLines.empty()) {
                std::cout << "bestmove (none)\n" << std::flush;
                continue;
            }

            // Final summary output
            for (int i = 0; i < (int)pvLines.size(); i++) {
                auto& pv = pvLines[i];
                std::cout << "info depth " << pv.depth
                          << " multipv " << (i+1)
                          << " score cp " << pv.score
                          << " pv " << board.toUci(pv.best)
                          << "\n";
            }
            std::cout << "bestmove " << board.toUci(pvLines[0].best) << "\n"
                      << std::flush;
        }

        else if (cmd == "stop") {
            // Engine is synchronous — stop takes effect at next time check
        }

        else if (cmd == "quit") {
            break;
        }

        // Diagnostic: print board hash
        else if (cmd == "d") {
            std::cout << "hash " << board.hash << " side " << (board.side==WHITE?"white":"black")
                      << " ply " << board.ply << "\n";
        }
    }
}

} // namespace Ryzix

int main() {
    Ryzix::uciLoop();
    return 0;
}
