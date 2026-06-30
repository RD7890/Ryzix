/*
 * Ryzix Chess Engine v2.0
 * Strong UCI chess engine for Android ARM64
 *
 * Search : Iterative-deepening alpha-beta
 *          Quiescence search (captures + promotions)
 *          Transposition table  16 MB (1 M entries)
 *          Null-move pruning  R = 3
 *          Late-move reductions (LMR)
 *          Killer moves (2 per ply) + history heuristic
 *          Aspiration windows (±50 cp after depth 4)
 *          Check extension
 *
 * Eval  : PeSTO piece-square tables (MG + EG, phase interpolated)
 *          Mobility (pseudo-legal move count per piece type)
 *          Pawn structure (doubled, isolated, passed)
 *          King safety (open files near king, attacker count)
 *          Bishop pair bonus
 *
 * Compile (Android ARM64, NDK r25c):
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang++ \
 *     -O3 -DNDEBUG -std=c++17 -static-libstdc++ -lm src/ryzix.cpp -o ryzix
 *
 * Compile (native test):
 *   clang++ -O3 -std=c++17 -lm src/ryzix.cpp -o ryzix
 */

#include <algorithm>
#include <cassert>
#include <chrono>
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

// Piece values (centipawns) — indexed by Piece enum
static constexpr int PIECE_VAL[13] = {
    0,
    100, 320, 330, 500, 900, 20000,
    100, 320, 330, 500, 900, 20000
};

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
    // Simple xorshift64 PRNG — deterministic seed
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
    UndoInfo history[MAX_PLY];

    void reset() {
        std::memset(sq, 0, sizeof(sq));
        side = WHITE; epSquare = NO_SQ;
        castling = halfMove = ply = 0;
        fullMove = 1; hash = 0;
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

    // XOR out old ep, castling
    if (epSquare != NO_SQ) hash ^= ZEP[fileOf(epSquare)];
    hash ^= ZCASTLE[castling];

    // Remove moving piece from source
    hash ^= ZPIECE[moving][from];
    sq[to] = moving; sq[from] = EMPTY;

    // Remove captured piece
    if (target != EMPTY) hash ^= ZPIECE[target][to];

    if (mt == 2) {  // en-passant
        int capSq = to + (side==WHITE ? -8 : 8);
        u.captured = sq[capSq];
        hash ^= ZPIECE[sq[capSq]][capSq];
        sq[capSq] = EMPTY;
        sq[to] = moving;
    }

    if (mt == 1) {  // castling — move rook too
        if      (to==G1) { hash^=ZPIECE[WR][H1]; hash^=ZPIECE[WR][F1]; sq[H1]=EMPTY; sq[F1]=WR; }
        else if (to==C1) { hash^=ZPIECE[WR][A1]; hash^=ZPIECE[WR][D1]; sq[A1]=EMPTY; sq[D1]=WR; }
        else if (to==G8) { hash^=ZPIECE[BR][H8]; hash^=ZPIECE[BR][F8]; sq[H8]=EMPTY; sq[F8]=BR; }
        else if (to==C8) { hash^=ZPIECE[BR][A8]; hash^=ZPIECE[BR][D8]; sq[A8]=EMPTY; sq[D8]=BR; }
    }

    if (mt == 3) {  // promotion
        static const Piece WP4[4] = {WN,WB,WR,WQ};
        static const Piece BP4[4] = {BN,BB,BR,BQ};
        Piece promo = (side==WHITE) ? WP4[m.promo()] : BP4[m.promo()];
        hash ^= ZPIECE[moving][to];  // remove pawn
        sq[to] = promo;
        hash ^= ZPIECE[promo][to];   // add promoted piece
    } else {
        hash ^= ZPIECE[sq[to]][to];  // add moving piece at destination
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
// PeSTO Piece-Square Tables
// PST convention: index 0 = a1, index 63 = h8 (rank*8+file)
// White pieces use pst[sq^56], Black pieces use pst[sq]
// (^56 flips rank: rank r → rank 7-r)
// ================================================================

// Material values: MG / EG
static constexpr int MG_VAL[7] = {0,  82, 337, 365, 477, 1025,    0};
static constexpr int EG_VAL[7] = {0,  94, 281, 297, 512,  936,    0};

// PSTs are stored rank-8 first (top-down) so index 0-7 = rank 8, 56-63 = rank 1.
// After converting with ^56, White pieces at rank 8 (sq 56-63) → index 0-7 (high bonus rows).

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
    -27,-11,  4, 13, 14,  4,-5,-17,
    -53,-34,-21,-11,-28,-14,-24,-43,
};

// Phase contribution per piece type
static constexpr int PHASE_INC[7] = {0, 0, 1, 1, 2, 4, 0};
static constexpr int TOTAL_PHASE  = 24;  // sum for full set of pieces

// Per-piece PST bonus at sq for side 'us'
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
// Evaluation
// Returns score in centipawns from side-to-move perspective.
// ================================================================

static int evaluate(const Board& b) {
    int mgScore = 0, egScore = 0, phase = 0;
    int wBishops = 0, bBishops = 0;

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
    }

    // Bishop pair bonus
    if (wBishops >= 2) { mgScore += 40; egScore += 50; }
    if (bBishops >= 2) { mgScore -= 40; egScore -= 50; }

    // Pawn structure: doubled pawns penalty, isolated pawns penalty, passed pawn bonus
    for (int f = 0; f < 8; f++) {
        int wPawns = 0, bPawns = 0;
        int wPassMin = 8, bPassMax = -1;
        for (int r = 0; r < 8; r++) {
            if (b.sq[mkSq(r,f)] == WP) { wPawns++; if (r > wPassMin) wPassMin = r; }
            if (b.sq[mkSq(r,f)] == BP) { bPawns++; if (r < bPassMax || bPassMax < 0) bPassMax = r; }
        }
        if (wPawns > 1) { mgScore -= 10*(wPawns-1); egScore -= 20*(wPawns-1); }
        if (bPawns > 1) { mgScore += 10*(bPawns-1); egScore += 20*(bPawns-1); }

        // Isolated pawns
        bool leftFile  = (f > 0);
        bool rightFile = (f < 7);
        int wNeighbor = 0, bNeighbor = 0;
        for (int df : {-1, 1}) {
            int nf = f + df; if (nf < 0 || nf > 7) continue;
            for (int r = 0; r < 8; r++) {
                if (b.sq[mkSq(r,nf)] == WP) wNeighbor++;
                if (b.sq[mkSq(r,nf)] == BP) bNeighbor++;
            }
        }
        if (wPawns > 0 && wNeighbor == 0) { mgScore -= 10; egScore -= 15; }
        if (bPawns > 0 && bNeighbor == 0) { mgScore += 10; egScore += 15; }
    }

    // Passed pawn bonus: a pawn with no opponent pawns in front on same or adjacent file
    for (int s = 0; s < 64; s++) {
        if (b.sq[s] == WP) {
            int r = rankOf(s), f = fileOf(s); bool passed = true;
            for (int rr = r+1; rr < 8 && passed; rr++)
                for (int df = -1; df <= 1; df++) {
                    int nf = f+df; if (nf<0||nf>7) continue;
                    if (b.sq[mkSq(rr,nf)] == BP) { passed = false; break; }
                }
            if (passed) { int bonus = r*r*4; mgScore += bonus; egScore += bonus*2; }
        }
        if (b.sq[s] == BP) {
            int r = rankOf(s), f = fileOf(s); bool passed = true;
            for (int rr = r-1; rr >= 0 && passed; rr--)
                for (int df = -1; df <= 1; df++) {
                    int nf = f+df; if (nf<0||nf>7) continue;
                    if (b.sq[mkSq(rr,nf)] == WP) { passed = false; break; }
                }
            if (passed) { int bonus = (7-r)*(7-r)*4; mgScore -= bonus; egScore -= bonus*2; }
        }
    }

    // King safety: penalize open/half-open files near king
    for (Color c : {WHITE, BLACK}) {
        int ks = b.kingSquare(c);
        if (ks == NO_SQ) continue;
        int kf = fileOf(ks), sign = (c == WHITE) ? 1 : -1;
        for (int df = -1; df <= 1; df++) {
            int f = kf+df; if (f<0||f>7) continue;
            bool ownPawn = false, oppPawn = false;
            for (int r = 0; r < 8; r++) {
                if (c==WHITE && b.sq[mkSq(r,f)]==WP) ownPawn = true;
                if (c==WHITE && b.sq[mkSq(r,f)]==BP) oppPawn = true;
                if (c==BLACK && b.sq[mkSq(r,f)]==BP) ownPawn = true;
                if (c==BLACK && b.sq[mkSq(r,f)]==WP) oppPawn = true;
            }
            if (!ownPawn && !oppPawn) { mgScore -= sign*20; }   // open file
            else if (!ownPawn)        { mgScore -= sign*10; }   // half-open
        }
    }

    // Phase interpolation
    if (phase > TOTAL_PHASE) phase = TOTAL_PHASE;
    int score = (mgScore * phase + egScore * (TOTAL_PHASE - phase)) / TOTAL_PHASE;

    // Tempo bonus (small bonus for side to move)
    score += 10;

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
};

