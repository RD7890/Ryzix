# Ryzix Chess Engine — Makefile
# Inspired by Stockfish's Makefile structure
#
# Usage:
#   make build              — native Linux/macOS build
#   make android            — Android ARM64 via NDK (set NDK_PATH)
#   make clean

CXX      ?= clang++
CXXFLAGS  = -O2 -DNDEBUG -std=c++17 -Wall -Wextra
LDFLAGS   = -lm
SRC       = src/ryzix.cpp
OUT       = ryzix

.PHONY: build android clean

# ── Native build ───────────────────────────────────────────────
build:
	$(CXX) $(CXXFLAGS) $(SRC) -o $(OUT) $(LDFLAGS)
	@echo "Built: $(OUT) (native)"

# ── Android ARM64 cross-compile via NDK r25c ──────────────────
# Usage: make android NDK_PATH=/path/to/ndk
android:
	@test -n "$(NDK_PATH)" || (echo "ERROR: NDK_PATH not set"; exit 1)
	$(eval CXX_A := $(NDK_PATH)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang++)
	$(eval STRIP := $(NDK_PATH)/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip)
	$(CXX_A) $(CXXFLAGS) -static-libstdc++ $(SRC) -o $(OUT) $(LDFLAGS)
	$(STRIP) $(OUT) 2>/dev/null || true
	cp $(OUT) libryzix.so
	@echo "Built: libryzix.so (Android ARM64, $(shell wc -c < libryzix.so) bytes)"

clean:
	rm -f $(OUT) libryzix.so
