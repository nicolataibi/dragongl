# Dragon GL - Copyright (C) 2026 Nicola Taibi
# License: GPL-3.0-or-later
"""Minimal client for the Dragon GL wire protocol, for the integration tests.

The messages come from the protocol table (protocol/messages.json),
through its Python codec (protocol/codec.py): the same rules as the C codec
generated from that table. Frames travel inside TLS 1.3 with the ALPN
protocol "dragongl/<version>".
"""

import hashlib
import os
import select
import socket
import ssl
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                                "protocol"))
import codec  # noqa: E402

SPEC = codec.Spec()
PROTOCOL_VERSION = SPEC.version
ALPN = "dragongl/%d" % PROTOCOL_VERSION

for _m in SPEC.messages:
    globals()["MSG_" + _m["name"]] = _m["id"]

HEADER = codec.HEADER  # version, type, length


def header(msg_type, length, version=PROTOCOL_VERSION):
    return HEADER.pack(version, msg_type, length)


def lstr(text, cap):
    """u8 length + bytes, unchecked (to build malformed payloads)."""
    raw = text.encode() if isinstance(text, str) else bytes(text)
    if len(raw) > 255:
        raise ValueError("string too long for the wire")
    return bytes([len(raw)]) + raw


def frame(msg_type, payload):
    return header(msg_type, len(payload)) + payload


def login_payload(username, password, server_pass, race=0, char_class=0):
    return SPEC.encode("LOGIN", {"username": username, "password": password,
                                 "server_pass": server_pass, "race_id": race, "subrace_id": -1,
                                 "class_id": char_class, "alignment": 0})


def text_cmd_payload(text):
    return SPEC.encode("TEXT_CMD", {"cmd": text})


def move_payload(dx, dy):
    return SPEC.encode("MOVE", {"dx": dx, "dy": dy})


def tls_context(alpn=ALPN):
    """TLS 1.3 client context. The tests check the server certificate by
    its fingerprint (Client.fingerprint), not by an authority."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    if alpn:
        ctx.set_alpn_protocols([alpn])
    return ctx


def fingerprint_of_der(der):
    """'SHA256:AB:CD:...' of a DER certificate, as the server logs it."""
    digest = hashlib.sha256(der).hexdigest().upper()
    return "SHA256:" + ":".join(digest[i:i + 2] for i in range(0, len(digest), 2))


class Client:
    """One TLS connection to the server, with a tiny message decoder.

    tls=False opens a bare TCP connection (to check that the server refuses
    it); alpn=None offers no application protocol."""

    def __init__(self, port, host="127.0.0.1", timeout=5.0, tls=True, alpn=ALPN):
        raw = socket.create_connection((host, port), timeout=timeout)
        self.fingerprint = None
        self.alpn = None
        if tls:
            try:
                self.sock = tls_context(alpn).wrap_socket(raw)
            except Exception:
                raw.close()
                raise
            self.fingerprint = fingerprint_of_der(self.sock.getpeercert(binary_form=True))
            self.alpn = self.sock.selected_alpn_protocol()
        else:
            self.sock = raw
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
            # Decrypted bytes kept by TLS are not seen by select().
            pending = isinstance(self.sock, ssl.SSLSocket) and self.sock.pending()
            if not pending:
                ready, _, _ = select.select([self.sock], [], [], left)
                if not ready:
                    break
            try:
                data = self.sock.recv(65536)
            except (ConnectionResetError, ssl.SSLError, OSError):
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
        """Every frame through the strict decoder of the table: a malformed
        message from the server raises codec.ProtocolError."""
        while len(self._buf) >= HEADER.size:
            version, msg_type, length = HEADER.unpack_from(self._buf)
            if len(self._buf) < HEADER.size + length:
                return
            payload = self._buf[HEADER.size:HEADER.size + length]
            self._buf = self._buf[HEADER.size + length:]
            if version != PROTOCOL_VERSION:
                raise codec.ProtocolError("frame of version %d" % version)
            m = SPEC.decode(SPEC.msg(msg_type)["name"], payload)
            if msg_type == MSG_WELCOME:
                self.welcomed = True
                self.entity_id = m["entity_id"]
                self.position = (m["x"], m["y"])
            elif msg_type == MSG_STATE:
                if m["entity_id"] == self.entity_id:
                    self.position = (m["x"], m["y"])
            elif msg_type == MSG_TEXT:
                self.texts.append(m["text"])
            elif msg_type == MSG_AUTH_FAIL:
                self.auth_fail.append(m["reason"])
            elif msg_type == MSG_COMBAT_EVENT:
                self.combat_events.append((m["kind"], m["entity_id"], m["amount"]))
