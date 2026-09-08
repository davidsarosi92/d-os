#!/usr/bin/env python3
# =============================================================================
# ttf2font.py — turn a TrueType font into kernel glyph outlines.
#
# WHY THIS EXISTS.  The system had exactly one typeface: an 8x8 bitmap, one bit
# per pixel, every glyph the same width.  That is sharp at one size and one size
# only, and §M61 made the resolution a runtime choice — so the same argument
# that made the boot logo vector artwork (§M62 / vpath.c) applies to text, only
# more so, because text is what the screen is mostly made of.
#
# THE SPLIT IS THE SAME ONE vpath.h ARGUES FOR, AND FOR THE SAME REASON.  All
# the parsing lives here, on the host: the table directory, `cmap` format 4,
# composite glyph recursion, quadratic Béziers.  What crosses into ring 0 is a
# flat array of points and one integer per glyph — no parser, so nothing to
# fail on malformed input, because by then there is no input.
#
# THREE DECISIONS WORTH THE READING:
#
#   1. CURVES ARE FLATTENED HERE, NOT IN THE KERNEL.  A quadratic subdivider is
#      forty lines and would have been fine; the reason not to is that the error
#      bound depends on the RENDER SIZE, and the kernel would then have to
#      re-flatten per size.  Flattening once against a stated size ceiling
#      (MAX_PX) makes the trade explicit and reviewable: past that size the
#      curves start to show facets, and the fix is one constant here.
#
#   2. WINDING DIRECTION IS PRESERVED, and the kernel fills NON-ZERO.  vpath.c
#      fills even-odd, which is right for a logo whose contours never overlap.
#      A font's do: an accented glyph is a COMPOSITE, the accent is placed by
#      transform, and nothing guarantees it clears the letter underneath.  Under
#      even-odd an overlap punches a hole; under non-zero it merges.  Using
#      vpath's rule would have worked for most of ASCII and failed on exactly
#      the Hungarian glyphs §4.66 went to the trouble of adding.
#
#   3. THE CHARACTER SET IS ISO-8859-2, indexed by BYTE.  Not a taste: the
#      existing font is `font8x8[256]` with a Latin-2 upper half (§4.66 — ő and
#      ű do not exist in Latin-1), the keyboard layers produce those bytes, and
#      every string in the tree is bytes.  A Unicode cmap here would make this a
#      UTF-8 project, which is a different piece of work with a different blast
#      radius.  So the table this emits is a drop-in for the one it replaces.
#
# Usage:
#   scripts/ttf2font.py <font.ttf> <out.c> <symbol> [--mono]
# =============================================================================
import struct
import sys

# The size ceiling the flattening is computed against — see decision 1.  Text
# rendered larger than this stays smooth to well under a pixel; it is set well
# above any size the UI asks for so that a title on a 4K display is covered.
MAX_PX = 96
# Allowed deviation at that size, in pixels.  A quarter of this would double the
# point count and be invisible.
TOL_PX = 0.08


class Reader:
    """Big-endian cursor over the font file.  TrueType is big-endian throughout,
    including on the little-endian machines it is normally read on — reading a
    field with the wrong endianness yields a plausible small number rather than
    an error, which is why every read goes through here."""

    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from('>H', self.d, self.p)[0]
        self.p += 2
        return v

    def s16(self):
        v = struct.unpack_from('>h', self.d, self.p)[0]
        self.p += 2
        return v

    def u32(self):
        v = struct.unpack_from('>I', self.d, self.p)[0]
        self.p += 4
        return v

    def f2dot14(self):
        """The 2.14 fixed-point format composite glyph transforms use."""
        return self.s16() / 16384.0


def read_tables(data):
    r = Reader(data)
    tag = r.u32()
    if tag == 0x74746366:                       # 'ttcf' — a collection
        raise SystemExit('ttf2font: TrueType collections are not supported')
    num = r.u16()
    r.p += 6
    tables = {}
    for _ in range(num):
        name = data[r.p:r.p + 4].decode('latin-1')
        r.p += 4
        r.u32()                                  # checksum, unused
        off = r.u32()
        length = r.u32()
        tables[name] = (off, length)
    return tables


