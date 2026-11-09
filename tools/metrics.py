#!/usr/bin/env python3
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Code metrics of src/, checked against the limits of tools/quality_limits.json.

    tools/metrics.py                 table of the metrics, exit 1 over a limit
    tools/metrics.py --json FILE     also write them as JSON (CI artifact)
    tools/metrics.py --top N         list the N longest functions (default 10)

Measured on the C sources of src/ (vendored cJSON excluded):
  * function length: lines from the first line of the signature to the
    closing brace, for every function definition (clang-format style: the
    body opens at the end of the signature, the closing brace is in
    column 0);
  * file length in lines;
  * line coverage of src/server + src/shared, when tools/quality.sh
    coverage has written build-quality/coverage/summary.json.

The limits are a ratchet: a change may lower them, never raise them.
"""

import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIMITS = os.path.join(ROOT, "tools", "quality_limits.json")
COVERAGE = os.path.join(ROOT, "build-quality", "coverage", "summary.json")
VENDORED = re.compile(r"^src/shared/cJSON\.[ch]$")

# A definition starts in column 0 with a type or a qualifier and opens its
# body at the end of the signature, which may wrap over several lines.
START_RE = re.compile(r"^(?!(?:if|for|while|switch|return|else|do|typedef|struct|enum|union)\b)"
                      r"[A-Za-z_][^;=]*\b([A-Za-z_]\w*)\s*\(")
SIGNATURE_MAX_LINES = 8


def sources():
    try:
        out = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard",
                              "src/*.c", "src/*.h"],
                             cwd=ROOT, check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError):
        out = ""
    files = sorted({f for f in out.split() if not VENDORED.match(f)})
    if not files:  # not a git checkout (release tarball): walk the tree
        for base, _, names in os.walk(os.path.join(ROOT, "src")):
            for n in names:
                rel = os.path.relpath(os.path.join(base, n), ROOT)
                if n.endswith((".c", ".h")) and not VENDORED.match(rel):
                    files.append(rel)
        files.sort()
    return files


def functions(lines):
    """(name, first line, length) of every function defined in the file."""
    found = []
    i = 0
    while i < len(lines):
        m = START_RE.match(lines[i])
        if not m:
            i += 1
            continue
        k = i
        while k < len(lines) and k - i < SIGNATURE_MAX_LINES:
            text = lines[k].rstrip()
            if ";" in text:
                k = -1
                break
            if text.endswith("{"):
                break
            k += 1
        if k < 0 or k >= len(lines) or k - i >= SIGNATURE_MAX_LINES:
            i += 1
            continue
        j = k + 1
        while j < len(lines) and not lines[j].startswith("}"):
            j += 1
        found.append((m.group(1), i + 1, j - i + 1))
        i = j + 1
    return found


def measure():
    funcs, files = [], []
    for rel in sources():
        with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        files.append((rel, len(lines)))
        for name, line, length in functions(lines):
            funcs.append((rel, name, line, length))
    m = {
        "files": len(files),
        "functions": len(funcs),
        "function_lines_max": max((f[3] for f in funcs), default=0),
        "functions_over_120": sum(1 for f in funcs if f[3] > 120),
        "functions_over_60": sum(1 for f in funcs if f[3] > 60),
        "file_lines_max": max((f[1] for f in files), default=0),
        "files_over_1000": sum(1 for f in files if f[1] > 1000),
        "coverage_line_percent": None,
    }
    if os.path.exists(COVERAGE):
        with open(COVERAGE, encoding="utf-8") as f:
            m["coverage_line_percent"] = json.load(f)["line_percent"]
    return m, sorted(funcs, key=lambda f: -f[3]), sorted(files, key=lambda f: -f[1])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--json", metavar="FILE", help="write the metrics as JSON")
    ap.add_argument("--top", type=int, default=10, metavar="N",
                    help="list the N longest functions and files")
    args = ap.parse_args()

    with open(LIMITS, encoding="utf-8") as f:
        limits = json.load(f)
    m, funcs, files = measure()

    print("%-28s %10s %10s" % ("metric", "value", "limit"))
    failed = []
    for key, value in m.items():
        limit = limits.get(key)
        shown = "-" if value is None else (("%.2f" % value) if isinstance(value, float) else value)
        print("%-28s %10s %10s" % (key, shown, "-" if limit is None else limit))
        if limit is None or value is None:
            continue
        over = value < limit if key.startswith("coverage") else value > limit
        if over:
            failed.append("%s = %s (limit %s)" % (key, shown, limit))
    if args.top:
        print("\nlongest functions:")
        for rel, name, line, length in funcs[:args.top]:
            print("  %5d  %s:%d %s()" % (length, rel, line, name))
        print("\nlongest files:")
        for rel, n in files[:args.top]:
            print("  %5d  %s" % (n, rel))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({"metrics": m, "limits": limits}, f, indent=2)
            f.write("\n")
    if failed:
        print("\nmetrics: over the limits of tools/quality_limits.json:")
        for line in failed:
            print("  " + line)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
