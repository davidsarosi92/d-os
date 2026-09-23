#!/bin/bash
# =============================================================================
# bench-taskman-sweep.sh — the Task Manager pointer-sweep benchmark, as a FILE.
#
# WHY THIS EXISTS: §M81 recorded a per-frame regression for the composed Task
# Manager (median 1386 -> 2428 us) and did not record the gesture that produced
# it.  Re-measuring with this script on both revisions of that commit, and on
# HEAD with the conversion reverted, found NO difference beyond noise — which
# says the original number came from a different gesture, and a benchmark whose
# recipe is not written down cannot be re-run, only re-guessed.  So the recipe
# is this file.
#
# WHAT IT DOES: boots headless (i386 by default, -smp 4), turns on
# `gui.stats_ms` (2 s windows), opens the Task Manager, homes the pointer, then
# sweeps it across the table in eight horizontal passes of twelve 40 px hops
# (<= 90 px each: the PS/2 delta is a signed byte).  The table renders no hover
# state, so what the sweep measures is the TICK's damage under a moving cursor,
# which is exactly the cost a composed layout could have changed.
#
# READING IT: only the 2 s windows with >= 24 frames are the sweep; the rest are
# boot, the window opening and the idle tail.  The summary prints the median
# and range of those windows.  §M69's measured noise floor is +-19 %, so two
# builds differ only when their ranges barely overlap.
#
# usage: scripts/bench-taskman-sweep.sh <label> [outdir]      (ARCH=... honoured)
# =============================================================================
set -eu
cd "$(dirname "$0")/.."

L=${1:?label}
OUT=${2:-build/bench}
mkdir -p "$OUT"

args=()
for _ in $(seq 30); do args+=(--monitor-cmd "mouse_move -90 -90"); done
for _ in 1 2 3;     do args+=(--monitor-cmd "mouse_move 83 83");   done
args+=(--monitor-cmd "sleep 1")
for p in 0 1 2 3 4 5 6 7; do
    if [ $((p % 2)) -eq 0 ]; then dx=40; else dx=-40; fi
    for _ in $(seq 12); do
        args+=(--monitor-cmd "mouse_move $dx 0" --monitor-cmd "sleep 0.05")
    done
    args+=(--monitor-cmd "mouse_move 0 40")
done
args+=(--monitor-cmd "sleep 3")

python3 scripts/dos-shell-test.py --arch "${ARCH:-i386}" --smp 4 \
    --cmd "setconf gui.stats_ms 2000" --cmd "launch Task Manager" \
    --settle 5 "${args[@]}" --log "$OUT/$L.log" > "$OUT/$L.out" 2>&1

python3 - "$OUT/$L.log" <<'EOF'
import re, statistics, sys
s = []
for l in open(sys.argv[1], errors="replace"):
    m = re.search(r"gui: (\d+) frames in 2000 ms .{1,3} (\d+) us mean \((\d+) kpx\)", l)
    if m and int(m.group(1)) >= 24:
        s.append((int(m.group(2)), int(m.group(3))))
if not s:
    sys.exit("no sweep windows found - did the Task Manager open?")
us = [a for a, _ in s]; kp = [b for _, b in s]
print("%d windows: median %d us/frame, range %d..%d, area %d..%d kpx"
      % (len(s), statistics.median(us), min(us), max(us), min(kp), max(kp)))
EOF
