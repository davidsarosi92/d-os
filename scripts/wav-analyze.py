#!/usr/bin/env python3
"""wav-analyze.py — measure a WAV captured from QEMU's `-audiodev wav`.

WHY THIS IS A FILE: every audio claim in §M23 and §M67 (duration to the
millisecond, peak amplitude, pitch by zero crossing, L == R, "zero internal
silence", "replays every 682.7 ms") was produced by an analysis like this one,
and none of those analyses was committed — so each new audio question started by
re-deriving the instrument.  A measurement whose method is not a file can be
re-guessed, not re-run.

What it reports, per NON-SILENT SEGMENT (a run of samples above --threshold,
with gaps shorter than --merge-ms folded in):
  start / length in ms, peak amplitude, frequency by zero crossing, whether the
  left and right channels are identical, and the longest INTERNAL silence
  (a stretch of exact zeros inside the segment — how a drain firing mid-stream
  shows up).
Plus the spacing between segment starts, because a cyclic DMA ring that was
never stopped replays at a fixed period and the period names the ring.

usage: scripts/wav-analyze.py capture.wav [--threshold 64] [--merge-ms 5]
       [--expect-ms 300]   (exit 1 if the segment count is not 1 or its
                            length differs from the expectation by > 1 %)
"""
import argparse
import struct
import sys
import wave


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--threshold", type=int, default=64)
    ap.add_argument("--merge-ms", type=float, default=5.0)
    ap.add_argument("--expect-ms", type=float, default=0.0)
    a = ap.parse_args()

    w = wave.open(a.path, "rb")
    ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    if sw != 2:
        sys.exit("only 16-bit PCM is supported (got %d-byte samples)" % sw)
    raw = w.readframes(n)
    s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    L = s[0::ch]
    R = s[1::ch] if ch > 1 else L
    print("%s: %d Hz, %d ch, %d frames = %.1f ms" % (a.path, rate, ch, n, n * 1000.0 / rate))

    loud = [max(abs(l), abs(r)) > a.threshold for l, r in zip(L, R)]
    merge = int(rate * a.merge_ms / 1000)
    segs, i = [], 0
    while i < n:
        if not loud[i]:
            i += 1
            continue
        start, last = i, i
        j = i
        while j < n and j - last <= merge:
            if loud[j]:
                last = j
            j += 1
        segs.append((start, last + 1))
        i = last + 1

    for k, (b, e) in enumerate(segs):
        seg_l, seg_r = L[b:e], R[b:e]
        peak = max(max(abs(x) for x in seg_l), max(abs(x) for x in seg_r))
        zc = sum(1 for p, q in zip(seg_l, seg_l[1:]) if (p < 0) != (q < 0))
        freq = zc / 2.0 / ((e - b) / rate) if e > b else 0.0
        same = all(x == y for x, y in zip(seg_l, seg_r))
        run = best = 0
        for x, y in zip(seg_l, seg_r):
            run = run + 1 if (x == 0 and y == 0) else 0
            best = max(best, run)
        print("  seg %d: start %.1f ms, length %.1f ms, peak %d, %.1f Hz, L==R %s, "
              "longest internal silence %d samples (%.1f ms)"
              % (k, b * 1000.0 / rate, (e - b) * 1000.0 / rate, peak, freq,
                 "yes" if same else "NO", best, best * 1000.0 / rate))
    if len(segs) > 1:
        gaps = [(segs[k + 1][0] - segs[k][0]) * 1000.0 / rate for k in range(len(segs) - 1)]
        print("  spacing between starts (ms): " + ", ".join("%.1f" % g for g in gaps))
    if not segs:
        print("  (silence)")

    if a.expect_ms:
        ok = len(segs) == 1 and abs((segs[0][1] - segs[0][0]) * 1000.0 / rate - a.expect_ms) <= a.expect_ms * 0.01
        print("VERDICT: %s" % ("PASS" if ok else "FAIL"))
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
