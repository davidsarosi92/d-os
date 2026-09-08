#!/usr/bin/env python3
"""svg2paths.py — turn an SVG's shapes into a C polygon table.

WHY THIS EXISTS.  A bitmap logo is sharp at exactly one size: at 1024×768 the
512 px BMP is too big, at 3840×2160 it is a postage stamp, and scaling it with
the kernel's nearest-neighbour sampler looks like scaling it with the kernel's
nearest-neighbour sampler.  The boot screen has to be right at EVERY mode
(§M61 made the mode a runtime choice), so the logo has to be resolution
INDEPENDENT.  The same argument applies, harder, to ICONS: §M63's Appearance
page offers 24/32/48 px, and a raster icon set would need three of everything
and still be wrong at the fourth size somebody asks for.

WHY NOT AN SVG PARSER IN THE KERNEL.  Ring 0 would gain an XML parser, a path
grammar, transforms, styles and a CSS cascade — a large attack surface and a
large amount of code for one picture.  Everything that makes SVG *general* is
exactly what a boot logo does not need.

SO THE SPLIT IS: this script (host, build time) does the parsing, the curve
flattening AND the stroking; the kernel keeps a scanline polygon filler
(kernel/gui/vpath.c), which is small, has no parser, cannot fail on malformed
input, and renders at whatever size the screen happens to be.

-----------------------------------------------------------------------------
STROKING HAPPENS HERE, AND THAT IS THE WHOLE REASON THIS FILE GREW

`vpath.c` FILLS polygons.  It does not stroke, and it should not learn to: a
stroker needs joins, caps, miter limits and a degenerate-segment policy, i.e.
exactly the family of edge cases that turns a small kernel rasteriser into a
source of new failure modes.  Line-art icons, however, are strokes almost
entirely — the set this was extended for is 43 <line>, 16 <circle>, 14 <rect>
and 30 <path>, drawn with stroke-width and no fill at all.

So a stroke is converted to its OUTLINE here, at build time, and the kernel
still only ever sees filled polygons.  The kernel gains nothing to go wrong.

THE FILL RULE CONSTRAINS THE ALGORITHM, and this is the part that is easy to
get wrong: vpath.c fills EVEN-ODD (deliberately — it is what lets the "d" keep
its counter without winding data).  Under even-odd, two overlapping polygons
CANCEL where they overlap.  So a stroke must NOT be emitted as one quad per
segment: consecutive quads overlap at every joint, and the overlap would come
out as a hole at every corner.  Each open polyline therefore becomes ONE closed
outline — down one side, around the cap, back the other side.

Closed shapes are the case where even-odd helps instead: a stroked circle is an
outer ring plus an inner ring, and even-odd renders that as an annulus with no
special handling at all.

-----------------------------------------------------------------------------
WHAT IS REFUSED

Path commands outside M/L/Q/Z, unknown transforms, and unknown stroke-linejoin
/ linecap values are REFUSED LOUDLY.  A converter that silently drops part of
the artwork produces a picture that is wrong in a way nobody can see in a diff
— the failure this project has paid for repeatedly.  Refusing costs a build
error and names the thing it could not handle.

Usage:  scripts/svg2paths.py <in.svg> <out.c> <symbol>
"""

import math
import re
import sys

# Flattening tolerance, in viewBox units, for quadratic Béziers and arcs.  0.5
# of a 600 unit box is ~0.08 % — well below one pixel at any plausible screen
# size, and the cost is only table size (measured in the output).
TOL = 0.5

# SVG's own default when stroke-miterlimit is unset.  A spike longer than this
# times the half-width is cut off square (bevel) instead — without a limit, two
# nearly-parallel segments produce a miter that shoots off to infinity.
MITER_LIMIT = 4.0


