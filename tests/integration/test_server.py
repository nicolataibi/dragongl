# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Network-level integration tests for dragongl_server.

Each test starts a real server process on a free port, in a private copy
of a pre-generated world, and talks to it with the protocol client in
dglproto.py.

Environment:
  DRAGONGL_SERVER  path of the server executable (set by CTest)
  DRAGONGL_DATA    path of the read-only JSON data directory

A test that reproduces a defect still open is decorated with
@unittest.expectedFailure and carries the review id (P0-x / P1-x) in its
docstring; remove the decorator in the same commit that fixes the defect.
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
# A sanitizer build (ASan/UBSan/TSan) reports into the server log and keeps
# running; any of these lines fails the test that owned the process.
SANITIZER_MARKS = ("ERROR: AddressSanitizer", "WARNING: ThreadSanitizer",
                   "ERROR: ThreadSanitizer", "runtime error:",
                   "ERROR: LeakSanitizer")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def open_fds(pid):
    return len(os.listdir("/proc/%d/fd" % pid))


def server_env(gate=SERVER_PASS, dm=DM_PASS):
    """Environment for a server process: secrets travel only here."""
    env = dict(os.environ)
    env.pop("DRAGONGL_SERVER_PASSWORD", None)
    env.pop("DRAGONGL_DM_PASSWORD", None)
    if gate is not None:
        env["DRAGONGL_SERVER_PASSWORD"] = gate
    if dm is not None:
        env["DRAGONGL_DM_PASSWORD"] = dm
    return env


def server_command(workdir, port, extra_args=()):
    """Read-only data from the source tree, state in <workdir>/state."""
    cmd = [SERVER, "--port", str(port), "--data-dir", DATA]
    if "--state-dir" not in extra_args:
        cmd += ["--state-dir", os.path.join(workdir, "state")]
    return cmd + list(extra_args)


