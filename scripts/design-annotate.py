#!/usr/bin/env python3
"""design-annotate.py — stack the design reference and a live d-os screenshot
into ONE annotated picture, with numbered callouts on both halves.

WHY THIS EXISTS.  design-compare.sh already renders the two sides, but two
pictures in two windows is not a comparison — the eye cannot hold a 40 px row
in one and a 44 px row in the other.  Putting them one above the other at the
SAME width, with the same number pointing at the same defect on both sides,
turns "it doesn't quite look like the design" into a list somebody can work
through.

The callouts are DATA at the bottom of this file, so adding a finding is one
line and the picture regenerates.  Coordinates are in each crop's own pixels.
"""
import sys
from PIL import Image, ImageDraw, ImageFont

OUT = "/tmp/dos-design"
W = 1180                                  # common width both halves are scaled to
MARK = (255, 92, 92)                      # callout red — not in either palette
BG = (18, 22, 30)
FG = (222, 230, 240)
DIM = (150, 165, 185)

def font(sz, bold=False):
    p = ("/System/Library/Fonts/Supplemental/Arial Bold.ttf" if bold
         else "/System/Library/Fonts/Supplemental/Arial.ttf")
    try:
        return ImageFont.truetype(p, sz)
    except OSError:
        return ImageFont.load_default()

def fit(im, w):
    """Scale to width `w`, keeping the aspect ratio."""
    return im.resize((w, round(im.height * w / im.width)), Image.LANCZOS)

def callout(dr, x, y, n, r=17):
    dr.ellipse((x - r, y - r, x + r, y + r), fill=MARK, outline=(255, 255, 255), width=2)
    f = font(20, True)
    b = dr.textbbox((0, 0), str(n), font=f)
    dr.text((x - (b[2] - b[0]) / 2, y - (b[3] - b[1]) / 2 - b[1]), str(n),
            font=f, fill=(255, 255, 255))

def build(design_crop, guest_crop, marks_design, marks_guest, notes, out):
    d = fit(Image.open(design_crop).convert("RGB"), W)
    g = fit(Image.open(guest_crop).convert("RGB"), W)

    pad, cap, gap = 24, 34, 18
    note_h = 30 + 26 * len(notes)
    H = pad + cap + d.height + gap + cap + g.height + gap + note_h + pad
    out_im = Image.new("RGB", (W + 2 * pad, H), BG)
    dr = ImageDraw.Draw(out_im)

    y = pad
    dr.text((pad, y), "A DESIGN (Console Plate §06 — folyamattábla)", font=font(21, True), fill=FG)
    y += cap
    out_im.paste(d, (pad, y))
    for (mx, my, n) in marks_design:
        callout(dr, pad + mx, y + my, n)
    y += d.height + gap

    dr.text((pad, y), "A d-os MOST (Task Manager, i386 1920x1200)", font=font(21, True), fill=FG)
    y += cap
    out_im.paste(g, (pad, y))
    for (mx, my, n) in marks_guest:
        callout(dr, pad + mx, y + my, n)
    y += g.height + gap

    dr.line((pad, y, W + pad, y), fill=(60, 72, 90), width=1)
    y += 16
    for i, t in enumerate(notes, 1):
        callout(dr, pad + 14, y + 9, i, r=13)
        dr.text((pad + 36, y - 2), t, font=font(17), fill=DIM)
        y += 26

    out_im.save(out)
    print(out, out_im.size)

if __name__ == "__main__":
    # The five findings of the first pass, all closed by moving the Task Manager
    # onto the table view.  The callouts now mark WHERE each one landed, so the
    # picture is a check rather than a claim.
    build(
        OUT + "/d_table.png",
        OUT + "/tbl2_tm.png",
        [(26, 42, 1), (620, 42, 2), (1128, 42, 3), (1128, 275, 4), (1128, 392, 5)],
        [(30, 130, 1), (700, 130, 2), (600, 130, 3), (30, 200, 4), (1090, 1108, 5)],
        [
            "Oszlopfejlec a HASAB folott: a tabla rajzolja a modell col_title-jebol,"
            " nem egy szokozokkel kitoltott cimke. KESZ.",
            "A NEV aranyos Barlow, csak a szamok Plex Mono (ICOL_MONO). KESZ.",
            "CPU jobbra zart (ICOL_RIGHT), a szeles NEV oszlop kozepen -> a jobb"
            " szel nem ures tobbe. KESZ.",
            "PID accent kekkel (ICOL_ACCENT), STATE tompitva (ICOL_DIM); a kijelolt"
            " soron a sel_fg felulirja, kulonben olvashatatlan lenne. KESZ.",
            "Ketoldali lablec: balra a szamlalo, jobbra az osszeg. KESZ."
            "  MEG NINCS: tabok a tabla folott, es az ures-allapot panel.",
        ],
        OUT + "/compare_table.png",
    )