def parse_path(d):
    """Return (subpaths, closed_flags) — each subpath a list of (x, y).

    A subpath is kept from TWO points up, not three.  The guard used to be
    `> 2`, which is right for a filled path (two points enclose no area) and
    silently wrong for a stroked one — a straight line IS two points, and line
    art is mostly straight lines.  The accessibility icon lost both legs and an
    arm that way, and nothing errored: the picture was simply missing pieces.
    For a fill a two-point subpath still contributes nothing, so keeping it
    costs a few unused points and closes the hole.

    `closed_flags[i]` records whether subpath i ended with Z.  The distinction
    is invisible for a FILLED path (a fill implies closure) and decisive for a
    STROKED one: an open polyline gets caps, a closed one gets two rings."""
    toks = re.findall(r'[MLQCHVZ]|-?\d*\.?\d+(?:[eE][-+]?\d+)?', d)
    subpaths, closed, cur, pos, start = [], [], [], (0.0, 0.0), (0.0, 0.0)
    i = 0
    while i < len(toks):
        t = toks[i]
        if t == 'M':
            if len(cur) >= 2:
                subpaths.append(cur)
                closed.append(False)
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
        elif t == 'H':
            pos = (float(toks[i + 1]), pos[1])
            cur.append(pos)
            i += 2
        elif t == 'V':
            pos = (pos[0], float(toks[i + 1]))
            cur.append(pos)
            i += 2
        elif t == 'C':
            x1, y1 = float(toks[i + 1]), float(toks[i + 2])
            x2, y2 = float(toks[i + 3]), float(toks[i + 4])
            x3, y3 = float(toks[i + 5]), float(toks[i + 6])
            x0, y0 = pos
            # Same rule as Q: the control points' deviation from the chord sets
            # the segment count, so a nearly-straight curve costs almost
            # nothing and a tight one gets the points it needs.
            dev = max(abs(x1 - x0), abs(y1 - y0), abs(x2 - x3), abs(y2 - y3))
            n = max(2, min(48, int((dev / TOL) ** 0.5 * 4)))
            for k in range(1, n + 1):
                s = k / n
                ms = 1 - s
                cur.append((ms**3 * x0 + 3 * ms*ms * s * x1 +
                            3 * ms * s*s * x2 + s**3 * x3,
                            ms**3 * y0 + 3 * ms*ms * s * y1 +
                            3 * ms * s*s * y2 + s**3 * y3))
            pos = (x3, y3)
            i += 7
        elif t == 'Z':
            if len(cur) >= 2:
                cur.append(start)
                subpaths.append(cur)
                closed.append(True)
            cur, pos, i = [], start, i + 1
        else:
            raise SystemExit("svg2paths: unsupported path token %r — this "
                             "converter handles M/L/Q/Z only, and refusing is "
                             "better than dropping part of the artwork" % t)
    if len(cur) >= 2:
        subpaths.append(cur)
        closed.append(False)
    return subpaths, closed


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


# ---------------------------------------------------------------------------
# Shape elements → point lists
#
# Every shape below reduces to "a list of points, and whether it is closed",
# which is the only vocabulary the stroker and the emitter share.  Keeping the
# reduction here means neither of them has to know what a <rect> is.
# ---------------------------------------------------------------------------

def _num(tag, name, default=None):
    m = re.search(r'\s%s="([^"]*)"' % name, tag)
    if not m:
        if default is None:
            raise SystemExit("svg2paths: <%s> is missing the %r attribute"
                             % (tag[1:tag.find(' ')], name))
        return default
    return float(m.group(1))


def _circle_pts(cx, cy, rx, ry):
    """Flatten an ellipse.  The segment count comes from the sagitta: the
    error of a chord against its arc is r(1-cos(θ/2)), so solving for TOL gives
    the angle a segment may span.  Deriving it beats picking 32 and hoping —
    a 4 px radio dot and a 600 px logo want very different counts."""
    r = max(abs(rx), abs(ry))
    if r <= TOL:
        n = 8
    else:
        n = int(math.ceil(math.pi / math.acos(max(-1.0, 1.0 - TOL / r))))
        n = max(8, min(256, n))
    return [(cx + rx * math.cos(2 * math.pi * k / n),
             cy + ry * math.sin(2 * math.pi * k / n)) for k in range(n)]


