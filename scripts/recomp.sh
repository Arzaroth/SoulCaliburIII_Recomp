#!/usr/bin/env bash
# Runs ps2_recomp with recomp/sc3.toml into generated/, copying only the files whose content changed.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -x "$TOOLS_BUILD/ps2xRecomp/ps2_recomp" ] || "$ROOT/scripts/build_tools.sh"
cd "$ROOT"

rm -rf build/recomp-staging
mkdir -p build/recomp-staging "$GENERATED"
sed 's|^output = .*|output = "build/recomp-staging/"|' "$CONFIG" > build/recomp-staging.toml
"$TOOLS_BUILD/ps2xRecomp/ps2_recomp" build/recomp-staging.toml

# generated/vu1/ belongs to scripts/compile_vu1.sh.
changed=$(rsync -rc --delete --exclude=/vu1/ --out-format='%n' build/recomp-staging/ "$GENERATED/" | grep -vc '/$' || true)
echo "Generated $(find "$GENERATED" -maxdepth 1 -name '*.cpp' | wc -l) sources in $GENERATED ($changed changed)"
