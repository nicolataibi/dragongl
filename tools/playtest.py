#!/usr/bin/env python3
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Playtest of a real dragongl_server (tools/quality.sh play): looks for
soft-locks.

Starts the server on a new world and logs in one character of every class
over TLS with the real protocol. Each one plays on its own: it walks to
the stairs of the town and down the dungeon, finding the way on the map
the server sends (MSG_MAP_CHUNK) and fighting what stands in it, drinks
when it is hurt, and every few seconds types one of the commands a player
types (look, inventory, stats, spells, search, who, help...).

Reported, and failing the run:
  - a command with no answer within --answer-timeout seconds;
  - a character that cannot move for --stuck seconds away from a boss
    floor (no step taken, no way found on the map it was sent);
  - a connection closed by the server, a server that exits, a sanitizer
    report in its log, a message the decoder rejects;
  - an answer "Unrecognized command" to a command of the list.
Reported only: the deepest floor of each character, its deaths, and the
[ERROR] answers by kind (most of them are the game saying no).
"""
import argparse
import collections
import os
import random
import re
import shutil
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests", "integration"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import dglproto  # noqa: E402
import loadtest  # noqa: E402

CLASSES = ("Barbarian", "Bard", "Cleric", "Druid", "Fighter", "Monk", "Paladin", "Ranger",
           "Rogue", "Sorcerer", "Warlock", "Wizard")
COMMANDS = ("look", "inventory", "stats", "spells", "search", "who", "help", "examine",
            "lore 1", "top", "event", "rest", "talk")
WALL, ROCK, STAIRS_DOWN = 2, 0, 5
STEPS = ((1, 0), (-1, 0), (0, 1), (0, -1))
MAP_W, MAP_H = loadtest.map_center()[0] * 2, loadtest.map_center()[1] * 2


class Player(dglproto.Client):
    """A client that keeps the map of its floor and its hit points."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.tiles = {}  # (x, y) -> voxel, of the current floor
        self.map_floor = None
        self.hp = self.max_hp = 0

    def _decode(self):
        # The base decoder drops what it does not keep: the map chunks and
        # the hit points are read from the same frames first.
        at = 0
        buf = self._buf
        while len(buf) - at >= dglproto.HEADER.size:
            _, msg_type, length = dglproto.HEADER.unpack_from(buf, at)
            if len(buf) - at < dglproto.HEADER.size + length:
                break
            payload = buf[at + dglproto.HEADER.size:at + dglproto.HEADER.size + length]
            at += dglproto.HEADER.size + length
            name = dglproto.SPEC.msg(msg_type)["name"]
            if name == "MAP_CHUNK":
                m = dglproto.SPEC.decode(name, payload)
                self.on_chunk(m)
            elif name == "STATE":
                m = dglproto.SPEC.decode(name, payload)
                if m["entity_id"] == self.entity_id:
                    self.hp, self.max_hp = m["hp"], m["max_hp"]
                    if m["floor_id"] != self.map_floor:
                        self.tiles = {}
                        self.map_floor = m["floor_id"]
        super()._decode()

    def on_chunk(self, m):
        w, h, tiles = m["width"], m["height"], m["tiles"]
        for j in range(h):
            row = (m["start_y"] + j)
            for i in range(w):
                self.tiles[(m["start_x"] + i, row)] = tiles[j * w + i]