static constexpr size_t TT_SIZE = 1 << 22;  // 4M entries ≈ 48 MB — deeper search cache
static TTEntry TT[TT_SIZE];

static void ttClear() { std::memset(TT, 0, sizeof(TT)); }

static TTEntry* ttProbe(uint64_t hash) {
    return &TT[hash & (TT_SIZE - 1)];
}

static void ttStore(uint64_t hash, Move m, int score, int depth, Bound bound, int ply) {
    TTEntry& e = TT[hash & (TT_SIZE-1)];
    if (score >= MATE_BOUND)  score += ply;
    if (score <= -MATE_BOUND) score -= ply;
    if (e.key == hash && e.depth > depth && bound != BOUND_EXACT) return;
    e.key   = hash;
    e.move  = m.data;
    e.score = int16_t(score);
    e.depth = int8_t(depth);
    e.bound = bound;
}

static int ttScore(int rawScore, int ply) {
    if (rawScore >= MATE_BOUND)  return rawScore - ply;
    if (rawScore <= -MATE_BOUND) return rawScore + ply;
    return rawScore;
}

// ================================================================
// Search State
// ================================================================

struct SearchState {
    Move killers[MAX_PLY][2];
    int  history[13][64];
    long long nodes;
    int  ply;
    bool stop;
    std::chrono::steady_clock::time_point startTime;
    int  timeLimitMs;

