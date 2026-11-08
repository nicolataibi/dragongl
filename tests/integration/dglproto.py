# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Minimal client for the Dragon GL wire protocol (PROTOCOL_VERSION 4).

Used only by the integration tests; mirrors src/shared/wire.c (frames of
u16 version, u16 type, u32 length, little-endian payloads, strings as u8
length + bytes).
"""

import select
import socket
import struct
import time

PROTOCOL_VERSION = 4

MSG_LOGIN = 1
MSG_WELCOME = 2
MSG_MOVE = 3
MSG_STATE = 4
MSG_MAP_CHUNK = 5
MSG_TEXT_CMD = 6
MSG_TEXT = 7
MSG_AUTH_FAIL = 8
MSG_COMBAT_EVENT = 13

HEADER = struct.Struct("<HHI")  # version, type, length


def header(msg_type, length, version=PROTOCOL_VERSION):
    return HEADER.pack(version, msg_type, length)


def lstr(text, cap):
    """u8 length + bytes; the receiving field holds cap-1 bytes."""
    raw = text.encode() if isinstance(text, str) else bytes(text)
    if len(raw) > 255:
        raise ValueError("string too long for the wire")
    return bytes([len(raw)]) + raw


def frame(msg_type, payload):
    return header(msg_type, len(payload)) + payload


def login_payload(username, password, server_pass, race=0, char_class=0):
    return (lstr(username, 32) + lstr(password, 64) + lstr(server_pass, 64) +
            struct.pack("<4b", race, -1, char_class, 0))


def text_cmd_payload(text):
    return lstr(text, 256)


def move_payload(dx, dy):
    return struct.pack("<2b", dx, dy)


def _read_str(payload, off):
    n = payload[off]
    return payload[off + 1:off + 1 + n].decode(errors="replace"), off + 1 + n


class Client:
    """One TCP connection to the server, with a tiny message decoder."""

    def __init__(self, port, host="127.0.0.1", timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self._buf = b""
        self.texts = []      # MSG_TEXT payloads, decoded
        self.auth_fail = []  # MSG_AUTH_FAIL reasons
        self.welcomed = False
        self.entity_id = None
        self.position = None  # (x, y) from MSG_WELCOME / own MSG_STATE
        self.closed_by_peer = False
        self.combat_events = []  # (kind, entity_id, amount)

    # --- sending ---------------------------------------------------------
    def login(self, username, password, server_pass):
        self.sock.sendall(frame(MSG_LOGIN, login_payload(username, password, server_pass)))

    def move(self, dx, dy):
        self.sock.sendall(frame(MSG_MOVE, move_payload(dx, dy)))

    def send_raw(self, data):
        self.sock.sendall(data)

    def command(self, text):
        self.sock.sendall(frame(MSG_TEXT_CMD, text_cmd_payload(text)))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    # --- receiving -------------------------------------------------------
    def pump(self, seconds):
        """Read and decode everything that arrives within `seconds`."""
        deadline = time.monotonic() + seconds
        while not self.closed_by_peer:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            ready, _, _ = select.select([self.sock], [], [], left)
            if not ready:
                break
            try:
                data = self.sock.recv(65536)
            except (ConnectionResetError, OSError):
                data = b""
            if not data:
                self.closed_by_peer = True
                break
            self._buf += data
            self._decode()

    def wait_for_text(self, needle, timeout):
        """Pump until a MSG_TEXT containing `needle` arrives; return the
        elapsed time in seconds, or None on timeout."""
        start = time.monotonic()
        while time.monotonic() - start < timeout:
            if any(needle in t for t in self.texts):
                return time.monotonic() - start
            self.pump(0.05)
            if self.closed_by_peer:
                break
        return None

    def wait_until_closed(self, timeout):
        """Pump until the server closes the connection; return the elapsed
        time in seconds, or None if it is still open after `timeout`."""
        start = time.monotonic()
        while time.monotonic() - start < timeout:
            self.pump(0.1)
            if self.closed_by_peer:
                return time.monotonic() - start
        return None

    def wait_for_login(self, timeout=10.0):
        start = time.monotonic()
        while time.monotonic() - start < timeout:
            if self.welcomed or self.auth_fail or self.closed_by_peer:
                break
            self.pump(0.05)
        return self.welcomed

    def _decode(self):
        while len(self._buf) >= HEADER.size:
            _version, msg_type, length = HEADER.unpack_from(self._buf)
            if len(self._buf) < HEADER.size + length:
                return
            payload = self._buf[HEADER.size:HEADER.size + length]
            self._buf = self._buf[HEADER.size + length:]
            if msg_type == MSG_WELCOME:
                self.welcomed = True
                self.entity_id, x, y = struct.unpack_from("<3i", payload)
                self.position = (x, y)
            elif msg_type == MSG_STATE:
                # u64 gold, then i32 entity_id, x, y
                entity_id, x, y = struct.unpack_from("<3i", payload, 8)
                if entity_id == self.entity_id:
                    self.position = (x, y)
            elif msg_type == MSG_TEXT:
                self.texts.append(_read_str(payload, 0)[0])
            elif msg_type == MSG_AUTH_FAIL:
                self.auth_fail.append(_read_str(payload, 0)[0])
            elif msg_type == MSG_COMBAT_EVENT:
                kind, entity_id, amount = struct.unpack("<Bii", payload)
                self.combat_events.append((kind, entity_id, amount))
