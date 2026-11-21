#!/usr/bin/env python3
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Tests of tools/analyze_log.py on hand-written logs."""
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_log  # noqa: E402

CLEAN = """\
[10:00:00] [SYS     ] Starting Dragon GL Server...
[10:00:05] [SYS     ] Server ready!
[10:00:09] [NET     ] Client 0 connected from 10.0.0.7
[10:00:10] [AUTH    ] 'kael' created
[10:01:10] [SYS     ] Auto-save: 1 players and the world.
[10:02:00] [DEATH   ] kael was killed by Goblin on floor 2 (40,51).
[10:02:30] [DEATH   ] kael starved to death.
[10:03:00] [LEVELUP ] Player kael has reached level 2
[10:04:00] [NET     ] Client 0 ('kael', 10.0.0.7) disconnected: connection closed by peer
[10:04:01] [AUTH    ] Incorrect password for: kael
[10:04:02] [AUTH    ] 'kael' logged in
[10:09:00] [PERF    ] total: 1500 rounds, round p50 2.00 p99 4.00 max 9.00 ms (0 over 50 ms)
[10:09:01] [SYS     ] Server shut down. Log saved.
"""


def run(text):
    r = analyze_log.analyze(io.StringIO(text))
    out = io.StringIO()
    problems = analyze_log.report(r, out)
    return r, problems, out.getvalue()


class AnalyzeLogTest(unittest.TestCase):
    def test_clean_log(self):
        r, problems, out = run(CLEAN)
        self.assertEqual(problems, 0, out)
        self.assertIn("no problem found", out)
        self.assertEqual((r["logins"], r["created"], r["levels"]), (1, 1, 1))
        self.assertEqual(r["deaths"], {"Goblin": 1, "starved to death": 1})
        self.assertEqual(r["wrong_password"], {"kael": 1})
        self.assertEqual(r["disconnects"], {"connection closed by peer": 1})
        self.assertIn("p99 4.00", out)

    def test_problems(self):
        text = CLEAN + (
            "[10:10:00] [SYS     ] Starting Dragon GL Server...\n"
            "[10:10:01] [SAVE    ] Failed to write saves/kael.save (No space left on device)\n"
            "[10:10:02] [SAVE    ] Corrupted/tampered fields in lyra.save - state sanitized on load.\n"
            "[10:10:03] [NET     ] Protocol violation from client 3 (bad frame): disconnect\n"
            "[10:10:04] [TOMB    ] WARNING: no free tombstone slots, reusing the oldest\n"
            "==12==ERROR: AddressSanitizer: heap-use-after-free on address 0x1\n"
            "[10:11:00] [SYS     ] Starting Dragon GL Server...\n")
        r, problems, out = run(text)
        self.assertEqual(r["problems"]["save failed"], 1)
        self.assertEqual(r["problems"]["damaged save"], 1)
        self.assertEqual(r["problems"]["protocol violation"], 1)
        self.assertEqual(r["problems"]["warning"], 1)
        self.assertEqual(len(r["sanitizer"]), 1)
        self.assertIn("stopped without one", out)
        self.assertEqual(problems, 6, out)

    def test_strict_status(self):
        fd, path = tempfile.mkstemp(suffix=".log")
        try:
            with os.fdopen(fd, "w") as f:
                f.write("[10:00:00] [SAVE    ] Failed to rename x (EIO)\n")
            sys.argv = ["analyze_log.py", path, "--strict"]
            saved, sys.stdout = sys.stdout, io.StringIO()
            try:
                self.assertEqual(analyze_log.main(), 1)
                sys.argv = ["analyze_log.py", path]
                self.assertEqual(analyze_log.main(), 0)
            finally:
                sys.stdout = saved
        finally:
            os.remove(path)


if __name__ == "__main__":
    unittest.main()