def parse_cmap(data, off):
    """Unicode codepoint -> glyph index, from a format 4 subtable.

    Format 4 is the one every Latin font ships; format 12 exists for beyond the
    BMP, which nothing in ISO-8859-2 needs.  A font offering neither is refused
    rather than silently producing a table of .notdef — a font that renders as
    256 empty boxes looks like a rasteriser bug, and would be debugged as one."""
    r = Reader(data, off)
    r.u16()                                      # version
    n = r.u16()
    best = None
    for _ in range(n):
        pid = r.u16()
        eid = r.u16()
        sub = r.u32()
        # Windows/Unicode BMP, then Unicode-platform: both are the same map in
        # practice, and preferring one keeps the choice deterministic.
        if (pid, eid) in ((3, 1), (0, 3), (0, 4), (0, 6)):
            best = off + sub
            break
        if pid == 0 and best is None:
            best = off + sub
    if best is None:
        raise SystemExit('ttf2font: no usable Unicode cmap subtable')

    r = Reader(data, best)
    fmt = r.u16()
    if fmt != 4:
        raise SystemExit('ttf2font: cmap format %d is not supported '
                         '(format 4 expected)' % fmt)
    r.u16()                                      # length
    r.u16()                                      # language
    segx2 = r.u16()
    seg = segx2 // 2
    r.p += 6                                     # searchRange/entrySel/rangeShift
    ends = [r.u16() for _ in range(seg)]
    r.u16()                                      # reservedPad
    starts = [r.u16() for _ in range(seg)]
    deltas = [r.s16() for _ in range(seg)]
    range_off_pos = r.p
    range_offs = [r.u16() for _ in range(seg)]

    def lookup(cp):
        for i in range(seg):
            if cp > ends[i]:
                continue
            if cp < starts[i]:
                return 0
            if range_offs[i] == 0:
                return (cp + deltas[i]) & 0xFFFF
            # The famous indirection: the offset is measured from the ADDRESS OF
            # THE OFFSET ITSELF, not from the start of anything.  Computing it
            # from the subtable base is the classic way to read a cmap that
            # works for the first segment and drifts after.
            addr = range_off_pos + i * 2 + range_offs[i] + (cp - starts[i]) * 2
            if addr + 1 >= len(data):
                return 0
            g = struct.unpack_from('>H', data, addr)[0]
            return 0 if g == 0 else (g + deltas[i]) & 0xFFFF
        return 0

    return lookup


