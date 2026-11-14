#!/usr/bin/env python3
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Release consistency check (run by CI and before tagging).

The version has one source, the Version: line of dragongl.spec (CMake
reads it from there). This script checks that the rest agrees with it:

  - the newest CHANGELOG.md entry carries that version, and its date is
    the date encoded in the version (YYYY.MM.DD.NN);
  - every CHANGELOG.md header is a valid RPM changelog line
    ("* Www Mmm DD YYYY Name <email> - VERSION") whose weekday matches
    the date (rpmbuild warns "bogus date" otherwise);
  - entries are in reverse chronological order and versions never repeat.

Exit status 0 when everything is consistent, 1 otherwise.

  check_release.py --notes [VERSION]

prints the CHANGELOG.md entry of VERSION (default: the version of
dragongl.spec) as Markdown, for the notes of the release; exit status 1
if there is none.
"""

import datetime
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
HEADER = re.compile(r"^\* (\w{3}) (\w{3}) (\d{2}) (\d{4}) (.+?) <([^>]+)> - (\S+)$")


def spec_version(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^Version:\s*(\S+)\s*$", line)
            if m:
                return m.group(1)
    return None


def notes(version):
    """The entry of `version`, as a Markdown list."""
    out, inside = [], False
    with open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8") as f:
        for line in f:
            m = HEADER.match(line.rstrip("\n"))
            if m:
                if inside:
                    break
                inside = m.group(7) == version
                continue
            if inside:
                out.append(line.rstrip("\n"))
    while out and not out[-1]:
        out.pop()
    if not out:
        print("CHANGELOG.md: no entry for %s" % version, file=sys.stderr)
        return 1
    print("\n".join(out))
    return 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--notes":
        version = sys.argv[2] if len(sys.argv) >= 3 else spec_version(os.path.join(ROOT, "dragongl.spec"))
        return notes(version)
    errors = []
    version = spec_version(os.path.join(ROOT, "dragongl.spec"))
    if not version:
        print("dragongl.spec: no Version: line")
        return 1
    if not re.fullmatch(r"\d{4}\.\d{2}\.\d{2}\.\d{2}", version):
        errors.append("dragongl.spec: Version %s is not YYYY.MM.DD.NN" % version)

    entries = []
    with open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            if not line.startswith("* "):
                continue
            m = HEADER.match(line.rstrip("\n"))
            if not m:
                errors.append("CHANGELOG.md:%d: not an RPM changelog header: %s" % (n, line.strip()))
                continue
            wday, mon, day, year, _name, _mail, ver = m.groups()
            try:
                date = datetime.datetime.strptime("%s %s %s" % (mon, day, year), "%b %d %Y").date()
            except ValueError:
                errors.append("CHANGELOG.md:%d: invalid date" % n)
                continue
            if date.strftime("%a") != wday:
                errors.append("CHANGELOG.md:%d: %s %s %s %s was a %s (bogus date)"
                              % (n, wday, mon, day, year, date.strftime("%a")))
            entries.append((n, date, ver))

    if not entries:
        errors.append("CHANGELOG.md: no entries")
    else:
        n, date, ver = entries[0]
        if ver != version:
            errors.append("CHANGELOG.md:%d: newest entry is %s, dragongl.spec says %s" % (n, ver, version))
        if re.fullmatch(r"\d{4}\.\d{2}\.\d{2}\.\d{2}", ver) and date.strftime("%Y.%m.%d") != ver[:10]:
            errors.append("CHANGELOG.md:%d: entry dated %s but version %s" % (n, date, ver))
    seen = set()
    for (n1, d1, v1), (n2, d2, _v2) in zip(entries, entries[1:]):
        if d2 > d1:
            errors.append("CHANGELOG.md:%d: dated after the newer entry at line %d" % (n2, n1))
    for n, _d, v in entries:
        if v in seen:
            errors.append("CHANGELOG.md:%d: version %s appears twice" % (n, v))
        seen.add(v)

    for e in errors:
        print(e)
    if not errors:
        print("release %s: consistent (%d changelog entries)" % (version, len(entries)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
