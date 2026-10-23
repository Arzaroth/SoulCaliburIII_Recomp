#!/bin/bash
# usage: compile_vu1.sh [image-dir]
# Compiles the VU1 code images (dumped by a run with PS2X_VU1_DUMP_CODE=<dir>) into the runner sources.
set -e
IMAGES=${1:-$HOME/Games/PS2/SoulcaliburIII/vu1code}
PS2RECOMP=${PS2RECOMP:-$HOME/repos/PS2Recomp.worktrees/sc3/bringup}
"$PS2RECOMP/out/build/ps2xRuntime/ps2_vu1_compile" "$PS2RECOMP/ps2xRuntime/src/runner/vu1_sc3.cpp" "$IMAGES"/*.bin
