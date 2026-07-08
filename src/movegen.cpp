/*
 * Ryzix Chess Engine
 * movegen.cpp — Move generation.
 *
 * Generates all pseudo-legal moves for the side to move.
 * Legality (king not left in check) is filtered by makeMove.
 */
#include "movegen.h"
#include <cstdlib>

namespace Ryzix {

// ── Sliding piece generation (bishop / rook directions) ──────────
static void genSliders(const Board& b, MoveList& ml, int s,
                       const int* dirs, int ndirs) {
    Color us = b.side;
    for (int i = 0; i < ndirs; i++) {
        int d = dirs[i], cur = s;
        while (true) {
            int t = cur + d;
            if (!onBoard(t)) break;
            // Guard against file-wrap on diagonal directions
            if ((d==9||d==-9||d==7||d==-7) &&
                std::abs(fileOf(t)-fileOf(cur)) != 1) break;
            // Guard against rank-wrap on horizontal directions
            if (d== 1 && fileOf(t)==0) break;
            if (d==-1 && fileOf(t)==7) break;

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

// ── Pseudo-legal move generation ─────────────────────────────────
void generatePseudoMoves(const Board& b, MoveList& ml) {
    Color us=b.side, them=~us;

    static constexpr int DIAG[4]     = {-9,-7, 7, 9};
    static constexpr int STRAIGHT[4] = {-8,-1, 1, 8};
    static constexpr int QUEEN_D[8]  = {-9,-8,-7,-1, 1, 7, 8, 9};
    static constexpr int KNIGHT_D[8] = {-17,-15,-10,-6, 6,10,15,17};

    int pushDir   = (us==WHITE) ?  8 : -8;
    int startRank = (us==WHITE) ?  1 :  6;
    int promRank  = (us==WHITE) ?  7 :  0;

    for (int s = 0; s < 64; s++) {
        Piece p = b.sq[s];
        if (p == EMPTY || colorOf(p) != us) continue;

        switch (typeOf(p)) {

        case PAWN: {
            int r=rankOf(s), f=fileOf(s);
            int fwd = s + pushDir;
            // Single push
            if (onBoard(fwd) && b.sq[fwd]==EMPTY) {
                if (rankOf(fwd) == promRank) {
                    for (int pr=0; pr<4; pr++) ml.push(s,fwd,3,pr);
                } else {
                    ml.push(s, fwd);
                    // Double push from start rank
                    if (r == startRank) {
                        int fwd2 = fwd + pushDir;
                        if (b.sq[fwd2] == EMPTY) ml.push(s, fwd2);
                    }
                }
            }
            // Captures and en-passant
            for (int df : {-1, 1}) {
                if (f+df < 0 || f+df > 7) continue;
                int t = fwd + df;
                if (!onBoard(t)) continue;
                bool isCap = (b.sq[t]!=EMPTY && colorOf(b.sq[t])==them);
                bool isEP  = (t == b.epSquare);
                if (isCap || isEP) {
                    if (rankOf(t) == promRank) {
                        for (int pr=0; pr<4; pr++) ml.push(s,t,3,pr);
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
                if (std::abs(fileOf(t)-fileOf(s)) > 2) continue;
                if (std::abs(rankOf(t)-rankOf(s)) > 2) continue;
                if (b.sq[t]==EMPTY || colorOf(b.sq[t])!=us)
                    ml.push(s, t);
            }
            break;

        case BISHOP: genSliders(b, ml, s, DIAG,     4); break;
        case ROOK:   genSliders(b, ml, s, STRAIGHT,  4); break;
        case QUEEN:  genSliders(b, ml, s, QUEEN_D,   8); break;

        case KING: {
            int r0=rankOf(s), f0=fileOf(s);
            for (int dr=-1; dr<=1; dr++)
                for (int df=-1; df<=1; df++) {
                    if (!dr && !df) continue;
                    int r2=r0+dr, f2=f0+df;
                    if (r2<0||r2>7||f2<0||f2>7) continue;
                    int t = mkSq(r2, f2);
                    if (b.sq[t]==EMPTY || colorOf(b.sq[t])!=us)
                        ml.push(s, t);
                }
            // Castling
            if (us==WHITE && s==E1) {
                if ((b.castling&1) && b.sq[F1]==EMPTY && b.sq[G1]==EMPTY &&
                    !b.isAttacked(E1,BLACK) && !b.isAttacked(F1,BLACK) && !b.isAttacked(G1,BLACK))
                    ml.push(E1, G1, 1);
                if ((b.castling&2) && b.sq[D1]==EMPTY && b.sq[C1]==EMPTY && b.sq[B1]==EMPTY &&
                    !b.isAttacked(E1,BLACK) && !b.isAttacked(D1,BLACK) && !b.isAttacked(C1,BLACK))
                    ml.push(E1, C1, 1);
            } else if (us==BLACK && s==E8) {
                if ((b.castling&4) && b.sq[F8]==EMPTY && b.sq[G8]==EMPTY &&
                    !b.isAttacked(E8,WHITE) && !b.isAttacked(F8,WHITE) && !b.isAttacked(G8,WHITE))
                    ml.push(E8, G8, 1);
                if ((b.castling&8) && b.sq[D8]==EMPTY && b.sq[C8]==EMPTY && b.sq[B8]==EMPTY &&
                    !b.isAttacked(E8,WHITE) && !b.isAttacked(D8,WHITE) && !b.isAttacked(C8,WHITE))
                    ml.push(E8, C8, 1);
            }
            break;
        }

        default: break;
        }
    }
}

// ── Legal move generation ────────────────────────────────────────
std::vector<Move> legalMoves(Board& b) {
    MoveList ml;
    generatePseudoMoves(b, ml);
    std::vector<Move> legal;
    legal.reserve(ml.count);
    for (int i = 0; i < ml.count; i++)
        if (b.makeMove(ml.moves[i])) {
            legal.push_back(ml.moves[i]);
            b.unmakeMove(ml.moves[i]);
        }
    return legal;
}

} // namespace Ryzix
