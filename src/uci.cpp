/*
 * Ryzix Chess Engine
 * uci.cpp — Universal Chess Interface protocol handler.
 *
 * Supported commands:
 *   uci            → identify + uciok
 *   isready        → readyok
 *   ucinewgame     → clear TT + book probe
 *   setoption      → MultiPV, Threads (ignored), Hash (ignored)
 *   position       → set board state from startpos / fen + moves
 *   go             → start search
 *   stop           → abort search (handled by innerTimeUp)
 *   quit           → exit
 */
#include "uci.h"
#include "position.h"
#include "movegen.h"
#include "search.h"
#include "tt.h"
#include "book.h"
#include "timeman.h"
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>
#include <functional>

namespace Ryzix {

void uciLoop() {
    // One-time initialisation
    initZobrist();
    initLMR();
    bookInit();
    ttClear();

    Board board;
    board.setFen(START_FEN);

    int multiPV = 1;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string token;
        iss >> token;

        // ── uci ──────────────────────────────────────────────────
        if (token == "uci") {
            std::cout
                << "id name Ryzix\n"
                << "id author Ryzix Team\n"
                << "option name MultiPV type spin default 1 min 1 max 128\n"
                << "option name Threads type spin default 1 min 1 max 1\n"
                << "option name Hash type spin default 256 min 1 max 1024\n"
                << "uciok\n";
            std::cout.flush();
        }

        // ── isready ───────────────────────────────────────────────
        else if (token == "isready") {
            std::cout << "readyok\n";
            std::cout.flush();
        }

        // ── ucinewgame ────────────────────────────────────────────
        else if (token == "ucinewgame") {
            ttClear();
            board.setFen(START_FEN);
        }

        // ── setoption ─────────────────────────────────────────────
        else if (token == "setoption") {
            std::string name_tok, name_val, val_tok, val_val;
            iss >> name_tok >> name_val >> val_tok >> val_val;
            if (name_val == "MultiPV" && !val_val.empty())
                multiPV = std::stoi(val_val);
            // Hash / Threads: accepted but ignored (fixed-size TT)
        }

        // ── position ──────────────────────────────────────────────
        else if (token == "position") {
            std::string keyword;
            iss >> keyword;

            if (keyword == "startpos") {
                board.setFen(START_FEN);
                board.gameHashCount = 0;
            } else if (keyword == "fen") {
                std::string fenStr;
                std::string part;
                // Read 6 FEN fields
                int fields = 0;
                while (fields < 6 && iss >> part) {
                    if (part == "moves") break;
                    if (!fenStr.empty()) fenStr += ' ';
                    fenStr += part;
                    fields++;
                }
                board.setFen(fenStr);
                board.gameHashCount = 0;
                // Check if next token was already "moves"
                if (part == "moves") {
                    goto parse_moves;
                }
                iss >> keyword;  // should be "moves" or EOF
                if (keyword != "moves") continue;
            } else {
                continue;
            }

            { std::string dummy; iss >> dummy; (void)dummy; } // consume "moves"

            parse_moves:
            {
                std::string mv;
                while (iss >> mv) {
                    Move m = board.fromUci(mv);
                    if (m.isNull()) break;
                    board.gameHashes[board.gameHashCount++] = board.hash;
                    if (board.gameHashCount >= 1024) board.gameHashCount = 0;
                    if (!board.makeMove(m)) break;
                }
            }
        }

        // ── go ────────────────────────────────────────────────────
        else if (token == "go") {
            int wtime=-1, btime=-1, winc=0, binc=0;
            int movestogo=0, movetime=-1, maxdepth=64;
            bool infinite = false;

            std::string opt;
            while (iss >> opt) {
                if      (opt=="wtime")     iss >> wtime;
                else if (opt=="btime")     iss >> btime;
                else if (opt=="winc")      iss >> winc;
                else if (opt=="binc")      iss >> binc;
                else if (opt=="movestogo") iss >> movestogo;
                else if (opt=="movetime")  iss >> movetime;
                else if (opt=="depth")     iss >> maxdepth;
                else if (opt=="infinite")  infinite = true;
                else if (opt=="nodes") {
                    // accepted but not enforced node limit
                    int dummy; iss >> dummy; (void)dummy;
                }
            }

            // Probe opening book first
            std::string bookMove = bookProbe(board);
            if (!bookMove.empty()) {
                std::cout << "bestmove " << bookMove << "\n";
                std::cout.flush();
                continue;
            }

            // Time management
            int myTime = (board.side == WHITE) ? wtime : btime;
            int myInc  = (board.side == WHITE) ? winc  : binc;

            TimeControl tc = calcTime(myTime, myInc, movestogo, movetime, infinite);
            if (infinite || maxdepth < 64) {
                tc.softLimitMs = tc.hardLimitMs = infinite ? 3'600'000 : 3'600'000;
            }
            if (movetime > 0) {
                tc.softLimitMs = tc.hardLimitMs = std::max(10, movetime - 30);
            }

            // Run search
            auto pvLines = rootSearch(board, tc.softLimitMs, tc.hardLimitMs,
                                      multiPV, maxdepth);

            // Output bestmove
            if (!pvLines.empty() && !pvLines[0].best.isNull()) {
                std::cout << "bestmove " << board.toUci(pvLines[0].best) << "\n";
            } else {
                // Fallback: first legal move
                std::vector<Move> legal = legalMoves(board);
                if (!legal.empty())
                    std::cout << "bestmove " << board.toUci(legal[0]) << "\n";
                else
                    std::cout << "bestmove 0000\n";
            }
            std::cout.flush();
        }

        // ── stop ──────────────────────────────────────────────────
        else if (token == "stop") {
            // Handled by innerTimeUp() — no action needed here
        }

        // ── quit ──────────────────────────────────────────────────
        else if (token == "quit") {
            break;
        }

        // ── debug helper: display ──────────────────────────────────
        else if (token == "d") {
            // Print board for debugging
            const char* PCH = ".PNBRQKpnbrqk";
            for (int r=7; r>=0; r--) {
                std::cout << (r+1) << " ";
                for (int f=0; f<8; f++)
                    std::cout << PCH[board.sq[mkSq(r,f)]] << " ";
                std::cout << "\n";
            }
            std::cout << "  a b c d e f g h\n";
            std::cout << "Side: " << (board.side==WHITE?"White":"Black") << "\n";
            std::cout << "Hash: " << board.hash << "\n";
            std::cout.flush();
        }

        // ── perft (for testing) ───────────────────────────────────
        else if (token == "perft") {
            int depth = 1;
            iss >> depth;
            // Simple perft
            std::function<long long(Board&, int)> perft = [&](Board& b, int d) -> long long {
                if (d == 0) return 1;
                std::vector<Move> mv = legalMoves(b);
                long long nodes = 0;
                for (Move m : mv) {
                    b.makeMove(m);
                    nodes += perft(b, d-1);
                    b.unmakeMove(m);
                }
                return nodes;
            };
            long long n = perft(board, depth);
            std::cout << "nodes " << n << "\n";
            std::cout.flush();
        }
    }
}

} // namespace Ryzix