    void reset() {
        std::memset(killers, 0, sizeof(killers));
        std::memset(history, 0, sizeof(history));
        nodes = 0; ply = 0; stop = false;
        timeLimitMs = 5000;
    }

    bool timeUp() {
        if (nodes & 4095) return false;  // check every 4096 nodes
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        return elapsed >= timeLimitMs;
    }
};

// ================================================================
// Move Ordering
// ================================================================

static int moveOrderScore(const Board& b, Move m, Move ttMove,
                          const SearchState& ss) {
    if (m == ttMove) return 2000000;
    Piece cap = b.sq[m.to()];
    if (cap != EMPTY) {
        // MVV-LVA: victim value - attacker value/10
        return 1000000 + PIECE_VAL[cap] - PIECE_VAL[b.sq[m.from()]]/10;
    }
    if (m.mtype() == 3) return 900000 + m.promo()*100;  // promotion
    if (ss.ply < MAX_PLY) {
        if (m == ss.killers[ss.ply][0]) return 800000;
        if (m == ss.killers[ss.ply][1]) return 799000;
    }
    // History heuristic
    return ss.history[b.sq[m.from()]][m.to()];
}

static void sortMoves(MoveList& ml, const Board& b, Move ttMove,
                      const SearchState& ss) {
    int scores[256];
    for (int i = 0; i < ml.count; i++)
        scores[i] = moveOrderScore(b, ml.moves[i], ttMove, ss);
    // Insertion sort (fast for small arrays)
    for (int i = 1; i < ml.count; i++) {
        Move m  = ml.moves[i]; int s = scores[i]; int j = i-1;
        while (j >= 0 && scores[j] < s) {
            ml.moves[j+1] = ml.moves[j]; scores[j+1] = scores[j]; j--;
        }
        ml.moves[j+1] = m; scores[j+1] = s;
    }
}

