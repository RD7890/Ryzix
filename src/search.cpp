/*
 * Ryzix Chess Engine
 * search.cpp — Iterative-deepening PVS with full pruning suite.
 *
 * Search hierarchy:
 *   rootSearch()  → iterative deepening + aspiration windows
 *   alphaBeta()   → principal variation search (fail-soft)
 *   quiesce()     → quiescence search (captures + checks)
 *
 * Pruning / reduction techniques:
 *   Aspiration windows     (rootSearch)
 *   Reverse futility       (RFP / static null-move)
 *   Null-move pruning      (R = 3 + depth/3 + min(3,(eval-beta)/200))
 *   ProbCut
 *   Late-Move Reductions   (LMR, log formula precomputed in LMR_TABLE)
 *   Late-Move Pruning      (LMP, depth-dependent count)
 *   Futility pruning       (depth ≤ 8)
 *   SEE pruning            (bad quiets + bad captures)
 *   Internal Iterative Red (IIR, no TT move → depth--)
 *   Singular extensions    (depth ≥ 8)
 *   Check extension        (+1 ply)
 *   Killers / counter-move / history heuristics (movepick.h)
 */
#include "search.h"
#include "movegen.h"
#include "evaluate.h"
#include "movepick.h"
#include "timeman.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace Ryzix {
// Forward declaration for ttScore helper used inside alphaBeta
static inline int ttScore_(int rawScore, int ply);
} // namespace Ryzix

namespace Ryzix {

// ── LMR table ────────────────────────────────────────────────────
int LMR_TABLE[MAX_PLY][64];

void initLMR() {
    for (int d = 1; d < MAX_PLY; d++)
        for (int m = 1; m < 64; m++)
            LMR_TABLE[d][m] = static_cast<int>(
                0.75 + std::log(d) * std::log(m) / 2.25);
}

// ── SearchState ──────────────────────────────────────────────────
void SearchState::reset() {
    for (int i = 0; i < MAX_PLY; i++) {
        killers[i][0] = killers[i][1] = NULL_MOVE;
        staticEval[i] = 0;
    }
    for (int p = 0; p < 13; p++) {
        for (int s = 0; s < 64; s++) {
            history[p][s] = 0;
            counterMove[p][s] = 0;
            for (int ct = 0; ct < 7; ct++)
                captureHistory[p][s][ct] = 0;
        }
    }
    nodes = 0; ply = 0; stop = false;
}

bool SearchState::timeUp() {
    if (timeLimitMs <= 0) return false;
    auto now = std::chrono::steady_clock::now();
    int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count());
    return ms >= timeLimitMs;
}

bool SearchState::innerTimeUp() {
    if (maxTimeLimitMs <= 0) return false;
    if ((nodes & 2047) != 0) return false;   // only check every 2048 nodes
    auto now = std::chrono::steady_clock::now();
    int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count());
    return ms >= maxTimeLimitMs;
}

// ── Quiescence search ────────────────────────────────────────────
static int quiesce(Board& b, SearchState& ss, int alpha, int beta) {
    ss.nodes++;

    // Stand-pat
    int standPat = evaluate(b);
    if (standPat >= beta) return standPat;
    if (standPat > alpha) alpha = standPat;

    // Delta pruning (endgame queen threshold)
    static constexpr int DELTA = 975;
    if (standPat < alpha - DELTA) return alpha;

    // Generate captures only
    MoveList ml;
    generatePseudoMoves(b, ml);

    // Score and sort captures by SEE+MVV-LVA
    static int scores[256];
    for (int i = 0; i < ml.count; i++) {
        Move m = ml.moves[i];
        int from = m.from(), to = m.to();
        Piece cp = b.sq[to];
        if (cp == EMPTY && m.mtype() != 2 && m.mtype() != 3) {
            scores[i] = -2'000'000; // skip quiet moves
            continue;
        }
        Piece mp = b.sq[from];
        int seeVal = see(b, to, cp, from, mp);
        // MVV-LVA component
        int victim = (cp != EMPTY) ? SEE_VAL[typeOf(cp)] : SEE_VAL[PAWN];
        scores[i] = seeVal + victim;
    }
    // Sort
    for (int i = 1; i < ml.count; i++) {
        int s = scores[i]; Move m = ml.moves[i]; int j = i-1;
        while (j >= 0 && scores[j] < s) { scores[j+1]=scores[j]; ml.moves[j+1]=ml.moves[j]; j--; }
        scores[j+1]=s; ml.moves[j+1]=m;
    }

    for (int i = 0; i < ml.count; i++) {
        if (scores[i] < -300) break;   // stop at clearly bad captures
        Move m = ml.moves[i];
        // Skip quiet moves
        if (b.sq[m.to()] == EMPTY && m.mtype() != 2 && m.mtype() != 3) continue;

        ss.ply++;
        if (!b.makeMove(m)) { ss.ply--; continue; }
        int score = -quiesce(b, ss, -beta, -alpha);
        b.unmakeMove(m);
        ss.ply--;

        if (ss.stop) return 0;
        if (score >= beta)  return score;
        if (score > alpha)  alpha = score;
    }
    return alpha;
}