def glyph_contours(data, glyf_off, loca, gid, depth=0):
    """Contours of one glyph, as lists of (x, y, on_curve) in font units.

    Composites are resolved by recursion, which is what accented glyphs are made
    of here.  The depth cap is not defensive dressing: a corrupt font can make a
    composite reference itself, and the failure mode without a cap is a host
    script that hangs rather than one that says why."""
    if depth > 5 or gid + 1 >= len(loca):
        return []
    start, end = loca[gid], loca[gid + 1]
    if start >= end:
        return []                                # an empty glyph, e.g. space

    r = Reader(data, glyf_off + start)
    ncont = r.s16()
    r.p += 8                                     # xMin/yMin/xMax/yMax

    if ncont < 0:
        # --- composite ---------------------------------------------------
        out = []
        while True:
            flags = r.u16()
            idx = r.u16()
            if flags & 0x0001:                   # ARG_1_AND_2_ARE_WORDS
                a1, a2 = r.s16(), r.s16()
            else:
                a1 = struct.unpack_from('>b', data, r.p)[0]
                a2 = struct.unpack_from('>b', data, r.p + 1)[0]
                r.p += 2
            xx = yy = 1.0
            xy = yx = 0.0
            if flags & 0x0008:                   # WE_HAVE_A_SCALE
                xx = yy = r.f2dot14()
            elif flags & 0x0040:                 # X_AND_Y_SCALE
                xx = r.f2dot14()
                yy = r.f2dot14()
            elif flags & 0x0080:                 # TWO_BY_TWO
                xx = r.f2dot14()
                yx = r.f2dot14()
                xy = r.f2dot14()
                yy = r.f2dot14()
            dx, dy = (a1, a2) if (flags & 0x0002) else (0, 0)  # ARGS_ARE_XY

            for c in glyph_contours(data, glyf_off, loca, idx, depth + 1):
                out.append([(int(round(xx * x + xy * y)) + dx,
                             int(round(yx * x + yy * y)) + dy, on)
                            for (x, y, on) in c])
            if not (flags & 0x0020):             # MORE_COMPONENTS
                break
        return out

    # --- simple glyph -----------------------------------------------------
    ends = [r.u16() for _ in range(ncont)]
    npts = (ends[-1] + 1) if ends else 0
    ilen = r.u16()
    r.p += ilen                                  # hinting bytecode, ignored

    flags = []
    while len(flags) < npts:
        f = r.u8()
        flags.append(f)
        if f & 0x08:                             # REPEAT
            n = r.u8()
            flags.extend([f] * n)
    flags = flags[:npts]

    xs, v = [], 0
    for f in flags:
        if f & 0x02:
            d = r.u8()
            v += d if (f & 0x10) else -d
        elif not (f & 0x10):
            v += r.s16()
        xs.append(v)
    ys, v = [], 0
    for f in flags:
        if f & 0x04:
            d = r.u8()
            v += d if (f & 0x20) else -d
        elif not (f & 0x20):
            v += r.s16()
        ys.append(v)

    contours, s = [], 0
    for e in ends:
        pts = [(xs[i], ys[i], bool(flags[i] & 0x01)) for i in range(s, e + 1)]
        if len(pts) >= 2:
            contours.append(pts)
        s = e + 1
    return contours


def flatten(contours, upem):
    """Contours of (x, y, on_curve) -> closed polygons of (x, y).

    TrueType stores quadratics, and two off-curve points in a row imply an
    on-curve point at their midpoint — a compression nobody writes out.  Missing
    that rule does not produce an error; it produces a glyph whose curves bulge
    towards the wrong control point, which looks like a badly drawn font."""
    tol = TOL_PX * upem / MAX_PX
    polys = []
    for c in contours:
        # Rotate so the contour starts on-curve; if none is (a fully curved
        # contour such as a dot or an 'o' counter can be), synthesise the start
        # at the midpoint of the first two.
        oncurve = [i for i, p in enumerate(c) if p[2]]
        if oncurve:
            k = oncurve[0]
            c = c[k:] + c[:k]
            start = (c[0][0], c[0][1])
            rest = c[1:]
        else:
            start = ((c[0][0] + c[-1][0]) / 2.0, (c[0][1] + c[-1][1]) / 2.0)
            rest = c[:]

        out = [start]
        cur = start
        ctrl = None
        for (x, y, on) in list(rest) + [(start[0], start[1], True)]:
            if on:
                if ctrl is None:
                    out.append((x, y))
                else:
                    out.extend(quad(cur, ctrl, (x, y), tol)[1:])
                    ctrl = None
                cur = (x, y)
            else:
                if ctrl is not None:
                    mid = ((ctrl[0] + x) / 2.0, (ctrl[1] + y) / 2.0)
                    out.extend(quad(cur, ctrl, mid, tol)[1:])
                    cur = mid
                ctrl = (x, y)
        if ctrl is not None:
            out.extend(quad(cur, ctrl, start, tol)[1:])

        # Integerise, drop consecutive duplicates, and close explicitly: the
        # kernel walks pts[k]..pts[k+1] and needs the last point to equal the
        # first, exactly as vpath's tables do.
        ip = []
        for (x, y) in out:
            p = (int(round(x)), int(round(y)))
            if not ip or p != ip[-1]:
                ip.append(p)
        if len(ip) >= 3:
            if ip[0] != ip[-1]:
                ip.append(ip[0])
            polys.append(ip)
    return polys


