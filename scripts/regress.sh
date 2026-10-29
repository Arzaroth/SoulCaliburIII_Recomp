#!/usr/bin/env bash
# usage: scripts/regress.sh <build-dir> <out-dir>
# Scripted regression route: boot with movies off, title, Mode Select, Single Player Modes, Practice into a Free
# Training fight, a minute of attacks, then stop. Each press waits for a number of presented frames rather than a
# fixed time, so a slow start does not throw the route off. Exits 0 once the run itself completed; the verdict is in
# <out-dir>/summary.txt (key=value, NA when unknown). Also writes run.log, last_frame.png (last_frame.raw, 640x448
# RGBA, without a PNG), errors.txt, details.txt and pad.txt (each press with its time and presented frame).
#   route_reached     1 once every press of the route and the fight's attacks were played without a hang or crash
#   exit              ok, hang (process alive, presented frame unchanged for REGRESS_HANG_SECS), crash (process
#                     ended early), timeout (still alive 10 s after being told to stop)
#   gs_frames_*, fps_mean, frames_presented
#                     GS FINISH frames from the 5 s state dump, over the fight once reached, else over the run;
#                     fps_p10 and frames_over_25ms are NA: the runtime logs no per-frame present times
#   spu2_underruns    host stream underruns from PS2X_SPU2_STATS, NA when the SPU2 runs on the wall clock (no stats)
#   spu2_adma_underruns
#                     AutoDMA input halves the SPU2 found empty while the game streams (ADX), from the state dump
#   REGRESS_HANG_SECS   default 20
#   REGRESS_FIGHT_SECS  length of the fight (default 60)
#   SC3_ELF             game ELF (default: game/SLUS_212.16 of this repo)
#   PS2X_SPU2_WALLCLOCK default 1, as scripts/run.sh
set -euo pipefail
export PS2X_SPU2_WALLCLOCK="${PS2X_SPU2_WALLCLOCK:-1}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ $# -eq 2 ] || { echo "usage: $0 <build-dir> <out-dir>" >&2; exit 2; }
target=$1
out=$2
elf=${SC3_ELF:-$root/game/SLUS_212.16}
hang_secs=${REGRESS_HANG_SECS:-20}
fight_secs=${REGRESS_FIGHT_SECS:-60}
period=5

runner=
if [ -f "$target" ] && [ -x "$target" ]; then
    runner=$target
else
    for candidate in "$target/PS2Recomp/ps2xRuntime/ps2EntryRunner" "$target/build/game/PS2Recomp/ps2xRuntime/ps2EntryRunner" \
        "$target/ps2xRuntime/ps2EntryRunner" "$target/ps2EntryRunner"; do
        if [ -x "$candidate" ]; then runner=$candidate; break; fi
    done
fi
[ -n "$runner" ] || { echo "no ps2EntryRunner in $target" >&2; exit 2; }
[ -f "$elf" ] || { echo "missing $elf" >&2; exit 2; }
runner=$(realpath "$runner")
ps2recomp=$(git -C "$root/PS2Recomp" rev-parse HEAD 2>/dev/null || echo NA)

mkdir -p "$out"
out=$(realpath "$out")
frames=$out/frames
shots=$out/shots
log=$out/run.log
input=$out/pad_input.txt
rm -rf "$frames" "$shots" "$out"/last_frame.* "$out/summary.txt" "$out/details.txt" "$out/errors.txt" "$out/pad.txt"
mkdir -p "$frames" "$shots"
: > "$input"
: > "$log"

pid=
stop_runner() { if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then kill -KILL "$pid" 2>/dev/null || true; fi; }
trap stop_runner EXIT
trap 'stop_runner; exit 143' TERM INT

start=$(date +%s)
(
    cd "$out"
    exec env DISPLAY="${DISPLAY:-:0}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}" PS2X_DEBUG_UI=0 \
        PS2X_SPU2_STATS=1 PS2X_SC3_NO_MOVIES=1 PS2X_STATE_DUMP=$period \
        PS2X_FRAME_DUMP="$frames" PS2X_FRAME_DUMP_EVERY=1 \
        PS2X_CAPTURE_DIR="$shots" PS2X_CAPTURE_INTERVAL=$period PS2X_PAD_INPUT="$input" \
        "$runner" "$elf"
) > "$log" 2>&1 &
pid=$!

result=ok
status=NA
last_hash=
same_since=$start
check()
{
    local now newest hash
    if ! kill -0 "$pid" 2>/dev/null; then
        set +e; wait "$pid"; status=$?; set -e
        pid=
        result=crash
        return 1
    fi
    now=$(date +%s)
    newest=$(ls "$frames" | sort -t_ -k2 -n | tail -1)
    if [ -n "$newest" ]; then
        hash=$(sha1sum "$frames/$newest" | cut -d' ' -f1)
        ls "$frames" | sort -t_ -k2 -n | head -n -1 | sed "s|^|$frames/|" | xargs -r rm -f
        if [ "$hash" != "$last_hash" ]; then
            last_hash=$hash
            same_since=$now
        elif [ $(( now - same_since )) -ge "$hang_secs" ]; then
            result=hang
            return 1
        fi
    fi
}
pause() { local i; for i in $(seq "$1"); do sleep 1; check || return 1; done; }
press() { echo "$*" >> "$input"; echo "$(( $(date +%s) - start )) $(presented) $*" >> "$out/pad.txt"; }
presented()
{
    local newest
    newest=$(ls "$frames" | sort -t_ -k2 -n | tail -1)
    if [ -n "$newest" ]; then echo $(( $(echo "$newest" | cut -d_ -f2) + 1 )); else echo 0; fi
}
await()
{
    local goal=$(( $(presented) + $1 ))
    while [ "$(presented)" -lt "$goal" ]; do
        sleep 0.5
        check || return 1
        [ $(( $(date +%s) - start )) -lt 400 ] || return 1
    done
}

