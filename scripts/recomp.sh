#!/usr/bin/env bash
# Runs ps2_recomp with recomp/sc3.toml and ps2_vu_aot on the ELF into generated/, then PS2Recomp/tools/vu1_lift.py on the VU1
# microcode (generated/vu1port/), copying only the files whose content changed.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -x "$TOOLS_BUILD/ps2xRecomp/ps2_recomp" ] && [ -x "$TOOLS_BUILD/ps2xRecomp/ps2_vu_aot" ] || "$ROOT/scripts/build_tools.sh"
cd "$ROOT"

rm -rf build/recomp-staging
mkdir -p build/recomp-staging "$GENERATED"
sed 's|^output = .*|output = "build/recomp-staging/"|' "$CONFIG" > build/recomp-staging.toml
"$TOOLS_BUILD/ps2xRecomp/ps2_recomp" build/recomp-staging.toml
"$TOOLS_BUILD/ps2xRecomp/ps2_vu_aot" "$ELF" build/recomp-staging/vu1
python3 -I "$ROOT/PS2Recomp/tools/vu1_lift.py" "$ROOT/recomp/vu1_lift_spec.json" build/recomp-staging/vu1 build/recomp-staging/vu1port

changed=$(rsync -rc --delete --out-format='%n' build/recomp-staging/ "$GENERATED/" | grep -vc '/$' || true)
echo "Generated $(find "$GENERATED" -maxdepth 1 -name '*.cpp' | wc -l) sources in $GENERATED ($changed changed)"
