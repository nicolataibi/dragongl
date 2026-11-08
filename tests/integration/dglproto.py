# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Minimal client for the Dragon GL wire protocol (PROTOCOL_VERSION 3).

Used only by the integration tests. Layouts mirror include/protocol.h on
x86-64 / AArch64 Linux (little-endian, 4-byte int).
"""

import select
import socket
import struct
import time

PROTOCOL_VERSION = 3

MSG_LOGIN = 1
MSG_WELCOME = 2
MSG_MOVE = 3
MSG_STATE = 4
MSG_MAP_CHUNK = 5
MSG_TEXT_CMD = 6
MSG_TEXT = 7
MSG_AUTH_FAIL = 8

HEADER = struct.Struct("<IiHH")  # type, length, version, reserved
LOGIN_SIZE = 156
MOVE_SIZE = 16
TEXT_CMD_SIZE = 256


def header(msg_type, length):
    return HEADER.pack(msg_type, length, PROTOCOL_VERSION, 0)


def _cstr(text, size):
    raw = text.encode()
    if len(raw) >= size:
        raise ValueError("string too long for a %d-byte field" % size)
    return raw.ljust(size, b"\0")


def login_payload(username, password, server_pass, race=0, char_class=0):
    payload = _cstr(username, 32) + _cstr(password, 32) + _cstr(server_pass, 32)
    # race, subrace, class, is_new_char, str..cha, age, height, weight,
    # social_class, alignment
    payload += struct.pack("<15i", race, -1, char_class, 1,
                           0, 0, 0, 0, 0, 0, 20, 170, 70, 50, 0)
    assert len(payload) == LOGIN_SIZE
    return payload


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

    # --- sending ---------------------------------------------------------
    def login(self, username, password, server_pass):
        payload = login_payload(username, password, server_pass)
        self.sock.sendall(header(MSG_LOGIN, len(payload)) + payload)

    def move(self, dx, dy):
        payload = struct.pack("<4i", self.entity_id or 0, dx, dy, 0)
        self.sock.sendall(header(MSG_MOVE, MOVE_SIZE) + payload)

    def send_raw(self, data):
        self.sock.sendall(data)

    def command(self, text):
        payload = _cstr(text, TEXT_CMD_SIZE)
        self.sock.sendall(header(MSG_TEXT_CMD, len(payload)) + payload)

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
            msg_type, length, _version, _ = HEADER.unpack_from(self._buf)
            if len(self._buf) < HEADER.size + length:
                return
            payload = self._buf[HEADER.size:HEADER.size + length]
            self._buf = self._buf[HEADER.size + length:]
            if msg_type == MSG_WELCOME:
                self.welcomed = True
                self.entity_id, x, y = struct.unpack_from("<3i", payload)
                self.position = (x, y)
            elif msg_type == MSG_STATE:
                # MsgState: uint64 gold, then int entity_id, x, y
                entity_id, x, y = struct.unpack_from("<3i", payload, 8)
                if entity_id == self.entity_id:
                    self.position = (x, y)
            elif msg_type == MSG_TEXT:
                self.texts.append(payload.split(b"\0", 1)[0].decode(errors="replace"))
            elif msg_type == MSG_AUTH_FAIL:
                self.auth_fail.append(payload.split(b"\0", 1)[0].decode(errors="replace"))