def shape_points(name, tag):
    """(list_of_subpaths, list_of_closed_flags) for one shape element."""
    if name == 'path':
        dm = re.search(r'\sd="([^"]+)"', tag)
        if not dm:
            return [], []
        return parse_path(dm.group(1))

    if name == 'line':
        p = [(_num(tag, 'x1', 0.0), _num(tag, 'y1', 0.0)),
             (_num(tag, 'x2', 0.0), _num(tag, 'y2', 0.0))]
        return [p], [False]

    if name == 'rect':
        x, y = _num(tag, 'x', 0.0), _num(tag, 'y', 0.0)
        w, h = _num(tag, 'width'), _num(tag, 'height')
        # rx/ry are IGNORED and that is deliberate at this size: a 2 px corner
        # radius on a 20 px icon grid disappears under the rasteriser, and
        # honouring it would mean four arcs per rect for no visible gain.
        p = [(x, y), (x + w, y), (x + w, y + h), (x, y + h), (x, y)]
        return [p], [True]

    if name in ('circle', 'ellipse'):
        cx, cy = _num(tag, 'cx', 0.0), _num(tag, 'cy', 0.0)
        if name == 'circle':
            rx = ry = _num(tag, 'r')
        else:
            rx, ry = _num(tag, 'rx'), _num(tag, 'ry')
        p = _circle_pts(cx, cy, rx, ry)
        return [p + [p[0]]], [True]

    if name in ('polyline', 'polygon'):
        m = re.search(r'\spoints="([^"]+)"', tag)
        if not m:
            return [], []
        v = [float(x) for x in re.split(r'[\s,]+', m.group(1).strip()) if x]
        p = list(zip(v[0::2], v[1::2]))
        if name == 'polygon' and p and p[0] != p[-1]:
            p = p + [p[0]]
        return [p], [name == 'polygon']

    raise SystemExit("svg2paths: unsupported element <%s>" % name)


# ---------------------------------------------------------------------------
# Stroking
# ---------------------------------------------------------------------------

def _dedup(pts):
    """Drop repeated points.  A zero-length segment has no direction, and every
    piece of the stroker divides by segment length."""
    out = []
    for p in pts:
        if not out or abs(p[0] - out[-1][0]) > 1e-9 or abs(p[1] - out[-1][1]) > 1e-9:
            out.append(p)
    return out


