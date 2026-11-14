# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Smoke test of the real Vulkan client.

A real server and the real client run on a virtual X display (Xvfb) with
Mesa's software Vulkan driver (lavapipe) and the Khronos validation layer.
The client logs in, renders, is resized and quits with Escape; a second
test stops the server under it. Any validation message fails the test.

Environment:
  DRAGONGL_CLIENT, DRAGONGL_SERVER  binaries (default: build/ of the tree)
  DRAGONGL_CLIENT_TEST_REQUIRE=1    fail instead of skipping when Xvfb,
                                    lavapipe, the validation layer or
                                    python3-xlib is missing (CI)
"""

import glob
import os
import re
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CLIENT = os.environ.get("DRAGONGL_CLIENT", os.path.join(ROOT, "build", "dragongl_client"))
SERVER = os.environ.get("DRAGONGL_SERVER", os.path.join(ROOT, "build", "dragongl_server"))
DATA = os.path.join(ROOT, "data")
REQUIRE = os.environ.get("DRAGONGL_CLIENT_TEST_REQUIRE") == "1"
GATE = "client-smoke-gate"
SCREEN = (1600, 1000)

try:
    from Xlib import X, XK, display
    from Xlib.ext import xtest
except ImportError:  # pragma: no cover - depends on the host
    display = None


def lavapipe_icd():
    for pattern in ("/usr/share/vulkan/icd.d/lvp_icd*.json",
                    "/etc/vulkan/icd.d/lvp_icd*.json"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return None


def validation_layer_present():
    for d in ("/usr/share/vulkan/explicit_layer.d", "/etc/vulkan/explicit_layer.d",
              "/usr/local/share/vulkan/explicit_layer.d"):
        if glob.glob(os.path.join(d, "*khronos_validation*.json")):
            return True
    return False


def missing_prerequisite():
    if not (os.access(CLIENT, os.X_OK) and os.access(SERVER, os.X_OK)):
        return "client/server not built (%s, %s)" % (CLIENT, SERVER)
    if not shutil.which("Xvfb"):
        return "Xvfb not installed"
    if not lavapipe_icd():
        return "Mesa lavapipe (software Vulkan) not installed"
    if not validation_layer_present():
        return "Vulkan validation layer not installed"
    if display is None:
        return "python3-xlib not installed"
    return None


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(predicate, timeout, step=0.1):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(step)
    return predicate()


SKIP = missing_prerequisite()


@unittest.skipIf(SKIP and not REQUIRE, SKIP or "")
class ClientSmokeTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        if SKIP:
            raise RuntimeError("client smoke test prerequisites missing: " + SKIP)
        cls.tmp = tempfile.mkdtemp(prefix="dragongl-client-")
        rfd, wfd = os.pipe()
        cls.xvfb = subprocess.Popen(
            ["Xvfb", "-displayfd", str(wfd), "-screen", "0", "%dx%dx24" % SCREEN,
             "-nolisten", "tcp"], pass_fds=(wfd,), stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        os.close(wfd)
        with os.fdopen(rfd) as f:
            number = f.readline().strip()
        if not number:
            raise RuntimeError("Xvfb did not start")
        cls.display_name = ":" + number
        cls.x = display.Display(cls.display_name)
        cls.x.set_error_handler(lambda *a, **k: None)

    @classmethod
    def tearDownClass(cls):
        cls.x.close()
        cls.xvfb.terminate()
        cls.xvfb.wait(10)
        shutil.rmtree(cls.tmp, ignore_errors=True)

    # --- helpers -------------------------------------------------------------
    def start_server(self):
        port = free_port()
        state = tempfile.mkdtemp(dir=self.tmp)
        env = dict(os.environ, DRAGONGL_SERVER_PASSWORD=GATE)
        env.pop("DRAGONGL_DM_PASSWORD", None)
        log = open(os.path.join(state, "server.log"), "wb")
        proc = subprocess.Popen([SERVER, "--port", str(port), "--data-dir", DATA,
                                 "--state-dir", os.path.join(state, "state")],
                                env=env, stdout=log, stderr=subprocess.STDOUT)
        self.addCleanup(self.stop, proc)
        self.addCleanup(log.close)
        path = log.name
        self.assertTrue(wait_for(lambda: "Server ready!" in self.read(path), 120),
                        "the server did not start")
        self.server_fingerprint = re.search(r"TLS fingerprint: (SHA256:[0-9A-F:]+)",
                                            self.read(path)).group(1)
        return proc, port

    def start_client(self, port, name, config=None, extra_args=(), extra_env=None):
        """The client, with its own known_servers under `config` (a new
        directory by default: the first connection records the server)."""
        out = open(os.path.join(self.tmp, name + ".log"), "wb")
        if config is None:
            config = tempfile.mkdtemp(dir=self.tmp)
        env = dict(os.environ, DISPLAY=self.display_name,
                   VK_ICD_FILENAMES=lavapipe_icd(), VK_DRIVER_FILES=lavapipe_icd(),
                   VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation",
                   DRAGONGL_WINDOW="1280x800", XDG_CONFIG_HOME=config)
        env.update(extra_env or {})
        # server password, name, password, new character, race, subrace,
        # class, sex, alignment
        answers = "%s\n%s\npw-%s\n1\n0\n-1\n0\nM\n0\n" % (GATE, name, name)
        proc = subprocess.Popen([CLIENT] + list(extra_args) + ["127.0.0.1", str(port)], env=env,
                                stdin=subprocess.PIPE, stdout=out,
                                stderr=subprocess.STDOUT)
        proc.stdin.write(answers.encode())
        proc.stdin.flush()
        self.addCleanup(proc.stdin.close)
        self.addCleanup(self.stop, proc)
        self.addCleanup(out.close)
        return proc, out.name

    @staticmethod
    def stop(proc):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    @staticmethod
    def read(path):
        with open(path, "rb") as f:
            return f.read().decode(errors="replace")

    def assert_no_validation_errors(self, log):
        text = self.read(log)
        self.assertIn("[VK] Enabling 1 layer(s)", text, "the validation layer was not enabled")
        bad = [line for line in text.splitlines()
               if "VUID-" in line or "Validation Error" in line or "Validation Warning" in line]
        self.assertEqual(bad, [], "validation messages:\n" + "\n".join(bad[:10]))

    def client_window(self):
        root = self.x.screen().root
        for win in root.query_tree().children:
            try:
                if win.get_attributes().map_state == X.IsViewable:
                    return win
            except Exception:
                continue
        return None

    def drawn_extent(self):
        """Width and height of the non-black area at the top-left of the
        screen (the client window: there is no window manager)."""
        root = self.x.screen().root
        w, h = SCREEN
        img = root.get_image(0, 0, w, h, X.ZPixmap, 0xFFFFFFFF)
        data = img.data
        bpp = len(data) // (w * h)

        def lit(x, y):
            o = (y * w + x) * bpp
            return any(data[o:o + 3])

        width = max((x + 1 for x in range(0, w, 2) if lit(x, 120)), default=0)
        height = max((y + 1 for y in range(0, h, 2) if lit(120, y)), default=0)
        return width, height

    def resize(self, width, height):
        win = self.client_window()
        self.assertIsNotNone(win, "no client window")
        win.configure(width=width, height=height)
        self.x.sync()

    def press_escape(self):
        win = self.client_window()
        self.assertIsNotNone(win, "no client window")
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        code = self.x.keysym_to_keycode(XK.string_to_keysym("Escape"))
        xtest.fake_input(self.x, X.KeyPress, code)
        self.x.sync()
        time.sleep(0.1)
        xtest.fake_input(self.x, X.KeyRelease, code)
        self.x.sync()

    def press(self, *keysyms, shift=False):
        """Key presses in the client window, one after the other."""
        win = self.client_window()
        self.assertIsNotNone(win, "no client window")
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        shift_code = self.x.keysym_to_keycode(XK.XK_Shift_L)
        for name in keysyms:
            code = self.x.keysym_to_keycode(XK.string_to_keysym(name))
            self.assertNotEqual(code, 0, "no key for %r" % name)
            if shift:
                xtest.fake_input(self.x, X.KeyPress, shift_code)
            xtest.fake_input(self.x, X.KeyPress, code)
            self.x.sync()
            time.sleep(0.05)
            xtest.fake_input(self.x, X.KeyRelease, code)
            if shift:
                xtest.fake_input(self.x, X.KeyRelease, shift_code)
            self.x.sync()
            time.sleep(0.05)

    def type_text(self, text):
        names = {" ": "space", "-": "minus", "/": "slash"}
        self.press(*[names.get(ch, ch) for ch in text])

    def logged_in(self, port, name, config=None, extra_args=(), env=None):
        client, log = self.start_client(port, name, config, extra_args, env)
        self.assertTrue(wait_for(lambda: "Authentication completed" in self.read(log), 60),
                        self.read(log)[-2000:])
        self.assertTrue(wait_for(lambda: "Vulkan initialization complete" in self.read(log), 60),
                        self.read(log)[-2000:])
        time.sleep(2)  # a few frames
        return client, log

    def count(self, log, needle):
        return self.read(log).count(needle)

    # --- tests -----------------------------------------------------------------
    def test_render_resize_and_quit(self):
        _server, port = self.start_server()
        client, log = self.start_client(port, "smoke")
        self.assertTrue(wait_for(lambda: "Vulkan initialization complete" in self.read(log), 60),
                        "the client did not initialize Vulkan:\n" + self.read(log)[-2000:])
        self.assertTrue(wait_for(lambda: "Authentication completed" in self.read(log), 30))
        time.sleep(4)  # a few frames with the map and the HUD
        width, height = self.drawn_extent()
        self.assertGreaterEqual((width, height), (1270, 790), "nothing drawn: %r" % ((width, height),))

        for w, h in ((900, 600), (1500, 950), (1100, 700)):
            self.resize(w, h)
            time.sleep(2.5)
            got = self.drawn_extent()
            self.assertTrue(abs(got[0] - w) <= 2 and abs(got[1] - h) <= 2,
                            "after a resize to %dx%d the client draws %r" % (w, h, got))
            self.assertIsNone(client.poll(), "the client exited during a resize")

        self.press_escape()
        try:
            status = client.wait(20)
        except subprocess.TimeoutExpired:
            self.fail("Escape did not close the client")
        self.assertEqual(status, 0, self.read(log)[-2000:])
        self.assert_no_validation_errors(log)

    def test_command_line_in_the_window(self):
        """Commands are typed in the window: Enter opens the line, Enter
        sends it, Up brings the last one back, Escape cancels the line
        without leaving the game."""
        _server, port = self.start_server()
        client, log = self.logged_in(port, "typist")
        self.press("Return")
        self.type_text("whox")
        self.press("BackSpace", "Return")
        self.assertTrue(wait_for(lambda: self.count(log, "[WHO]") == 1, 10),
                        "the command typed in the window got no answer:\n" + self.read(log)[-800:])
        # History: Up brings "who" back; edited at the start with Home.
        self.press("Return", "Up", "Return")
        self.assertTrue(wait_for(lambda: self.count(log, "[WHO]") == 2, 10),
                        "the command from the history got no answer")
        # The slash key opens the line and is not typed into it.
        self.press("slash")
        self.type_text("who")
        self.press("Return")
        self.assertTrue(wait_for(lambda: self.count(log, "[WHO]") == 3, 10),
                        "a line opened with '/' was not sent as typed")
        # While typing, the movement keys are text and Escape only cancels.
        self.press("Return")
        self.type_text("wasd")
        self.press("Escape")
        time.sleep(0.5)
        self.assertIsNone(client.poll(), "Escape left the game while a command was being typed")
        self.assertEqual(self.count(log, "[WHO]"), 3)
        # The other keys of the HUD: list of keys, frame time, panels, size.
        self.press("F1", "F3", "Tab")
        self.press("Escape")  # closes the list of keys, not the game
        time.sleep(0.5)
        self.assertIsNone(client.poll(), "Escape left the game instead of closing the key list")
        self.press("Escape")
        try:
            status = client.wait(20)
        except subprocess.TimeoutExpired:
            self.fail("Escape did not close the client")
        self.assertEqual(status, 0)
        self.assert_no_validation_errors(log)

    def test_settings_file_and_keys(self):
        """client.conf: a rebound key, a key bound to a command, the HUD
        scale; a line that is not understood is reported and the rest
        applies."""
        _server, port = self.start_server()
        config = tempfile.mkdtemp(dir=self.tmp)
        os.makedirs(os.path.join(config, "dragongl"))
        with open(os.path.join(config, "dragongl", "client.conf"), "w") as f:
            f.write("ui_scale = 2\nreduce_motion = yes\nshow_fps = yes\n"
                    "key.quit = F10\nbind.F5 = who\ncolour = red\n")
        client, log = self.logged_in(port, "tuner", config)
        self.assertIn("client.conf, line 6: unknown setting", self.read(log))
        self.press("F5")
        self.assertTrue(wait_for(lambda: self.count(log, "[WHO]") == 1, 10),
                        "the key bound to a command sent nothing")
        self.press("Escape")
        time.sleep(1)
        self.assertIsNone(client.poll(), "Escape still quits after key.quit = F10")
        self.press("F10")
        try:
            self.assertEqual(client.wait(20), 0)
        except subprocess.TimeoutExpired:
            self.fail("the rebound quit key did not close the client")
        self.assert_no_validation_errors(log)

        # --print-config: the file with every default; it needs no server.
        out = subprocess.run([CLIENT, "--print-config"], capture_output=True, text=True,
                             env=dict(os.environ, XDG_CONFIG_HOME=self.tmp), timeout=20)
        self.assertEqual(out.returncode, 0)
        for line in ("ui_scale = auto", "key.move_north", "key.command", "# bind.F5 = look"):
            self.assertIn(line, out.stdout)
        bad = subprocess.run([CLIENT, "--ui-scale", "9"], capture_output=True, text=True,
                             timeout=20)
        self.assertEqual(bad.returncode, 2)
        self.assertIn("--ui-scale takes a number from 0.5 to 4", bad.stderr)

    def test_device_lost_restarts_the_renderer(self):
        """The device is reported lost at frame 20 (DRAGONGL_TEST_DEVICE_LOST):
        the renderer is rebuilt on the same window and the game goes on."""
        _server, port = self.start_server()
        client, log = self.logged_in(port, "phoenix", None, (),
                                     {"DRAGONGL_TEST_DEVICE_LOST": "20"})
        self.assertTrue(wait_for(lambda: "[VK] Renderer restarted." in self.read(log), 30),
                        self.read(log)[-1500:])
        self.assertEqual(self.count(log, "Vulkan initialization complete"), 2)
        time.sleep(2)
        self.assertIsNone(client.poll(), "the client exited after the restart")
        width, height = self.drawn_extent()
        self.assertGreaterEqual((width, height), (1270, 790), "nothing drawn after the restart")
        # Still playing: a command typed in the window is answered.
        self.press("Return")
        self.type_text("who")
        self.press("Return")
        self.assertTrue(wait_for(lambda: self.count(log, "[WHO]") == 1, 10))
        self.press("Escape")
        self.assertEqual(client.wait(20), 0)
        self.assert_no_validation_errors(log)

    def test_frame_times_are_measured(self):
        """--frames N draws N frames, prints their times and exits."""
        _server, port = self.start_server()
        client, log = self.start_client(port, "timer", None, ("--frames", "120"))
        try:
            self.assertEqual(client.wait(120), 0, self.read(log)[-1500:])
        except subprocess.TimeoutExpired:
            self.fail("--frames 120 did not end")
        m = re.search(r"\[VK\] Frames: (\d+), frame time p50 ([\d.]+) p99 ([\d.]+) max ([\d.]+) ms",
                      self.read(log))
        self.assertIsNotNone(m, self.read(log)[-1500:])
        self.assertEqual(int(m.group(1)), 119)  # intervals between 120 frames
        p50, p99, worst = float(m.group(2)), float(m.group(3)), float(m.group(4))
        self.assertTrue(0.0 < p50 <= p99 <= worst, m.group(0))
        # A software renderer in CI: a frame over a second means a stall,
        # not a slow machine.
        self.assertLess(p99, 1000.0, m.group(0))
        self.assert_no_validation_errors(log)

    def test_server_certificate_is_pinned_on_first_use(self):
        """The first connection records the server fingerprint; the next
        one finds it; a changed certificate or a wrong --fingerprint is
        refused before any password leaves the client."""
        _server, port = self.start_server()
        config = tempfile.mkdtemp(dir=self.tmp)
        hex_fp = self.server_fingerprint[7:].replace(":", "").lower()

        first, log = self.start_client(port, "pioneer", config)
        self.assertTrue(wait_for(lambda: "Authentication completed" in self.read(log), 60),
                        self.read(log)[-2000:])
        self.assertIn("First connection to 127.0.0.1 port %d" % port, self.read(log))
        self.assertIn(self.server_fingerprint, self.read(log))
        known = os.path.join(config, "dragongl", "known_servers")
        with open(known) as f:
            self.assertIn("127.0.0.1 %d %s" % (port, hex_fp), f.read())
        self.assertEqual(os.stat(known).st_mode & 0o777, 0o600)
        self.stop(first)

        second, log = self.start_client(port, "pioneer", config)
        self.assertTrue(wait_for(lambda: "Authentication completed" in self.read(log), 60))
        self.assertIn("[NET] Known server", self.read(log))
        self.stop(second)

        with open(known, "w") as f:
            f.write("127.0.0.1 %d %s\n" % (port, "ab" * 32))
        changed, log = self.start_client(port, "pioneer", config)
        self.assertEqual(changed.wait(30), 1)
        self.assertIn("THE SERVER CERTIFICATE HAS CHANGED", self.read(log))
        self.assertNotIn("Authentication completed", self.read(log))

        wrong, log = self.start_client(port, "pioneer", None, ("--fingerprint", "cd" * 32))
        self.assertEqual(wrong.wait(30), 1)
        self.assertIn("not the one given with --fingerprint", self.read(log))

        given, log = self.start_client(port, "pioneer", config,
                                       ("--fingerprint", self.server_fingerprint))
        self.assertTrue(wait_for(lambda: "Authentication completed" in self.read(log), 60))
        self.assertIn("matches the given fingerprint", self.read(log))
        with open(known) as f:
            self.assertIn("127.0.0.1 %d %s" % (port, hex_fp), f.read())

    def test_client_exits_when_the_server_stops(self):
        server, port = self.start_server()
        client, log = self.start_client(port, "orphan")
        self.assertTrue(wait_for(lambda: "Authentication completed" in self.read(log), 60))
        time.sleep(3)
        self.stop(server)
        try:
            status = client.wait(30)
        except subprocess.TimeoutExpired:
            self.fail("the client kept running without its server")
        self.assertEqual(status, 0)
        self.assertIn("[NET] Server disconnected.", self.read(log))
        self.assert_no_validation_errors(log)


if __name__ == "__main__":
    unittest.main(verbosity=2)
