#!/usr/bin/env python3
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Load test of a real dragongl_server (tools/quality.sh load).

Starts the server on a new world, logs in --bots characters over TLS with
the real protocol and lets them play for --seconds: each one walks to the
stairs of the town and wanders in the dungeon, looks around, reads its
inventory and asks who is online. The server is then stopped with SIGTERM
while they are still playing, started again on the same state, and every
character logs in again.

Checked against the "load" limits of tests/bench_limits.json:
  - no character loses its connection and the server never exits;
  - the round, as the server measured it ([PERF] total), has its p99 under
    round_p99_ms and no more than rounds_over_budget_pct percent of the
    rounds over the budget;
  - a command gets its answer within command_p99_ms (p99, at the client);
  - the shutdown under load ends within shutdown_seconds with status 0,
    no sanitizer report, and every character is found again as it was
    saved (not created anew).

The numbers depend on the machine: run it on a quiet one.
"""
import argparse
import json
import os
import random
import re
import selectors
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests", "integration"))
import dglproto  # noqa: E402

SERVER_PASS = "load-gate-secret"
READY_MARK = "Server ready!"
SANITIZER_MARKS = ("ERROR: AddressSanitizer", "WARNING: ThreadSanitizer",
                   "ERROR: ThreadSanitizer", "runtime error:", "ERROR: LeakSanitizer")
PERF_TOTAL = re.compile(
    r"\[PERF\s*\].*total: (\d+) rounds, round p50 ([\d.]+) p99 ([\d.]+) max ([\d.]+) ms "
    r"\((\d+) over")
# The server accepts 8 connections that have not logged in yet per address.
LOGIN_BATCH = 6
STEPS = ((1, 0), (-1, 0), (0, 1), (0, -1))
# Head of MSG_STATE (protocol/messages.json): gold, entity_id, x, y, hp,
# max_hp, floor_id.
STATE_HEAD = struct.Struct("<Qiiiiii")


def map_center():
    """MAP_WIDTH / 2, MAP_HEIGHT / 2 from include/map.h: the stairs of the
    town."""
    with open(os.path.join(ROOT, "include", "map.h"), encoding="utf-8") as f:
        text = f.read()
    w = int(re.search(r"#define MAP_WIDTH\s+(\d+)", text).group(1))
    h = int(re.search(r"#define MAP_HEIGHT\s+(\d+)", text).group(1))
    return w // 2, h // 2


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def percentile(values, pct):
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = max(1, -(-len(ordered) * pct // 100))  # nearest rank, rounded up
    return ordered[int(rank) - 1]


class Server:
    def __init__(self, binary, data, workdir):
        self.port = free_port()
        self.log_path = os.path.join(workdir, "server.log")
        self._log = open(self.log_path, "a")
        self._start = os.path.getsize(self.log_path)
        env = dict(os.environ)
        env["DRAGONGL_SERVER_PASSWORD"] = SERVER_PASS
        env.pop("DRAGONGL_DM_PASSWORD", None)
        self.proc = subprocess.Popen(
            [binary, "--port", str(self.port), "--data-dir", data,
             "--state-dir", os.path.join(workdir, "state")],
            cwd=workdir, env=env, stdout=self._log, stderr=subprocess.STDOUT)

    def log(self):
        with open(self.log_path, errors="replace") as f:
            f.seek(self._start)
            return f.read()

    def wait_ready(self, timeout=600):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("the server exited while starting:\n" + self.log()[-2000:])
            if READY_MARK in self.log():
                return
            time.sleep(0.2)
        raise RuntimeError("the server is not ready after %d s" % timeout)

    def alive(self):
        return self.proc.poll() is None

    def stop(self, timeout):
        """SIGTERM; (exit status or None on timeout, seconds it took)."""
        start = time.monotonic()
        self.proc.send_signal(signal.SIGTERM)
        try:
            status = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            status = None
        self._log.close()
        return status, time.monotonic() - start


class Bot:
    """One character. After the login its socket is non-blocking and the
    one loop of play() serves every bot: 5 steps a second, a command every
    few seconds. Only the messages it needs are decoded, so that 64 bots
    cost less CPU than the server they measure."""

    DECODED = None  # message ids decoded; the others are skipped by length

    def __init__(self, index, port, center):
        self.name = "load%02d" % index
        self.port = port
        self.center = center
        self.rng = random.Random(index)
        self.sock = None
        self.buf = b""
        self.out = b""
        self.error = None
        self.closed = False
        self.entity_id = None
        self.position = None
        self.floor_id = None
        self.was_new = None
        self.latencies = []   # seconds, "who" -> "[WHO]"
        self.asked_at = None  # when the pending "who" was sent
        self.moves = 0
        self.commands = 0
        self.deepest = 0
        self.next_move = 0.0
        self.next_ask = 0.0

    def login(self):
        c = dglproto.Client(self.port, timeout=20.0)
        c.login(self.name, "pw-" + self.name, SERVER_PASS)
        if not c.wait_for_login(timeout=60.0):
            c.close()
            raise RuntimeError("login refused: %r" % (c.auth_fail,))
        self.was_new = c.is_new
        self.entity_id = c.entity_id
        self.position = c.position
        self.floor_id = c.floor_id
        self.buf = c._buf
        self.sock = c.sock
        self.sock.setblocking(False)

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass

    # --- network ---------------------------------------------------------
    def send(self, data):
        self.out += data
        self.flush()

    def flush(self):
        while self.out and not self.closed:
            try:
                n = self.sock.send(self.out)
            except (ssl.SSLWantWriteError, ssl.SSLWantReadError, BlockingIOError):
                return
            except OSError:
                self.closed = True
                return
            self.out = self.out[n:]

    def receive(self, now):
        while not self.closed:
            try:
                data = self.sock.recv(65536)
            except (ssl.SSLWantReadError, ssl.SSLWantWriteError, BlockingIOError):
                break
            except OSError:
                data = b""
            if not data:
                self.closed = True
                break
            self.buf += data
        size = dglproto.HEADER.size
        buf, at = self.buf, 0
        while len(buf) - at >= size:
            _, msg_type, length = dglproto.HEADER.unpack_from(buf, at)
            if len(buf) - at < size + length:
                break
            start = at + size
            at = start + length
            if msg_type == dglproto.MSG_STATE and length >= STATE_HEAD.size:
                # Only the head of the record: a full decode of every state
                # of every entity in sight would cost more than the server.
                _, entity_id, x, y, _, _, floor_id = STATE_HEAD.unpack_from(buf, start)
                if entity_id == self.entity_id:
                    self.position = (x, y)
                    self.floor_id = floor_id
                    self.deepest = max(self.deepest, floor_id)
            elif msg_type == dglproto.MSG_TEXT and self.asked_at is not None:
                if buf.find(b"[WHO]", start, at) >= 0:
                    self.latencies.append(now - self.asked_at)
                    self.asked_at = None
        self.buf = buf[at:]

    # --- play ------------------------------------------------------------
    def act(self, now):
        if self.asked_at is not None and now - self.asked_at > 5.0:
            self.error = "no answer to 'who' in 5 s"
            return
        if now >= self.next_move:
            self.next_move = now + 0.2
            self.step()
        if now >= self.next_ask and self.asked_at is None:
            self.next_ask = now + self.rng.uniform(2.0, 6.0)
            self.ask(now)

    def step(self):
        if self.floor_id == 0 and self.position and self.position != self.center:
            # In town: towards the stairs, one axis at a time.
            px, py = self.position
            dx = (self.center[0] > px) - (self.center[0] < px)
            dy = 0 if dx else (self.center[1] > py) - (self.center[1] < py)
            if self.rng.random() < 0.2:  # around what is in the way
                dx, dy = self.rng.choice(STEPS)
        else:
            dx, dy = self.rng.choice(STEPS)
        self.send(dglproto.frame(dglproto.MSG_MOVE, dglproto.move_payload(dx, dy)))
        self.moves += 1

    def ask(self, now):
        if self.rng.random() < 0.5:
            text = "who"
            self.asked_at = now
        else:
            text = self.rng.choice(("look", "inventory", "stats", "search", "spells"))
        self.send(dglproto.frame(dglproto.MSG_TEXT_CMD, dglproto.text_cmd_payload(text)))
        self.commands += 1


def serve(bots, sel, seconds, should_stop, stop_on_error=True):
    """Runs every bot for `seconds`, or until should_stop() or, with
    stop_on_error, the first error (a closed connection is one)."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        for key, _ in sel.select(timeout=0.005):
            key.data.receive(time.monotonic())
        now = time.monotonic()
        for b in bots:
            if b.closed:
                if not b.error:
                    b.error = "connection closed by the server"
                continue
            # Decrypted bytes kept by TLS are not seen by select().
            if b.sock.pending():
                b.receive(now)
            b.flush()
            b.act(now)
        if should_stop() or (stop_on_error and any(b.error for b in bots)):
            return


