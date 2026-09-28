#!/usr/bin/env python3
"""gui-coupling.py — how many COMPOSITOR-CORE functions each GUI file calls.

§M81's metric, made a script so the table can be re-measured instead of
re-counted by hand.  The compositor core is gui.c, wm.c, compose.c, input.c;
for every other file under kernel/gui (and kernel/gui/apps) the number is how
many DISTINCT non-static functions defined in those four files it names.

VALIDATED against a known answer before it is believed: §M70 found that
`gterm.c` reaches the compositor through exactly two calls (gui_damage_win,
gui_window_raise).  The script prints that check first, and exits non-zero if
it does not reproduce it — a metric that cannot reproduce a known answer is
measuring something else (§M81's own rule).

Usage:  scripts/gui-coupling.py [--names]
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = ["kernel/gui/gui.c", "kernel/gui/wm.c", "kernel/gui/compose.c", "kernel/gui/input.c"]


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    s = re.sub(r'//[^\n]*', ' ', s)
    s = re.sub(r'"(?:[^"\\]|\\.)*"', '""', s)
    return s


def defined_functions(path):
    """Non-static functions defined at file scope."""
    s = strip_comments(open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read())
    names = set()
    for m in re.finditer(r'^(?!static\b)[A-Za-z_][\w \*]*?\b([a-z_]\w*)\s*\([^;{]*\)\s*\{', s, re.M):
        name = m.group(1)
        if name not in ("if", "for", "while", "switch", "return", "sizeof"):
            names.add(name)
    return names


def declared_in(path):
    """Function names a header declares."""
    s = strip_comments(open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read())
    return set(re.findall(r'\b([a-z_]\w*)\s*\([^;{)]*\)\s*;', s)) | \
           set(re.findall(r'\b([a-z_]\w*)\s*\([^;{]*\)\s*;', s))


def tier(name, pub, shell, priv):
    """Which audience a core function was declared for (§M70's three headers)."""
    if name in pub:   return "pub"
    if name in shell: return "shell"
    if name in priv:  return "PRIV"
    return "undeclared"


def main():
    pub   = declared_in("kernel/includes/gui.h") | declared_in("kernel/includes/gui_app.h")
    shell = declared_in("kernel/gui/gui_internal.h")
    priv  = declared_in("kernel/gui/gui_priv.h")
    core_syms = set()
    for c in CORE:
        core_syms |= defined_functions(c)
    files = sorted(glob.glob(os.path.join(ROOT, "kernel/gui/*.c")) +
                   glob.glob(os.path.join(ROOT, "kernel/gui/apps/*.c")))
    rows = []
    for f in files:
        rel = os.path.relpath(f, ROOT)
        if rel in CORE:
            continue
        s = strip_comments(open(f, encoding="utf-8", errors="replace").read())
        # A name the file defines ITSELF (a local static helper that happens
        # to share a core function's name — widget.c's str_copy) is not a call
        # across the seam.
        own = set(re.findall(r'^static\b[^;{(]*?\b([a-z_]\w*)\s*\([^;{]*\)\s*\{', s, re.M))
        used = sorted(n for n in core_syms
                      if n not in own and re.search(r'\b%s\s*\(' % re.escape(n), s))
        rows.append((len(used), rel, used))

    check = next((r for r in rows if r[1].endswith("gterm.c")), None)
    ok = check is not None and check[0] == 2
    print("validation: gterm.c calls %s core function(s) %s -> %s"
          % (check[0] if check else "?", check[2] if check else "",
             "OK (reproduces §M70)" if ok else "FAILED — do not trust this table"))
    bands = [(0, 3, "component"), (4, 8, "client of the toolkit surface"),
             (9, 999, "entangled")]
    for lo, hi, what in bands:
        sel = [r for r in rows if lo <= r[0] <= hi]
        print("\n%d-%s calls — %s (%d files)" % (lo, hi if hi < 999 else "", what, len(sel)))
        for n, rel, used in sorted(sel, key=lambda r: (-r[0], r[1])):
            t = [tier(u, pub, shell, priv) for u in used]
            split = "pub %d / shell %d / PRIV %d%s" % (
                t.count("pub"), t.count("shell"), t.count("PRIV"),
                (" / undeclared %d" % t.count("undeclared")) if "undeclared" in t else "")
            extra = ""
            if "--names" in sys.argv:
                extra = "  " + ", ".join("%s[%s]" % (u, tt) for u, tt in zip(used, t))
            print("  %3d  %-34s %s%s" % (n, rel, split, extra))
    # The seam that matters: who reaches PRIVATE or UNDECLARED symbols.
    print("\nreaching past the public/shell headers:")
    for n, rel, used in rows:
        bad = [u for u in used if tier(u, pub, shell, priv) in ("PRIV", "undeclared")]
        if bad:
            print("  %-34s %s" % (rel, ", ".join(bad)))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
