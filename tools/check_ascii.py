#!/usr/bin/env python3
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every string literal of the C sources, and the game data, is ASCII.

The client draws its HUD with a 5x7 ASCII font and shows any other byte as
'?': an em dash or a box-drawing character in a message of the server, or
a typographic apostrophe in the description of a spell, became '???' in
the message panel. Comments may use any character.

  check_ascii.py        report what is not ASCII (exit 1)
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ("src/server", "src/shared", "src/client", "include")
DATA = ("data/bestiary.json", "data/items.json", "data/spells.json")

# Comments, string and character literals, in source order.
TOKEN = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def literals(text):
    for m in TOKEN.finditer(text):
        tok = m.group(0)
        if tok[0] in "\"'":
            yield text.count("\n", 0, m.start()) + 1, tok


def main():
    bad = 0
    for d in DIRS:
        for name in sorted(os.listdir(os.path.join(ROOT, d))):
            if not name.endswith((".c", ".h")):
                continue
            rel = os.path.join(d, name)
            with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
                text = f.read()
            for line, lit in literals(text):
                if any(ord(ch) > 126 for ch in lit):
                    print("%s:%d: not ASCII: %s" % (rel, line, lit[:70]))
                    bad += 1
    for rel in DATA:
        with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                odd = sorted({ch for ch in line if ord(ch) > 126})
                if odd:
                    print("%s:%d: not ASCII: %s" % (rel, n, " ".join("U+%04X" % ord(c) for c in odd)))
                    bad += 1
    if bad:
        print("%d place(s) with characters the HUD cannot draw" % bad)
        return 1
    print("ascii: the string literals of %s and the game data are ASCII" % ", ".join(DIRS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
