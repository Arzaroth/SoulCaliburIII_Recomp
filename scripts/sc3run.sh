#!/usr/bin/env bash
# usage: sc3run.sh <name> <seconds> [extra env...]
set -u
name=$1; secs=$2; shift 2
runs=${SC3_RUNS:-$HOME/Games/PS2/SoulcaliburIII/runs}
runner=${SC3_RUNNER:-$HOME/repos/PS2Recomp.worktrees/sc3/bringup/out/build/ps2xRuntime/ps2EntryRunner}
elf=${SC3_ELF:-$HOME/Games/PS2/SoulcaliburIII/disc/SLUS_212.16}
mkdir -p "$runs"; cd "$runs"
env PS2X_DEBUG_UI=0 PS2X_STATE_DUMP=5 "$@" timeout $((secs + 15)) \
  "$runner" "$elf" > "$name.log" 2>&1 &
pid=$!
start=$(date +%s)
while kill -0 $pid 2>/dev/null && [ $(( $(date +%s) - start )) -lt "$secs" ]; do sleep 1; done
if kill -0 $pid 2>/dev/null; then
  geo=$(hyprctl clients -j | python3 -I -c 'import json,sys
for c in json.load(sys.stdin):
    if c["title"].startswith("PS2-Recomp"):
        print("%d,%d %dx%d" % (c["at"][0], c["at"][1], c["size"][0], c["size"][1])); break')
  [ -n "$geo" ] && timeout 10 grim -g "$geo" "$name.png" && echo "shot $runs/$name.png"
  kill $pid 2>/dev/null
else
  echo "exited early"
fi
wait $pid 2>/dev/null
grep "^\[state\]   thread" "$name.log" | tail -4
