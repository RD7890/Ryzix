/*
 * Ryzix Chess Engine
 * position.cpp — Board implementation.
 *
 * Covers: Zobrist key initialisation, FEN parsing, UCI move
 * encoding, make-move / unmake-move (with incremental hash
 * updates), attack detection, repetition detection.
 */
#include "position.h"
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace Ryzix {

// ── Zobrist tables ───────────────────────────────────────────────
uint64_t ZPIECE[13][64];
uint64_t ZSIDE;
uint64_t ZEP[8];
uint64_t ZCASTLE[16];

const std::string START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

void initZobrist() {
    uint64_t s = 0x5EED1234CAFEBABEull;
    auto rng = [&]() -> uint64_t {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s;
    };
    for (int p = 0; p < 13; p++)
        for (int q = 0; q < 64; q++)
            ZPIECE[p][q] = rng();
    ZSIDE = rng();
    for (int f = 0; f < 8;  f++) ZEP[f]     = rng();
    for (int c = 0; c < 16; c++) ZCASTLE[c] = rng();
}

// ── Board::reset ─────────────────────────────────────────────────
void Board::reset() {
    std::memset(sq, 0, sizeof(sq));
    side = WHITE; epSquare = NO_SQ;
    castling = halfMove = ply = 0;
    fullMove = 1; hash = 0;
    gameHashCount = 0;
}

void Board::computeHash() {
    hash = 0;
    for (int s = 0; s < 64; s++)
        if (sq[s] != EMPTY) hash ^= ZPIECE[sq[s]][s];
    if (side == BLACK) hash ^= ZSIDE;
    if (epSquare != NO_SQ) hash ^= ZEP[fileOf(epSquare)];
    hash ^= ZCASTLE[castling];
}

// ── FEN parsing ──────────────────────────────────────────────────
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
    if (ep.size() == 2 && ep[0] >= 'a' && ep[0] <= 'h' &&
        (ep[1] == '3' || ep[1] == '6'))
        epSquare = mkSq(ep[1]-'1', ep[0]-'a');
    if (ss >> hm) halfMove = std::stoi(hm);
    if (ss >> fm) fullMove = std::stoi(fm);
    computeHash();
}

// ── UCI move formatting ──────────────────────────────────────────
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

// ── King square ──────────────────────────────────────────────────
int Board::kingSquare(Color c) const {
    Piece k = (c == WHITE) ? WK : BK;
    for (int i = 0; i < 64; i++) if (sq[i] == k) return i;
    return NO_SQ;
}

// ── Repetition detection ─────────────────────────────────────────
bool Board::isRepetition(int /*searchPly*/) const {
    int count = 0;
    // Walk search history (same side = step 2)
    for (int i = ply - 2; i >= 0; i -= 2) {
        if (history[i].hash == hash) {
            if (++count >= 1) return true;
        }
        if (history[i].halfMove == 0) break;
    }
    // Walk game history (before the current search)
    for (int i = gameHashCount - 1; i >= 0; i--) {
        if (gameHashes[i] == hash) {
            if (++count >= 2) return true;
        }
    }
    return false;
}