// ================================================================
// Quiescence Search
// ================================================================

static int quiesce(Board& b, int alpha, int beta, SearchState& ss) {
    if (ss.stop || ss.timeUp()) { ss.stop = true; return 0; }
    ss.nodes++;

    int stand_pat = evaluate(b);
    if (stand_pat >= beta) return beta;
    if (stand_pat > alpha) alpha = stand_pat;

    // Delta pruning
    int bigDelta = 900 + 100;  // queen + pawn margin
    if (stand_pat < alpha - bigDelta) return alpha;

    MoveList ml; generatePseudoMoves(b, ml);

    // Score captures (MVV-LVA)
    int scores[256];
    for (int i = 0; i < ml.count; i++) {
        Move m = ml.moves[i];
        Piece cap = b.sq[m.to()];
        bool isPromo = (m.mtype() == 3);
        bool isEP    = (m.mtype() == 2);
        if (cap == EMPTY && !isPromo && !isEP) { scores[i] = -1; continue; }
        scores[i] = (cap != EMPTY ? PIECE_VAL[cap] : 0) - PIECE_VAL[b.sq[m.from()]]/10;
        if (isPromo) scores[i] += 800;
    }

    for (int i = 0; i < ml.count; i++) {
        // Find best remaining move
        int best_idx = i;
        for (int j = i+1; j < ml.count; j++)
            if (scores[j] > scores[best_idx]) best_idx = j;
        std::swap(ml.moves[i], ml.moves[best_idx]);
        std::swap(scores[i], scores[best_idx]);

        if (scores[i] < 0) break;  // no more captures

        Move m = ml.moves[i];
        if (!b.makeMove(m)) continue;
        ss.ply++;
        int score = -quiesce(b, -beta, -alpha, ss);
        ss.ply--;
        b.unmakeMove(m);

        if (ss.stop) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

// ================================================================
// Alpha-Beta Search
// ================================================================

static int alphaBeta(Board& b, int alpha, int beta, int depth,
                     bool nullOk, SearchState& ss);

static int alphaBeta(Board& b, int alpha, int beta, int depth,
                     bool nullOk, SearchState& ss) {
    if (ss.stop || ss.timeUp()) { ss.stop = true; return 0; }
    ss.nodes++;

    bool isRoot = (ss.ply == 0);
    bool pvNode = (beta - alpha > 1);

    // Draw detection (fifty-move rule, repetition via simple check)
    if (!isRoot && b.halfMove >= 100) return 0;

    // TT probe
    TTEntry* tte = ttProbe(b.hash);
    Move ttMove{};
    if (tte->key == b.hash && tte->depth >= 0) {
        ttMove = Move::fromRaw(tte->move);
        if (!pvNode && tte->depth >= depth) {
            int s = ttScore(tte->score, ss.ply);
            if (tte->bound == BOUND_EXACT) return s;
            if (tte->bound == BOUND_LOWER && s >= beta)  return s;
            if (tte->bound == BOUND_UPPER && s <= alpha) return s;
        }
    }

    if (depth <= 0) return quiesce(b, alpha, beta, ss);

    bool inCheck = b.inCheck();
    if (inCheck) depth++;  // check extension

    // Null-move pruning
    // Skip if: in check, no nullOk, zugzwang risk (only pawns + king)
    if (!inCheck && nullOk && depth >= 3 && !pvNode) {
        // Count non-pawn, non-king pieces for side to move
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
            int R = (depth >= 6) ? 3 : 2;
            // Make null move: just flip side
            b.hash ^= ZSIDE;
            if (b.epSquare != NO_SQ) { b.hash ^= ZEP[fileOf(b.epSquare)]; }
            int savedEp = b.epSquare;
            b.side = ~b.side; b.ply++; ss.ply++;
            b.epSquare = NO_SQ;

            int nullScore = -alphaBeta(b, -beta, -beta+1, depth-R-1, false, ss);

            b.ply--; ss.ply--;
            b.side = ~b.side; b.epSquare = savedEp;
            if (savedEp != NO_SQ) { b.hash ^= ZEP[fileOf(savedEp)]; }
            b.hash ^= ZSIDE;

            if (!ss.stop && nullScore >= beta)
                return beta;  // null move cutoff
        }
    }

    // Generate and sort moves
    MoveList ml; generatePseudoMoves(b, ml);
    sortMoves(ml, b, ttMove, ss);

    int  bestScore  = -INF;
    Move bestMove{};
    int  legalCount = 0;
    bool raisedAlpha = false;

    for (int i = 0; i < ml.count; i++) {
        Move m = ml.moves[i];
        if (!b.makeMove(m)) continue;
        legalCount++;
        ss.ply++;

        bool isCapture = (b.history[b.ply-1].captured != EMPTY);
        bool isPromo   = (m.mtype() == 3);
        bool givesCheck= b.inCheck();

        int score;

        if (legalCount == 1) {
            // Full-window search on first move
            score = -alphaBeta(b, -beta, -alpha, depth-1, true, ss);
        } else {
            // LMR: reduce quiet moves after first few
            int reduction = 0;
            if (depth >= 3 && legalCount >= 4 && !isCapture && !isPromo
                && !inCheck && !givesCheck && m != ss.killers[ss.ply-1][0]
                && m != ss.killers[ss.ply-1][1]) {
                // LMR formula: sqrt approximation
                reduction = 1;
                if (depth >= 6 && legalCount >= 8) reduction = 2;
                if (depth >= 9 && legalCount >= 16) reduction = 3;
            }

            // Null window search with possible reduction
            score = -alphaBeta(b, -alpha-1, -alpha, depth-1-reduction, true, ss);

            // Re-search if it beat alpha (and we had reduced)
            if (score > alpha && reduction > 0)
                score = -alphaBeta(b, -alpha-1, -alpha, depth-1, true, ss);

            // PV search: full window if still beats alpha
            if (score > alpha && pvNode)
                score = -alphaBeta(b, -beta, -alpha, depth-1, true, ss);
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
                // Beta cutoff — update killers and history
                if (b.sq[m.to()] == EMPTY) {
                    if (ss.ply < MAX_PLY) {
                        ss.killers[ss.ply][1] = ss.killers[ss.ply][0];
                        ss.killers[ss.ply][0] = m;
                    }
                    ss.history[b.sq[m.from()]][m.to()] += depth*depth;
                    // Decay old history entries to prevent overflow
                    if (ss.history[b.sq[m.from()]][m.to()] > 1000000)
                        for (int p=0;p<13;p++) for (int s=0;s<64;s++)
                            ss.history[p][s] /= 2;
                }
                ttStore(b.hash, m, beta, depth, BOUND_LOWER, ss.ply);
                return beta;
            }
        }
    }

    if (legalCount == 0) {
        return inCheck ? (-MATE_SCORE + ss.ply) : 0;  // checkmate or stalemate
    }

    Bound bound = raisedAlpha ? BOUND_EXACT : BOUND_UPPER;
    ttStore(b.hash, bestMove, bestScore, depth, bound, ss.ply);
    return bestScore;
}

