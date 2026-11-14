#!/usr/bin/env python3
# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Tests of check_links.py: a checker that finds nothing must not be one
that looks at nothing. Run by `tools/quality.sh links`."""

import http.server
import os
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_links  # noqa: E402


class Parsing(unittest.TestCase):
    def test_slug_is_the_anchor_of_github(self):
        for heading, anchor in (
            ("Quick Dev Loop: `build.sh`", "quick-dev-loop-buildsh"),
            ("1. Toolchain & Dependencies", "1-toolchain--dependencies"),
            ("Step 3: Install (optional)", "step-3-install-optional"),
            ("The [Game](x.md) state", "the-game-state"),
            ("NPC lifecycle, floor walks", "npc-lifecycle-floor-walks"),
        ):
            self.assertEqual(check_links.slug(heading), anchor)

    def test_targets(self):
        t = check_links.targets
        self.assertEqual(t("see [a](b.md#c) and ![i](img/x.png \"title\")"), ["b.md#c", "img/x.png"])
        self.assertEqual(t("<img src='a b/it\"s.png' width='2'> <a href=\"d.md\">"),
                         ["a b/it\"s.png", "d.md"])
        self.assertEqual(t("<https://example.org/x> and `[not](a-link.md)`"),
                         ["https://example.org/x"])
        self.assertEqual(t("[ref]: docs/COMMANDS.md"), ["docs/COMMANDS.md"])
        self.assertEqual(t("an array[i](x) is not written like this"), ["x"])  # known limit

    def test_cited_paths(self):
        c = check_links.cited_paths
        self.assertEqual(c("`src/server/cmd_help.c`, `tools/quality.sh` and `tests/*.c`."),
                         ["src/server/cmd_help.c", "tools/quality.sh", "tests/*.c"])
        self.assertEqual(c("`src/a.c:12`, `src/b.c:3-9`"), ["src/a.c", "src/b.c"])
        # Commands, placeholders and paths outside the tree are not files.
        self.assertEqual(c("`tools/quality.sh format`, `src/<name>.c`, `build/x`, `-Wall`"), [])


class Tree(unittest.TestCase):
    """check_internal() on a small tree with one error of each kind."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = check_links.ROOT
        check_links.ROOT = self.tmp.name
        os.makedirs(os.path.join(self.tmp.name, "docs"))
        os.makedirs(os.path.join(self.tmp.name, "src"))
        self.write("src/real.c", "")
        self.write("docs/OTHER.md", "# Other\n\n## A section\n\n## A section\n")

    def tearDown(self):
        check_links.ROOT = self.root
        self.tmp.cleanup()

    def write(self, path, text):
        with open(os.path.join(self.tmp.name, path), "w", encoding="utf-8") as f:
            f.write(text)

    def test_good_and_broken_links(self):
        self.write("README.md", "\n".join([
            "# Title",
            "[ok](docs/OTHER.md) [ok](docs/OTHER.md#a-section) [ok](docs/OTHER.md#a-section-1)",
            "[ok](#title) [ok](docs/) `src/real.c` `src/*.c` <https://example.org/>",
            "[bad](docs/MISSING.md)",
            "[bad](docs/OTHER.md#no-such-heading)",
            "[bad](#nowhere)",
            "[bad](../outside.md)",
            "`src/gone.c` and `src/*.h`",
            "```",
            "[ignored](in/a/code/block.md) `src/ignored.c`",
            "```",
        ]) + "\n")
        errors, external = check_links.check_internal(["README.md", "docs/OTHER.md"])
        self.assertEqual(list(external), ["https://example.org/"])
        self.assertEqual([e.split(": ", 1)[1] for e in errors], [
            "docs/MISSING.md does not exist",
            "docs/OTHER.md has no heading for #no-such-heading",
            "no heading for #nowhere",
            "../outside.md points outside the repository",
            "`src/gone.c` does not exist",
            "`src/*.h` does not exist",
        ])
        self.assertTrue(errors[0].startswith("README.md:4:"))

    def test_history_files_may_name_files_that_are_gone(self):
        self.write("CHANGELOG.md", "- moved `src/gone.c`; see [x](docs/MISSING.md)\n")
        errors, _ = check_links.check_internal(["CHANGELOG.md"])
        self.assertEqual(len(errors), 1)
        self.assertIn("docs/MISSING.md does not exist", errors[0])


class External(unittest.TestCase):
    def test_fetch(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def answer(self):
                code = {"/ok": 200, "/gone": 404, "/private": 403, "/busy": 429}.get(self.path, 500)
                # A server that knows GET only.
                if self.path == "/get-only":
                    code = 405 if self.command == "HEAD" else 200
                self.send_response(code)
                self.end_headers()

            do_HEAD = answer
            do_GET = answer

            def log_message(self, *args):
                pass

        os.environ["NO_PROXY"] = "127.0.0.1"  # a proxy of the environment is not for this
        server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        base = "http://127.0.0.1:%d" % server.server_address[1]
        try:
            self.assertIsNone(check_links.fetch(base + "/ok"))
            self.assertIsNone(check_links.fetch(base + "/get-only"))
            self.assertIsNone(check_links.fetch(base + "/private"))  # refuses scripts
            self.assertIsNone(check_links.fetch(base + "/busy"))
            self.assertEqual(check_links.fetch(base + "/gone"), "HTTP 404")
            errors = check_links.check_external({base + "/gone": "README.md:3",
                                                 base + "/ok": "README.md:4"})
            self.assertEqual(errors, ["README.md:3: %s/gone: HTTP 404" % base])
        finally:
            server.shutdown()
            server.server_close()
        self.assertIsNotNone(check_links.fetch(base + "/ok", timeout=2))  # nobody listens now


if __name__ == "__main__":
    unittest.main()
