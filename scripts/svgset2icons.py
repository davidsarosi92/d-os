#!/usr/bin/env python3
"""svgset2icons.py — the icon set's HTML reference into one C vpath table.

WHY THE SOURCE IS AN HTML FILE.  The icon set arrived from Claude Design as a
browser prototype (design/icons/project/), which is the design medium, not a
delivery format.  Rather than hand-copying 24 drawings into C — 24 chances to
transpose a coordinate, with no way to tell — this reads the prototype's own
<svg> elements.  The design file stays the source of truth and can be re-run
when it changes.

WHY VECTOR AND NOT MASKS.  A 1-bit mask is a raster: it is sharp at the size it
was generated for and nowhere else, which is the same defect as a PNG with
extra steps.  §M63's Appearance page offers 24/32/48 px and a person can ask
for a fourth; `vpath.c` renders any of them from the same points, anti-aliased.

THE LABEL IS THE JOIN, AND IT IS EXPLICIT ON PURPOSE.  Each icon is matched to
an `enum icon_id` by the Hungarian caption next to it in the prototype.  That
table is written out below rather than inferred, because a positional match
(the 4th <svg> is ICON_FOLDER) silently re-points every icon the moment somebody
inserts one in the design — and the failure would be a wrong picture, not an
error.  An unmapped caption is REPORTED, never dropped.

Usage:  scripts/svgset2icons.py <in.html> <out.c>
"""

import importlib.util
import os
import re
import sys

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "svg2paths", os.path.join(_here, "svg2paths.py"))
svg2paths = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(svg2paths)

# Caption in the design → enum icon_id.  Captions absent here are reported at
# the end; ids absent from the design keep whatever icons.c draws for them.
LABEL_TO_ID = {
    "Vezérlőpult":        "ICON_SETTINGS",
    "Kijelző":            "ICON_DISPLAY",
    "Eszközkezelő":       "ICON_CHIP",
    "Fájlkezelő":         "ICON_FOLDER",
    "Hálózat":            "ICON_GLOBE",
    "Tűzfal":             "ICON_FIREWALL",
    "Felhasználók":       "ICON_USERS",
    "Tárhely":            "ICON_STORAGE",
    "Nyomtatók":          "ICON_PRINTER",
    "Bluetooth":          "ICON_BLUETOOTH",
    "Hangbeállítások":    "ICON_VOLUME",
    "Rendszerinfó":       "ICON_INFO",
    "Hibajelentő":        "ICON_WARN",
    "Feladatkezelő":      "ICON_CHART",
    "Alkalmazások":       "ICON_APP",
    "Frissítések":        "ICON_UPDATE",
    "Energiagazd.":       "ICON_POWER",
    "Billentyűzet":       "ICON_KEYBOARD",
    "Egér":               "ICON_MOUSE",
    "Dátum és idő":       "ICON_CLOCK",
    "Nyelv és régió":     "ICON_LOCALE",
    "Kisegítő lehet.":    "ICON_ACCESS",
    "Biztonsági mentés":  "ICON_BACKUP",
    "Távoli asztal":      "ICON_REMOTE",
    # Added in the second drop, which closed the gap this script's own report
    # named: these five ids previously had no vector artwork and fell back to
    # the primitive drawings in icons.c.
    "Dokumentum":         "ICON_DOC",
    "Terminál":           "ICON_TERMINAL",
    "Ecset":              "ICON_BRUSH",
    "Csomag":             "ICON_PACKAGE",
    "Kód":                "ICON_CODE",
    # 2026-09-28 — the Memory page (swap, page cache, the reserve).  Drawn in
    # the set's own language: a module with three chips and its pins.
    "Memória":            "ICON_MEMORY",
    # 2026-09-28 — §M88: the Monitors page (more than one screen).
    "Monitorok":          "ICON_MONITORS",
}