def _offset_side(pts, d, closed):
    """One side of a stroke: each segment shifted by `d` along its left normal,
    with the corners joined.  Returns the offset polyline."""
    n = len(pts)
    segs = []
    for i in range(n - 1):
        (x0, y0), (x1, y1) = pts[i], pts[i + 1]
        dx, dy = x1 - x0, y1 - y0
        L = math.hypot(dx, dy)
        ux, uy = dx / L, dy / L
        nx, ny = -uy * d, ux * d              # left normal, scaled
        segs.append(((x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (ux, uy)))

    if not segs:
        return []

    out = [segs[0][0]]
    for i in range(len(segs) - 1):
        a_end, (u1x, u1y) = segs[i][1], segs[i][2]
        b_start, (u2x, u2y) = segs[i + 1][0], segs[i + 1][2]
        cross = u1x * u2y - u1y * u2x
        if abs(cross) < 1e-9:
            out.append(a_end)                 # collinear — nothing to join
            continue
        # Intersect the two offset lines.  `t` is how far along segment i's
        # offset line the corner sits.
        t = ((b_start[0] - a_end[0]) * u2y - (b_start[1] - a_end[1]) * u2x) / cross
        mx, my = a_end[0] + u1x * t, a_end[1] + u1y * t
        # Miter limit: measured from the ORIGINAL vertex, which is what the
        # limit is defined against.
        vx, vy = pts[i + 1]
        if math.hypot(mx - vx, my - vy) > MITER_LIMIT * abs(d):
            out.append(a_end)                 # bevel
            out.append(b_start)
        else:
            out.append((mx, my))
    out.append(segs[-1][1])

    if closed:
        # Join the last segment back to the first the same way, so the ring has
        # no notch at the seam.
        a_end, (u1x, u1y) = segs[-1][1], segs[-1][2]
        b_start, (u2x, u2y) = segs[0][0], segs[0][2]
        cross = u1x * u2y - u1y * u2x
        if abs(cross) > 1e-9:
            t = ((b_start[0] - a_end[0]) * u2y -
                 (b_start[1] - a_end[1]) * u2x) / cross
            out[0] = out[-1] = (a_end[0] + u1x * t, a_end[1] + u1y * t)
    return out


def _cap(p, u, d, kind):
    """Points bridging the two sides at an end.  `u` points OUT of the line."""
    if kind == 'butt':
        return []
    if kind == 'square':
        ux, uy = u
        nx, ny = -uy * d, ux * d
        return [(p[0] + nx + ux * d, p[1] + ny + uy * d),
                (p[0] - nx + ux * d, p[1] - ny + uy * d)]
    if kind == 'round':
        ux, uy = u
        a0 = math.atan2(ux * d, -uy * d)      # angle of the +normal side
        n = max(4, int(math.ceil(math.pi / math.acos(
            max(-1.0, 1.0 - TOL / max(abs(d), TOL))))) // 2)
        return [(p[0] + abs(d) * math.cos(a0 - math.pi * k / n),
                 p[1] + abs(d) * math.sin(a0 - math.pi * k / n))
                for k in range(1, n)]
    raise SystemExit("svg2paths: unsupported stroke-linecap %r" % kind)


def stroke_outline(pts, width, closed, cap):
    """Return the filled polygons that represent stroking `pts`.

    Open  → ONE closed outline (see the module header: overlapping per-segment
            quads would cancel under even-odd and hole every corner).
    Closed → TWO rings; even-odd renders the pair as an annulus for free."""
    pts = _dedup(pts)
    if len(pts) < 2:
        return []
    d = width / 2.0

    if closed:
        if pts[0] == pts[-1]:
            pts = pts[:-1]
        ring = pts + [pts[0]]
        outer = _offset_side(ring, d, True)
        inner = _offset_side(ring, -d, True)
        out = []
        if len(outer) > 2:
            out.append(outer + [outer[0]])
        if len(inner) > 2:
            out.append(inner + [inner[0]])
        return out

    left = _offset_side(pts, d, False)
    right = _offset_side(pts, -d, False)
    if len(left) < 2 or len(right) < 2:
        return []

    # Direction out of each end, for the caps.
    (ax, ay), (bx, by) = pts[1], pts[0]
    u_start = ((bx - ax) / math.hypot(bx - ax, by - ay),
               (by - ay) / math.hypot(bx - ax, by - ay))
    (ax, ay), (bx, by) = pts[-2], pts[-1]
    u_end = ((bx - ax) / math.hypot(bx - ax, by - ay),
             (by - ay) / math.hypot(bx - ax, by - ay))

    poly = list(left)
    poly += _cap(pts[-1], u_end, d, cap)
    poly += list(reversed(right))
    poly += _cap(pts[0], u_start, d, cap)
    poly.append(poly[0])
    return [poly]


# ---------------------------------------------------------------------------

SHAPES = ('path', 'line', 'rect', 'circle', 'ellipse', 'polyline', 'polygon')


def _attr(tag, name, default=None):
    m = re.search(r'\s%s="([^"]*)"' % name, tag)
    return m.group(1).strip() if m else default


def _covers_viewbox(poly, vb):
    """Does this polygon blanket the whole canvas?

    A vpath is rendered in ONE colour, so a shape covering the entire viewBox
    cannot be artwork: under vpath.c's even-odd rule it INVERTS everything
    inside it, and under any other rule it hides it.  It is a background — the
    logo carries `<rect width="600" height="600" fill="white"/>`, which the
    old <path>-only converter never saw.

    Detected by geometry rather than by colour: "fill is white" is a guess that
    breaks on the first dark-canvas asset, while "covers the canvas" is the
    property that actually makes the shape unusable."""
    vx, vy, vw, vh = vb
    xs = [p[0] for p in poly]
    ys = [p[1] for p in poly]
    return (min(xs) <= vx + vw * 0.01 and max(xs) >= vx + vw * 0.99 and
            min(ys) <= vy + vh * 0.01 and max(ys) >= vy + vh * 0.99)


#: The presentation attributes a shape inherits from its <svg>/<g> ancestors.
INHERITED = ('fill', 'stroke', 'stroke-width', 'stroke-linecap',
             'stroke-linejoin')


def _inherited_attrs(svg):
    """Presentation attributes on the root <svg>, which children inherit.

    THIS IS NOT OPTIONAL AND IT FAILS SILENTLY WITHOUT IT.  Icon sets put the
    style on the container exactly once —

        <svg fill="none" stroke="currentColor" stroke-width="2" ...>
          <line .../><circle .../>

    — so the shapes themselves carry NO stroke attribute.  Reading only each
    shape's own tag makes every one of them look unstroked and (since SVG's
    default fill is black) FILLED: a filled <line> is degenerate and vanishes,
    and a filled <circle> comes out a solid disc where the icon wanted a ring.
    Both are pictures that are wrong without anything having errored."""
    m = re.match(r'<svg\b([^>]*)>', svg.strip())
    if not m:
        return {}
    head = '<svg' + m.group(1) + '>'
    out = {}
    for a in INHERITED:
        v = _attr(head, a)
        if v is not None:
            out[a] = v
    return out


def svg_polygons(svg, vb):
    """Every shape in `svg`, reduced to filled polygons in viewBox units."""
    polys = []
    dropped = 0
    inh = _inherited_attrs(svg)
    pattern = r'<(%s)\b([^>]*)>' % '|'.join(SHAPES)
    for name, attrs in re.findall(pattern, svg):
        tag = '<' + name + attrs + '>'
        subpaths, closed_flags = shape_points(name, tag)
        if not subpaths:
            continue

        # A shape's own attribute wins; otherwise the container's.
        def pres(a, default=None):
            v = _attr(tag, a)
            return v if v is not None else inh.get(a, default)

        mat = parse_transform(_attr(tag, 'transform'))
        stroke = pres('stroke', 'none')
        fill = pres('fill')

        join = pres('stroke-linejoin', 'miter')
        if join not in ('miter', 'bevel', 'round'):
            raise SystemExit("svg2paths: unsupported stroke-linejoin %r" % join)

        # DEFAULTING IS THE ONE PLACE THIS COULD SILENTLY CHANGE THE LOGO.
        # SVG's own default fill is black, and the logo's <path>s carry no
        # fill attribute at all — so "no fill attribute" MUST keep meaning
        # filled.  A stroke is only applied when one is actually asked for.
        filled = (fill or 'black') != 'none'
        stroked = stroke != 'none'

        if stroked:
            w = float(pres("stroke-width", "1"))
            cap = pres("stroke-linecap", "butt")
            for sp, cl in zip(subpaths, closed_flags):
                for poly in stroke_outline(sp, w, cl, cap):
                    polys.append([apply_tf(mat, x, y) for (x, y) in poly])

        if filled:
            for sp in subpaths:
                poly = [apply_tf(mat, x, y) for (x, y) in sp]
                if _covers_viewbox(poly, vb):
                    dropped += 1
                    continue
                polys.append(poly)

    if dropped:
        print("svg2paths: dropped %d background shape(s) covering the whole "
              "viewBox — a vpath is one colour, so a full-canvas fill is not "
              "artwork (see _covers_viewbox)" % dropped)
    return polys


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

    polys = svg_polygons(svg, (vx, vy, vw, vh))
    if not polys:
        raise SystemExit("svg2paths: no drawable shapes found (looked for %s)"
                         % ', '.join('<%s>' % s for s in SHAPES))

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
