#!/usr/bin/env bash
# Builds ps2EntryRunner with the generated code linked in -> build/game/PS2Recomp/ps2xRuntime/ps2EntryRunner
# Usage: scripts/build_runtime.sh [Release|RelWithDebInfo|Debug]
source "$(dirname "$0")/common.sh"
BUILD_TYPE="${1:-RelWithDebInfo}"
[ -f "$GENERATED/register_functions.cpp" ] || { echo "no generated code, run scripts/recomp.sh first"; exit 1; }
compiler=()
if [ ! -f "$GAME_BUILD/CMakeCache.txt" ] && [ -z "${CXX:-}" ] && command -v clang++ >/dev/null; then
    compiler=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
fi
cmake -S "$ROOT" -B "$GAME_BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DPS2X_BUILD_STUDIO=OFF "${compiler[@]}"
cmake --build "$GAME_BUILD" --target ps2EntryRunner -j "$JOBS"
