#!/usr/bin/env python3
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""What happened on a server, from its log.

The server writes one line "[HH:MM:SS] [CATEGORY] text" on standard output
(the journal, under systemd). This reads such a log, from a file or from
standard input, and reports:
  - problems: WARNING and ERROR lines, failed saves, corrupted or
    unreadable saves, protocol violations, TLS failures, refused
    connections, sanitizer reports, a start without a clean shutdown;
  - players: logins, new characters, wrong passwords and lockouts,
    disconnections by reason, deaths by cause, levels gained;
  - the server: starts and shutdowns, autosaves, the last [PERF] line.

    journalctl -u dragongl-server -o cat | python3 tools/analyze_log.py
    python3 tools/analyze_log.py server.log --strict

--strict exits with status 1 when there is a problem (for a cron job or a
CI step); without it the report is all.
"""
import argparse
import collections
import re
import sys

LINE = re.compile(r"^\[(\d\d:\d\d:\d\d)\] \[([A-Z]+)\s*\] (.*)$")
SANITIZER = ("ERROR: AddressSanitizer", "WARNING: ThreadSanitizer", "ERROR: LeakSanitizer",
             "runtime error:")
# (category, pattern, kind of problem): lines that are a problem.
PROBLEMS = (
    (None, re.compile(r"\bWARNING\b"), "warning"),
    (None, re.compile(r"\bERROR\b"), "error"),
    ("SAVE", re.compile(r"^Failed to "), "save failed"),
    ("SAVE", re.compile(r"unreadable|Corrupted/tampered"), "damaged save"),
    ("NET", re.compile(r"^Protocol violation"), "protocol violation"),
    ("NET", re.compile(r"^TLS .* failed|^Cannot start TLS"), "TLS failure"),
    ("NET", re.compile(r"^accept failed|^epoll_ctl"), "network failure"),
    ("AUTH", re.compile(r"^Password hashing failed"), "hashing failed"),
)


def numbers_out(text):
    """The kind of a line: its numbers, names in quotes and addresses
    replaced, so that lines of one kind are counted together."""
    text = re.sub(r"'[^']*'", "'*'", text)
    text = re.sub(r"\b\d+(\.\d+){3}\b", "ADDR", text)
    return re.sub(r"\d+", "N", text)


def analyze(lines):
    r = {
        "categories": collections.Counter(),
        "problems": collections.Counter(),
        "problem_lines": collections.defaultdict(list),
        "sanitizer": [],
        "logins": 0, "created": 0, "players": set(),
        "wrong_password": collections.Counter(), "lockouts": 0, "refused": collections.Counter(),
        "disconnects": collections.Counter(), "deaths": collections.Counter(),
        "levels": 0, "starts": 0, "shutdowns": 0, "autosaves": 0, "perf": None,
        "first": None, "last": None, "unparsed": 0,
    }
    for raw in lines:
        line = raw.rstrip("\n")
        if any(m in line for m in SANITIZER):
            r["sanitizer"].append(line)
            continue
        m = LINE.match(line)
        if not m:
            r["unparsed"] += 1
            continue
        clock, cat, text = m.groups()
        r["first"] = r["first"] or clock
        r["last"] = clock
        r["categories"][cat] += 1
        for pcat, pattern, kind in PROBLEMS:
            if (pcat is None or pcat == cat) and pattern.search(text):
                r["problems"][kind] += 1
                if len(r["problem_lines"][kind]) < 5:
                    r["problem_lines"][kind].append(line)
                break
        note(r, cat, text)
    return r


def note(r, cat, text):
    if cat == "SYS":
        if text.startswith("Starting Dragon GL Server"):
            r["starts"] += 1
        elif text.startswith("Server shut down"):
            r["shutdowns"] += 1
        elif text.startswith("Auto-save"):
            r["autosaves"] += 1
    elif cat == "AUTH":
        m = re.match(r"'(\w+)' (logged in|created)$", text)
        if m:
            r["players"].add(m.group(1))
            r["logins" if m.group(2) == "logged in" else "created"] += 1
        elif text.startswith("Incorrect password for:"):
            r["wrong_password"][text.split(":", 1)[1].strip()] += 1
        elif text.startswith("Too many failed logins"):
            r["lockouts"] += 1
        elif text.startswith("Server access denied"):
            r["refused"]["wrong server password"] += 1
    elif cat == "NET":
        m = re.search(r"disconnected: (.*)$", text)
        if m:
            r["disconnects"][numbers_out(m.group(1))] += 1
        m = re.search(r"refused: (.*)$", text)
        if m:
            r["refused"][numbers_out(m.group(1))] += 1
    elif cat == "DEATH":
        m = re.search(r" was killed by (.*) on floor", text)
        cause = m.group(1) if m else re.sub(r"^\w+ ", "", text).rstrip(".")
        r["deaths"][cause] += 1
    elif cat == "LEVELUP":
        r["levels"] += 1
    elif cat == "PERF":
        r["perf"] = text


def report(r, out):
    w = out.write
    w("log: %s to %s, %d lines by category: %s\n" % (
        r["first"] or "-", r["last"] or "-", sum(r["categories"].values()),
        ", ".join("%s %d" % kv for kv in r["categories"].most_common())))
    w("server: %d start(s), %d clean shutdown(s), %d autosave(s)\n" % (
        r["starts"], r["shutdowns"], r["autosaves"]))
    if r["perf"]:
        w("  last PERF: %s\n" % r["perf"])
    w("players: %d logins, %d new characters, %d different names, %d levels gained\n" % (
        r["logins"], r["created"], len(r["players"]), r["levels"]))
    for title, counter in (("wrong passwords", r["wrong_password"]),
                           ("connections refused", r["refused"]),
                           ("disconnections", r["disconnects"]),
                           ("deaths", r["deaths"])):
        if counter:
            w("  %s: %s\n" % (title, ", ".join("%s %d" % kv for kv in counter.most_common(8))))
    if r["lockouts"]:
        w("  lockouts after failed logins: %d\n" % r["lockouts"])
    problems = sum(r["problems"].values()) + len(r["sanitizer"])
    unclean = r["starts"] > r["shutdowns"] + 1
    if unclean:
        problems += 1
        w("PROBLEM: %d starts but %d clean shutdowns: the server stopped without one\n" % (
            r["starts"], r["shutdowns"]))
    for kind, n in r["problems"].most_common():
        w("PROBLEM: %d %s line(s), e.g.\n" % (n, kind))
        for line in r["problem_lines"][kind]:
            w("    %s\n" % line)
    if r["sanitizer"]:
        w("PROBLEM: %d sanitizer report(s), e.g.\n    %s\n" % (len(r["sanitizer"]),
                                                          r["sanitizer"][0]))
    w("%s\n" % ("no problem found" if problems == 0 else "%d problem(s)" % problems))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", nargs="?", help="the log file (default: standard input)")
    ap.add_argument("--strict", action="store_true", help="exit 1 when there is a problem")
    args = ap.parse_args()
    if args.log:
        with open(args.log, errors="replace") as f:
            r = analyze(f)
    else:
        r = analyze(sys.stdin)
    problems = report(r, sys.stdout)
    return 1 if args.strict and problems else 0


if __name__ == "__main__":
    sys.exit(main())