S = 16384          # the fixed-point box vpath.c expects


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    html = open(src).read()

    icons, unmapped = [], []
    for m in re.finditer(r'<svg\b.*?</svg>', html, re.S):
        svg = m.group(0)
        after = html[m.end():m.end() + 240]
        caption = next((t.strip() for t in re.findall(r'>([^<>]{2,30})<', after)
                        if t.strip()), None)
        if caption not in LABEL_TO_ID:
            unmapped.append(caption)
            continue

        vb = re.search(r'viewBox="([\d.\-\s]+)"', svg)
        if not vb:
            raise SystemExit("svgset2icons: icon %r has no viewBox" % caption)
        vx, vy, vw, vh = (float(v) for v in vb.group(1).split())

        polys = svg2paths.svg_polygons(svg, (vx, vy, vw, vh))
        if not polys:
            raise SystemExit("svgset2icons: icon %r produced no geometry — a "
                             "silently empty icon is worse than a build error"
                             % caption)

        # THE BOX IS SQUARE AND THE ART MUST NOT BE STRETCHED INTO IT.
        # vpath_fill maps 0..scale onto a square of `size`, so normalising x by
        # the viewBox width and y by its height would distort any icon whose
        # viewBox is not square.  One divisor, and the shorter axis is centred.
        span = max(vw, vh)
        ox, oy = vx - (span - vw) / 2.0, vy - (span - vh) / 2.0

        def fx(v, o):
            return max(0, min(S, int(round((v - o) / span * S))))

        pts, offs = [], []
        for p in polys:
            offs.append(len(pts))
            for (x, y) in p:
                pts.append((fx(x, ox), fx(y, oy)))
        offs.append(len(pts))
        icons.append((LABEL_TO_ID[caption], caption, pts, offs))

    if not icons:
        raise SystemExit("svgset2icons: no captioned <svg> found in %s" % src)

    with open(out, 'w') as f:
        f.write("/* GENERATED by scripts/svgset2icons.py from\n"
                " * %s — do not edit.\n"
                " *\n"
                " * The Console Plate icon set as filled polygons.  Strokes were\n"
                " * converted to outlines at build time (see svg2paths.py); the\n"
                " * kernel only ever fills, and renders these at whatever size\n"
                " * gui.icon_size asks for.\n"
                " */\n#include \"vpath.h\"\n#include \"icons.h\"\n"
                "#include <stddef.h>\n\n" % src)

        for sym, caption, pts, offs in icons:
            low = sym.lower()
            f.write("/* %s */\n" % caption)
            f.write("static const int16_t %s_pts[][2] = {\n" % low)
            for i in range(0, len(pts), 6):
                f.write("    " + " ".join("{%d,%d}," % p
                                          for p in pts[i:i + 6]) + "\n")
            f.write("};\n")
            f.write("static const uint16_t %s_offs[] = {\n    " % low)
            f.write(", ".join(str(o) for o in offs))
            f.write("\n};\n")
            f.write("static const struct vpath %s_vp = {\n"
                    "    .pts = %s_pts, .offs = %s_offs,\n"
                    "    .npoly = %d, .npts = %d, .scale = %d\n};\n\n"
                    % (low, low, low, len(offs) - 1, len(pts), S))

        # INDEXED BY icon_id, NOT A LIST TO SEARCH.  A lookup that scanned for
        # a matching id would put a loop on every icon draw, and the desktop
        # draws a lot of them; a sparse array costs one load.
        f.write("/* Indexed by enum icon_id.  NULL = this id has no vector\n"
                " * artwork in the design set, and icons.c keeps drawing it\n"
                " * from primitives — which is why the fallback stays. */\n")
        f.write("const struct vpath* const icon_vpaths[ICON__COUNT] = {\n")
        for sym, _c, _p, _o in icons:
            f.write("    [%s] = &%s_vp,\n" % (sym, sym.lower()))
        f.write("};\n")

    total = sum(len(p) for _s, _c, p, _o in icons)
    print("svgset2icons: %s → %s  (%d icons, %d points, %.1f KB)"
          % (src, out, len(icons), total, total * 4 / 1024.0))
    if unmapped:
        print("svgset2icons: %d caption(s) NOT in LABEL_TO_ID and therefore "
              "skipped: %s" % (len(unmapped), ", ".join(map(str, unmapped))))


if __name__ == "__main__":
    main()