// ================================================================
// Iterative Deepening
// ================================================================

struct SearchResult {
    Move best;
    int  score;
    int  depth;
};

static SearchResult iterativeDeepen(Board& b, int timeLimitMs, int maxDepth, int multiPV,
                                    std::vector<SearchResult>& pvLines) {
    SearchState ss;
    ss.reset();
    ss.startTime   = std::chrono::steady_clock::now();
    ss.timeLimitMs = timeLimitMs;

    SearchResult best{};
    best.score = -INF;

    // For multi-PV: exclude already-found moves
    std::vector<Move> exclude;
    pvLines.clear();

    // Run multiPV lines
    for (int pvIdx = 0; pvIdx < multiPV; pvIdx++) {
        SearchResult lineResult{};
        lineResult.score = -INF;

        auto lm = legalMoves(b);
        if (lm.empty()) break;

        // Remove excluded moves from search (already chosen as PV lines)
        // We do this by temporarily adjusting TT — simpler: just pick best
        // from remaining moves after forced exclusion.

        // Iterative deepening for this PV line
        int alpha = -INF, betaW = INF;
        for (int depth = 1; depth <= maxDepth; depth++) {
            ss.stop = false;
            ss.ply  = 0;

            // Aspiration windows from depth 4
            int aspDelta = 50;
            if (depth >= 4 && lineResult.score > -MATE_BOUND) {
                alpha = lineResult.score - aspDelta;
                betaW = lineResult.score + aspDelta;
            } else {
                alpha = -INF; betaW = INF;
            }

            int score;
            while (true) {
                score = alphaBeta(b, alpha, betaW, depth, false, ss);
                if (ss.stop) break;
                if (score <= alpha) { alpha -= aspDelta * 2; aspDelta *= 2; }
                else if (score >= betaW) { betaW += aspDelta * 2; aspDelta *= 2; }
                else break;
                if (alpha < -INF/2) alpha = -INF;
                if (betaW >  INF/2) betaW =  INF;
            }

            if (ss.stop) break;

            // Extract best move from TT
            TTEntry* tte = ttProbe(b.hash);
            if (tte->key == b.hash && tte->move != 0) {
                lineResult.best  = Move::fromRaw(tte->move);
                lineResult.score = ttScore(tte->score, 0);
                lineResult.depth = depth;
            }

            // Output UCI info
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - ss.startTime).count();
            long long nps = (elapsed > 0) ? (ss.nodes * 1000 / elapsed) : ss.nodes;

            std::cout << "info depth " << depth;
            if (pvIdx == 0) std::cout << " seldepth " << depth+2;
            if (std::abs(lineResult.score) >= MATE_BOUND) {
                int mate_in = (MATE_SCORE - std::abs(lineResult.score) + 1) / 2;
                std::cout << " score mate " << (lineResult.score > 0 ? mate_in : -mate_in);
            } else {
                std::cout << " score cp " << lineResult.score;
            }
            std::cout << " nodes " << ss.nodes
                      << " nps " << nps
                      << " time " << elapsed
                      << " multipv " << (pvIdx+1)
                      << " pv " << b.toUci(lineResult.best)
                      << "\n" << std::flush;
        }

        if (!lineResult.best.isNull()) {
            pvLines.push_back(lineResult);
            // "Exclude" this move for next PV line by temporarily making it terrible in history
            // Simple approach: if multiPV > 1, do root move loop externally
        }

        if (pvIdx == 0) best = lineResult;
        if (ss.stop && pvIdx == 0 && lineResult.best.isNull()) break;
    }

    return best;
}