def quad(p0, p1, p2, tol):
    """One quadratic, subdivided into enough line segments to stay within `tol`.

    The error of an n-segment chord approximation of a quadratic is
    |P0 - 2*P1 + P2| / (8*n^2), which is exact rather than a heuristic — so the
    segment count is computed, not tuned, and a flat curve costs one segment
    while a tight one costs what it needs."""
    ax = p0[0] - 2 * p1[0] + p2[0]
    ay = p0[1] - 2 * p1[1] + p2[1]
    d = (ax * ax + ay * ay) ** 0.5
    n = 1
    if d > 0 and tol > 0:
        n = int((d / (8.0 * tol)) ** 0.5) + 1
    if n > 32:
        n = 32
    pts = []
    for i in range(n + 1):
        t = i / float(n)
        u = 1.0 - t
        pts.append((u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
                    u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]))
    return pts


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__ or 'usage: ttf2font.py <in.ttf> <out.c> <symbol>')
    src, dst, sym = sys.argv[1], sys.argv[2], sys.argv[3]
    mono = '--mono' in sys.argv

    data = open(src, 'rb').read()
    t = read_tables(data)
    for need in ('head', 'hhea', 'hmtx', 'loca', 'glyf', 'cmap', 'maxp'):
        if need not in t:
            raise SystemExit('ttf2font: %s has no %r table — CFF/OpenType '
                             'outlines are not supported, only TrueType glyf'
                             % (src, need))

    r = Reader(data, t['head'][0])
    r.p += 18
    upem = r.u16()
    r.p = t['head'][0] + 50
    loc_fmt = r.s16()

    r = Reader(data, t['maxp'][0] + 4)
    nglyphs_font = r.u16()

    r = Reader(data, t['hhea'][0] + 4)
    ascent = r.s16()
    descent = r.s16()
    line_gap = r.s16()
    r.p = t['hhea'][0] + 34
    n_hmetrics = r.u16()

    hoff = t['hmtx'][0]
    advances = []
    for i in range(n_hmetrics):
        advances.append(struct.unpack_from('>H', data, hoff + i * 4)[0])

    def advance_of(gid):
        # Monospaced and many proportional fonts compress the tail of hmtx: every
        # glyph past numberOfHMetrics repeats the LAST advance.  Reading past the
        # array instead would pick up left-side bearings and give a handful of
        # glyphs wildly wrong widths.
        if gid < len(advances):
            return advances[gid]
        return advances[-1] if advances else 0

    lo, ln = t['loca']
    if loc_fmt == 0:
        loca = [struct.unpack_from('>H', data, lo + i * 2)[0] * 2
                for i in range(min(nglyphs_font + 1, ln // 2))]
    else:
        loca = [struct.unpack_from('>I', data, lo + i * 4)[0]
                for i in range(min(nglyphs_font + 1, ln // 4))]

    cmap = parse_cmap(data, t['cmap'][0])
    glyf_off = t['glyf'][0]

    # --- the ISO-8859-2 character set (decision 3) -------------------------
    codes = []
    for b in range(256):
        if b < 0x20 or 0x7F <= b < 0xA0:
            codes.append(None)                   # control range: no glyph
            continue
        try:
            codes.append(ord(bytes([b]).decode('iso8859-2')))
        except UnicodeDecodeError:
            codes.append(None)

    pts_all = []
    ctr_all = []
    glyphs = []
    byte_to_glyph = [0] * 256
    missing = []

    # Slot 0 is the blank the table falls back to, so an unmapped byte draws
    # nothing rather than indexing off the end.
    glyphs.append((0, 0, 0, 0, 0, 0, 0, 0))

    seen = {}
    for b, cp in enumerate(codes):
        if cp is None:
            continue
        gid = cmap(cp)
        if gid == 0:
            missing.append(b)
            continue
        if gid in seen:
            byte_to_glyph[b] = seen[gid]
            continue

        polys = flatten(glyph_contours(data, glyf_off, loca, gid), upem)
        first_pt = len(pts_all)
        first_ctr = len(ctr_all)
        xs = [p[0] for poly in polys for p in poly]
        ys = [p[1] for poly in polys for p in poly]
        for poly in polys:
            pts_all.extend(poly)
            ctr_all.append(len(pts_all))         # exclusive end, absolute
        glyphs.append((first_pt, first_ctr, len(polys), advance_of(gid),
                       min(xs) if xs else 0, min(ys) if ys else 0,
                       max(xs) if xs else 0, max(ys) if ys else 0))
        seen[gid] = len(glyphs) - 1
        byte_to_glyph[b] = len(glyphs) - 1

    # A space has no contours and a real advance — that is correct and must not
    # be mistaken for a missing glyph, which is why `missing` is only about the
    # cmap lookup failing.
    if missing:
        sys.stderr.write('ttf2font: %s has no glyph for %d byte(s): %s\n'
                         % (src, len(missing),
                            ' '.join('%02x' % m for m in missing)))

    with open(dst, 'w') as f:
        f.write('/* GENERATED by scripts/ttf2font.py from %s — do not edit.\n'
                ' *\n'
                ' * %d glyphs, %d points, %d contours.  Flattened for a %d px\n'
                ' * size ceiling at %.2f px tolerance; past that the curves\n'
                ' * begin to show facets and MAX_PX in the converter is the\n'
                ' * one number to change. */\n'
                % (src.split('/')[-1], len(glyphs) - 1, len(pts_all),
                   len(ctr_all), MAX_PX, TOL_PX))
        f.write('#include "vfont.h"\n\n')

        f.write('static const int16_t %s_pts[][2] = {\n' % sym)
        for i in range(0, len(pts_all), 8):
            f.write('    ' + ' '.join('{%d,%d},' % p for p in pts_all[i:i + 8])
                    + '\n')
        f.write('};\n\n')

        f.write('static const uint16_t %s_ctr[] = {\n' % sym)
        for i in range(0, len(ctr_all), 12):
            f.write('    ' + ' '.join('%d,' % c for c in ctr_all[i:i + 12]) + '\n')
        f.write('};\n\n')

        f.write('static const struct vfont_glyph %s_glyphs[] = {\n' % sym)
        for g in glyphs:
            f.write('    {%d,%d,%d,%d,%d,%d,%d,%d},\n' % g)
        f.write('};\n\n')

        f.write('static const uint16_t %s_map[256] = {\n' % sym)
        for i in range(0, 256, 16):
            f.write('    ' + ' '.join('%d,' % g for g in byte_to_glyph[i:i + 16])
                    + '\n')
        f.write('};\n\n')

        f.write('const struct vfont %s = {\n' % sym)
        f.write('    .pts = %s_pts, .ctr_end = %s_ctr,\n' % (sym, sym))
        f.write('    .glyphs = %s_glyphs, .map = %s_map,\n' % (sym, sym))
        f.write('    .nglyphs = %d,\n' % len(glyphs))
        f.write('    .upem = %d, .ascent = %d, .descent = %d, .line_gap = %d,\n'
                % (upem, ascent, descent, line_gap))
        f.write('    .mono_advance = %d,\n' % (advance_of(cmap(ord('M')))
                                               if mono else 0))
        f.write('};\n')

    sys.stderr.write('%s: %d glyphs, %d pts, %d contours -> %s\n'
                     % (src.split('/')[-1], len(glyphs) - 1, len(pts_all),
                        len(ctr_all), dst))


if __name__ == '__main__':
    main()


# =============================================================================
# --preview — render a sample string on the HOST, with the same fill rule the
# kernel will use.
#
# This exists because of §M56.2's lesson: write the thing that can falsify the
# work BEFORE writing the work.  Without it the first sight of these outlines
# would be a framebuffer, where a wrong winding rule, a mis-parsed composite and
# a broken rasteriser all look identical — "the text is wrong" — and each would
# be debugged through a QEMU boot.  Here a bad glyph is a picture in a second.
# =============================================================================
def preview(src, text, px, out):
    data = open(src, 'rb').read()
    t = read_tables(data)
    r = Reader(data, t['head'][0]); r.p += 18
    upem = r.u16()
    r.p = t['head'][0] + 50
    loc_fmt = r.s16()
    r = Reader(data, t['maxp'][0] + 4); nglyphs = r.u16()
    r = Reader(data, t['hhea'][0] + 4)
    ascent = r.s16(); descent = r.s16(); r.s16()
    r.p = t['hhea'][0] + 34; n_hm = r.u16()
    hoff = t['hmtx'][0]
    adv = [struct.unpack_from('>H', data, hoff + i * 4)[0] for i in range(n_hm)]
    lo, ln = t['loca']
    if loc_fmt == 0:
        loca = [struct.unpack_from('>H', data, lo + i*2)[0]*2 for i in range(min(nglyphs+1, ln//2))]
    else:
        loca = [struct.unpack_from('>I', data, lo + i*4)[0] for i in range(min(nglyphs+1, ln//4))]
    cmap = parse_cmap(data, t['cmap'][0])
    glyf = t['glyf'][0]

    scale = px / float(upem)
    W = int(sum((adv[cmap(ord(c))] if cmap(ord(c)) < len(adv) else adv[-1])
                for c in text) * scale) + 8
    H = int((ascent - descent) * scale) + 8
    base = int(ascent * scale) + 4
    img = [[0.0] * W for _ in range(H)]

    SUB = 5
    penx = 4.0
    for ch in text:
        gid = cmap(ord(ch))
        polys = flatten(glyph_contours(data, glyf, loca, gid), upem)
        edges = []
        for p in polys:
            for k in range(len(p) - 1):
                edges.append((p[k], p[k + 1]))
        for py in range(H):
            for s in range(SUB):
                sy = (py + (s + 0.5) / SUB - base) / -scale   # device y -> font y
                xs = []
                for (a, b) in edges:
                    ay, by = a[1], b[1]
                    if (ay <= sy) == (by <= sy):
                        continue
                    tt = (sy - ay) / float(by - ay)
                    xs.append((penx + (a[0] + (b[0] - a[0]) * tt) * scale,
                               1 if by > ay else -1))
                if not xs:
                    continue
                xs.sort()
                # NON-ZERO: a span is open while the accumulated winding is not
                # zero.  Even-odd would put a hole wherever two contours overlap.
                wind = 0
                start = None
                for (x, d) in xs:
                    if wind == 0:
                        start = x
                    wind += d
                    if wind == 0 and start is not None:
                        # EXACT horizontal coverage: partial pixels at both ends.
                        a0, b0 = start, x
                        i0, i1 = int(a0), int(b0)
                        if i0 == i1:
                            if 0 <= i0 < W:
                                img[py][i0] += (b0 - a0) / SUB
                        else:
                            if 0 <= i0 < W:
                                img[py][i0] += (i0 + 1 - a0) / SUB
                            for i in range(i0 + 1, min(i1, W)):
                                if i >= 0:
                                    img[py][i] += 1.0 / SUB
                            if 0 <= i1 < W:
                                img[py][i1] += (b0 - i1) / SUB
                        start = None
        penx += (adv[gid] if gid < len(adv) else adv[-1]) * scale

    with open(out, 'wb') as f:
        f.write(b'P5\n%d %d\n255\n' % (W, H))
        f.write(bytes(255 - min(255, int(v * 255)) for row in img for v in row))
    sys.stderr.write('preview %dx%d -> %s\n' % (W, H, out))


if '--preview' in sys.argv:
    i = sys.argv.index('--preview')
    preview(sys.argv[1], sys.argv[i + 1], int(sys.argv[i + 2]), sys.argv[i + 3])
    sys.exit(0)