class ServerProcess:
    """A dragongl_server running in its own working directory."""

    def __init__(self, workdir, port, env=None, extra_args=()):
        self.workdir = workdir
        self.port = port
        self.log_path = os.path.join(workdir, "server.log")
        self._log = open(self.log_path, "a")
        # A restart appends to the same log: only look past this point.
        self._log_start = os.path.getsize(self.log_path)
        self.proc = subprocess.Popen(
            server_command(workdir, port, extra_args),
            cwd=workdir, env=server_env() if env is None else env,
            stdout=self._log, stderr=subprocess.STDOUT)

    def wait_ready(self, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("server exited during start-up:\n" + self.log_tail())
            with open(self.log_path, errors="replace") as f:
                f.seek(self._log_start)
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
                self.proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if not self._log.closed:
            self._log.close()
        self.check_sanitizers()

    def check_sanitizers(self):
        with open(self.log_path, errors="replace") as f:
            f.seek(self._log_start)
            log = f.read()
        for mark in SANITIZER_MARKS:
            at = log.find(mark)
            if at >= 0:
                raise AssertionError("sanitizer report in the server log:\n"
                                     + log[max(0, at - 200):at + 4000])

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
        server = ServerProcess(cls.template, free_port())
        try:
            server.wait_ready(timeout=600)
        finally:
            server.stop()
        os.remove(os.path.join(cls.template, "server.log"))
        state = os.path.join(cls.template, "state")
        for leftover in ("combat_log.json",):
            path = os.path.join(state, leftover)
            if os.path.exists(path):
                os.remove(path)
        for name in os.listdir(os.path.join(state, "saves")):
            os.remove(os.path.join(state, "saves", name))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.root, ignore_errors=True)

    def setUp(self):
        self.workdir = os.path.join(self.root, self.id().rsplit(".", 1)[-1])
        shutil.copytree(self.template, self.workdir)
        self.server = ServerProcess(self.workdir, free_port())
        self.server.wait_ready(timeout=300)
        self.clients = []

    def restart_server(self, env=None, extra_args=()):
        """Stop the server gracefully and start it again on the same world."""
        for c in self.clients:
            c.close()
        self.clients = []
        self.server.stop()
        self.server = ServerProcess(self.workdir, free_port(), env=env,
                                    extra_args=extra_args)
        self.server.wait_ready(timeout=300)

    def run_server_expecting_exit(self, env, extra_args=()):
        """Run the server binary that must refuse to start; return
        (exit status, output)."""
        proc = subprocess.run(server_command(self.workdir, free_port(), extra_args),
                              cwd=self.workdir, env=env, timeout=60,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = proc.stdout.decode(errors="replace")
        for mark in SANITIZER_MARKS:
            self.assertNotIn(mark, out)
        return proc.returncode, out

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
        save = os.path.join(self.workdir, "state", "saves", "owner.save")
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
        rogue.send_raw(dglproto.frame(dglproto.MSG_MOVE, dglproto.move_payload(1, 0)))
        self.assertIsNotNone(rogue.wait_until_closed(3), "MSG_MOVE before login accepted")
        self.assert_server_alive()

    def test_malformed_header_disconnects_only_the_sender(self):
        hero = self.player("hero")
        rogue = self.connect()
        rogue.send_raw(dglproto.HEADER.pack(dglproto.PROTOCOL_VERSION, 99, 1 << 30))
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

    # --- credentials (phase 3) -------------------------------------------------
    def test_server_refuses_to_start_without_a_password(self):
        status, out = self.run_server_expecting_exit(server_env(gate=None, dm=None))
        self.assertEqual(status, 2, out)
        self.assertIn("No server password", out)

    def test_passwords_on_the_command_line_are_refused(self):
        for opt in ("--password", "-p", "--dm-password", "-d"):
            status, out = self.run_server_expecting_exit(server_env(), (opt, "whatever123"))
            self.assertEqual(status, 2, out)
            self.assertIn("not accepted on the command line", out)

    def test_weak_server_password_is_refused(self):
        status, out = self.run_server_expecting_exit(server_env(gate="short"))
        self.assertEqual(status, 2, out)
        self.assertIn("too short", out)

    def test_secrets_are_not_visible_to_other_processes(self):
        pid = self.server.proc.pid
        with open("/proc/%d/cmdline" % pid, "rb") as f:
            cmdline = f.read()
        with open("/proc/%d/environ" % pid, "rb") as f:
            environ = f.read()
        for secret in (SERVER_PASS, DM_PASS):
            self.assertNotIn(secret.encode(), cmdline)
            self.assertNotIn(secret.encode(), environ)
        with open(self.server.log_path, errors="replace") as f:
            log = f.read()
        self.assertNotIn(SERVER_PASS, log)
        self.assertNotIn("RNG seeded", log)

    def test_password_file(self):
        pw_file = os.path.join(self.workdir, "gate.pw")
        with open(pw_file, "w") as f:
            f.write("from-a-file-123\n")
        os.chmod(pw_file, 0o600)
        self.restart_server(env=server_env(gate=None, dm=None),
                            extra_args=("--password-file", pw_file))
        c = self.connect()
        c.login("filed", "pw", "from-a-file-123")
        self.assertTrue(c.wait_for_login(), c.auth_fail)

    def test_dm_account_is_disabled_without_a_dm_password(self):
        self.restart_server(env=server_env(dm=None))
        c = self.connect()
        c.login("dm", DM_PASS, SERVER_PASS)
        self.assertFalse(c.wait_for_login())
        self.assertTrue(any("disabled" in r for r in c.auth_fail), c.auth_fail)

    def test_dm_login_needs_the_dm_password(self):
        wrong = self.connect()
        wrong.login("dm", "not-the-dm-password", SERVER_PASS)
        self.assertFalse(wrong.wait_for_login())
        self.assertTrue(any("DM password" in r for r in wrong.auth_fail), wrong.auth_fail)
        dm = self.player("dm", password=DM_PASS)
        dm.command("dm_telemetry")
        self.assertIsNotNone(dm.wait_for_text("[DM]", 5), dm.texts[-5:])

    def test_save_keeps_only_an_argon2id_hash(self):
        self.player("keeper", password="plain-text-pw").close()
        time.sleep(1.0)
        with open(os.path.join(self.workdir, "state", "saves", "keeper.save"), "rb") as f:
            data = f.read()
        self.assertIn(b"$argon2id$", data)
        self.assertNotIn(b"plain-text-pw", data)
        self.player("keeper", password="plain-text-pw")

    def test_chat_control_characters_are_stripped(self):
        speaker = self.player("speaker")
        listener = self.player("listener")
        speaker.command("say hi\x1b[2J\rthere")
        self.assertIsNotNone(listener.wait_for_text("[CHAT] speaker:", 5), listener.texts)
        for text in listener.texts:
            self.assertFalse(any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in text), repr(text))

    # --- state directory and file formats (phase 4a) ----------------------------
    def state(self, *parts):
        return os.path.join(self.workdir, "state", *parts)

    def test_state_directory_layout(self):
        self.player("settler").close()
        time.sleep(1.0)
        self.restart_server()
        mode = os.stat(self.state()).st_mode & 0o777
        self.assertEqual(mode, 0o700, oct(mode))
        for name in ("world.dat", "npcs.dat", "artifacts.dat", "combat_log.json"):
            self.assertTrue(os.path.isfile(self.state(name)), name)
        for name in ("saves", "maps"):
            self.assertTrue(os.path.isdir(self.state(name)), name)
        self.assertTrue(os.path.isfile(self.state("saves", "settler.save")))
        self.assertFalse(any(n.endswith(".tmp") for n in os.listdir(self.state())))

    def test_data_directory_is_never_written(self):
        before = sorted((n, os.stat(os.path.join(DATA, n)).st_mtime_ns) for n in os.listdir(DATA))
        self.player("reader").close()
        self.restart_server()
        after = sorted((n, os.stat(os.path.join(DATA, n)).st_mtime_ns) for n in os.listdir(DATA))
        self.assertEqual(before, after)

    def test_unusable_state_directory_is_refused(self):
        blocker = os.path.join(self.workdir, "not-a-dir")
        with open(blocker, "w") as f:
            f.write("x")
        status, out = self.run_server_expecting_exit(server_env(), ("--state-dir", blocker))
        self.assertEqual(status, 2, out)
        self.assertIn("State directory", out)

    def test_unreadable_game_data_is_refused(self):
        """A data file that does not parse stops the server with the file
        name and the position of the error (it used to start without it)."""
        data = os.path.join(self.workdir, "bad-data")
        shutil.copytree(DATA, data, ignore=shutil.ignore_patterns("schema"))
        with open(os.path.join(data, "spells.json"), "w") as f:
            f.write('{\n  "spells": [\n    {"name": }\n  ]\n}\n')
        status, out = self.run_server_expecting_exit(
            server_env(), ("--data-dir", data, "--state-dir", os.path.join(self.workdir, "s2")))
        self.assertEqual(status, 1, out)
        self.assertIn("spells.json: invalid JSON at line 3, column", out)
        self.assertIn("cannot be loaded", out)

    def test_second_server_on_the_same_state_is_refused(self):
        status, out = self.run_server_expecting_exit(server_env())
        self.assertEqual(status, 2, out)
        self.assertIn("in use by another dragongl_server", out)

    def test_corrupt_world_is_replaced_by_a_new_one(self):
        self.server.stop()
        with open(self.state("world.dat"), "r+b") as f:
            f.seek(4096)
            byte = f.read(1)
            f.seek(4096)
            f.write(bytes([byte[0] ^ 0xFF]))
        self.restart_server()
        with open(self.server.log_path, errors="replace") as f:
            log = f.read()
        self.assertIn("checksum mismatch", log)
        self.assertIn("New world generated and saved", log)

    def test_corrupt_save_is_refused_and_kept(self):
        self.player("fragile", password="pw-fragile").close()
        time.sleep(1.0)
        path = self.state("saves", "fragile.save")
        with open(path, "r+b") as f:
            data = bytearray(f.read())
            data[-1] ^= 0xFF
            f.seek(0)
            f.write(data)
        c = self.connect()
        c.login("fragile", "pw-fragile", SERVER_PASS)
        self.assertFalse(c.wait_for_login())
        self.assertTrue(any("unreadable" in r for r in c.auth_fail), c.auth_fail)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), bytes(data), "the corrupt save was overwritten")

    def test_second_login_of_same_character_is_rejected(self):
        """P1-2: the same character must not be online twice (item
        duplication through independent saves)."""
        first = self.player("twin")
        second = self.connect()
        second.login("twin", "pw", SERVER_PASS)
        self.assertFalse(second.wait_for_login(), "second session was accepted")
        self.assertTrue(any("already online" in r for r in second.auth_fail), second.auth_fail)
        # The first session is untouched, and the character is free again
        # once it leaves.
        first.texts.clear()
        first.command("who")
        self.assertIsNotNone(first.wait_for_text("[WHO] 1 player(s) online", 5))
        first.close()
        time.sleep(1.0)
        self.player("twin")


    # --- work off the game loop (phase 4b) -------------------------------------
    def login_retrying(self, name, password="pw", tries=40):
        """Log in, retrying while the previous session is still being
        released (the reaper runs once per loop pass)."""
        for _ in range(tries):
            c = self.connect()
            c.login(name, password, SERVER_PASS)
            if c.wait_for_login():
                return c
            if not any("already online" in r for r in c.auth_fail):
                self.fail("login of %r failed: %r" % (name, c.auth_fail))
            time.sleep(0.05)
        self.fail("%r still online after %d tries" % (name, tries))

    def test_relogin_right_after_logout_sees_the_latest_save(self):
        """The logout save is written by the writer thread; the next login
        of the same character must wait for it, never read the old file."""
        walker = self.player("courier")
        start = walker.position
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            walker.move(dx, dy)
            time.sleep(0.25)
            walker.pump(0.2)
            if walker.position != start:
                break
        self.assertNotEqual(walker.position, start, "could not move")
        here = walker.position
        walker.close()
        again = self.login_retrying("courier")
        self.assertEqual(again.position, here)

    def test_concurrent_logins_of_a_new_character(self):
        """Two connections creating the same new character at once: the
        second is refused while the first is in the login worker."""
        a, b = self.connect(), self.connect()
        a.login("racer", "pw", SERVER_PASS)
        b.login("racer", "pw", SERVER_PASS)
        results = [a.wait_for_login(), b.wait_for_login()]
        self.assertEqual(results.count(True), 1, (a.auth_fail, b.auth_fail))
        loser = b if results[0] else a
        self.assertTrue(any("already online" in r for r in loser.auth_fail), loser.auth_fail)

    def test_game_loop_answers_while_logins_are_hashed(self):
        """Argon2id runs in the login worker: a burst of logins must not
        freeze the players already in the game."""
        hero = self.player("watcher")
        time.sleep(0.3)  # past the per-action rate limit of the login
        newcomers = []
        start = time.monotonic()
        for k in range(6):
            c = self.connect()
            c.login("burst%d" % k, "pw-burst", SERVER_PASS)
            newcomers.append(c)
        hero.texts.clear()
        hero.command("who")
        answered = hero.wait_for_text("[WHO]", 10)
        self.assertIsNotNone(answered, "no answer to 'who'")
        answered_at = time.monotonic() - start
        for c in newcomers:
            self.assertTrue(c.wait_for_login(20), c.auth_fail)
        all_in = time.monotonic() - start
        if all_in < 0.3:
            self.skipTest("hashing too fast here (%.2f s) to tell the difference" % all_in)
        self.assertLess(answered_at, all_in * 0.7,
                        "'who' answered after %.2f s, logins done after %.2f s"
                        % (answered_at, all_in))

    def test_second_login_message_disconnects(self):
        """Two MSG_LOGIN back to back: the second waits behind the first,
        then arrives on an authenticated session and drops it."""
        c = self.connect()
        frame = dglproto.frame(dglproto.MSG_LOGIN,
                               dglproto.login_payload("doubled", "pw", SERVER_PASS))
        c.send_raw(frame + frame)
        self.assertIsNotNone(c.wait_until_closed(10), "second MSG_LOGIN accepted")
        self.login_retrying("doubled")

    def test_messages_queued_behind_the_login_are_served(self):
        """A command sent right after MSG_LOGIN (before MSG_WELCOME) waits
        for the login to complete and is then executed."""
        c = self.connect()
        c.send_raw(dglproto.frame(dglproto.MSG_LOGIN,
                                  dglproto.login_payload("eager", "pw", SERVER_PASS)) +
                   dglproto.frame(dglproto.MSG_TEXT_CMD, dglproto.text_cmd_payload("who")))
        self.assertTrue(c.wait_for_login(), c.auth_fail)
        self.assertIsNotNone(c.wait_for_text("[WHO]", 5), c.texts[-5:])

    def test_leaderboard_lists_characters_in_play(self):
        """'top' reads the saves at most once a minute; the characters
        online are taken live (a new one has no save yet)."""
        self.player("veteran").close()
        time.sleep(0.5)
        fresh = self.player("rookie")
        fresh.command("top")
        self.assertIsNotNone(fresh.wait_for_text("rookie (Level 1)", 5), fresh.texts[-12:])
        self.assertTrue(any("veteran" in t for t in fresh.texts), fresh.texts[-12:])

    def test_dm_pdf_runs_in_the_background(self):
        dm = self.player("dm", password=DM_PASS)
        time.sleep(0.3)
        dm.command("dm_pdf 0")
        first = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and first is None:
            dm.pump(0.1)
            first = next((t for t in dm.texts if t.startswith("[DM]")), None)
        self.assertIsNotNone(first, "no answer to dm_pdf")
        if "not found" in first:
            self.skipTest(first)
        self.assertIn("background", first)
        time.sleep(0.3)
        dm.command("dm_pdf 0")  # same floor: refused while running (or restarted)
        time.sleep(0.3)
        dm.command("who")
        self.assertIsNotNone(dm.wait_for_text("[WHO]", 5), "game loop blocked by the generator")
        done = None
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and done is None:
            dm.pump(0.2)
            done = next((t for t in dm.texts
                         if "Generated PDF Map" in t or "Error generating PDF" in t), None)
        self.assertIsNotNone(done, "no completion notice")
        if "Generated PDF Map" in done:
            self.assertTrue(os.path.exists(self.state("maps", "map_floor_0.pdf")))

    # --- protocol v4 (phase 4c) ------------------------------------------------
    def test_older_protocol_version_is_refused(self):
        old = self.connect()
        old.send_raw(dglproto.header(dglproto.MSG_LOGIN, 4, version=3) + bytes(4))
        self.assertIsNotNone(old.wait_until_closed(5), "a v3 frame was accepted")
        self.assert_server_alive()

    def test_oversized_string_in_login_disconnects(self):
        """A string longer than its field is a protocol error, not
        silently truncated."""
        c = self.connect()
        payload = (dglproto.lstr("x" * 40, 32) + dglproto.lstr("pw", 64) +
                   dglproto.lstr(SERVER_PASS, 64) + bytes(4))
        c.send_raw(dglproto.frame(dglproto.MSG_LOGIN, payload))
        self.assertFalse(c.wait_for_login(3))
        self.assertIsNotNone(c.wait_until_closed(5))
        self.assertIn("malformed MSG_LOGIN", self.server.log_tail(50))

    def test_trailing_bytes_disconnect(self):
        hero = self.player("strict")
        time.sleep(0.3)
        hero.send_raw(dglproto.frame(dglproto.MSG_TEXT_CMD,
                                     dglproto.text_cmd_payload("who") + b"!"))
        self.assertIsNotNone(hero.wait_until_closed(5), "trailing bytes accepted")
        self.assert_server_alive()

    def test_long_passwords_are_accepted(self):
        """64-byte credential fields: 63-character passwords work end to end."""
        secret = "S" * 63
        self.restart_server(env=server_env(gate=secret))
        c = self.connect()
        c.login("longpass", "p" * 63, secret)
        self.assertTrue(c.wait_for_login(), c.auth_fail)
        c.close()
        time.sleep(0.3)
        again = self.connect()
        again.login("longpass", "p" * 62, secret)
        self.assertFalse(again.wait_for_login(), "a different password was accepted")

    def test_level_up_is_a_combat_event(self):
        """Floating combat text comes from MSG_COMBAT_EVENT, not from the
        wording of MSG_TEXT."""
        dm = self.player("dm", password=DM_PASS)
        time.sleep(0.3)
        dm.command("dm_level 3")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not dm.combat_events:
            dm.pump(0.1)
        self.assertIn((8, dm.entity_id, 2), dm.combat_events)  # COMBAT_EV_LEVEL_UP

    def test_ipv4_clients_are_logged_as_ipv4(self):
        """The listening socket is dual-stack: an IPv4 peer must still be
        reported (and rate-limited) as a.b.c.d, not ::ffff:a.b.c.d."""
        self.player("v4peer")
        log = self.server.log_tail(80)
        self.assertIn("from 127.0.0.1", log)
        self.assertNotIn("::ffff:", log)

    def test_ipv6_loopback(self):
        try:
            c = dglproto.Client(self.server.port, host="::1")
        except OSError as e:
            self.skipTest("no IPv6 loopback here: %s" % e)
        self.clients.append(c)
        c.login("sixer", "pw", SERVER_PASS)
        self.assertTrue(c.wait_for_login(), c.auth_fail)

if __name__ == "__main__":
    unittest.main()
