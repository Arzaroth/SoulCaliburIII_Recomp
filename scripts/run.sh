#!/usr/bin/env bash
# Launches the recompiled game with game/ as cdrom0:. Extra args are passed to ps2EntryRunner.
source "$(dirname "$0")/common.sh"
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -z "${PS2X_VU1_DUMP_CODE:-}" ] || mkdir -p "$PS2X_VU1_DUMP_CODE"
exec "$BIN" "$ELF" "$@"
