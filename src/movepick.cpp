/*
 * Ryzix Chess Engine
 * movepick.cpp — Move ordering and Static Exchange Evaluation.
 *
 * SEE: approximates capture exchange sequences on a single square
 * to determine whether a capture is likely to gain or lose material.
 *
 * Move ordering priority (descending):
 *   TT move                 (1 000 000)
 *   Queen promotion         (  900 000)
 *   Good captures (SEE≥0)   (  600 000 + MVV-LVA)
 *   Killers                 (  500 000 / 490 000)
 *   Counter-move            (  480 000)
 *   Other promotions        (  400 000)
 *   Quiet history           (history score)
 *   Bad captures (SEE<0)    ( -100 000 + SEE)
 */
#include "movepick.h"
#include "search.h"
#include <cstdlib>
#include <algorithm>

namespace Ryzix {

// ── Static Exchange Evaluation ───────────────────────────────────
// Returns approximate material gain/loss for a capture on square 'to'.
// Based on the classic SEE with a simulated board copy.
int see(const Board& b, int to, Piece /*target*/, int from, Piece moving) {
    // We'll simulate the capture exchange using a scratch board
    Board tmp = b;   // copy (~15 KB; acceptable at search root)

    int gain[32]; int d = 0;
    Piece captured = tmp.sq[to];
    gain[d] = SEE_VAL[typeOf(captured)];
    // Make the initial capture manually
    tmp.sq[to]   = moving;
    tmp.sq[from] = EMPTY;

    Color stm = ~colorOf(moving);  // side to recapture
    while (true) {
        d++;
        gain[d] = SEE_VAL[typeOf(moving)] - gain[d-1];
        // Find least-valuable attacker for 'stm'
        Piece atk = EMPTY;
        int   atkSq = NO_SQ;
        int   bestVal = 20001;
        Piece stkPawn = (stm==WHITE) ? WP : BP;

        // Pawns
        if (stm==WHITE && rankOf(to)>0) {
            if (fileOf(to)>0 && tmp.sq[to-9]==WP && SEE_VAL[PAWN]<bestVal) { atk=WP;atkSq=to-9;bestVal=SEE_VAL[PAWN]; }
            if (fileOf(to)<7 && tmp.sq[to-7]==WP && SEE_VAL[PAWN]<bestVal) { atk=WP;atkSq=to-7;bestVal=SEE_VAL[PAWN]; }
        } else if (stm==BLACK && rankOf(to)<7) {
            if (fileOf(to)>0 && tmp.sq[to+7]==BP && SEE_VAL[PAWN]<bestVal) { atk=BP;atkSq=to+7;bestVal=SEE_VAL[PAWN]; }
            if (fileOf(to)<7 && tmp.sq[to+9]==BP && SEE_VAL[PAWN]<bestVal) { atk=BP;atkSq=to+9;bestVal=SEE_VAL[PAWN]; }
        }
        (void)stkPawn;

        // Knights
        Piece kn = (stm==WHITE)?WN:BN;
        static constexpr int KD[8]={-17,-15,-10,-6,6,10,15,17};
        for (int dd : KD) {
            int t=to+dd;
            if (!onBoard(t)) continue;
            if (std::abs(fileOf(t)-fileOf(to))>2) continue;
            if (tmp.sq[t]==kn && SEE_VAL[KNIGHT]<bestVal) { atk=kn;atkSq=t;bestVal=SEE_VAL[KNIGHT]; break; }
        }

        // Sliders: bishops/queens on diagonals
        Piece bi=(stm==WHITE)?WB:BB, qu=(stm==WHITE)?WQ:BQ;
        static constexpr int DD[4]={-9,-7,7,9};
        for (int dd : DD) {
            int cur=to;
            while (true) {
                int t=cur+dd;
                if (!onBoard(t)) break;
                if (std::abs(fileOf(t)-fileOf(cur))!=1) break;
                if (tmp.sq[t]!=EMPTY) {
                    if ((tmp.sq[t]==bi||tmp.sq[t]==qu) && SEE_VAL[typeOf(tmp.sq[t])]<bestVal) {
                        atk=tmp.sq[t]; atkSq=t; bestVal=SEE_VAL[typeOf(tmp.sq[t])];
                    }
                    break;
                }
                cur=t;
            }
        }

        // Sliders: rooks/queens on straights
        Piece ro=(stm==WHITE)?WR:BR;
        static constexpr int SS[4]={-8,-1,1,8};
        for (int dd : SS) {
            int cur=to;
            while (true) {
                int t=cur+dd;
                if (!onBoard(t)) break;
                if (dd==1&&fileOf(t)==0) break;
                if (dd==-1&&fileOf(t)==7) break;
                if (tmp.sq[t]!=EMPTY) {
                    if ((tmp.sq[t]==ro||tmp.sq[t]==qu) && SEE_VAL[typeOf(tmp.sq[t])]<bestVal) {
                        atk=tmp.sq[t]; atkSq=t; bestVal=SEE_VAL[typeOf(tmp.sq[t])];
                    }
                    break;
                }
                cur=t;
            }
        }

        // King
        Piece ki=(stm==WHITE)?WK:BK;
        int r0=rankOf(to), f0=fileOf(to);
        for (int dr=-1;dr<=1;dr++)
            for (int df=-1;df<=1;df++) {
                if (!dr&&!df) continue;
                int r2=r0+dr, f2=f0+df;
                if (r2<0||r2>7||f2<0||f2>7) continue;
                if (tmp.sq[mkSq(r2,f2)]==ki && SEE_VAL[KING]<bestVal) {
                    atk=ki; atkSq=mkSq(r2,f2); bestVal=SEE_VAL[KING];
                }
            }

        if (atk == EMPTY) break;

        // Make the recapture
        moving = atk;
        tmp.sq[to]    = moving;
        tmp.sq[atkSq] = EMPTY;
        stm = ~stm;
    }

    // Minimax up the gain array
    while (--d > 0)
        gain[d-1] = -std::max(-gain[d-1], gain[d]);
    return gain[0];
}

// ── Move scoring ─────────────────────────────────────────────────
int moveScore(const Board& b, Move m, Move ttMove,
              const SearchState& ss, Move prevMove) {
    if (m == ttMove) return 1'000'000;

    int from  = m.from(), to = m.to(), mt = m.mtype();
    Piece  mp = b.sq[from];
    Piece  cp = b.sq[to];

    // Promotion
    if (mt == 3) {
        return (m.promo() == 3) ? 900'000 : 400'000 + m.promo() * 1000;
    }

    // En-passant
    if (mt == 2) return 600'000 + SEE_VAL[PAWN];

    // Captures: SEE + MVV-LVA
    if (cp != EMPTY) {
        int seeVal = see(b, to, cp, from, mp);
        if (seeVal >= 0) {
            int mvvlva = SEE_VAL[typeOf(cp)] - SEE_VAL[typeOf(mp)] / 10;
            return 600'000 + mvvlva + seeVal;
        } else {
            // Bad capture — still ranked above nothingness but below quiets
            return -100'000 + seeVal;
        }
    }

    // Killer moves
    if (m == ss.killers[ss.ply][0]) return 500'000;
    if (m == ss.killers[ss.ply][1]) return 490'000;

    // Counter-move
    if (!prevMove.isNull() && ss.counterMove[b.sq[prevMove.to()]][prevMove.to()] == (int)m.data)
        return 480'000;

    // Quiet history
    return ss.history[mp][to];
}

// ── Sort moves (insertion sort — fast for small N) ───────────────
void sortMoves(MoveList& ml, const Board& b, Move ttMove,
               const SearchState& ss, Move prevMove) {
    // Score all moves
    static int scores[256];
    for (int i = 0; i < ml.count; i++)
        scores[i] = moveScore(b, ml.moves[i], ttMove, ss, prevMove);

    // Insertion sort by descending score
    for (int i = 1; i < ml.count; i++) {
        int s = scores[i];
        Move m = ml.moves[i];
        int j = i - 1;
        while (j >= 0 && scores[j] < s) {
            scores[j+1]   = scores[j];
            ml.moves[j+1] = ml.moves[j];
            j--;
        }
        scores[j+1]   = s;
        ml.moves[j+1] = m;
    }
}

} // namespace Ryzix