// ── Principal Variation Search ───────────────────────────────────
static int alphaBeta(Board& b, SearchState& ss,
                     int alpha, int beta, int depth,
                     bool isPV, Move excludeMove = NULL_MOVE) {
    if (ss.stop) return 0;

    // Quiescence at leaf
    if (depth <= 0) return quiesce(b, ss, alpha, beta);

    ss.nodes++;

    // Draw detection
    if (ss.ply > 0) {
        if (b.halfMove >= 100) return 0;
        if (b.isRepetition(ss.ply)) return 0;
    }

    // Mate-distance pruning
    int mateAlpha = -MATE_SCORE + ss.ply;
    int mateBeta  =  MATE_SCORE - ss.ply;
    if (mateAlpha >= beta)  return mateAlpha;
    if (mateBeta  <= alpha) return mateBeta;

    bool inCheck = b.inCheck();
    if (inCheck) depth++;  // check extension

    // TT probe
    TTEntry* tte    = ttProbe(b.hash);
    bool     ttHit  = (tte->key == b.hash);
    Move     ttMove = ttHit ? Move::fromRaw(tte->move) : NULL_MOVE;
    int      ttSc   = ttHit ? ttScore_(tte->score, ss.ply) : 0;

    if (!isPV && ttHit && tte->depth >= depth && excludeMove.isNull()) {
        if (tte->bound == BOUND_EXACT) return ttSc;
        if (tte->bound == BOUND_LOWER && ttSc >= beta)  return ttSc;
        if (tte->bound == BOUND_UPPER && ttSc <= alpha) return ttSc;
    }

    // Static evaluation
    int staticEval;
    if (inCheck) {
        staticEval = -INF;
    } else if (ttHit && tte->bound != BOUND_NONE) {
        staticEval = ttSc;
    } else {
        staticEval = evaluate(b);
    }
    ss.staticEval[ss.ply] = staticEval;

    bool improving = (ss.ply >= 2 && !inCheck &&
                      staticEval > ss.staticEval[ss.ply-2]);

    // ── Pruning (non-PV, non-check) ───────────────────────────────
    if (!isPV && !inCheck && excludeMove.isNull()) {

        // Reverse Futility Pruning (static null-move)
        if (depth <= 8 && staticEval - 70 * depth + 40 * improving >= beta
            && std::abs(beta) < MATE_BOUND)
            return staticEval;

        // Null-move pruning
        if (depth >= 2 && staticEval >= beta
            && b.halfMove < 100
            && std::abs(beta) < MATE_BOUND) {
            // Verify at least one non-pawn piece
            bool hasNonPawn = false;
            Piece kp = (b.side==WHITE) ? WP : BP;
            Piece kk = (b.side==WHITE) ? WK : BK;
            for (int s=0;s<64;s++) {
                if (b.sq[s]!=EMPTY && colorOf(b.sq[s])==b.side
                    && b.sq[s]!=kp && b.sq[s]!=kk) { hasNonPawn=true; break; }
            }
            if (hasNonPawn) {
                int R = 3 + depth/3 + std::min(3, (staticEval-beta)/200);
                // Make null move
                b.side = ~b.side; b.hash ^= ZSIDE;
                int epSave = b.epSquare;
                if (b.epSquare != NO_SQ) { b.hash ^= ZEP[fileOf(b.epSquare)]; b.epSquare = NO_SQ; }
                b.ply++;
                ss.ply++;

                int nullScore = -alphaBeta(b, ss, -beta, -beta+1, depth-R, false);

                ss.ply--;
                b.ply--;
                b.side = ~b.side; b.hash ^= ZSIDE;
                if (b.epSquare != epSave) {
                    b.epSquare = epSave;
                    if (epSave != NO_SQ) b.hash ^= ZEP[fileOf(epSave)];
                }

                if (ss.stop) return 0;
                if (nullScore >= beta) {
                    if (nullScore >= MATE_BOUND) nullScore = beta;
                    return nullScore;
                }
            }
        }

        // ProbCut
        if (depth >= 5 && std::abs(beta) < MATE_BOUND) {
            int pcBeta = beta + 200;
            MoveList pcml;
            generatePseudoMoves(b, pcml);
            for (int i = 0; i < pcml.count; i++) {
                Move m = pcml.moves[i];
                Piece cp = b.sq[m.to()];
                if (cp == EMPTY && m.mtype() != 2) continue;
                Piece mpp = b.sq[m.from()];
                if (see(b, m.to(), cp, m.from(), mpp) < pcBeta - staticEval) continue;
                ss.ply++;
                if (!b.makeMove(m)) { ss.ply--; continue; }
                int score = -quiesce(b, ss, -pcBeta, -pcBeta+1);
                if (score >= pcBeta)
                    score = -alphaBeta(b, ss, -pcBeta, -pcBeta+1, depth-4, false);
                b.unmakeMove(m);
                ss.ply--;
                if (ss.stop) return 0;
                if (score >= pcBeta) {
                    ttStore(b.hash, m, score, depth-3, BOUND_LOWER, ss.ply);
                    return score;
                }
            }
        }
    }

    // Internal Iterative Reduction (no TT move → reduce depth)
    if (depth >= 4 && ttMove.isNull() && isPV) depth--;
    if (depth >= 6 && ttMove.isNull() && !isPV) depth--;

    // ── Move loop ─────────────────────────────────────────────────
    MoveList ml;
    generatePseudoMoves(b, ml);

    // Determine previous move for counter-move lookup
    sortMoves(ml, b, ttMove, ss, NULL_MOVE);

    int  bestScore = -INF;
    Move bestMove  = NULL_MOVE;
    int  legalCount= 0;
    bool raisedAlpha = false;

    for (int i = 0; i < ml.count; i++) {
        Move m = ml.moves[i];
        if (m == excludeMove) continue;

        Piece  mp       = b.sq[m.from()];
        Piece  cp       = b.sq[m.to()];
        bool   isCapture= (cp != EMPTY || m.mtype()==2);
        bool   isPromo  = (m.mtype() == 3);
        bool   isQuiet  = (!isCapture && !isPromo);

        // ── Pre-move pruning ──────────────────────────────────────
        if (!isPV && legalCount > 0 && !inCheck && bestScore > -MATE_BOUND) {

            // Late-Move Pruning
            int lmpCount = (3 + depth * depth) * (improving ? 2 : 1);
            if (isQuiet && legalCount >= lmpCount && depth <= 8) continue;

            // Futility pruning
            if (depth <= 7 && isQuiet &&
                staticEval + 80 * depth + 120 <= alpha) continue;

            // SEE pruning
            if (depth <= 6 && isQuiet &&
                see(b, m.to(), cp, m.from(), mp) < -60 * depth) continue;

            if (!isQuiet && depth <= 4 &&
                see(b, m.to(), cp, m.from(), mp) < -SEE_VAL[PAWN] * depth) continue;
        }

        // ── Singular extension ────────────────────────────────────
        int extension = 0;
        if (!inCheck && depth >= 8 && excludeMove.isNull() &&
            m == ttMove && ttHit &&
            tte->depth >= depth - 3 &&
            tte->bound != BOUND_UPPER &&
            std::abs(ttSc) < MATE_BOUND) {
            int singBeta  = ttSc - depth * 2;
            int singDepth = (depth - 1) / 2;
            ss.ply++;
            int singScore = alphaBeta(b, ss, singBeta-1, singBeta, singDepth, false, m);
            ss.ply--;
            if (singScore < singBeta) extension = 1;
            else if (singBeta >= beta) return singBeta;  // multi-cut
        }

        ss.ply++;
        if (!b.makeMove(m)) { ss.ply--; continue; }
        legalCount++;

        int score;
        int newDepth = depth - 1 + extension;

        // ── Late-Move Reductions (LMR) ────────────────────────────
        if (depth >= 2 && legalCount > (isPV ? 5 : 3) && isQuiet) {
            int R = LMR_TABLE[std::min(depth, MAX_PLY-1)][std::min(legalCount, 63)];
            // Adjust R
            R -= isPV;
            R -= b.inCheck();
            R -= (m == ss.killers[ss.ply-1][0] || m == ss.killers[ss.ply-1][1]);
            R += !improving;
            R = std::max(1, std::min(R, newDepth - 1));

            // Search with reduction (null window)
            score = -alphaBeta(b, ss, -alpha-1, -alpha, newDepth-R, false);

            // Re-search at full depth if it beat alpha
            if (score > alpha && R > 1)
                score = -alphaBeta(b, ss, -alpha-1, -alpha, newDepth, false);
        } else if (!isPV || legalCount > 1) {
            // Null-window search for non-PV moves
            score = -alphaBeta(b, ss, -alpha-1, -alpha, newDepth, false);
        } else {
            score = INF; // trigger full-window below
        }

        // PV search: full window
        if (isPV && (legalCount == 1 || score > alpha)) {
            score = -alphaBeta(b, ss, -beta, -alpha, newDepth, true);
        }

        b.unmakeMove(m);
        ss.ply--;

        if (ss.stop) return 0;

        if (score > bestScore) {
            bestScore = score;
            bestMove  = m;
        }
        if (score > alpha) {
            alpha = score;
            raisedAlpha = true;
            if (alpha >= beta) {
                // Beta cutoff — update heuristics
                if (isQuiet) {
                    // Killer
                    if (m != ss.killers[ss.ply][0]) {
                        ss.killers[ss.ply][1] = ss.killers[ss.ply][0];
                        ss.killers[ss.ply][0] = m;
                    }
                    // History bonus
                    int bonus = std::min(depth * depth, 400);
                    ss.history[mp][m.to()] = SearchState::clamp_hist(ss.history[mp][m.to()] + bonus);
                    // History malus for previously tried quiets
                    for (int j = 0; j < i; j++) {
                        Move prev = ml.moves[j];
                        if (b.sq[prev.to()] == EMPTY && prev.mtype() != 2 && prev.mtype() != 3) {
                            Piece pm = b.sq[prev.from()];
                            ss.history[pm][prev.to()] = SearchState::clamp_hist(ss.history[pm][prev.to()] - bonus);
                        }
                    }
                } else {
                    // Capture history
                    int bonus = std::min(depth * depth, 400);
                    int ct = static_cast<int>(typeOf(b.history[b.ply].captured));
                    ss.captureHistory[mp][m.to()][ct] =
                        SearchState::clamp_hist(ss.captureHistory[mp][m.to()][ct] + bonus);
                }
                break;
            }
        }
    }

    // Checkmate / stalemate
    if (legalCount == 0) {
        return inCheck ? (-MATE_SCORE + ss.ply) : 0;
    }

    // TT store
    Bound bound = (bestScore >= beta)  ? BOUND_LOWER
                : (raisedAlpha)        ? BOUND_EXACT
                                       : BOUND_UPPER;
    ttStore(b.hash, bestMove, bestScore, depth, bound, ss.ply);

    return bestScore;
}

