# Dragon GL — Security Policy & Threat Model

This document outlines the security architecture, threat model, authentication safeguards, and responsible disclosure procedures for Dragon GL.

---

## 1. Supported Versions

Security fixes and patches are applied to the active development branch and packaged for Fedora Linux:

| Version | Status |
| :--- | :--- |
| `2026.09.x` / Current master | Supported |
| Legacy releases | Best effort |

---

## 2. Reporting a Vulnerability

If you discover a potential vulnerability in Dragon GL (such as remote denial of service, memory corruption in packet processing, or authentication bypass), please report it responsibly:

- **Contact:** Open a private security advisory through the repository's security advisory portal or email `nicola.taibi.1967@gmail.com`.
- **Information to include:**
  - Detailed description of the vulnerability.
  - Minimal reproducible example or packet trace.
  - Affected components (`dragongl_server`, `dragongl_client`, or tools).
  - Target operating system and compiler version.

Please do not disclose potential vulnerabilities in public issue trackers until an assessment and patch can be coordinated.

---

## 3. Threat Model & Security Architecture

Dragon GL is a multi-user client-server game engine communicating over non-blocking TCP sockets. The security design focuses on defending the server process, player persistence data, and client render pipelines against malicious actors.

### A. Dual-Key Authentication & DM Separation
To protect server resources and player accounts from unauthorized access:
1. **Server Access Password (`--password`)**:
   - Acts as a perimeter gate. All incoming connections must supply the correct server password in `MsgLogin`.
   - Connections failing this check receive `MSG_AUTH_FAIL` and are immediately closed.
2. **Player Account Password (`password`)**:
   - Each player character is secured with an individual password.
   - Passwords are validated using salted hashes against saved binary profiles (`.save`), protecting character data from impersonation.
3. **Dedicated Dungeon Master Account (`--dm-password`)**:
   - Administrative commands (`dm_*`) require authenticating as the reserved user `dm`.
   - DM authentication uses an isolated, independent password configured at server boot (`-d/--dm-password`), hashed with a unique salt (`SERVER_DM_PASSWORD`).
   - Standard players cannot elevate privileges even if they know the general server gate password.

### B. Network Framing & Buffer Bounds
- **Strict Packet Bounds:** All network messages begin with a 12-byte `MsgHeader` containing an explicit `length` field and `PROTOCOL_VERSION`.
- **String Sanitization:** Text buffers (`username`, `password`, `cmd`, `text`) are constrained to fixed boundaries (32 to 256 bytes) and guaranteed NUL-terminated upon receipt.
- **Protocol Version Lock:** Any packet arriving with an unexpected `version` is rejected immediately, disconnecting out-of-date or malformed clients before memory allocation occurs.

### C. Denial-of-Service Resistance
- **Non-blocking server loop:** Each client has a receive buffer and a send queue (`src/server/server_conn.c`). A slow, silent or half-open connection can never stall the game loop for the other players.
- **Admission control:** Connections that cannot be served (server full, too many pending logins from one address, address locked out) receive `MSG_AUTH_FAIL` and are closed immediately; their descriptors are never leaked.
- **Login limits:** 10 s to authenticate after connecting; 5 failed logins within 60 s lock the address out for 60 s.
- **Back-pressure:** A client that lets more than 2 MiB of output pile up is disconnected.
- **Peer resets:** `SIGPIPE` is ignored and every send uses `MSG_NOSIGNAL`, so a connection closed by the peer is handled as an ordinary disconnect.
- **One session per character:** a second login of a character already online is refused, so two sessions can never save the same character independently (item duplication). TCP keepalive releases the character of a client that vanished without closing the connection within about 90 s.
- **Single disconnect path:** A connection with an error is only marked; the main loop removes it once per iteration (save, notify, close). A rejected login clears its authentication state first, so the reaper never saves a character whose password was wrong.

### D. Concurrency & Thread Safety
Dragon GL separates networking I/O from rendering:
- **Lock-Free Frame Snapshots:** The client render loop works on an immutable `FrameSnapshot` captured each frame, avoiding dirty reads while the network thread receives map chunks and state updates.
- **Granular Mutexes:** Shared visualization pools (Floating Combat Text and particle systems) use dedicated mutexes (`g_particles_mutex`, `g_fct_mutex`) to eliminate data races under heavy combat loads.

### E. Compiler Hardening & Fedora Compliance
Dragon GL enforces Fedora-standard compiler security flags across all builds:
- Position Independent Executable (`-fPIE` / `-pie`).
- Stack smashing protection (`-fstack-protector-strong`).
- Format string vulnerabilities defense (`-Wformat -Wformat-security`).
- Full RELRO and BIND_NOW (`-Wl,-z,relro,-z,now`).
- Source fortification (`-D_FORTIFY_SOURCE=2` in Release builds).