def login_all(bots):
    """In batches: the server limits the logins in progress per address."""
    for start in range(0, len(bots), LOGIN_BATCH):
        batch = bots[start:start + LOGIN_BATCH]
        errors = []

        def one(bot):
            try:
                bot.login()
            except (OSError, RuntimeError) as exc:
                errors.append("%s: %s" % (bot.name, exc))

        threads = [threading.Thread(target=one, args=(b,)) for b in batch]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        if errors:
            raise RuntimeError("; ".join(errors))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--server", required=True, help="dragongl_server binary")
    ap.add_argument("--data", default=os.path.join(ROOT, "data"))
    ap.add_argument("--limits", default=os.path.join(ROOT, "tests", "bench_limits.json"))
    ap.add_argument("--bots", type=int, default=64)
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--json", help="write the measurements to this file")
    ap.add_argument("--keep", action="store_true", help="keep the working directory")
    args = ap.parse_args()
    args.server = os.path.abspath(args.server)
    args.data = os.path.abspath(args.data)
    with open(args.limits, encoding="utf-8") as f:
        limits = json.load(f)["load"]

    workdir = tempfile.mkdtemp(prefix="dgl-load-")
    failures = []
    result = {"bots": args.bots, "seconds": args.seconds}
    server = Server(args.server, args.data, workdir)
    bots = []
    try:
        server.wait_ready()
        bots = [Bot(i, server.port, map_center()) for i in range(args.bots)]
        t0 = time.monotonic()
        login_all(bots)
        print("loadtest: %d characters logged in (%.1f s)" % (len(bots), time.monotonic() - t0))
        sel = selectors.DefaultSelector()
        for b in bots:
            sel.register(b.sock, selectors.EVENT_READ, b)
        serve(bots, sel, args.seconds, lambda: not server.alive())
        if not server.alive():
            failures.append("the server exited with status %s" % server.proc.returncode)
        errors = ["%s: %s" % (b.name, b.error) for b in bots if b.error]

        # Shutdown while everyone is still playing: SIGTERM, and the bots
        # go on for as long as the server takes.
        server.proc.send_signal(signal.SIGTERM)
        start = time.monotonic()
        serve(bots, sel, limits["shutdown_seconds"], lambda: not server.alive(),
              stop_on_error=False)
        took = time.monotonic() - start
        status, _ = server.stop(timeout=1)
        sel.close()
        for b in bots:
            b.close()
        failures.extend(errors)
        log = server.log()
        if status is None:
            failures.append("the shutdown under load took more than %d s" %
                            limits["shutdown_seconds"])
        elif status != 0:
            failures.append("the server exited with status %d on SIGTERM" % status)
        if "Server shut down" not in log:
            failures.append("no 'Server shut down' in the log")
        for mark in SANITIZER_MARKS:
            if mark in log:
                failures.append("sanitizer report in the server log: " + mark)

        lat_ms = [1000.0 * v for b in bots for v in b.latencies]
        result.update({
            "moves": sum(b.moves for b in bots),
            "commands": sum(b.commands for b in bots),
            "in_dungeon": sum(1 for b in bots if b.deepest > 0),
            "command_p50_ms": round(percentile(lat_ms, 50), 2),
            "command_p99_ms": round(percentile(lat_ms, 99), 2),
            "command_max_ms": round(max(lat_ms), 2) if lat_ms else 0.0,
            "shutdown_seconds": round(took, 2),
        })
        perf = PERF_TOTAL.search(log)
        if not perf:
            failures.append("no '[PERF] total' line in the server log")
        else:
            result.update({"rounds": int(perf.group(1)), "round_p50_ms": float(perf.group(2)),
                           "round_p99_ms": float(perf.group(3)),
                           "round_max_ms": float(perf.group(4)),
                           "rounds_over_budget": int(perf.group(5))})
            if result["round_p99_ms"] > limits["round_p99_ms"]:
                failures.append("round p99 %.2f ms over %.2f ms" %
                                (result["round_p99_ms"], limits["round_p99_ms"]))
            allowed = limits["rounds_over_budget_pct"] * result["rounds"] / 100.0
            if result["rounds_over_budget"] > allowed:
                failures.append("%d rounds over the budget (limit %.1f%% of %d)" %
                                (result["rounds_over_budget"],
                                 limits["rounds_over_budget_pct"], result["rounds"]))
            # 5 rounds a second, whatever the load.
            expected = 5 * args.seconds
            if result["rounds"] < 0.9 * expected:
                failures.append("%d rounds in %d s: the game ran slow (expected %d)" %
                                (result["rounds"], args.seconds, expected))
        if result["command_p99_ms"] > limits["command_p99_ms"]:
            failures.append("command p99 %.1f ms over %.1f ms" %
                            (result["command_p99_ms"], limits["command_p99_ms"]))
        if len(lat_ms) < args.bots:
            failures.append("only %d answers measured" % len(lat_ms))
        if result["in_dungeon"] < args.bots // 2:
            failures.append("only %d characters reached the dungeon" % result["in_dungeon"])

        # The same state, started again: every character is there.
        server = Server(args.server, args.data, workdir)
        server.wait_ready()
        if "NPCs loaded" not in server.log():
            failures.append("the world saved at the shutdown was not loaded again")
        again = [Bot(i, server.port, map_center()) for i in range(args.bots)]
        login_all(again)
        created = [b.name for b in again if b.was_new]
        if created:
            failures.append("%d characters were not saved at the shutdown: %s" %
                            (len(created), ", ".join(created[:5])))
        for b in again:
            b.close()
        status, _ = server.stop(timeout=limits["shutdown_seconds"])
        if status != 0:
            failures.append("second shutdown: status %s" % status)
    except RuntimeError as exc:
        failures.append(str(exc))
    finally:
        if server.alive():
            server.proc.kill()
        if args.keep or failures:
            print("loadtest: working directory kept: " + workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    print("loadtest: " + json.dumps(result, sort_keys=True))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2, sort_keys=True)
            f.write("\n")
    for line in failures:
        print("loadtest: FAIL: " + line, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
