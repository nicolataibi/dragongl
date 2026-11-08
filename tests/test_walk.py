#!/usr/bin/env python3
"""Test client: logs in, walks the character around, optionally descends
stairs to a target floor (default: 100 = deepest, to test the STAIRS_DOWN
boundary at MAX_FLOORS-1).

Usage: python3 test_walk.py [host] [port] [target_floor] [username]
"""
import os, socket, struct, sys, time, random

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
TARGET_FLOOR = int(sys.argv[3]) if len(sys.argv) > 3 else 100
USER = sys.argv[4] if len(sys.argv) > 4 else f"test_{random.randint(10000,99999)}"

MSG_LOGIN, MSG_WELCOME, MSG_MOVE = 1, 2, 3
MSG_STATE, MSG_MAP_CHUNK, MSG_TEXT_CMD = 4, 5, 6
MSG_TEXT, MSG_AUTH_FAIL, MSG_SPELL_VFX = 7, 8, 9
MSG_TOMBSTONE_REMOVE, MSG_TIME_SYNC, MSG_ENTITY_UPDATE = 10, 11, 12
PROTOCOL_VERSION = 3

SIZE_MOVE = 16

def build_login(user, pwd, spass, new=1):
    b = user.encode().ljust(32, b'\0')
    b += pwd.encode().ljust(32, b'\0')
    b += spass.encode().ljust(32, b'\0')
    b += struct.pack("<16i", 0, -1, 0, new, 10, 10, 10, 10, 10, 10, 0, 0, 0, 0, 0)
    return b

def hdr(mtype, ln):
    return struct.pack("<iiHH", mtype, ln, PROTOCOL_VERSION, 0)

class Net:
    def __init__(self, s):
        self.s = s
        self.buf = b''
    def recv_exact(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise ConnectionError("closed")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out
    def next_msg(self):
        h = struct.unpack("<iiHH", self.recv_exact(12))
        t, ln = h[0], h[1]
        if ln < 0 or ln > 4 * 1024 * 1024:
            raise ValueError(f"bad length {ln}")
        return t, self.recv_exact(ln)

def drain(n, seconds, on_text):
    end = time.time() + seconds
    got = 0
    while time.time() < end:
        try:
            t, body = n.next_msg()
        except socket.timeout:
            continue
        if t == MSG_TEXT:
            txt = body.rstrip(b'\0').decode(errors='replace')
            on_text(txt)
        got += 1
    return got

def main():
    s = socket.create_connection((HOST, PORT), timeout=10)
    s.settimeout(0.5)
    n = Net(s)
    # Same variable the server reads: there is no default gate password.
    server_pass = os.environ.get("DRAGONGL_SERVER_PASSWORD", "")
    payload = build_login(USER, "testpw", server_pass, 1)
    s.sendall(hdr(MSG_LOGIN, len(payload)) + payload)

    eid = x = y = None
    for _ in range(20000):
        t, body = n.next_msg()
        if t == MSG_AUTH_FAIL:
            print("AUTH FAIL:", body.rstrip(b'\0').decode(errors='replace'))
            return 1
        if t == MSG_WELCOME:
            eid, x, y = struct.unpack("<3i", body[:12])
            hp = struct.unpack("<i", body[20:24])[0]
            print(f"LOGGED IN as {USER}: id={eid} pos=({x},{y}) hp={hp}")
            break
    if eid is None:
        print("no welcome")
        return 1

    cur_floor = 0
    pos = (x, y)
    steps = 0
    max_steps = 6000
    floor_changes = 0

    def on_text(txt):
        low = txt.lower()
        if any(k in low for k in ("died", "death", "stairs", "boss", "floor")):
            print("   TXT:", txt[:130])

    while steps < max_steps:
        dx, dy = random.choice([(-1,0),(1,0),(0,-1),(0,1)])
        s.sendall(hdr(MSG_MOVE, SIZE_MOVE) + struct.pack("<4i", eid, dx, dy, 0))
        steps += 1
        try:
            end = time.time() + 0.25
            while time.time() < end:
                t, body = n.next_msg()
                if t == MSG_STATE:
                    nx, ny = struct.unpack("<2i", body[12:20])
                    nf = struct.unpack("<i", body[28:32])[0]
                    if (nx, ny) != pos:
                        pos = (nx, ny)
                    if nf != cur_floor:
                        floor_changes += 1
                        print(f"*** FLOOR CHANGE: {cur_floor} -> {nf} at {pos}")
                        cur_floor = nf
                elif t == MSG_TEXT:
                    on_text(body.rstrip(b'\0').decode(errors='replace'))
        except (ConnectionError, OSError) as e:
            print(f"CONNECTION CLOSED at step {steps}, floor {cur_floor}, pos {pos}: {e}")
            return 1

        if cur_floor >= TARGET_FLOOR:
            print(f"REACHED FLOOR {cur_floor} at {pos} — stepping in all directions to hit the STAIRS_DOWN tile")
            # search around: try many moves, watching for floor 101 (crash) or close
            for _ in range(40):
                d = random.choice([(-1,0),(1,0),(0,-1),(0,1)])
                s.sendall(hdr(MSG_MOVE, SIZE_MOVE) + struct.pack("<4i", eid, d[0], d[1], 0))
                try:
                    t, body = n.next_msg()
                    if t == MSG_STATE:
                        nx, ny = struct.unpack("<2i", body[12:20])
                        nf = struct.unpack("<i", body[28:32])[0]
                        pos = (nx, ny)
                        if nf > TARGET_FLOOR:
                            print(f"!!! floor became {nf} — past the max floor!")
                except (ConnectionError, OSError) as e:
                    print(f"*** CONNECTION CLOSED on floor {cur_floor} (likely server crash): {e}")
                    return 2
                except socket.timeout:
                    break
                time.sleep(0.21)
            break

    # final drain
    try:
        drain(n, 1.5, on_text)
    except ConnectionError:
        print("CONNECTION CLOSED during final drain (server crash?)")
        return 1
    print(f"DONE: steps={steps} floor={cur_floor} pos={pos} floor_changes={floor_changes}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
