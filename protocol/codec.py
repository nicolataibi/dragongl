# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""The wire protocol of protocol/messages.json, in Python.

A table-driven codec: the same rules as the C codec that
tools/gen_protocol.py generates from the same table (src/shared/wire.c).
Used by the generator (test vectors, documentation) and by the
integration tests (tests/integration/dglproto.py).

    spec = Spec()                          # protocol/messages.json
    payload = spec.encode("LOGIN", {...})  # bytes after the header
    values = spec.decode("LOGIN", payload) # dict, or ProtocolError
    data = spec.frame("MOVE", {"dx": 1, "dy": 0})
"""

import json
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC_PATH = os.path.join(HERE, "messages.json")

HEADER = struct.Struct("<HHI")  # version, type, length
HEADER_SIZE = HEADER.size

SCALARS = {
    # type: (struct format, size, min, max)
    "i8": ("<b", 1, -128, 127),
    "i16": ("<h", 2, -32768, 32767),
    "i32": ("<i", 4, -(1 << 31), (1 << 31) - 1),
    "u8": ("<B", 1, 0, 255),
    "u16": ("<H", 2, 0, 65535),
    "u32": ("<I", 4, 0, (1 << 32) - 1),
    "u64": ("<Q", 8, 0, (1 << 64) - 1),
    "bool": ("<B", 1, 0, 1),
    "f32": ("<f", 4, None, None),
    "unit8": ("<B", 1, None, None),
}


class ProtocolError(ValueError):
    """A payload or a value the protocol does not allow."""


def unit_to_byte(v):
    """A colour channel 0..1 as one byte, as put_unit() in wire.c."""
    v = float(v)
    if not v > 0.0:
        v = 0.0
    if v > 1.0:
        v = 1.0
    return int(v * 255.0 + 0.5)


class Spec:
    def __init__(self, path=SPEC_PATH):
        with open(path, encoding="utf-8") as f:
            self.raw = json.load(f)
        self.version = self.raw["version"]
        self.constants = {k: v["value"] for k, v in self.raw["constants"].items()}
        self.constants.update({k: v["value"] for k, v in self.raw.get("external", {}).items()})
        self.records = self.raw.get("records", {})
        self.messages = self.raw["messages"]
        self.by_name = {m["name"]: m for m in self.messages}
        self.by_id = {m["id"]: m for m in self.messages}

    # --- values of the table --------------------------------------------
    def value(self, expr, fields=None):
        """An int, a constant name or an expression of constants (and of
        decoded fields)."""
        if expr is None or isinstance(expr, (int, float)):
            return expr
        names = dict(self.constants)
        if fields:
            names.update(fields)
        return int(eval(expr, {"__builtins__": {}}, names))  # noqa: S307 - our own table

    def bounds(self, field):
        """(min, max) a field may take: its own range, else its wire type's."""
        fmt = SCALARS.get(field["type"])
        lo = self.value(field.get("min"))
        hi = self.value(field.get("max"))
        if fmt:
            lo = fmt[2] if lo is None else lo
            hi = fmt[3] if hi is None else hi
        return lo, hi

    def field_max_size(self, field, fields_max):
        t = field["type"]
        count = self.value(field.get("count", 1))
        if t in SCALARS:
            return SCALARS[t][1] * count
        if t == "str":
            return (1 + self.value(field["cap"]) - 1) * count
        if t == "bytes":
            return self.value(field["length"], fields_max)
        if t == "records":
            rec = sum(SCALARS[f["type"]][1] for f in self.records[field["record"]]["fields"])
            return SCALARS[field["count_type"]][1] + rec * self.value(field["max"])
        raise ValueError("unknown field type %s" % t)

    def record_size(self, name):
        return sum(SCALARS[f["type"]][1] for f in self.records[name]["fields"])

    def max_payload(self, msg):
        fields_max = {}
        size = 0
        for f in msg["fields"]:
            if f["type"] in SCALARS:
                fields_max[f["name"]] = self.bounds(f)[1]
            size += self.field_max_size(f, fields_max)
        return size

    def msg(self, name_or_id):
        m = self.by_name.get(name_or_id) if isinstance(name_or_id, str) else \
            self.by_id.get(name_or_id)
        if m is None:
            raise KeyError(name_or_id)
        return m

    # --- encoding --------------------------------------------------------
    def _put_scalar(self, out, field, v, check):
        t = field["type"]
        fmt, _size, _lo, _hi = SCALARS[t]
        if t == "unit8":
            out += struct.pack("<B", unit_to_byte(v))
            return
        if t == "f32":
            out += struct.pack("<f", float(v))
            return
        v = int(v)
        lo, hi = self.bounds(field)
        if check and not lo <= v <= hi:
            raise ProtocolError("%s = %d outside %d..%d" % (field["name"], v, lo, hi))
        out += struct.pack(fmt, v)

    def _put_str(self, out, field, v, check):
        raw = v.encode("utf-8") if isinstance(v, str) else bytes(v)
        cap = self.value(field["cap"])
        if len(raw) > cap - 1 or len(raw) > 255:
            if check:
                raise ProtocolError("%s: %d bytes, at most %d" % (field["name"], len(raw), cap - 1))
        out += bytes([len(raw) & 0xFF]) + raw

    def _put_field(self, out, field, v, values, check):
        t = field["type"]
        if t == "bytes":
            length = self.value(field["length"], values)
            hi = self.value(field.get("max"))
            data = bytes(v)
            if check and (len(data) != length or (hi is not None and any(b > hi for b in data))):
                raise ProtocolError("%s: %d bytes for %d, or a byte above %s"
                                    % (field["name"], len(data), length, hi))
            out += data
            return
        if t == "records":
            lo, hi = self.value(field["min"]), self.value(field["max"])
            if check and not lo <= len(v) <= hi:
                raise ProtocolError("%s: %d records outside %d..%d" % (field["name"], len(v), lo, hi))
            out += struct.pack(SCALARS[field["count_type"]][0], len(v))
            for rec in v:
                for rf in self.records[field["record"]]["fields"]:
                    self._put_scalar(out, rf, rec[rf["name"]], check)
            return
        count = field.get("count")
        items = v if count is not None else [v]
        if count is not None and len(items) != self.value(count):
            raise ProtocolError("%s: %d items for %d" % (field["name"], len(items), self.value(count)))
        for item in items:
            if t == "str":
                self._put_str(out, field, item, check)
            else:
                self._put_scalar(out, field, item, check)

    def encode(self, name, values, check=True):
        """Payload of message `name`. check=False writes out-of-range
        values (for the vectors a decoder must refuse)."""
        msg = self.msg(name)
        out = bytearray()
        for f in msg["fields"]:
            self._put_field(out, f, values[f["name"]], values, check)
        if check:
            self._checks(msg, values)
        return bytes(out)

    def frame(self, name, values, version=None, check=True):
        msg = self.msg(name)
        payload = self.encode(name, values, check)
        return HEADER.pack(self.version if version is None else version, msg["id"],
                           len(payload)) + payload

    def _checks(self, msg, values):
        for c in msg.get("checks", []):
            if not self.value(c["expr"], values):
                raise ProtocolError("%s: %s" % (msg["name"], c["expr"]))

    # --- decoding --------------------------------------------------------
    def decode(self, name, payload):
        msg = self.msg(name)
        r = _Reader(payload)
        values = {}
        for f in msg["fields"]:
            values[f["name"]] = self._get_field(r, f, values)
        if r.pos != len(payload):
            raise ProtocolError("%s: %d trailing bytes" % (name, len(payload) - r.pos))
        self._checks(msg, values)
        return values

    def _get_scalar(self, r, field):
        t = field["type"]
        fmt, size, _lo, _hi = SCALARS[t]
        (v,) = struct.unpack(fmt, r.take(size))
        if t == "unit8":
            return v / 255.0
        if t == "f32":
            return v
        lo, hi = self.bounds(field)
        if not lo <= v <= hi:
            raise ProtocolError("%s = %d outside %d..%d" % (field["name"], v, lo, hi))
        return v

    def _get_field(self, r, field, values):
        t = field["type"]
        if t == "bytes":
            data = r.take(self.value(field["length"], values))
            hi = self.value(field.get("max"))
            if hi is not None and any(b > hi for b in data):
                raise ProtocolError("%s: a byte above %d" % (field["name"], hi))
            return list(data)
        if t == "records":
            (n,) = struct.unpack(SCALARS[field["count_type"]][0],
                                 r.take(SCALARS[field["count_type"]][1]))
            lo, hi = self.value(field["min"]), self.value(field["max"])
            if not lo <= n <= hi:
                raise ProtocolError("%s: %d records outside %d..%d" % (field["name"], n, lo, hi))
            return [{rf["name"]: self._get_scalar(r, rf)
                     for rf in self.records[field["record"]]["fields"]} for _ in range(n)]
        count = field.get("count")
        items = []
        for _ in range(self.value(count) if count is not None else 1):
            if t == "str":
                n = r.take(1)[0]
                if n >= self.value(field["cap"]):
                    raise ProtocolError("%s: %d bytes, at most %d"
                                        % (field["name"], n, self.value(field["cap"]) - 1))
                items.append(r.take(n).decode("utf-8", errors="replace"))
            else:
                items.append(self._get_scalar(r, field))
        return items if count is not None else items[0]


class _Reader:
    def __init__(self, data):
        self.data = bytes(data)
        self.pos = 0

    def take(self, n):
        if n < 0 or self.pos + n > len(self.data):
            raise ProtocolError("truncated payload")
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out