route()
{
    local i
    await 26 || return 1
    for i in 1 2 3 4; do press START; await 3 || return 1; done
    await 6 || return 1
    press CROSS; await 4 || return 1
    for i in 1 2 3 4; do press DOWN; await 1 || return 1; done
    await 1 || return 1
    press CROSS; await 4 || return 1
    press DOWN; await 2 || return 1
    press CROSS; await 8 || return 1
    for i in 1 2 3 4; do press CROSS; await 4 || return 1; done
    await 4 || return 1
    press RIGHT; await 2 || return 1
    for i in 1 2 3 4 5; do press CROSS; await 4 || return 1; done
    await 20 || return 1
    fight_mark=$(grep -a -c '^\[state\] gs frames=' "$log" || true)
    fight_at_s=$(( $(date +%s) - start ))
    local t=0
    while [ "$t" -lt "$fight_secs" ]; do
        press SQUARE; await 1 || return 1
        press TRIANGLE; await 1 || return 1
        press RIGHT; await 1 || return 1
        t=$((t + 3))
    done
    route_reached=1
}

route_reached=0
fight_mark=
fight_at_s=NA
route || true
duration=$(( $(date +%s) - start ))
if [ -n "$pid" ]; then
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 10); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    if kill -0 "$pid" 2>/dev/null; then
        kill -KILL "$pid" 2>/dev/null || true
        [ "$result" != ok ] || result=timeout
    fi
    set +e; wait "$pid"; status=$?; set -e
    pid=
fi

last=$(ls "$shots" | sort | tail -1)
if [ -n "$last" ]; then
    mv "$shots/$last" "$out/last_frame.png"
else
    last=$(ls "$frames" | sort -t_ -k2 -n | tail -1)
    if [ -n "$last" ]; then mv "$frames/$last" "$out/last_frame.raw"; fi
fi
rm -rf "$frames" "$shots" "$input"

gs_samples() { grep -a '^\[state\] gs frames=' "$log" | sed 's/^\[state\] gs frames=\([0-9]*\).*/\1/'; }
if [ -n "$fight_mark" ]; then
    scope=fight
    samples=$(gs_samples | awk -v f="$fight_mark" 'NR > f + 1')
else
    scope=run
    samples=$(gs_samples | awk 'seen || $1 > 0 { seen = 1; print }')
fi
read -r median minimum mean count < <(echo "$samples" | sort -n | awk -v p=$period 'NF { v[++n] = $1; s += $1 }
    END {
        if (!n) { print "NA NA NA 0"; exit }
        m = n % 2 ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2
        printf "%s %s %.2f %d\n", m, v[1], s / (n * p), n
    }')
frames_presented=$(gs_samples | awk '{ s += $1 } END { print NR ? s : "NA" }')

errors='error|fatal|abort|exception|unimplemented|unhandled|invalid|crash|assert'
underruns=$(grep -ao '^\[spu2:stats\].* underruns [0-9]*' "$log" | tail -1 | awk '{ print $NF }' || true)
adma_underruns=$(grep -ao '^\[state\] *\[SPU2\] samples=.*underruns [0-9]*' "$log" | tail -1 | awk '{ print $NF }' || true)
grep -a -v '^\[state\]' "$log" | grep -a -i -E "$errors" \
    | sed -E 's/0x[0-9a-fA-F]+/X/g; s/[0-9]+/N/g' | sort | uniq -c | sort -rn > "$out/errors.txt" || true
error_lines=$(grep -a -v '^\[state\]' "$log" | grep -a -c -i -E "$errors" || true)
warning_lines=$(grep -a -v '^\[state\]' "$log" | grep -a -c -i 'warn' || true)

cat > "$out/summary.txt" <<EOF
port=sc3
ps2recomp=$ps2recomp
route_reached=$route_reached
exit=$result
duration_s=$duration
frames_presented=$frames_presented
fps_mean=$mean
fps_p10=NA
frames_over_25ms=NA
gs_frames_per_5s_median=$median
gs_frames_per_5s_min=$minimum
spu2_underruns=${underruns:-NA}
spu2_adma_underruns=${adma_underruns:-NA}
error_lines=$error_lines
warning_lines=$warning_lines
EOF
cat > "$out/details.txt" <<EOF
fight_at_s=$fight_at_s
gs_frames_scope=$scope
gs_samples=$count
runner_status=$status
runner=$runner
EOF
cat "$out/summary.txt"
