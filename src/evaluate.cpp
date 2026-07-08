/*
 * Ryzix Chess Engine
 * evaluate.cpp — Classical hand-crafted evaluation (no NNUE).
 *
 * Evaluation features:
 *   - PeSTO piece-square tables (MG + EG, phase-tapered)
 *   - Mobility (pseudo-legal attacks counted per piece)
 *   - Pawn structure: doubled, isolated, passed pawns
 *   - Knight outpost bonuses
 *   - Bishop pair bonus
 *   - Rook on open / semi-open file + rook on 7th rank
 *   - King safety: pawn-shield score + weighted attacker sum
 *   - Tempo bonus (+10 cp for side to move)
 */
#include "evaluate.h"
#include <cmath>
#include <algorithm>

namespace Ryzix {

// ═══════════════════════════════════════════════════════════════════
//  PeSTO piece-square tables (from Rofchade / PeSTO family)
//  Values from White's perspective; flip rank for Black.
//  Indexed [square] = [A1..H8] order.
// ═══════════════════════════════════════════════════════════════════

// ── Midgame piece values (used as offset in PST) ─────────────────
static constexpr int MG_VAL[7] = {0,82,337,365,477,1025,0};
static constexpr int EG_VAL[7] = {0,94,281,297,512,936, 0};

// ── Pawn PST ─────────────────────────────────────────────────────
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

// ── Knight PST ───────────────────────────────────────────────────
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

// ── Bishop PST ───────────────────────────────────────────────────
static constexpr int MG_BISHOP[64] = {
    -29,  4,-82,-37,-25,-42,  7, -8,
    -26, 16,-18,-13, 30, 59, 18,-47,
    -16, 37, 43, 40, 35, 50, 37, -2,
     -4,  5, 19, 50, 37, 37,  7, -2,
     -6, 13, 13, 26, 34, 12, 10,  4,
      0, 15, 15, 15, 14, 27, 18, 10,
      4, 15, 16,  0,  7, 21, 33,  1,
    -33, -3,-14,-21,-13,-12,-39,-21,
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

// ── Rook PST ─────────────────────────────────────────────────────
static constexpr int MG_ROOK[64] = {
    32, 42, 32, 51,63, 9, 31, 43,
    27, 32, 58, 62,80,67, 26, 44,
    -5, 19, 26, 36,17,45, 61, 16,
   -24,-11,  7, 26,24,35, -8,-20,
   -36,-26,-12, -1, 9,-7,  6,-23,
   -45,-25,-16,-17, 3, 0, -5,-33,
   -44,-16,-20, -9,-1,11, -6,-71,
   -19,-13,  1, 17,16, 7,-37,-26,
};
static constexpr int EG_ROOK[64] = {
    13, 10, 18, 15, 12,-5,  2,  1,
    11, 13, 13, 11,-3, 3,  8,  3,
     7,  7,  7,  5,  4,-3, -5,  -3,
     4,  3, 13,  1,  2, 1,-1,  2,
     3,  5,  8,  4, -5, -6, -8, -11,
    -4,  0, -5, -1, -7,-12, -8,-16,
    -6, -6,  0,  2, -9, -9,-11, -3,
    -9,  2,  3, -1, -5,-13,  4,-20,
};

// ── Queen PST ────────────────────────────────────────────────────
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

// ── King PST ─────────────────────────────────────────────────────
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

// Flip square for Black (mirror vertically)
static inline int flip(int sq) { return sq ^ 56; }

// Per-piece table lookup; from == 0 for White (or 1 for Black)
static inline int mgPST(PieceType pt, int sq, Color c) {
    int s = (c==WHITE) ? sq : flip(sq);
    switch (pt) {
        case PAWN:   return MG_PAWN[s];
        case KNIGHT: return MG_KNIGHT[s];
        case BISHOP: return MG_BISHOP[s];
        case ROOK:   return MG_ROOK[s];
        case QUEEN:  return MG_QUEEN[s];
        case KING:   return MG_KING[s];
        default:     return 0;
    }
}
static inline int egPST(PieceType pt, int sq, Color c) {
    int s = (c==WHITE) ? sq : flip(sq);
    switch (pt) {
        case PAWN:   return EG_PAWN[s];
        case KNIGHT: return EG_KNIGHT[s];
        case BISHOP: return EG_BISHOP[s];
        case ROOK:   return EG_ROOK[s];
        case QUEEN:  return EG_QUEEN[s];
        case KING:   return EG_KING[s];
        default:     return 0;
    }
}

// ═══════════════════════════════════════════════════════════════════
//  Mobility bonus tables
// ═══════════════════════════════════════════════════════════════════
// Indexed by number of safe squares (0..27 for queen, etc.)
static constexpr int MOB_KNIGHT_MG[9]  = {-62,-53,-12,-4, 3,13,22,28,33};
static constexpr int MOB_KNIGHT_EG[9]  = {-81,-56,-31,-16, 5,11,17,20,25};
static constexpr int MOB_BISHOP_MG[14] = {-48,-20, 16, 26, 38, 51, 55,63,63,68,81,81,91,98};
static constexpr int MOB_BISHOP_EG[14] = {-59,-23,-3, 13, 24, 42, 54,57,65,73,78,86,88,97};
static constexpr int MOB_ROOK_MG[15]   = {-58,-27,-15,-10,-5, -2, 9,16,30,29,32,38,46,48,58};
static constexpr int MOB_ROOK_EG[15]   = {-76,-18, 28, 55, 69, 82,112,118,132,142,155,165,166,169,172};
static constexpr int MOB_QUEEN_MG[28]  = {3,-5,-5, 4, 8,12,16,22,26,30,35,38,42,44,46,48,53,57,60,64,68,73,74,80,84,89,95,101};
static constexpr int MOB_QUEEN_EG[28]  = {-69,-57,-47,-26,-17,-6, 31,22,41,54,58,68,73,80,86,91,96,99,101,103,113,114,116,117,118,119,122,124};

// ═══════════════════════════════════════════════════════════════════
//  King safety attacker weights
// ═══════════════════════════════════════════════════════════════════
static constexpr int KING_ATK_WEIGHT[7] = {0, 0, 2, 2, 3, 5, 0}; // by piece type

// ═══════════════════════════════════════════════════════════════════
//  evaluate()
// ═══════════════════════════════════════════════════════════════════
int evaluate(const Board& b) {
    int mgScore[2] = {0, 0};   // [WHITE, BLACK]
    int egScore[2] = {0, 0};
    int phase = 0;

    // ── Per-square pass: material + PST + mobility ────────────────
    int pawnFile[2][8] = {};   // pawn count per file [color][file]
    bool hasBishop[2]  = {};   // does each side have a bishop?
    int  bishopCount[2]= {};

    // King safety
    int  ksq[2];
    ksq[WHITE] = b.kingSquare(WHITE);
    ksq[BLACK] = b.kingSquare(BLACK);
    int  kingAttackers[2]     = {0, 0};   // attacker weight sum
    int  kingAttackerCount[2] = {0, 0};

    for (int s = 0; s < 64; s++) {
        Piece p = b.sq[s];
        if (p == EMPTY) continue;

        Color    c  = colorOf(p);
        PieceType t = typeOf(p);
        Color     opp = ~c;

        // Phase contribution
        static constexpr int PHASE_VAL[7] = {0,0,1,1,2,4,0};
        phase += PHASE_VAL[t];

        // Material + PST
        mgScore[c] += MG_VAL[t] + mgPST(t, s, c);
        egScore[c] += EG_VAL[t] + egPST(t, s, c);

        // Pawn file tracking
        if (t == PAWN) pawnFile[c][fileOf(s)]++;

        // Bishop tracking
        if (t == BISHOP) { hasBishop[c] = true; bishopCount[c]++; }

        // ── Mobility (count pseudo-legal quiet squares) ────────────
        int mob = 0;
        switch (t) {
        case KNIGHT: {
            static constexpr int KD[8] = {-17,-15,-10,-6,6,10,15,17};
            for (int d : KD) {
                int t2 = s + d;
                if (!onBoard(t2)) continue;
                if (std::abs(fileOf(t2)-fileOf(s)) > 2) continue;
                if (b.sq[t2]==EMPTY || colorOf(b.sq[t2])==opp) mob++;
                // King attack
                if (b.sq[t2]==EMPTY && b.isAttacked(t2, c)) {
                    // near enemy king?
                    if (ksq[opp]!=NO_SQ && std::abs(rankOf(t2)-rankOf(ksq[opp]))<=2 &&
                        std::abs(fileOf(t2)-fileOf(ksq[opp]))<=2) {
                        kingAttackers[opp] += KING_ATK_WEIGHT[KNIGHT];
                        kingAttackerCount[opp]++;
                    }
                }
            }
            mob = std::min(mob, 8);
            mgScore[c] += MOB_KNIGHT_MG[mob];
            egScore[c] += MOB_KNIGHT_EG[mob];
            break;
        }
        case BISHOP: {
            static constexpr int DD[4] = {-9,-7,7,9};
            for (int d : DD) {
                int cur=s;
                while (true) {
                    int t2=cur+d;
                    if (!onBoard(t2)) break;
                    if (std::abs(fileOf(t2)-fileOf(cur))!=1) break;
                    if (b.sq[t2]==EMPTY) { mob++; cur=t2; }
                    else { if (colorOf(b.sq[t2])==opp) mob++; break; }
                }
            }
            mob = std::min(mob, 13);
            mgScore[c] += MOB_BISHOP_MG[mob];
            egScore[c] += MOB_BISHOP_EG[mob];
            break;
        }
        case ROOK: {
            static constexpr int SD[4] = {-8,-1,1,8};
            int r0=rankOf(s), f0=fileOf(s);
            // Open / semi-open file bonus
            bool openForUs  = (pawnFile[c][f0]   == 0);
            bool openForOpp = (pawnFile[opp][f0]  == 0);
            if (openForUs && openForOpp) { mgScore[c] += 25; egScore[c] += 16; }
            else if (openForUs)          { mgScore[c] += 14; egScore[c] +=  7; }
            // Rook on 7th rank
            int rank7 = (c==WHITE) ? 6 : 1;
            if (rankOf(s) == rank7) { mgScore[c] += 11; egScore[c] += 16; }

            for (int d : SD) {
                int cur=s;
                while (true) {
                    int t2=cur+d;
                    if (!onBoard(t2)) break;
                    if (d==1&&fileOf(t2)==0) break;
                    if (d==-1&&fileOf(t2)==7) break;
                    if (b.sq[t2]==EMPTY) { mob++; cur=t2; }
                    else { if (colorOf(b.sq[t2])==opp) mob++; break; }
                }
            }
            (void)r0; (void)f0;
            mob = std::min(mob, 14);
            mgScore[c] += MOB_ROOK_MG[mob];
            egScore[c] += MOB_ROOK_EG[mob];
            break;
        }
        case QUEEN: {
            static constexpr int QD[8] = {-9,-8,-7,-1,1,7,8,9};
            for (int d : QD) {
                int cur=s;
                while (true) {
                    int t2=cur+d;
                    if (!onBoard(t2)) break;
                    if ((d==9||d==-9||d==7||d==-7) &&
                        std::abs(fileOf(t2)-fileOf(cur))!=1) break;
                    if (d==1&&fileOf(t2)==0) break;
                    if (d==-1&&fileOf(t2)==7) break;
                    if (b.sq[t2]==EMPTY) { mob++; cur=t2; }
                    else { if (colorOf(b.sq[t2])==opp) mob++; break; }
                }
            }
            mob = std::min(mob, 27);
            mgScore[c] += MOB_QUEEN_MG[mob];
            egScore[c] += MOB_QUEEN_EG[mob];
            break;
        }
        default: break;
        }
    }

    // ── Bishop pair bonus ─────────────────────────────────────────
    for (int c = 0; c < 2; c++) {
        if (bishopCount[c] >= 2) {
            mgScore[c] += 57;
            egScore[c] += 58;
        }
    }

    // ── Pawn structure ────────────────────────────────────────────
    for (int c = 0; c < 2; c++) {
        Color opp = Color(c ^ 1);
        for (int f = 0; f < 8; f++) {
            if (pawnFile[c][f] == 0) continue;
            // Doubled pawns
            if (pawnFile[c][f] >= 2) {
                mgScore[c] -= 11 * (pawnFile[c][f]-1);
                egScore[c] -= 56 * (pawnFile[c][f]-1);
            }
            // Isolated pawns
            bool leftEmpty  = (f==0 || pawnFile[c][f-1]==0);
            bool rightEmpty = (f==7 || pawnFile[c][f+1]==0);
            if (leftEmpty && rightEmpty) {
                mgScore[c] -= 5;
                egScore[c] -= 15;
            }
        }
        // Passed pawns (no opposing pawn on same or adjacent file ahead)
        for (int s = 0; s < 64; s++) {
            Piece p = b.sq[s];
            if (p==EMPTY || colorOf(p)!=(Color)c || typeOf(p)!=PAWN) continue;
            bool passed = true;
            int  pushDir = (c==WHITE) ? 1 : -1;
            int  startR  = rankOf(s);
            for (int r = startR + pushDir; r>=0 && r<8; r += pushDir) {
                for (int df = -1; df <= 1; df++) {
                    int ff = fileOf(s)+df;
                    if (ff<0||ff>7) continue;
                    if (b.sq[mkSq(r,ff)] == (opp==WHITE?WP:BP)) { passed=false; break; }
                }
                if (!passed) break;
            }
            if (passed) {
                // Bonus scales with advancement
                int advance = (c==WHITE) ? rankOf(s) : (7-rankOf(s));
                int mg_bonus = advance * advance * 3;
                int eg_bonus = advance * advance * 7 + advance * 10;
                mgScore[c] += mg_bonus;
                egScore[c] += eg_bonus;
            }
        }
    }

    // ── King safety ───────────────────────────────────────────────
    for (int c = 0; c < 2; c++) {
        if (ksq[c] == NO_SQ) continue;
        Color opp = Color(c ^ 1);
        // Pawn shield
        int shieldScore = 0;
        int kfile = fileOf(ksq[c]), krank = rankOf(ksq[c]);
        int shieldRankDir = (c==WHITE) ? 1 : -1;
        for (int df = -1; df <= 1; df++) {
            int f = kfile+df; if (f<0||f>7) continue;
            int r1 = krank + shieldRankDir;
            int r2 = krank + 2*shieldRankDir;
            Piece shieldPawn = (c==WHITE) ? WP : BP;
            if (r1>=0&&r1<8 && b.sq[mkSq(r1,f)] == shieldPawn) shieldScore += 18;
            else if (r2>=0&&r2<8 && b.sq[mkSq(r2,f)] == shieldPawn) shieldScore +=  9;
            else shieldScore -= 20;
        }
        mgScore[c] += shieldScore;

        // Attacker pressure
        int atk = kingAttackers[c];
        int cnt = kingAttackerCount[c];
        if (cnt >= 2) {
            int danger = atk * 50 / (cnt + 1);
            mgScore[c] -= danger;
        }
    }

    // ── Tempo bonus ───────────────────────────────────────────────
    mgScore[b.side] += 10;

    // ── Phase tapering ────────────────────────────────────────────
    // Max phase = 4*1 (knights) + 4*1 (bishops) + 4*2 (rooks) + 2*4 (queens) = 24
    constexpr int MAX_PHASE = 24;
    phase = std::min(phase, MAX_PHASE);

    int mgDiff = mgScore[WHITE] - mgScore[BLACK];
    int egDiff = egScore[WHITE] - egScore[BLACK];
    int score  = (mgDiff * phase + egDiff * (MAX_PHASE - phase)) / MAX_PHASE;

    return (b.side == WHITE) ? score : -score;
}

} // namespace Ryzix
