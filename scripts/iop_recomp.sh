#!/usr/bin/env bash
# Recompiles SC3's IRX modules to native C++ in generated/iop/ with PS2Recomp's tools/iop_recomp.py, run with
# PS2X_IOP_NATIVE=1 (the default, see runtime/sc3_overrides.cpp). The recompiler works from each module as the IOP
# loaded it, relocated at its base, so this first boots the game with PS2X_IOP_DUMP_MODULES into build/iop_modules/.
# Like the rest of generated/, the output is derived from the game and never committed. Rebuild the runner after it.
# A game run: take the bench lock around it.
#   scripts/iop_recomp.sh [seconds of boot, default 40] [modules, default: all eight]
source "$(dirname "$0")/common.sh"
cd "$ROOT"
SECONDS_TO_BOOT=${1:-40}
MODULES=${2:-"CRI_ADXI LIBSD MCMAN MCSERV NUSOUND PADMAN SDRDRV SIO2MAN"}
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner"
DUMPS="$ROOT/build/iop_modules"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
rm -rf "$DUMPS" && mkdir -p "$DUMPS" "$GENERATED/iop"
PS2X_SPU2_WALLCLOCK="${PS2X_SPU2_WALLCLOCK:-1}" PS2X_SC3_NO_MOVIES=1 PS2X_IOP_NATIVE=0 PS2X_IOP_DUMP_MODULES="$DUMPS" \
    timeout "$SECONDS_TO_BOOT" "$BIN" "$ELF" > build/iop_dump.log 2>&1 || true
for module in $MODULES; do
    json=$(ls "$DUMPS" | grep -i "^$module\.IRX.*\.json$" | head -1)
    [ -n "$json" ] || { echo "no dump of $module (see build/iop_dump.log)"; exit 1; }
    python3 PS2Recomp/tools/iop_recomp.py "$DUMPS/$json" --out "$GENERATED/iop" | grep -E '^[^ ]|functions,'
    python3 PS2Recomp/tools/iop_recomp.py "$DUMPS/$json" --list > "build/iop_${module}_list.txt"
done