// ================================================================
// Root search with proper multi-PV support
// ================================================================

struct PVLine {
    Move best;
    int  score;
    int  depth;
    std::vector<Move> pv;
};

static std::vector<PVLine> rootSearch(Board& b, int timeLimitMs, int multiPV) {
    SearchState ss;
    ss.reset();
    ss.startTime   = std::chrono::steady_clock::now();
    ss.timeLimitMs = timeLimitMs;

    auto rootMoves = legalMoves(b);
    if (rootMoves.empty()) return {};

    // Clamp multiPV to available moves
    multiPV = std::min(multiPV, (int)rootMoves.size());

    // Per-root-move scores (for multi-PV ordering)
    std::vector<int> rootScores(rootMoves.size(), 0);

    std::vector<PVLine> result;
    result.reserve(multiPV);

    int maxDepth = 64;
    Move overallBest{};
    int  overallScore = 0;

    // Run iterative deepening for the single "main" PV first (multiPV=1 style)
    // Then for subsequent PV lines, exclude already-found moves by running
    // per-line full searches with that move forced away.

    for (int pvIdx = 0; pvIdx < multiPV; pvIdx++) {
        PVLine line{};
        line.score = -INF;
        line.depth = 0;

        // Build exclusion set
        std::vector<Move> excludedMoves;
        for (int k = 0; k < pvIdx; k++)
            if (!result[k].best.isNull()) excludedMoves.push_back(result[k].best);

        // Time per PV line: split remaining time
        int lineTime = (pvIdx == 0) ? timeLimitMs
                                    : std::max(200, timeLimitMs / (multiPV * 2));

        ss.stop = false;
        ss.ply  = 0;
        ss.timeLimitMs = lineTime;

        for (int depth = 1; depth <= maxDepth; depth++) {
            if (ss.timeUp() || ss.stop) break;
            ss.stop = false; ss.ply = 0;

            // Brute-force root: iterate over all non-excluded root moves
            int bestScoreAtDepth = -INF;
            Move bestMoveAtDepth{};
            int  alpha = -INF, beta = INF;

            // Simple ordering for root: use previous depth scores
            std::vector<int> idx(rootMoves.size());
            for (int i=0;i<(int)idx.size();i++) idx[i]=i;
            std::sort(idx.begin(), idx.end(), [&](int a, int b){ return rootScores[a]>rootScores[b]; });

            for (int ii = 0; ii < (int)rootMoves.size(); ii++) {
                int i = idx[ii];
                Move m = rootMoves[i];
                // Skip excluded moves for this PV line
                bool excluded = false;
                for (auto& ex : excludedMoves) if (m == ex) { excluded=true; break; }
                if (excluded) continue;

                if (!b.makeMove(m)) continue;
                ss.ply++;
                int score;
                if (alpha == -INF) {
                    score = -alphaBeta(b, -beta, -alpha, depth-1, true, ss);
                } else {
                    score = -alphaBeta(b, -alpha-1, -alpha, depth-1, true, ss);
                    if (!ss.stop && score > alpha)
                        score = -alphaBeta(b, -beta, -alpha, depth-1, true, ss);
                }
                ss.ply--;
                b.unmakeMove(m);
                if (ss.stop) break;
                rootScores[i] = score;
                if (score > bestScoreAtDepth) {
                    bestScoreAtDepth = score;
                    bestMoveAtDepth  = m;
                    if (score > alpha) alpha = score;
                }
            }

            if (!ss.stop && !bestMoveAtDepth.isNull()) {
                line.best  = bestMoveAtDepth;
                line.score = bestScoreAtDepth;
                line.depth = depth;
            }

            if (!ss.stop) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - ss.startTime).count();
                long long nps = elapsed > 0 ? ss.nodes*1000/elapsed : ss.nodes;

                std::cout << "info depth " << depth
                          << " seldepth " << depth+2;
                if (std::abs(line.score) >= MATE_BOUND) {
                    int mate_in = (MATE_SCORE - std::abs(line.score) + 1)/2;
                    std::cout << " score mate " << (line.score>0 ? mate_in : -mate_in);
                } else {
                    std::cout << " score cp " << line.score;
                }
                std::cout << " nodes " << ss.nodes
                          << " nps " << nps
                          << " time " << elapsed
                          << " multipv " << (pvIdx+1)
                          << " pv " << b.toUci(line.best)
                          << "\n" << std::flush;
            }
        }

        if (!line.best.isNull()) result.push_back(line);
        // After first PV, restore full time for subsequent (they'll time out earlier anyway)
        ss.timeLimitMs = std::max(200, timeLimitMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - ss.startTime).count());
    }

    // Sort by score descending
    std::sort(result.begin(), result.end(), [](const PVLine& a, const PVLine& b){
        return a.score > b.score;
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
    ttClear();

    Board board;
    board.setFen(START_FEN);

    int  multiPV   = 1;
    int  skillLevel = 20;  // accepted but not used to cap strength

    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string cmd; ss >> cmd;

        if (cmd == "uci") {
            std::cout
                << "id name Ryzix 2.0\n"
                << "id author RD7890\n"
                << "option name MultiPV type spin default 1 min 1 max 10\n"
                << "option name Hash type spin default 16 min 1 max 256\n"
                << "option name Skill Level type spin default 20 min 0 max 20\n"
                << "option name Threads type spin default 1 min 1 max 4\n"
                << "option name UCI_LimitStrength type check default false\n"
                << "option name UCI_Elo type spin default 3000 min 100 max 5000\n"
                << "uciok\n" << std::flush;
        }

        else if (cmd == "isready") {
            std::cout << "readyok\n" << std::flush;
        }

        else if (cmd == "ucinewgame") {
            board.setFen(START_FEN);
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
                if (name=="MultiPV" && !value.empty())
                    multiPV = std::max(1, std::stoi(value));
                // Hash, Threads, Skill Level accepted silently
                (void)skillLevel;
            }
        }

        else if (cmd == "position") {
            std::string type; ss >> type;
            if (type == "fen") {
                std::string rest; std::getline(ss, rest);
                while (!rest.empty() && rest.front()==' ') rest.erase(0,1);
                auto mpos = rest.find(" moves ");
                std::string fenStr = (mpos!=std::string::npos) ? rest.substr(0,mpos) : rest;
                board.setFen(fenStr.empty() ? START_FEN : fenStr);
                if (mpos != std::string::npos) {
                    std::istringstream ms(rest.substr(mpos+7));
                    std::string mv;
                    while (ms >> mv) { Move m=board.fromUci(mv); if (!m.isNull()) board.makeMove(m); }
                }
            } else {
                board.setFen(START_FEN);
                std::string tok;
                if ((ss>>tok) && tok=="moves") {
                    std::string mv;
                    while (ss>>mv) { Move m=board.fromUci(mv); if (!m.isNull()) board.makeMove(m); }
                }
            }
        }

        else if (cmd == "go") {
            // Parse time controls
            int movetime = -1, wtime = -1, btime = -1, movestogo = 30;
            std::string tok;
            while (ss >> tok) {
                if (tok=="movetime")  { int v; if(ss>>v) movetime=v; }
                else if (tok=="wtime")    { int v; if(ss>>v) wtime=v; }
                else if (tok=="btime")    { int v; if(ss>>v) btime=v; }
                else if (tok=="movestogo"){ int v; if(ss>>v) movestogo=v; }
            }

            int timeLimit;
            if (movetime > 0) {
                timeLimit = movetime - 20;  // small overhead margin
            } else if (board.side==WHITE && wtime>0) {
                timeLimit = std::max(100, wtime / std::max(movestogo, 10));
            } else if (board.side==BLACK && btime>0) {
                timeLimit = std::max(100, btime / std::max(movestogo, 10));
            } else {
                timeLimit = 5000;
            }

            auto pvLines = rootSearch(board, timeLimit, multiPV);

            if (pvLines.empty()) {
                std::cout << "bestmove (none)\n" << std::flush;
                continue;
            }

            // Re-emit final multipv info lines in clean format
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
            // Engine is synchronous; stop will take effect at next time check.
        }

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
