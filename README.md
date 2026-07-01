<div align="center">

<img src="logo.png" alt="Ryzix Chess Engine" width="220"/>

# Ryzix Chess Engine

**A strong UCI chess engine for Android ARM64 & Linux**

[![Build](https://github.com/RD7890/Ryzix/actions/workflows/build.yml/badge.svg)](https://github.com/RD7890/Ryzix/actions)
![Version](https://img.shields.io/badge/version-3.0-blue?style=flat-square)
![ELO](https://img.shields.io/badge/ELO-~2500--3000-brightgreen?style=flat-square)
![Language](https://img.shields.io/badge/C%2B%2B-17-blue?style=flat-square)
![License](https://img.shields.io/badge/license-GPL--3.0-green?style=flat-square)
![Target](https://img.shields.io/badge/target-Android%20ARM64%20%7C%20Linux-orange?style=flat-square)

*Drop-in companion engine for [RyzixChess-Android](https://github.com/RD7890/RyzixChess-Android)*

</div>

---

## About

Ryzix is a **strong classical UCI chess engine** built from scratch in modern C++17.  
Version 3.0 introduces a complete search and evaluation overhaul — every major technique from the modern engine playbook is implemented.

It is designed to be **binary-compatible** with any UCI-capable Android chess GUI (including A1Chess / RyzixChess-Android) and compiles natively for Linux and cross-compiles for Android ARM64 via the NDK.

---

## Engine Icon

<div align="center">
<img src="logo-transparent.png" alt="Ryzix Knight" width="180"/>
<br/>
<sub><i>The Ryzix Knight — obsidian and liquid metal, powered by electric intelligence</i></sub>
</div>

---

## Architecture

```
src/ryzix.cpp  (~2000 lines · single-file · zero dependencies)
 │
 ├── Types          Color · PieceType · Piece · Move (16-bit packed)
 ├── Board          64-square mailbox · FEN parser · incremental Zobrist hash
 ├── Repetition     Two-fold (game) + one-fold (search path) detection
 ├── SEE            Static Exchange Evaluation — move ordering & pruning
 ├── Evaluation     PeSTO MG/EG tapering · mobility · pawn structure ·
 │                  king safety · outposts · open files · 7th rank · bishop pair
 ├── Search         Iterative-deepening PVS — see full table below
 ├── Move Order     TT move → SEE captures → promotions → killers →
 │                  counter-move → history heuristic
 └── UCI            Full protocol · multi-PV · increment-aware time management
```

---

## Search

| Technique | Detail |
|-----------|--------|
| **Algorithm** | Principal Variation Search (PVS / negamax) |
| **Iterative Deepening** | Depth 1 → 64 with aspiration windows (±25 cp, doubling on fail) |
| **Transposition Table** | 4 M entries · 48 MB · Zobrist 64-bit · age-based replacement |
| **Null-Move Pruning** | R = 3 + depth/3 + eval-margin bonus; skips zugzwang-prone positions |
| **Late-Move Reductions** | Log formula: `0.75 + ln(depth) × ln(moveIdx) / 2.25`; adjusted for PV, history, improving flag |
| **Singular Extensions** | Verification search at reduced depth; extend TT move if singularly best; multi-cut prune if not |
| **ProbCut** | Probabilistic cut at depth ≥ 5 with SEE-filtered threshold |
| **Reverse Futility Pruning** | depth ≤ 8; margin = 80 × depth |
| **Futility Pruning** | depth ≤ 8; margin = 100 × depth |
| **Internal Iterative Reduction** | Reduce depth by 1 when no TT move at depth ≥ 4 |
| **Quiescence Search** | Captures + promotions; SEE-filtered; delta pruning |
| **Check Extension** | +1 ply when side to move is in check |
| **Repetition Detection** | One-fold within search path · two-fold in game history |
| **SEE Pruning** | Skip losing captures in quiescence; history-gate quiet moves in main search |
| **Killer Moves** | 2 killers per ply |
| **Counter-Move Table** | Per (piece, to-square) refutation move |
| **History Heuristic** | Quiet + capture history with depth² bonus and malus |

---

## Evaluation

| Feature | Detail |
|---------|--------|
| **Material** | PeSTO middlegame / endgame values, phase-tapered |
| **Piece-Square Tables** | Full PeSTO MG + EG tables for all 6 piece types |
| **Mobility** | Pseudo-legal move count per piece type; weighted per phase |
| **Pawn Structure** | Doubled, isolated, passed pawns (rank² scaling) |
| **Passed Pawn Support** | King proximity bonus in endgame |
| **Knight Outposts** | Bonus for centralized knights on squares supported by own pawn |
| **Rook Open Files** | Open file +25 mg · semi-open +12 mg |
| **Rook on 7th Rank** | +20 mg / +30 eg |
| **Bishop Pair** | +40 mg / +50 eg |
| **King Safety** | Pawn shield · open files near king · weighted attacker count table |
| **Tempo** | +14 cp for side to move |

---

## Strength Comparison

| | **Ryzix v1.0** | **Ryzix v3.0** | **Stockfish 17** |
|-|:-:|:-:|:-:|
| **ELO** | ~200 | **~2500–3000** | ~3600+ |
| **Search** | Depth-1 + noise | Full PVS, all modern techniques | MCTS-inspired + NNUE |
| **Evaluation** | Material only | Full positional (8 features) | NNUE 87M params |
| **Move Ordering** | Random | TT + SEE + killers + history | Highly tuned |
| **Time Mgmt** | Ignored | Soft/hard limits + increment | Full tournament TC |
| **Repetition** | None | ✅ Two-fold / one-fold | ✅ |
| **NNUE** | ❌ | ❌ | ✅ |
| **Tablebases** | ❌ | ❌ | ✅ Syzygy 7-piece |

> **On the 5000 ELO question:** No engine in existence reaches 5000 ELO.
> Stockfish 17 is rated ~3600 CCRL — the world record.
> The gap between ~3000 and ~3500 is almost entirely closed by **NNUE** (a trained neural net eval).
> Ryzix v3.0 maxes out every proven *classical* technique; NNUE integration is the clear next step.

---

## Build

### Native (Linux / macOS)

```bash
git clone https://github.com/RD7890/Ryzix.git
cd Ryzix
make build          # g++ -O3 -march=native -std=c++17
./ryzix
uci                 # → id name Ryzix 3.0 … uciok
quit
```

### Android ARM64  (NDK r25c)

```bash
make android NDK_PATH=/path/to/android-ndk-r25c
# → libryzix.so  (stripped ARM64 ELF)
```

### CI / GitHub Actions

Every push to `main` triggers the build workflow:
- Cross-compiles for `aarch64-linux-android21`
- Strips binary
- Uploads `libryzix.so` as a release artifact

---

## UCI Options

| Option | Default | Range | Notes |
|--------|---------|-------|-------|
| `MultiPV` | 1 | 1–10 | Number of principal variation lines |
| `Hash` | 48 | 1–512 | TT size hint in MB |
| `Threads` | 1 | 1 | Single-threaded (multi-thread is future work) |
| `UCI_LimitStrength` | false | — | Accepted |
| `UCI_Elo` | 3000 | 500–3200 | Accepted |
| `Skill Level` | 20 | 0–20 | Accepted |

### Supported `go` Parameters

```
go movetime <ms>                    — exact time per move
go wtime <ms> btime <ms> [winc/binc <ms>] [movestogo <n>]
go depth <n>                        — search exactly n plies
go infinite                         — search until "stop"
```

---

## RyzixChess-Android Integration

```
1. Download libryzix.so from the latest GitHub Release
2. Copy to:  app/src/main/jniLibs/arm64-v8a/libryzix.so
3. In StockfishEngine.kt:
       private const val LIB_NAME = "libryzix.so"
4. Rebuild → Ryzix runs as the engine
```

All commands sent by RyzixChess-Android are fully supported.

---

## Project Structure

```
Ryzix/
├── src/
│   └── ryzix.cpp          — Complete engine (~2000 lines, C++17, no deps)
├── logo.png               — Engine logo (dark background)
├── logo-transparent.png   — Engine icon (transparent PNG)
├── .github/
│   └── workflows/
│       └── build.yml      — Android ARM64 CI/CD pipeline
├── Makefile               — Native + Android ARM64 build targets
└── README.md
```

---

## Roadmap

- [ ] **NNUE evaluation** — train or integrate a small NNUE net (+300–500 ELO)
- [ ] **Syzygy tablebases** — perfect endgame play
- [ ] **Multi-threading** — parallel search (Lazy SMP)
- [ ] **Opening book** — PolyGlot format
- [ ] **Pondering** — search during opponent's clock

---

## License

GNU General Public License v3.0 — same license as Stockfish.  
See [LICENSE](LICENSE) for details.

---

<div align="center">
<img src="logo-transparent.png" width="80"/>
<br/>
<sub>Built with precision. Powered by obsidian logic.</sub>
</div>
