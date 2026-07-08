# ──────────────────────────────────────────────────────────────────
#  Ryzix Chess Engine — Makefile
#
#  Targets:
#    make            → native Linux build (development / testing)
#    make android    → Android ARM64 cross-build (requires NDK in PATH)
#    make clean      → remove binaries
#    make perft      → run perft-5 smoke test
# ──────────────────────────────────────────────────────────────────

# Source files (explicit list — no wildcards)
SRCS := src/position.cpp \
        src/movegen.cpp  \
        src/evaluate.cpp \
        src/tt.cpp       \
        src/timeman.cpp  \
        src/movepick.cpp \
        src/search.cpp   \
        src/book.cpp     \
        src/uci.cpp      \
        src/main.cpp

# ── Native build ──────────────────────────────────────────────────
CXX     ?= g++
CXXFLAGS = -O3 -DNDEBUG -std=c++17 -march=native \
           -ffast-math -funroll-loops \
           -Wall -Wno-unused-variable \
           -I src

TARGET  = ryzix

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $^ -lm -o $@

# ── Android ARM64 cross-build ─────────────────────────────────────
# Set ANDROID_NDK or have the NDK clang++ on your PATH.
NDK_CXX ?= aarch64-linux-android21-clang++

android: $(SRCS)
	$(NDK_CXX) \
	  -O3 -DNDEBUG -std=c++17 \
	  -march=armv8-a -ffast-math -funroll-loops \
	  -static-libstdc++ \
	  -I src \
	  $^ \
	  -lm -o ryzix_android

# ── Perft smoke test ──────────────────────────────────────────────
perft: $(TARGET)
	echo "perft 5" | ./$(TARGET)

# ── Clean ─────────────────────────────────────────────────────────
clean:
	rm -f $(TARGET) ryzix_android

.PHONY: android perft clean
