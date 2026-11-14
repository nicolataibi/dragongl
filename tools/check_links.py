#!/usr/bin/env python3
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Checks the links of the Markdown documentation.

  check_links.py              links inside the repository:
                                - [text](path), images, <a href>, <img src>:
                                  the file or directory exists;
                                - #anchor, alone or after a path to a .md
                                  file: a heading of that file has it;
                                - `path/of/a/file` in backticks, when it
                                  starts with a directory of the tree: the
                                  file exists (a glob must match something).
  check_links.py --external   the http(s) links as well: each URL must
                              answer (needs the network; nightly in CI).

CHANGELOG.md and LORE.md are history and narrative: their links are
checked, the paths they name in backticks are not (files come and go).

Exit status 0 when every link holds, 1 otherwise.
"""

import argparse
import concurrent.futures
import glob
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Directories a path in backticks may start with.
TREE = ("src/", "include/", "tests/", "tools/", "data/", "docs/", "man/", "shaders/", "cmake/",
        "protocol/", ".github/", ".copr/")
# Paths that exist only after a build or a run, named in the documentation.
GENERATED = ("build", "dist/", "state/", "saves/")
NO_PATHS = ("CHANGELOG.md", "LORE.md")

FENCE = re.compile(r"^\s*(```|~~~)")
INLINE = re.compile(r"!?\[(?:[^\]\\]|\\.)*\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
REFDEF = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s|$)")
HTML = re.compile(r"""(?:href|src)\s*=\s*(?:"([^"]+)"|'([^']+)')""", re.I)
BARE = re.compile(r"<(https?://[^>\s]+)>")
CODE = re.compile(r"`([^`\n]+)`")
HEADING = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
ANCHOR = re.compile(r"""<a\s+(?:name|id)\s*=\s*["']([^"']+)["']""", re.I)


def markdown_files():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "--cached", "--others",
                          "--exclude-standard", "*.md"],
                         capture_output=True, text=True, check=True).stdout.split("\n")
    return sorted(f for f in out if f and os.path.exists(os.path.join(ROOT, f)))


def slug(heading):
    """The anchor GitHub gives a heading."""
    text = re.sub(r"`([^`]*)`", r"\1", heading)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)  # the text of a link
    text = re.sub(r"<[^>]+>", "", text)
    text = text.strip().lower()
    text = "".join(ch for ch in text if ch.isalnum() or ch in " -_")
    return text.replace(" ", "-")


def scan(path):
    """(lines outside code blocks as (number, text), anchors) of a file."""
    lines, anchors, seen, fenced = [], set(), {}, False
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            if FENCE.match(line):
                fenced = not fenced
                continue
            if fenced:
                continue
            lines.append((n, line.rstrip("\n")))
            m = HEADING.match(line)
            if m:
                s = slug(m.group(2))
                k = seen.get(s, 0)
                seen[s] = k + 1
                anchors.add(s if k == 0 else "%s-%d" % (s, k))
            anchors.update(ANCHOR.findall(line))
    return lines, anchors


def targets(line):
    """The link targets of a line (inline code removed first)."""
    bare = CODE.sub("", line)
    found = INLINE.findall(bare) + BARE.findall(bare)
    found += [a or b for a, b in HTML.findall(bare)]
    m = REFDEF.match(bare)
    if m:
        found.append(m.group(1))
    return found


def cited_paths(line):
    """Paths of the tree named in backticks."""
    out = []
    for code in CODE.findall(line):
        word = code.strip()
        if not word.startswith(TREE) or re.search(r"[\s<>{}$|…]|\.\.\.", word):
            continue
        word = re.sub(r"(:\d+(-\d+)?|\(\))$", "", word)  # file:line, function()
        out.append(word.rstrip(".,;:"))
    return out


def check_internal(files):
    errors, external = [], {}
    scanned = {f: scan(f) for f in files}
    for f in files:
        lines, own_anchors = scanned[f]
        base = os.path.dirname(f)
        for n, line in lines:
            for t in targets(line):
                where = "%s:%d" % (f, n)
                if re.match(r"https?://", t):
                    external.setdefault(t, where)
                    continue
                if re.match(r"[a-z][a-z0-9+.-]*:", t, re.I):
                    continue  # mailto: and the like
                path, _, frag = t.partition("#")
                path = urllib.parse.unquote(path)
                frag = urllib.parse.unquote(frag).lower()
                if not path:
                    if frag and frag not in own_anchors:
                        errors.append("%s: no heading for #%s" % (where, frag))
                    continue
                rel = os.path.normpath(os.path.join(base, path))
                if rel.startswith(".."):
                    errors.append("%s: %s points outside the repository" % (where, t))
                    continue
                if not os.path.exists(os.path.join(ROOT, rel)):
                    errors.append("%s: %s does not exist" % (where, path))
                    continue
                if frag and rel.endswith(".md"):
                    anchors = scanned[rel][1] if rel in scanned else scan(rel)[1]
                    if frag not in anchors:
                        errors.append("%s: %s has no heading for #%s" % (where, path, frag))
            if f in NO_PATHS:
                continue
            for p in cited_paths(line):
                if p.startswith(GENERATED):
                    continue
                full = os.path.join(ROOT, p)
                if any(c in p for c in "*?["):
                    found = bool(glob.glob(full))
                else:
                    # A program is named after its source: tests/x for tests/x.c.
                    found = os.path.exists(full) or os.path.exists(full + ".c")
                if not found:
                    errors.append("%s:%d: `%s` does not exist" % (f, n, p))
    return errors, external


def fetch(url, timeout=20):
    """None when the URL answers, else the reason. A server that refuses a
    script (401, 403, 429) is not a broken link."""
    headers = {"User-Agent": "dragongl-check-links/1 (+https://github.com/nicolataibi/dragongl)"}
    reason = None
    for method in ("HEAD", "GET"):
        try:
            req = urllib.request.Request(url, method=method, headers=headers)
            with urllib.request.urlopen(req, timeout=timeout) as r:
                if r.status < 400:
                    return None
                reason = "HTTP %d" % r.status
        except urllib.error.HTTPError as e:
            if e.code in (401, 403, 429):
                return None
            reason = "HTTP %d" % e.code
        except (urllib.error.URLError, OSError, ValueError) as e:
            reason = str(getattr(e, "reason", e))
    return reason


def check_external(external, jobs=8):
    errors = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for (url, where), reason in zip(external.items(), pool.map(fetch, external)):
            if reason:
                errors.append("%s: %s: %s" % (where, url, reason))
    return errors


def main():
    ap = argparse.ArgumentParser(description="Check the links of the Markdown documentation.")
    ap.add_argument("--external", action="store_true", help="also fetch the http(s) links")
    args = ap.parse_args()
    files = markdown_files()
    errors, external = check_internal(files)
    if args.external:
        errors += check_external(external)
    for e in errors:
        print(e)
    if errors:
        print("%d broken link(s)" % len(errors))
        return 1
    print("links: %d Markdown files, internal links and cited paths hold%s"
          % (len(files), "; %d external links answer" % len(external) if args.external
             else " (%d external links not fetched)" % len(external)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
