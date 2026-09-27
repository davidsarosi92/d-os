#!/usr/bin/env python3
"""locale-sweep.py — which user-visible strings have no catalogue entry.

WHY A STATIC SWEEP AS WELL AS `locale missing`.  The runtime recorder sees
only what was DRAWN: a setting scrolled out of its panel's viewport, a dialog
nobody opened, an error message nobody triggered — none of them reach lstr(),
so none of them are reported.  This reads the SOURCE for the places a string
becomes visible and diffs them against the catalogues in kernel/core/locale.c.
The two are complements: the recorder has no false positives about what a
person saw, and the sweep has no blind spots about what a person could see.

What counts as visible (each is a registry or a call the toolkit resolves
through lstr at draw time):
  * CONFIG_KEY  .key (the setting's label), .help, and every word of .values
  * SETTINGS_PANEL .name and .summary
  * GUI_APP(name, …) — the app's name is its catalogue key (locale.h)
  * .title / .text / .body / .info / .ok_text / .cancel_text = "…" in specs
  * lstr("…"), w_label_set(…, "…"), w_label_create(…, "…"), w_button_create(…, "…")

Deliberately NOT reported: numbers (the same in every language), strings with
no letter in them, and identifiers the catalogue policy keeps untranslated
(listed in KEEP below, with the reason next to each).

Usage:  scripts/locale-sweep.py [--lang hu] [--all]
        exit status 1 when something is missing, so it can gate a build.
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Strings that are correct untranslated.  Each needs a reason, or it does not
# belong here.
KEEP = {
    "exfat", "ramfs", "devfs", "procfs",        # filesystem names are names
    "DOS", "WPA2",                               # a partition type, a protocol
    " v",                                        # the dropdown's arrow glyph
    # The Crash Reports list is space-padded MONO text and this is its header:
    # a translation of a different length would misalign every column under it.
    "UPTIME KIND         PID  NAME  PC",
    "d-os", "NetSurf", "BASIC", "Wi-Fi",         # product / protocol names
    "US", "HU", "us", "hu", "de", "en",          # language / keymap codes
    "vista",                                     # a shell's proper name
}


def c_unescape(lit):
    """A C string literal's body (possibly several adjacent pieces) → text."""
    out = bytearray()
    for piece in re.findall(r'"((?:[^"\\]|\\.)*)"', lit):
        i = 0
        while i < len(piece):
            ch = piece[i]
            if ch == '\\' and i + 1 < len(piece):
                n = piece[i + 1]
                if n == 'x':
                    m = re.match(r'[0-9a-fA-F]{1,2}', piece[i + 2:])
                    out.append(int(m.group(0), 16))
                    i += 2 + len(m.group(0))
                    continue
                out += {'n': b'\n', 't': b'\t', '"': b'"', '\\': b'\\'}.get(n, n.encode())
                i += 2
                continue
            out += ch.encode('latin-1', 'replace')
            i += 1
    return out.decode('latin-1')


def catalogues():
    """{lang: set(keys)} from locale.c's tables."""
    src = open(os.path.join(ROOT, "kernel/core/locale.c"), encoding="utf-8").read()
    cats = {}
    # Each table is `static const struct locale_entry <name>[] = { ... };` and
    # is registered with LOCALE_CATALOG(... "<code>" ... <name> ...).
    tables = {}
    for m in re.finditer(r'struct\s+locale_(?:entry|msg)\s+(\w+)\s*\[\]\s*=\s*\{(.*?)\n\};', src, re.S):
        keys = set()
        for e in re.finditer(r'\{\s*((?:"(?:[^"\\]|\\.)*"\s*)+),', m.group(2)):
            keys.add(c_unescape(e.group(1)))
        tables[m.group(1)] = keys
    for m in re.finditer(r'LOCALE_CATALOG\s*\(\s*\w+\s*\)\s*=\s*\{(.*?)\};', src, re.S):
        body = m.group(1)
        code = re.search(r'"(\w\w)"', body)
        tab = next((t for t in tables if re.search(r'\b%s\b' % t, body)), None)
        if code and tab:
            cats.setdefault(code.group(1), set()).update(tables[tab])
    return cats