// ── Attack detection ─────────────────────────────────────────────
bool Board::isAttacked(int s, Color by) const {
    // Pawns
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

    // Knights
    Piece kn = (by == WHITE) ? WN : BN;
    static constexpr int KD[8] = {-17,-15,-10,-6,6,10,15,17};
    for (int d : KD) {
        int t = s + d;
        if (!onBoard(t)) continue;
        if (std::abs(fileOf(t)-fileOf(s)) > 2) continue;
        if (std::abs(rankOf(t)-rankOf(s)) > 2) continue;
        if (sq[t] == kn) return true;
    }

    // Diagonals (bishop / queen)
    Piece bi=(by==WHITE)?WB:BB, qu=(by==WHITE)?WQ:BQ;
    static constexpr int DD[4] = {-9,-7,7,9};
    for (int d : DD) {
        int cur = s;
        while (true) {
            int t = cur + d;
            if (!onBoard(t)) break;
            if (std::abs(fileOf(t)-fileOf(cur)) != 1) break;
            if (sq[t] == bi || sq[t] == qu) return true;
            if (sq[t] != EMPTY) break;
            cur = t;
        }
    }

    // Straights (rook / queen)
    Piece ro = (by==WHITE)?WR:BR;
    int r0=rankOf(s), f0=fileOf(s);
    for (int f=f0-1;f>=0;f--) { Piece p=sq[mkSq(r0,f)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }
    for (int f=f0+1;f< 8;f++) { Piece p=sq[mkSq(r0,f)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }
    for (int r=r0-1;r>=0;r--) { Piece p=sq[mkSq(r,f0)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }
    for (int r=r0+1;r< 8;r++) { Piece p=sq[mkSq(r,f0)]; if(p==ro||p==qu) return true; if(p!=EMPTY) break; }

    // King
    Piece ki = (by==WHITE)?WK:BK;
    for (int dr=-1;dr<=1;dr++)
        for (int df=-1;df<=1;df++) {
            if (!dr && !df) continue;
            int r=r0+dr, f=f0+df;
            if (r>=0&&r<8&&f>=0&&f<8&&sq[mkSq(r,f)]==ki) return true;
        }
    return false;
}

bool Board::inCheck() const {
    int ks = kingSquare(side);
    return ks != NO_SQ && isAttacked(ks, ~side);
}

// ── Make move (incremental Zobrist) ─────────────────────────────
bool Board::makeMove(Move m) {
    UndoInfo& u = history[ply];
    u.epSquare = epSquare;
    u.castling = castling;
    u.halfMove = halfMove;
    u.hash     = hash;

    int   from   = m.from(), to = m.to(), mt = m.mtype();
    Piece moving = sq[from],  target = sq[to];
    u.captured   = target;

    if (epSquare != NO_SQ) hash ^= ZEP[fileOf(epSquare)];
    hash ^= ZCASTLE[castling];
    hash ^= ZPIECE[moving][from];
    sq[to] = moving; sq[from] = EMPTY;
    if (target != EMPTY) hash ^= ZPIECE[target][to];

    // En-passant capture
    if (mt == 2) {
        int capSq = to + (side==WHITE ? -8 : 8);
        u.captured = sq[capSq];
        hash ^= ZPIECE[sq[capSq]][capSq];
        sq[capSq] = EMPTY;
        sq[to]    = moving;
    }

    // Castling: move the rook
    if (mt == 1) {
        if      (to==G1){hash^=ZPIECE[WR][H1];hash^=ZPIECE[WR][F1];sq[H1]=EMPTY;sq[F1]=WR;}
        else if (to==C1){hash^=ZPIECE[WR][A1];hash^=ZPIECE[WR][D1];sq[A1]=EMPTY;sq[D1]=WR;}
        else if (to==G8){hash^=ZPIECE[BR][H8];hash^=ZPIECE[BR][F8];sq[H8]=EMPTY;sq[F8]=BR;}
        else if (to==C8){hash^=ZPIECE[BR][A8];hash^=ZPIECE[BR][D8];sq[A8]=EMPTY;sq[D8]=BR;}
    }

    // Promotion: replace pawn
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

    // Update en-passant square
    epSquare = NO_SQ;
    if (typeOf(moving)==PAWN && std::abs(to-from)==16) {
        epSquare = (from+to)/2;
        hash ^= ZEP[fileOf(epSquare)];
    }

    // Update castling rights
    if (moving==WK) castling &= ~3;
    if (moving==BK) castling &= ~12;
    if (from==A1||to==A1) castling &= ~2;
    if (from==H1||to==H1) castling &= ~1;
    if (from==A8||to==A8) castling &= ~8;
    if (from==H8||to==H8) castling &= ~4;

    hash ^= ZCASTLE[castling];
    halfMove = (typeOf(moving)==PAWN || target!=EMPTY) ? 0 : halfMove+1;
    hash ^= ZSIDE;
    side = ~side;
    if (side == WHITE) fullMove++;
    ply++;

    // Validate: moving side's king must not be in check
    Color movedSide = ~side;
    if (isAttacked(kingSquare(movedSide), side)) {
        unmakeMove(m);
        return false;
    }
    return true;
}

// ── Unmake move ──────────────────────────────────────────────────
void Board::unmakeMove(Move m) {
    ply--;
    side = ~side;
    if (side == BLACK) fullMove--;

    UndoInfo& u = history[ply];
    epSquare = u.epSquare;
    castling = u.castling;
    halfMove = u.halfMove;
    hash     = u.hash;

    int   from = m.from(), to = m.to(), mt = m.mtype();
    Piece moved = sq[to];

    if (mt == 3) moved = (side==WHITE) ? WP : BP;
    sq[from] = moved;
    sq[to]   = u.captured;

    if (mt == 2) {
        int capSq = to + (side==WHITE ? -8 : 8);
        sq[capSq] = (side==WHITE) ? BP : WP;
        sq[to]    = EMPTY;
    }
    if (mt == 1) {
        if      (to==G1){sq[F1]=EMPTY;sq[H1]=WR;}
        else if (to==C1){sq[D1]=EMPTY;sq[A1]=WR;}
        else if (to==G8){sq[F8]=EMPTY;sq[H8]=BR;}
        else if (to==C8){sq[D8]=EMPTY;sq[A8]=BR;}
    }
}

} // namespace Ryzix
