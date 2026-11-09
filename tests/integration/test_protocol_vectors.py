# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""The Python codec of the protocol table against the test vectors.

tests/protocol_vectors.json is generated from protocol/messages.json; the
C codec is checked against the same vectors by test_protocol_conformance.
Here the Python codec (protocol/codec.py, used by dglproto.py) must give
the same bytes and refuse the same payloads.
"""

import json
import math
import os
import unittest

import dglproto

VECTORS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "protocol_vectors.json")
SPEC = dglproto.SPEC


def same(a, b):
    if isinstance(a, float) or isinstance(b, float):
        return math.isclose(a, b, abs_tol=1.0 / 255 / 2)
    if isinstance(a, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    return a == b


class ProtocolVectorsTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        with open(VECTORS, encoding="utf-8") as f:
            cls.vec = json.load(f)

    def test_vectors_are_for_this_version(self):
        self.assertEqual(self.vec["version"], SPEC.version)
        self.assertEqual({v["message"] for v in self.vec["valid"]},
                         {m["name"] for m in SPEC.messages}, "a message without an example")

    def test_valid_vectors_encode_and_decode(self):
        for v in self.vec["valid"]:
            with self.subTest(message=v["message"], example=v["example"]):
                want = bytes.fromhex(v["frame"])
                self.assertEqual(SPEC.frame(v["message"], v["values"]), want)
                back = SPEC.decode(v["message"], want[dglproto.HEADER.size:])
                self.assertTrue(same(back, v["values"]), back)
                self.assertLessEqual(len(want) - dglproto.HEADER.size,
                                     SPEC.max_payload(SPEC.msg(v["message"])))

    def test_invalid_vectors_are_refused(self):
        for v in self.vec["invalid"]:
            with self.subTest(message=v["message"], why=v["why"]):
                with self.assertRaises(dglproto.codec.ProtocolError):
                    SPEC.decode(v["message"], bytes.fromhex(v["payload"]))
                if v["values"] is not None:
                    with self.assertRaises(dglproto.codec.ProtocolError):
                        SPEC.encode(v["message"], v["values"])


if __name__ == "__main__":
    unittest.main()
