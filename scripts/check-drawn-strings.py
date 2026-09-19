#!/usr/bin/env python3
"""check-drawn-strings.py — no UTF-8 in a string this kernel DRAWS (§4.66).

THE TRAP, AND WHY IT NEEDS A SCRIPT.

The framebuffer font is indexed BY BYTE and its upper half is ISO-8859-2
(§4.66: Latin-1 has no ő or ű, so Latin-2 was forced).  A UTF-8 character in a
drawn string is therefore two or three bytes that become two or three WRONG
GLYPHS — an em-dash renders as `â€"`, and in the accounts panel it came out as
*"david á uid 1000"* and was reported from use.

CLAUDE.md already records this trap three times: a literal `á` in a catalogue
entry, a Hungarian string added WHILE FIXING that one, and a middle dot that is
a caron in Latin-2.  Each time the answer was "grep for bytes >= 0x80", and each
time the grep was run once and not again.  *A rule stated in a header is not a
rule the compiler checks* — so this is the check, and it belongs in any run that
touches the GUI.

WHAT IT DELIBERATELY DOES NOT FLAG:

  comments      they are read, not drawn.  This whole tree is commented in
                English prose with em-dashes in it, and forbidding those would
                be forbidding the house style to protect the font.
  kprintf/klog  the serial line and the console are not the framebuffer font;
                a terminal renders UTF-8 perfectly well.
  \\xNN escapes  that is the CORRECT way to write a Hungarian string here, and
                it is what locale.c's catalogue uses throughout.

So what is left is exactly the dangerous case: a literal with a raw high byte,
in GUI code, on a line that is not a log call.
"""

import os
import re
import sys

ROOTS = ["kernel/gui"]
LOGGERS = ("kprintf", "klog", "console_write", "serial")


def strip_comments(src):
    """Blank out comments, preserving offsets so line numbers stay true."""
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(c if c == "\n" else " " for c in src[i:j]))
            i = j
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif src[i] == '"':
            j, esc = i + 1, False
            while j < n and (esc or src[j] != '"'):
                esc = (src[j] == "\\") and not esc
                j += 1
            out.append(src[i:j + 1])
            i = j + 1
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def logger_spans(src):
    """Byte ranges covered by a call to a logging function.

    Spans rather than LINES, because a `kprintf` argument list wraps — and the
    first version of this check matched the logger name on the same line only,
    so every continuation line of a multi-line log call came back as a false
    positive.  *A check that cries wolf on its own tree is one nobody runs.*
    """
    spans = []
    for m in re.finditer(r"\b(%s)\s*\(" % "|".join(LOGGERS), src):
        i, depth = m.end() - 1, 0
        while i < len(src):
            if src[i] == "(":
                depth += 1
            elif src[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        spans.append((m.start(), i))
    return spans


def scan():
    bad = []
    for root in ROOTS:
        for dirpath, _dirs, files in os.walk(root):
            for f in files:
                if not f.endswith((".c", ".h")):
                    continue
                p = os.path.join(dirpath, f)
                src = open(p, encoding="utf-8", errors="replace").read()
                code = strip_comments(src)
                spans = logger_spans(code)
                for m in re.finditer(r'"(?:[^"\\\n]|\\.)*"', code):
                    lit = m.group(0)
                    if not any(ord(c) > 127 for c in lit):
                        continue
                    if any(s <= m.start() < e for s, e in spans):
                        continue            # goes to the serial line, not the font
                    line = code.count("\n", 0, m.start()) + 1
                    bad.append((p, line, lit.strip()))
    return bad


def main():
    bad = scan()
    if not bad:
        print("check-drawn-strings: clean — no raw UTF-8 in a drawn literal")
        return 0
    print("check-drawn-strings: %d drawn literal(s) carry a raw high byte."
          % len(bad))
    print("The framebuffer font is byte-indexed ISO-8859-2, so each one draws")
    print("as two or three wrong glyphs.  Use ASCII, or an \\xNN escape.")
    for p, n, lit in bad:
        print("  %s:%d  %s" % (p, n, lit[:72]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
