#!/usr/bin/env python3
"""svg2paths.py — turn an SVG's filled paths into a C polygon table.

WHY THIS EXISTS.  A bitmap logo is sharp at exactly one size: at 1024×768 the
512 px BMP is too big, at 3840×2160 it is a postage stamp, and scaling it with
the kernel's nearest-neighbour sampler looks like scaling it with the kernel's
nearest-neighbour sampler.  The boot screen has to be right at EVERY mode
(§M61 made the mode a runtime choice), so the logo has to be resolution
INDEPENDENT.

WHY NOT AN SVG PARSER IN THE KERNEL.  Ring 0 would gain an XML parser, a path
grammar, transforms, styles and a CSS cascade — a large attack surface and a
large amount of code for one picture.  Everything that makes SVG *general* is
exactly what a boot logo does not need.

SO THE SPLIT IS: this script (host, build time) does the parsing and the curve
flattening; the kernel keeps a scanline polygon filler (kernel/gui/vpath.c),
which is small, has no parser, cannot fail on malformed input, and renders at
whatever size the screen happens to be.

Supports the subset this logo uses — M/L/Q/Z, absolute — and REFUSES anything
else loudly rather than silently dropping it: a logo that quietly loses a
stroke is worse than a build error.

Usage:  scripts/svg2paths.py <in.svg> <out.c> <symbol>
"""

import re
import sys

# Flattening tolerance, in viewBox units, for quadratic Béziers.  0.5 of a 600
# unit box is ~0.08 % — well below one pixel at any plausible screen size, and
# the cost is only table size (measured in the output).
TOL = 0.5


def parse_path(d):
    """Return a list of closed subpaths, each a list of (x, y) points."""
    toks = re.findall(r'[MLQZ]|-?\d*\.?\d+(?:[eE][-+]?\d+)?', d)
    subpaths, cur, pos, start = [], [], (0.0, 0.0), (0.0, 0.0)
    i = 0
    while i < len(toks):
        t = toks[i]
        if t == 'M':
            if len(cur) > 2:
                subpaths.append(cur)
            pos = (float(toks[i + 1]), float(toks[i + 2]))
            start, cur, i = pos, [pos], i + 3
        elif t == 'L':
            pos = (float(toks[i + 1]), float(toks[i + 2]))
            cur.append(pos)
            i += 3
        elif t == 'Q':
            cx, cy = float(toks[i + 1]), float(toks[i + 2])
            x1, y1 = float(toks[i + 3]), float(toks[i + 4])
            x0, y0 = pos
            # Segment count from the control polygon's deviation — flat curves
            # get few segments, tight ones get many, and neither is guessed.
            dev = max(abs(cx - (x0 + x1) / 2), abs(cy - (y0 + y1) / 2))
            n = max(2, min(48, int((dev / TOL) ** 0.5 * 4)))
            for k in range(1, n + 1):
                t2 = k / n
                mt = 1 - t2
                cur.append((mt * mt * x0 + 2 * mt * t2 * cx + t2 * t2 * x1,
                            mt * mt * y0 + 2 * mt * t2 * cy + t2 * t2 * y1))
            pos = (x1, y1)
            i += 5
        elif t == 'Z':
            if len(cur) > 2:
                cur.append(start)
                subpaths.append(cur)
            cur, pos, i = [], start, i + 1
        else:
            raise SystemExit("svg2paths: unsupported path token %r — this "
                             "converter handles M/L/Q/Z only, and refusing is "
                             "better than dropping part of the artwork" % t)
    if len(cur) > 2:
        subpaths.append(cur)
    return subpaths