// Helper: rename ttScore to avoid name clash with the function in tt.cpp
static inline int ttScore_(int rawScore, int ply) {
    if (rawScore >= MATE_BOUND)  return rawScore - ply;
    if (rawScore <= -MATE_BOUND) return rawScore + ply;
    return rawScore;
}

// ── Root search (iterative deepening + aspiration) ───────────────
std::vector<PVLine> rootSearch(Board& b,
                               int timeLimitMs,
                               int maxTimeLimitMs,
                               int multiPV,
                               int maxDepthLimit) {
    TT_AGE++;
    SearchState ss;
    ss.reset();
    ss.startTime    = std::chrono::steady_clock::now();
    ss.timeLimitMs  = timeLimitMs;
    ss.maxTimeLimitMs = maxTimeLimitMs;

    // Collect root legal moves
    std::vector<Move> rootMoves = legalMoves(b);
    if (rootMoves.empty()) return {};

    int pvCount = std::min(multiPV, (int)rootMoves.size());
    std::vector<PVLine> pvLines(pvCount);

    for (int d = 1; d <= maxDepthLimit && !ss.stop; d++) {

        // Per-PV search (multi-PV mode)
        for (int pvidx = 0; pvidx < pvCount && !ss.stop; pvidx++) {
            int alpha = -INF, beta = INF;
            int aspDelta = 25;

            // Aspiration windows from depth 5+
            if (d >= 5 && pvLines[pvidx].depth > 0) {
                alpha = pvLines[pvidx].score - aspDelta;
                beta  = pvLines[pvidx].score + aspDelta;
            }

            // Exclude already-found PV moves in multi-PV
            std::vector<Move> excluded;
            for (int j = 0; j < pvidx; j++)
                excluded.push_back(pvLines[j].best);

            int score = -INF;
            Move best = NULL_MOVE;

            while (true) {
                ss.ply = 0;
                score  = -INF;
                best   = NULL_MOVE;

                // Search each root move
                for (Move m : rootMoves) {
                    // Skip already-found PV moves
                    bool skip = false;
                    for (Move e : excluded) if (m == e) { skip=true; break; }
                    if (skip) continue;

                    ss.ply++;
                    if (!b.makeMove(m)) { ss.ply--; continue; }
                    int sc;
                    if (score == -INF) {
                        sc = -alphaBeta(b, ss, -beta, -alpha, d-1, true);
                    } else {
                        sc = -alphaBeta(b, ss, -alpha-1, -alpha, d-1, false);
                        if (sc > alpha)
                            sc = -alphaBeta(b, ss, -beta, -alpha, d-1, true);
                    }
                    b.unmakeMove(m);
                    ss.ply--;

                    if (ss.stop) break;
                    if (sc > score) { score = sc; best = m; }
                    if (sc > alpha) alpha = sc;
                    if (alpha >= beta) break;
                }

                if (ss.stop) break;

                // Aspiration window re-search
                if (score <= alpha) {
                    alpha = std::max(-INF, alpha - aspDelta);
                    aspDelta += aspDelta / 2;
                } else if (score >= beta) {
                    beta = std::min(INF, beta + aspDelta);
                    aspDelta += aspDelta / 2;
                } else {
                    break;
                }
            }

            if (!best.isNull()) {
                pvLines[pvidx].best  = best;
                pvLines[pvidx].score = score;
                pvLines[pvidx].depth = d;
            }
        }

        if (ss.timeUp()) ss.stop = true;

        // UCI info output for each depth
        for (int pvidx = 0; pvidx < pvCount; pvidx++) {
            const PVLine& pv = pvLines[pvidx];
            if (pv.depth == 0) continue;

            auto now = std::chrono::steady_clock::now();
            long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - ss.startTime).count();
            elapsed = std::max(elapsed, 1LL);

            std::cout << "info depth " << pv.depth;
            if (pvCount > 1) std::cout << " multipv " << (pvidx+1);
            if (std::abs(pv.score) >= MATE_BOUND) {
                int mateIn = (pv.score > 0)
                    ? (MATE_SCORE - pv.score + 1) / 2
                    : -(MATE_SCORE + pv.score + 1) / 2;
                std::cout << " score mate " << mateIn;
            } else {
                std::cout << " score cp " << pv.score;
            }
            std::cout << " nodes "   << ss.nodes
                      << " nps "     << (ss.nodes * 1000 / elapsed)
                      << " time "    << elapsed
                      << " pv "      << b.toUci(pv.best)
                      << "\n";
        }
        std::cout.flush();
    }

    return pvLines;
}

} // namespace Ryzix
