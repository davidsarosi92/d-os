#!/usr/bin/env bash
# design-compare.sh — render the design reference and a live d-os screenshot
# side by side, so a claim about "it looks like the design" can be checked
# instead of asserted.
#
# WHY THIS IS A SCRIPT.  The visual work on the Console Plate port kept going
# wrong in ways no measurement caught: a taskbar that was the right colour and
# a sixth of the right height, a clock drawn twice because its box assumed an
# 8 px font, a button whose label was clipped at both ends.  Every one of those
# was obvious in a picture and invisible in a pixel probe.  So the comparison
# has to be cheap enough to run constantly.
#
# Chrome renders the design's own HTML prototype; the guest's screenshot comes
# from the ordinary test harness.  Neither side is redrawn by hand, so neither
# can flatter the other.
#
# Usage:
#   scripts/design-compare.sh                      # design reference only
#   scripts/design-compare.sh "launch Task Manager"  # + a guest screenshot
set -euo pipefail

CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
OUT=/tmp/dos-design
mkdir -p "$OUT"

# --- the design side -------------------------------------------------------
if [ -f OS_HTML_DES.html ]; then
    "$CHROME" --headless --disable-gpu --hide-scrollbars \
        --window-size=1400,2400 \
        --screenshot="$OUT/design.png" \
        "file://$PWD/OS_HTML_DES.html" >/dev/null 2>&1
    echo "design    → $OUT/design.png"
else
    echo "design    → OS_HTML_DES.html not found, skipping"
fi

# --- the guest side --------------------------------------------------------
if [ $# -gt 0 ]; then
    args=()
    for c in "$@"; do args+=(--cmd "$c"); done
    python3 scripts/dos-shell-test.py --arch i386 \
        "${args[@]}" --screenshot "$OUT/guest.ppm" >/dev/null 2>&1
    sips -s format png "$OUT/guest.ppm" --out "$OUT/guest.png" >/dev/null 2>&1
    echo "guest     → $OUT/guest.png"
fi