def parse_transform(spec):
    """Return (a, b, c, d, e, f) for translate()/scale() chains.

    SVG applies a transform list RIGHT TO LEFT, and this logo's paths carry
    `translate(x,y) scale(1,-1)` — a Y FLIP.  Ignoring it does not fail, it
    produces a mirrored logo in the wrong place, which is exactly the kind of
    silent wrongness a converter must not have: parse it, or refuse."""
    m = [1.0, 0.0, 0.0, 1.0, 0.0, 0.0]      # identity, as (a b c d e f)

    def mul(m1, m2):
        a1, b1, c1, d1, e1, f1 = m1
        a2, b2, c2, d2, e2, f2 = m2
        return [a1 * a2 + c1 * b2,      b1 * a2 + d1 * b2,
                a1 * c2 + c1 * d2,      b1 * c2 + d1 * d2,
                a1 * e2 + c1 * f2 + e1, b1 * e2 + d1 * f2 + f1]

    for name, args in re.findall(r'(\w+)\s*\(([^)]*)\)', spec or ""):
        v = [float(x) for x in re.split(r'[\s,]+', args.strip()) if x]
        if name == 'translate':
            t = [1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0]
        elif name == 'scale':
            t = [v[0], 0, 0, v[1] if len(v) > 1 else v[0], 0, 0]
        elif name == 'matrix' and len(v) == 6:
            t = v
        else:
            raise SystemExit("svg2paths: unsupported transform %r — refusing "
                             "rather than dropping it silently" % name)
        m = mul(m, t)
    return m


def apply_tf(m, x, y):
    a, b, c, d, e, f = m
    return (a * x + c * y + e, b * x + d * y + f)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    src, out, sym = sys.argv[1], sys.argv[2], sys.argv[3]
    svg = open(src).read()

    m = re.search(r'viewBox="([\d.\-\s]+)"', svg)
    if not m:
        raise SystemExit("svg2paths: no viewBox — the coordinate space is not "
                         "knowable without one")
    vx, vy, vw, vh = (float(v) for v in m.group(1).split())

    polys = []
    for tag in re.findall(r'<path\b[^>]*>', svg):
        dm = re.search(r'\sd="([^"]+)"', tag)
        if not dm:
            continue
        tm = re.search(r'\stransform="([^"]+)"', tag)
        mat = parse_transform(tm.group(1) if tm else None)
        for sp in parse_path(dm.group(1)):
            polys.append([apply_tf(mat, x, y) for (x, y) in sp])
    if not polys:
        raise SystemExit("svg2paths: no <path> elements found")

    # Normalise into a 0..16384 fixed-point box: integers keep the kernel free
    # of floating point (which it cannot use without saving FP state), and 14
    # bits is ~0.006 % of the box — far finer than any pixel grid.
    S = 16384

    def fx(v, o, span):
        return max(0, min(S, int(round((v - o) / span * S))))

    pts, offs = [], []
    for p in polys:
        offs.append(len(pts))
        for (x, y) in p:
            pts.append((fx(x, vx, vw), fx(y, vy, vh)))
    offs.append(len(pts))

    with open(out, 'w') as f:
        f.write("/* GENERATED by scripts/svg2paths.py from %s — do not edit.\n"
                " *\n"
                " * Filled polygons in a 0..%d fixed-point box (see the script\n"
                " * for why the flattening happens at build time and not in the\n"
                " * kernel).  Rendered by kernel/gui/vpath.c at any size.\n"
                " */\n#include \"vpath.h\"\n\n" % (src, S))
        f.write("static const int16_t %s_pts[][2] = {\n" % sym)
        for i in range(0, len(pts), 6):
            f.write("    " + " ".join("{%d,%d}," % p for p in pts[i:i + 6]) + "\n")
        f.write("};\n\n")
        f.write("static const uint16_t %s_offs[] = {\n    " % sym)
        f.write(", ".join(str(o) for o in offs))
        f.write("\n};\n\n")
        f.write("const struct vpath %s = {\n"
                "    .pts = %s_pts, .offs = %s_offs,\n"
                "    .npoly = %d, .npts = %d, .scale = %d\n};\n"
                % (sym, sym, sym, len(offs) - 1, len(pts), S))

    print("svg2paths: %s → %s  (%d polygons, %d points, %.1f KB)"
          % (src, out, len(offs) - 1, len(pts), len(pts) * 4 / 1024.0))


if __name__ == "__main__":
    main()
