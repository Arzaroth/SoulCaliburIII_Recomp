#!/usr/bin/env bash
# Launches the recompiled game with game/ as cdrom0:. Extra args are passed to ps2EntryRunner.
# The SPU2 follows the host's clock unless PS2X_SPU2_WALLCLOCK=0.
source "$(dirname "$0")/common.sh"
export PS2X_SPU2_WALLCLOCK="${PS2X_SPU2_WALLCLOCK:-1}"
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
exec "$BIN" "$ELF" "$@"
