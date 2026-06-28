# Ryzix Chess Engine

![Build Status](https://github.com/RD7890/Ryzix/actions/workflows/build.yml/badge.svg)
![Version](https://img.shields.io/badge/version-1.0-blue)
![ELO](https://img.shields.io/badge/ELO-~200-orange)
![Target](https://img.shields.io/badge/target-Android%20ARM64-brightgreen)
![License](https://img.shields.io/badge/license-GPL--3.0-green)

A lightweight **~200 ELO** UCI chess engine compiled for Android ARM64.

Designed for **beginner-level practice** — intentionally weak, fully UCI-compatible, and a drop-in companion to the [A1Chess Android app](https://github.com/RD7890/A1Chess-Android).

---

## Architecture

Architecture patterns and UCI protocol structure inspired by [Stockfish](https://github.com/official-stockfish/Stockfish):

```
src/ryzix.cpp  (~550 lines, single-file engine)
 │
 ├── Types        Color / PieceType / Piece / Move (16-bit encoding)
 ├── Board        64-square array, FEN parser, UndoInfo history[]
 ├── makeMove     Handles castling, en-passant, promotion + legality check
 ├── unmakeMove   Full restore from UndoInfo
 ├── isAttacked   Pawn / knight / slider / king attack detection
 ├── generatePseudoMoves  Slider walker (anti-wrap), pawn push/capture
 ├── legalMoves   Filter pseudo-legal → legal via makeMove
 ├── evaluate     Material count + pawn PST (centipawns)
 ├── search       Depth-1 + ±2000 cp noise → ~200 ELO, MultiPV output
 └── uciLoop      Full UCI protocol handler (A1Chess-compatible)
```

### Strength Design

| Factor | Effect |
|--------|--------|
| Depth 1 only | No look-ahead beyond one move |
| ±2000 cp random noise | Even good captures often ignored |
| No endgame tables | No tablebase awareness |
| No NNUE | No positional understanding |
| **Result** | **~200 ELO** |

---

## Build

### Native (Linux / macOS)

```bash
git clone https://github.com/RD7890/Ryzix.git
cd Ryzix
make build
./ryzix
# type: uci    →  should print uciok
# type: quit
```

### Android ARM64 (NDK r25c)

```bash
make android NDK_PATH=/path/to/android-ndk-r25c
# outputs: libryzix.so  (ARM64 ELF executable)
```

### CI (automatic)

Every push to `main` triggers the [build workflow](.github/workflows/build.yml):
- Compiles `src/ryzix.cpp` with NDK r25c for `aarch64-linux-android21`
- Strips the binary
- Uploads `libryzix.so` as a GitHub Actions artifact
- Creates a GitHub Release on `main` pushes

---

## UCI Commands

| Command | Supported | Notes |
|---------|-----------|-------|
| `uci` | ✅ | Returns `id name Ryzix 1.0`, options, `uciok` |
| `isready` | ✅ | Returns `readyok` immediately |
| `ucinewgame` | ✅ | Resets board to starting position |
| `position startpos [moves …]` | ✅ | Replays move list |
| `position fen <fen> [moves …]` | ✅ | Sets arbitrary position |
| `go movetime <ms>` | ✅ | Returns result instantly (synchronous) |
| `go wtime/btime …` | ✅ | Accepted; time management ignored |
| `stop` | ✅ | No-op (synchronous engine) |
| `quit` | ✅ | Clean exit |
| `setoption name MultiPV value N` | ✅ | 1–10 PV lines |
| `setoption name Skill Level value N` | ✅ | Accepted silently (always ~200 ELO) |
| `setoption name Threads value N` | ✅ | Accepted silently (single-threaded) |

### Output Format (A1Chess compatible)

```
info depth 1 seldepth 1 multipv 1 score cp 47 nodes 100 nps 100000 time 1 pv e2e4 e7e5
info depth 1 seldepth 1 multipv 2 score cp 31 nodes 100 nps 100000 time 1 pv d2d4 d7d5
info depth 1 seldepth 1 multipv 3 score cp 18 nodes 100 nps 100000 time 1 pv g1f3 g8f6
bestmove e2e4
```

---

## A1Chess Integration

Ryzix is **binary-compatible** with A1Chess's engine loader.

A1Chess runs the engine as a subprocess and communicates via stdin/stdout UCI — same as how Stockfish 16 is used.

### Steps

1. Download `libryzix.so` from the [latest release](https://github.com/RD7890/Ryzix/releases)
2. Copy it into the A1Chess project:
   ```
   app/src/main/jniLibs/arm64-v8a/libryzix.so
   ```
3. In `StockfishEngine.kt`, change one constant:
   ```kotlin
   // Before:
   private const val LIB_NAME = "libstockfish.so"
   // After:
   private const val LIB_NAME = "libryzix.so"
   ```
4. Rebuild A1Chess — Ryzix will run as the engine

> Ryzix accepts all UCI commands A1Chess sends:
> `setoption name Skill Level`, `setoption name MultiPV`,
> `setoption name Threads`, `position fen`, `go movetime`, `stop`

---

## Project Structure

```
Ryzix/
├── src/
│   └── ryzix.cpp            — Complete engine (single file)
├── .github/
│   └── workflows/
│       └── build.yml        — Android ARM64 CI pipeline
├── Makefile                  — Native + Android ARM64 build targets
└── README.md
```

---

## Difference from Stockfish

| | Ryzix | Stockfish 16 |
|--|-------|-------------|
| ELO | ~200 | ~3500+ |
| Lines of code | ~550 | ~100 000 |
| Search | Depth-1 + noise | Iterative deepening + α-β |
| Evaluation | Material + PST | NNUE (87M param neural net) |
| Move ordering | Random heuristic | History + SEE + LMR |
| Threads | 1 | Up to 512 |
| Tablebases | None | Syzygy 7-piece |
| SIMD | None | AVX-512 |

---

## License

GNU General Public License v3.0 — same license as Stockfish.

See [LICENSE](LICENSE) for details.
