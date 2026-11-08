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
MAX_CLIENTS = 64  # MAX_CLIENTS in src/server/server_entities.h
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
        workdir = os.path.join(self.root, self.id().rsplit(".", 1)[-1])
        shutil.copytree(self.template, workdir)
        self.server = ServerProcess(workdir, free_port())
        self.server.wait_ready(timeout=300)
        self.clients = []

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
    @unittest.expectedFailure
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

    @unittest.expectedFailure
    def test_partial_header_does_not_stall_other_players(self):
        """P0-2: an unauthenticated socket that sends one byte must not
        freeze the game loop for everybody else."""
        hero = self.player("hero")
        slow = self.connect()
        time.sleep(0.3)
        slow.sock.send(b"\x01")
        time.sleep(0.05)
        hero.texts.clear()
        hero.command("who")
        latency = hero.wait_for_text("[WHO]", 15)
        self.assertIsNotNone(latency, "no answer to 'who'")
        self.assertLess(latency, 1.0, "'who' took %.2fs while a slow socket was pending" % latency)

    @unittest.expectedFailure
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
