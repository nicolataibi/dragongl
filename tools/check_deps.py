#!/usr/bin/env python3
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Dependency rules of the source tree (tools/quality.sh deps).

Every source file belongs to a layer; it may include only headers of its
own layer or of a lower one, and the headers must not include each other
in a cycle. The layers, lowest first:

  shared    include/, src/shared/      rules, data, map, protocol
  model     server.h server_entities.h the types of the server, the
            auth.h                     credentials
  infra     log, persistence, TLS,     no knowledge of the game being
            connections, workers       played
  game      everything else            the simulation
  commands  cmd_*, server_commands     the text commands of the players
  main      main_server, startup,      composition of the program
            login, options

The client (src/client/) is a separate program: it sees `shared` only.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAYERS = ["shared", "model", "infra", "game", "commands", "main"]
MODEL = {"auth", "server", "server_entities"}
INFRA = {
    "combat_log", "login_worker", "pathfinding", "persist", "server_conn",
    "server_log", "server_metrics", "server_tls", "writer",
}
MAIN = {"main_server", "server_login", "server_options", "server_startup"}
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def layer_of(path):
    """Layer of a file of the server or of the shared code."""
    rel = os.path.relpath(path, ROOT)
    stem = os.path.splitext(os.path.basename(rel))[0]
    if rel.startswith(("include/", "src/shared/")):
        return "shared"
    if rel.startswith("src/client/"):
        return "client"
    if stem in MODEL:
        return "model"
    if stem in INFRA:
        return "infra"
    if stem in MAIN:
        return "main"
    if stem.startswith("cmd_") or stem in ("server_commands", "server_cmd"):
        return "commands"
    return "game"


def sources():
    out = []
    for sub in ("include", "src/shared", "src/server", "src/client"):
        base = os.path.join(ROOT, sub)
        for name in sorted(os.listdir(base)):
            if name.endswith((".c", ".h")):
                out.append(os.path.join(base, name))
    return out


def main():
    files = sources()
    by_name = {}
    for path in files:
        if path.endswith(".h"):
            by_name.setdefault(os.path.basename(path), path)
    errors = []
    graph = {}
    for path in files:
        mine = layer_of(path)
        with open(path, encoding="utf-8") as fh:
            names = INCLUDE.findall(fh.read())
        deps = [by_name[n] for n in names if n in by_name]
        if path.endswith(".h"):
            graph[path] = deps
        for dep in deps:
            theirs = layer_of(dep)
            rel = os.path.relpath(path, ROOT)
            if mine == "client" or theirs == "client":
                ok = theirs in ("shared", "client") and mine == "client"
            else:
                ok = LAYERS.index(theirs) <= LAYERS.index(mine)
            if not ok:
                errors.append("%s (%s) includes %s (%s)" % (
                    rel, mine, os.path.basename(dep), theirs))
    # cycles among headers
    state = {}

    def visit(node, trail):
        state[node] = 1
        for dep in graph.get(node, []):
            if state.get(dep) == 1:
                loop = trail[trail.index(dep):] if dep in trail else [dep]
                errors.append("include cycle: " + " -> ".join(
                    os.path.basename(p) for p in loop + [dep]))
            elif dep not in state:
                visit(dep, trail + [dep])
        state[node] = 2

    for header in sorted(graph):
        if header not in state:
            visit(header, [header])
    for line in errors:
        print("check_deps: " + line, file=sys.stderr)
    if errors:
        return 1
    print("check_deps: %d files, %d layers, no violation" % (
        len(files), len(LAYERS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