# Files whose windows are developer instruments, not the product: their text is
# test output and reads the same in every language.
SKIP_FILES = ("kernel/gui/gui_diag.c",)


def visible_strings():
    """[(text, where)] for every place a string becomes visible."""
    found = []
    files = glob.glob(os.path.join(ROOT, "kernel/**/*.c"), recursive=True)
    lit = r'((?:"(?:[^"\\]|\\.)*"\s*)+)'
    for f in files:
        if f.endswith("locale.c"):
            continue
        s = open(f, encoding="utf-8", errors="replace").read()
        rel = os.path.relpath(f, ROOT)
        if rel in SKIP_FILES:
            continue
        def add(text, what):
            line = s.count("\n", 0, what) + 1
            found.append((text, "%s:%d" % (rel, line)))
        for m in re.finditer(r'CONFIG_KEY\(\w+\)\s*=\s*\{(.*?)\};', s, re.S):
            b = m.group(1)
            for fld in ("key", "help"):
                v = re.search(r'\.%s\s*=\s*%s' % (fld, lit), b)
                if v:
                    add(c_unescape(v.group(1)), m.start())
            v = re.search(r'\.values\s*=\s*%s' % lit, b)
            if v:
                for w in c_unescape(v.group(1)).split():
                    add(w, m.start())
        for m in re.finditer(r'SETTINGS_PANEL\(\w+\)\s*=\s*\{(.*?)\};', s, re.S):
            for fld in ("name", "summary"):
                v = re.search(r'\.%s\s*=\s*%s' % (fld, lit), m.group(1))
                if v:
                    add(c_unescape(v.group(1)), m.start())
        for m in re.finditer(r'GUI_APP\(\s*%s' % lit, s):
            add(c_unescape(m.group(1)), m.start())
        for m in re.finditer(r'\.(?:title|text|body|info|ok_text|cancel_text)\s*=\s*%s' % lit, s):
            add(c_unescape(m.group(1)), m.start())
        # lstr takes the literal DIRECTLY; the widget calls take it as a later
        # argument (the first version let `lstr(x), "…"` match across a
        # printf's arguments and reported a log line's "CANCEL").
        for m in re.finditer(r'\blstr\s*\(\s*%s\s*\)' % lit, s):
            add(c_unescape(m.group(1)), m.start())
        for m in re.finditer(r'\b(?:w_label_set|w_label_create|w_button_create)\s*\(([^;()]*?)%s\s*[,)]' % lit, s):
            add(c_unescape(m.group(2)), m.start())
    return found


def interesting(t):
    if not re.search(r'[A-Za-z\xc0-\xff]', t):
        return False                       # numbers, punctuation
    if t in KEEP:
        return False
    if re.fullmatch(r'%\w', t):
        return False
    return True


def main():
    lang = "hu"
    if "--lang" in sys.argv:
        lang = sys.argv[sys.argv.index("--lang") + 1]
    cats = catalogues()
    if lang not in cats or "en" not in cats:
        raise SystemExit("locale-sweep: catalogues found: %s" % sorted(cats))
    seen, missing = set(), []
    items = []
    for text, where in visible_strings():
        # A radio/combo takes its options as ONE space-separated list of keys
        # ("net.mode.dhcp net.mode.static"); each word is looked up alone.
        words = text.split()
        if len(words) > 1 and all("." in w for w in words):
            items += [(w, where) for w in words]
        else:
            items.append((text, where))
    for text, where in items:
        if not interesting(text) or text in seen:
            continue
        seen.add(text)
        if text not in cats[lang]:
            missing.append((where, text))
    missing.sort()
    for where, text in missing:
        print("MISSING[%s] %-40s %r" % (lang, where, text if "--all" in sys.argv else text[:70]))
    print("locale-sweep: %d visible string(s) checked, %d missing from '%s'"
          % (len(seen), len(missing), lang))
    sys.exit(1 if missing else 0)


if __name__ == "__main__":
    main()
