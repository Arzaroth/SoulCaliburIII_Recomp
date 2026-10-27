#!/usr/bin/env bash
# Builds ps2_recomp, ps2_analyzer and ps2_vu_aot from the PS2Recomp submodule into build/tools.
source "$(dirname "$0")/common.sh"
cmake -S "$ROOT/PS2Recomp" -B "$TOOLS_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_ENABLE_DEBUG_UI=OFF -DPS2X_ENABLE_PARALLEL_GS=OFF
cmake --build "$TOOLS_BUILD" --target ps2_recomp ps2_analyzer ps2_vu_aot -j "$JOBS"