class Bot(threading.Thread):
    def __init__(self, index, port, args, report):
        super().__init__(daemon=True)
        self.index = index
        self.cls = index % len(CLASSES)
        self.name = "play%02d" % index
        self.port, self.args, self.report = port, args, report
        self.rng = random.Random(1000 + index)
        self.problems = []
        self.errors = collections.Counter()
        self.deaths = 0
        self.deepest = 0
        self.moves = 0
        self.received = 0  # MSG_TEXT received so far
        self.next_trace = 0.0
        self.stop = threading.Event()
        self.blocked = set()  # tiles a step into failed, on this floor

    # --- the way down ------------------------------------------------------
    def target(self, c):
        if c.floor_id == 0:
            return loadtest.map_center()
        downs = [p for p, v in c.tiles.items() if v == STAIRS_DOWN]
        if not downs:
            return None
        x, y = c.position
        return min(downs, key=lambda p: abs(p[0] - x) + abs(p[1] - y))

    def next_step(self, c, goal):
        """First step of a shortest way on the known map to goal; with no
        goal, to the nearest tile next to the unknown (the map is sent as
        the character sees it). None if there is no way."""
        start = tuple(c.position)
        if goal is not None and start == tuple(goal):
            return None
        prev = {start: None}
        queue = collections.deque([start])
        found = None
        while queue:
            p = queue.popleft()
            if goal is not None and p == tuple(goal):
                found = p
                break
            if goal is None and p != start and any(
                    (p[0] + dx, p[1] + dy) not in c.tiles for dx, dy in STEPS):
                found = p
                break
            for dx, dy in STEPS:
                q = (p[0] + dx, p[1] + dy)
                if q in prev or not (0 < q[0] < MAP_W - 1 and 0 < q[1] < MAP_H - 1):
                    continue
                if q not in c.tiles and goal is None:
                    continue
                if c.tiles.get(q, 1) in (WALL, ROCK) or q in self.blocked:
                    continue
                prev[q] = p
                queue.append(q)
        if found is None:
            return None
        p = found
        while prev[p] != start:
            p = prev[p]
        return (p[0] - start[0], p[1] - start[1])

    # --- play --------------------------------------------------------------
    def run(self):
        try:
            self.play()
        except Exception as e:  # a decoder error is a finding, not a crash
            self.problems.append("%s: %s" % (type(e).__name__, e))

    def play(self):
        c = Player(self.port, timeout=20.0)
        c.sock.sendall(dglproto.frame(dglproto.MSG_LOGIN, dglproto.login_payload(
            self.name, "pw-" + self.name, loadtest.SERVER_PASS, char_class=self.cls)))
        if not c.wait_for_login(timeout=60.0):
            self.problems.append("login refused: %r" % (c.auth_fail,))
            return
        c.pump(1.0)
        last_move_at = time.monotonic()
        last_pos, last_floor = tuple(c.position), c.floor_id
        next_cmd = time.monotonic() + 2.0
        pending = None  # (command, sent at, texts seen then)
        tries = 0
        while not self.stop.is_set():
            c.pump(0.05)
            now = time.monotonic()
            if c.closed_by_peer:
                self.problems.append("connection closed by the server")
                return
            self.scan_texts(c)
            if c.floor_id is not None:
                self.deepest = max(self.deepest, c.floor_id)
            if (tuple(c.position), c.floor_id) != (last_pos, last_floor):
                if c.floor_id != last_floor:
                    self.blocked.clear()
                last_pos, last_floor = tuple(c.position), c.floor_id
                last_move_at = now
                tries = 0
            boss_gate = c.floor_id and c.floor_id % 10 == 0
            if (now - last_move_at > self.args.stuck and not boss_gate and c.floor_id is not None):
                known = len(c.tiles)
                self.problems.append(
                    "stuck %ds on floor %d at %s (map tiles known: %d, way down %s)"
                    % (self.args.stuck, c.floor_id, last_pos, known,
                       "unknown" if self.target(c) is None else "known"))
                last_move_at = now  # one report per stall
            if pending and now - pending[1] > self.args.answer_timeout:
                self.problems.append("no answer to '%s' in %.0fs" % (pending[0],
                                                                    self.args.answer_timeout))
                pending = None
            if pending and self.received > pending[2]:
                pending = None
            if c.max_hp and c.hp * 3 < c.max_hp and self.rng.random() < 0.05:
                c.command("use healing")
            if now >= next_cmd and not pending:
                cmd = self.rng.choice(COMMANDS)
                pending = (cmd, now, self.received)
                c.command(cmd)
                next_cmd = now + self.rng.uniform(2.0, 5.0)
            step = self.next_step(c, self.target(c))
            if step is None or tries > 3:
                if tries > 3 and step is not None:
                    x, y = c.position
                    self.blocked.add((x + step[0], y + step[1]))
                step = self.rng.choice(STEPS)
                tries = 0
            if self.args.verbose and self.index == 0 and now >= self.next_trace:
                self.next_trace = now + 10.0
                print("trace %s floor %s at %s goal %s step %s known %d blocked %d hp %d/%d" % (
                    self.name, c.floor_id, c.position, self.target(c), step, len(c.tiles),
                    len(self.blocked), c.hp, c.max_hp), flush=True)
            c.move(*step)
            self.moves += 1
            tries += 1
            time.sleep(0.2)
        c.close()

    def scan_texts(self, c):
        self.received += len(c.texts)
        for t in c.texts:
            if "Unrecognized command" in t:
                self.problems.append("unrecognized: %s" % t[:120])
            elif "You died" in t:
                self.deaths += 1
                self.blocked.clear()
            elif t.startswith("[ERROR]"):
                self.errors[re.sub(r"\d+", "N", t[:70])] += 1
        c.texts.clear()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--server", required=True, help="dragongl_server binary")
    ap.add_argument("--data", default=os.path.join(ROOT, "data"))
    ap.add_argument("--bots", type=int, default=len(CLASSES))
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--stuck", type=int, default=30)
    ap.add_argument("--answer-timeout", type=float, default=5.0)
    ap.add_argument("--keep", action="store_true", help="keep the working directory")
    ap.add_argument("--work", metavar="DIR",
                    help="working directory, created if missing and kept: a run on the DIR of "
                         "an earlier one goes on with its world and its characters (the weekly "
                         "soak, .github/workflows/soak-weekly.yml)")
    ap.add_argument("--verbose", action="store_true", help="trace the first character")
    args = ap.parse_args()

    if args.work:
        work = os.path.abspath(args.work)
        os.makedirs(work, exist_ok=True)
    else:
        work = tempfile.mkdtemp(prefix="dgl-play-")
    server = loadtest.Server(os.path.abspath(args.server), os.path.abspath(args.data), work)
    failed = False
    try:
        server.wait_ready()
        bots = [Bot(i, server.port, args, None) for i in range(args.bots)]
        for i, b in enumerate(bots):
            b.start()
            if i % loadtest.LOGIN_BATCH == loadtest.LOGIN_BATCH - 1:
                time.sleep(2.0)
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline and server.alive():
            time.sleep(0.5)
        for b in bots:
            b.stop.set()
        for b in bots:
            b.join(timeout=10)
        alive = server.alive()
        status, _ = server.stop(30) if alive else (server.proc.returncode, 0)
        log = server.log()
        print("%-8s %-10s %5s %6s %6s  %s" % ("bot", "class", "floor", "deaths", "moves",
                                               "problems"))
        errors = collections.Counter()
        for b in bots:
            errors.update(b.errors)
            print("%-8s %-10s %5d %6d %6d  %s" % (b.name, CLASSES[b.cls], b.deepest, b.deaths,
                                                   b.moves, "; ".join(b.problems) or "-"))
            failed = failed or bool(b.problems)
        if errors:
            print("\n[ERROR] answers (the game saying no):")
            for text, n in errors.most_common(15):
                print("  %4d  %s" % (n, text))
        marks = [m for m in loadtest.SANITIZER_MARKS if m in log]
        if not alive or status != 0 or marks:
            print("\nserver: %s, exit status %s, sanitizer %s" % (
                "alive" if alive else "EXITED", status, marks or "clean"))
            failed = True
    finally:
        if server.alive():
            server.stop(30)
        if args.keep or args.work:
            print("working directory:", work)
        else:
            shutil.rmtree(work, ignore_errors=True)
    print("\nplaytest: %s" % ("FAILED" if failed else "passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
