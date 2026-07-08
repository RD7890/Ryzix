/*
 * Ryzix Chess Engine
 * main.cpp — Entry point.
 *
 * Personality:
 *   White: London System (d4, Nf3, Bf4, e3, Bd3, c3, 0-0)
 *   Black: Caro-Kann Defence vs 1.e4; solid QGD vs 1.d4
 *
 * Build (native Linux):
 *   g++ -O3 -DNDEBUG -std=c++17 -march=native -I src \
 *       src/position.cpp src/movegen.cpp src/evaluate.cpp \
 *       src/tt.cpp src/timeman.cpp src/movepick.cpp \
 *       src/search.cpp src/book.cpp src/uci.cpp src/main.cpp \
 *       -o ryzix
 *
 * Build (Android ARM64, NDK r25c):
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang++ \
 *     -O3 -DNDEBUG -std=c++17 -march=armv8-a -ffast-math -static-libstdc++ \
 *     -I src \
 *     src/position.cpp src/movegen.cpp src/evaluate.cpp \
 *     src/tt.cpp src/timeman.cpp src/movepick.cpp \
 *     src/search.cpp src/book.cpp src/uci.cpp src/main.cpp \
 *     -o ryzix
 */
#include "uci.h"

int main() {
    Ryzix::uciLoop();
    return 0;
}
