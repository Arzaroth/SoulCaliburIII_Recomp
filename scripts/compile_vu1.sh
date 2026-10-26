#!/usr/bin/env bash
# Compiles the VU1 code images written by a run with PS2X_VU1_DUMP_CODE=<dir> into generated/vu1/vu1_sc3.cpp.
# Usage: scripts/compile_vu1.sh [image-dir]   (default: game/vu1code, or $VU1_IMAGES)
source "$(dirname "$0")/common.sh"
IMAGES="${1:-$VU1_IMAGES}"
COMPILER="$TOOLS_BUILD/ps2xRuntime/ps2_vu1_compile"
[ -x "$COMPILER" ] || "$ROOT/scripts/build_tools.sh"
shopt -s nullglob
images=("$IMAGES"/*.bin)
[ ${#images[@]} -gt 0 ] || { echo "no VU code images in $IMAGES, run the game once with PS2X_VU1_DUMP_CODE=$IMAGES"; exit 1; }
mkdir -p "$GENERATED/vu1" "$ROOT/build"
"$COMPILER" "$ROOT/build/vu1_sc3.cpp.new" "${images[@]}"
if cmp -s "$ROOT/build/vu1_sc3.cpp.new" "$GENERATED/vu1/vu1_sc3.cpp"; then
    rm "$ROOT/build/vu1_sc3.cpp.new"
else
    mv "$ROOT/build/vu1_sc3.cpp.new" "$GENERATED/vu1/vu1_sc3.cpp"
fi
echo "Compiled ${#images[@]} code image(s) from $IMAGES into $GENERATED/vu1/vu1_sc3.cpp"
