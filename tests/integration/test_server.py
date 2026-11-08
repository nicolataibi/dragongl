# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Network-level integration tests for dragongl_server.

Each test starts a real server process on a free port, in a private copy
of a pre-generated world, and talks to it with the protocol client in
dglproto.py.

Environment:
  DRAGONGL_SERVER  path of the server executable (set by CTest)
  DRAGONGL_DATA    path of the read-only JSON data directory

Tests decorated with @unittest.expectedFailure reproduce a defect that is
still open; the review id (P0-x / P1-x) is in the docstring. Remove the
decorator in the same commit that fixes the defect.
"""

import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest

import dglproto

SERVER = os.environ.get("DRAGONGL_SERVER", "")
DATA = os.environ.get("DRAGONGL_DATA", "")
SERVER_PASS = "it-server-secret"
DM_PASS = "it-dm-secret"
MAX_CLIENTS = 64          # MAX_CLIENTS in src/server/server_entities.h
MAX_PENDING_PER_IP = 8    # src/server/server_conn.h
LOGIN_TIMEOUT_S = 10      # LOGIN_TIMEOUT_MS in src/server/server_conn.h
LOGIN_FAIL_LIMIT = 5      # src/server/server_conn.h
READY_MARK = "Server ready!"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def open_fds(pid):
    return len(os.listdir("/proc/%d/fd" % pid))


class ServerProcess:
    """A dragongl_server running in its own working directory."""

    def __init__(self, workdir, port):
        self.workdir = workdir
        self.port = port
        self.log_path = os.path.join(workdir, "server.log")
        self._log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [SERVER, "--password", SERVER_PASS, "--dm-password", DM_PASS,
             "--port", str(port)],
            cwd=workdir, stdout=self._log, stderr=subprocess.STDOUT)

    def wait_ready(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("server exited during start-up:\n" + self.log_tail())
            with open(self.log_path, errors="replace") as f:
                if READY_MARK in f.read():
                    return
            time.sleep(0.2)
        raise RuntimeError("server not ready after %ss:\n%s" % (timeout, self.log_tail()))

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        if self.alive():
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self._log.close()

    def log_tail(self, lines=30):
        with open(self.log_path, errors="replace") as f:
            return "".join(f.readlines()[-lines:])


@unittest.skipUnless(SERVER and os.access(SERVER, os.X_OK) and DATA,
                     "DRAGONGL_SERVER / DRAGONGL_DATA not set")
class ServerTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        # Generate the world ONCE (slow), then give every test a copy.
        cls.root = tempfile.mkdtemp(prefix="dragongl-it-")
        cls.template = os.path.join(cls.root, "template")
        os.makedirs(cls.template)
        shutil.copytree(DATA, os.path.join(cls.template, "data"))
        server = ServerProcess(cls.template, free_port())
        try:
            server.wait_ready(timeout=600)
        finally:
            server.stop()
        for leftover in ("server.log", "combat_log.json"):
            path = os.path.join(cls.template, leftover)
            if os.path.exists(path):
                os.remove(path)
        shutil.rmtree(os.path.join(cls.template, "saves"), ignore_errors=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def setUp(self):
        self.workdir = os.path.join(self.root, self.id().rsplit(".", 1)[-1])
        shutil.copytree(self.template, self.workdir)
        self.server = ServerProcess(self.workdir, free_port())
        self.server.wait_ready(timeout=300)
        self.clients = []

    def restart_server(self):
        """Stop the server gracefully and start it again on the same world."""
        for c in self.clients:
            c.close()
        self.clients = []
        self.server.stop()
        self.server = ServerProcess(self.workdir, free_port())
        self.server.wait_ready(timeout=300)

    def tearDown(self):
        for c in self.clients:
            c.close()
        self.server.stop()

    # --- helpers -----------------------------------------------------------
    def connect(self):
        c = dglproto.Client(self.server.port)
        self.clients.append(c)
        return c

    def player(self, name, password="pw"):
        c = self.connect()
        c.login(name, password, SERVER_PASS)
        self.assertTrue(c.wait_for_login(), "login of %r failed: %r" % (name, c.auth_fail))
        return c

    def assert_server_alive(self):
        self.assertTrue(self.server.alive(),
                        "server exited with status %s\n%s"
                        % (self.server.proc.returncode, self.server.log_tail()))

    # --- baseline behaviour (must pass) -------------------------------------
    def test_login_and_who(self):
        hero = self.player("hero")
        hero.command("who")
        self.assertIsNotNone(hero.wait_for_text("[WHO] 1 player(s) online", 5))

    def test_wrong_server_password_is_rejected(self):
        c = self.connect()
        c.login("intruder", "pw", "not-the-password")
        self.assertFalse(c.wait_for_login())
        self.assertTrue(c.auth_fail, "no MSG_AUTH_FAIL received")

    # --- open defects (expected failures) ------------------------------------
    def test_simultaneous_disconnect_keeps_server_alive(self):
        """P0-1: players quitting at the same moment must not kill the
        server (SIGPIPE on a send to an already closed socket)."""
        for round_no in range(5):
            group = [self.player("quit%d_%d" % (round_no, i)) for i in range(3)]
            for c in group:
                c.close()
            time.sleep(1.5)
            self.assert_server_alive()
        self.player("survivor")

    def test_partial_header_does_not_stall_other_players(self):
        """P0-2: unauthenticated sockets that send a partial header must not
        freeze the game loop for everybody else."""
        hero = self.player("hero")
        hostile = [self.connect() for _ in range(MAX_PENDING_PER_IP)]
        time.sleep(0.3)
        for h in hostile:
            h.sock.send(b"\x01")
        time.sleep(0.05)
        hero.texts.clear()
        hero.command("who")
        latency = hero.wait_for_text("[WHO]", 15)
        self.assertIsNotNone(latency, "no answer to 'who'")
        self.assertLess(latency, 0.25,
                        "'who' took %.2fs with %d partial headers pending"
                        % (latency, len(hostile)))

    def test_rejected_connections_are_closed(self):
        """P0-3: connections refused because the server is full must be
        closed, not leaked."""
        pid = self.server.proc.pid
        baseline = open_fds(pid)
        socks = [socket.create_connection(("127.0.0.1", self.server.port))
                 for _ in range(MAX_CLIENTS + 16)]
        time.sleep(1.5)
        for s in socks:
            s.close()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and open_fds(pid) > baseline:
            time.sleep(0.2)
        self.assertEqual(open_fds(pid), baseline, "file descriptors leaked")

    def test_server_full_is_reported(self):
        """A connection beyond MAX_CLIENTS gets a reason and is closed."""
        for i in range(MAX_CLIENTS):
            self.player("crowd%02d" % i)
        late = self.connect()
        self.assertIsNotNone(late.wait_until_closed(5), "connection left open")
        self.assertTrue(any("full" in r.lower() for r in late.auth_fail), late.auth_fail)
        self.assert_server_alive()

    def test_idle_connection_times_out(self):
        """A socket that never logs in is closed after the login timeout."""
        idle = self.connect()
        elapsed = idle.wait_until_closed(LOGIN_TIMEOUT_S + 5)
        self.assertIsNotNone(elapsed, "idle connection never closed")
        self.assertGreaterEqual(elapsed, LOGIN_TIMEOUT_S - 1)
        self.assertTrue(any("timeout" in r.lower() for r in idle.auth_fail), idle.auth_fail)

    def test_pending_connections_per_address_are_limited(self):
        pending = [self.connect() for _ in range(MAX_PENDING_PER_IP)]
        time.sleep(0.5)
        extra = self.connect()
        self.assertIsNotNone(extra.wait_until_closed(3), "extra connection accepted")
        self.assertTrue(extra.auth_fail, "no reason sent")
        for p in pending:
            self.assertFalse(p.closed_by_peer)
        # A logged-in player does not count as pending.
        for p in pending:
            p.close()
        time.sleep(0.5)
        self.player("after_pending")

    def test_repeated_failed_logins_lock_out_the_address(self):
        for _ in range(LOGIN_FAIL_LIMIT):
            c = self.connect()
            c.login("guesser", "pw", "wrong-guess")
            self.assertFalse(c.wait_for_login())
            c.close()
        locked = self.connect()
        locked.login("guesser", "pw", SERVER_PASS)  # even the right password
        self.assertFalse(locked.wait_for_login())
        self.assertTrue(any("failed logins" in r for r in locked.auth_fail), locked.auth_fail)

    def test_wrong_character_password_does_not_overwrite_the_save(self):
        """A rejected login must never save the character it tried."""
        owner = self.player("owner", password="right")
        owner.close()
        time.sleep(1.0)
        save = os.path.join(self.workdir, "saves", "owner.save")
        with open(save, "rb") as f:
            before = f.read()
        thief = self.connect()
        thief.login("owner", "wrong", SERVER_PASS)
        self.assertFalse(thief.wait_for_login())
        time.sleep(1.0)
        with open(save, "rb") as f:
            self.assertEqual(f.read(), before, "save file rewritten by a failed login")
        self.player("owner", password="right")

    def test_messages_before_login_disconnect(self):
        rogue = self.connect()
        rogue.send_raw(dglproto.header(dglproto.MSG_MOVE, dglproto.MOVE_SIZE) + bytes(16))
        self.assertIsNotNone(rogue.wait_until_closed(3), "MSG_MOVE before login accepted")
        self.assert_server_alive()

    def test_malformed_header_disconnects_only_the_sender(self):
        hero = self.player("hero")
        rogue = self.connect()
        rogue.send_raw(dglproto.HEADER.pack(99, 1 << 30, 3, 0))
        self.assertIsNotNone(rogue.wait_until_closed(3), "malformed header accepted")
        hero.texts.clear()
        hero.command("who")
        self.assertIsNotNone(hero.wait_for_text("[WHO]", 5))

    def test_players_are_saved_on_shutdown(self):
        """SIGTERM must persist the characters online (the autosave runs
        only once a minute)."""
        walker = self.player("walker")
        start = walker.position
        moved = False
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            walker.move(dx, dy)
            time.sleep(0.4)
            walker.pump(0.3)
            if walker.position != start:
                moved = True
                break
        self.assertTrue(moved, "could not move away from %r" % (start,))
        here = walker.position
        self.restart_server()
        again = self.player("walker")
        self.assertEqual(again.position, here)

    @unittest.expectedFailure
    def test_second_login_of_same_character_is_rejected(self):
        """P1-2: the same character must not be online twice (item
        duplication through independent saves)."""
        self.player("twin")
        second = self.connect()
        second.login("twin", "pw", SERVER_PASS)
        self.assertFalse(second.wait_for_login(), "second session was accepted")
        self.assertTrue(second.auth_fail, "no MSG_AUTH_FAIL for the second session")


if __name__ == "__main__":
    unittest.main()
