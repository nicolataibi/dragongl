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
1. **Server Access Password** (required, no default):
   - Acts as a perimeter gate. All incoming connections must supply the correct server password in `MsgLogin`.
   - Kept in memory only as a keyed BLAKE2b digest (random per-process key) and compared in constant time.
   - Connections failing this check receive `MSG_AUTH_FAIL` and are immediately closed.
2. **Player Account Password**:
   - Each character has its own password, hashed with **Argon2id** (libsodium `crypto_pwhash_str`, interactive limits, random salt) when the character is created. The `.save` file stores only that string.
   - The plaintext is verified once per login and wiped: it is not kept for the session, and every save writes the stored hash.
3. **Dungeon Master Account** (optional):
   - Administrative commands (`dm_*`) require authenticating as the reserved user `dm` with a dedicated password, kept only as an Argon2id hash.
   - Without a DM password the account is disabled.
4. **Where secrets come from:** a file (`--password-file`, `--dm-password-file`; a warning is printed if it is readable by group/others) or the environment (`DRAGONGL_SERVER_PASSWORD`, `DRAGONGL_DM_PASSWORD`). Command-line passwords are refused, because every local user can read them with `ps`. The server wipes the environment values in place at start-up, so they disappear from `/proc/<pid>/environ`. Server secrets have 8-31 printable ASCII characters without spaces.
5. **Transport:** the protocol is not encrypted. Passwords travel in clear over TCP: run the server on a trusted LAN, or behind a VPN or TLS tunnel.
6. **Known limit:** Argon2id runs on the main loop, so each login pauses the game for roughly 50-100 ms. The per-address limits below bound how often this can be triggered.

### B. Network Framing & Buffer Bounds
- **Strict Packet Bounds:** All network messages begin with a 12-byte `MsgHeader` containing an explicit `length` field and `PROTOCOL_VERSION`.
- **String Sanitization:** Text buffers (`username`, `password`, `cmd`, `text`) are constrained to fixed boundaries (32 to 256 bytes) and guaranteed NUL-terminated upon receipt.
- **Protocol Version Lock:** Any packet arriving with an unexpected `version` is rejected immediately, disconnecting out-of-date or malformed clients before memory allocation occurs.

Text commands are stripped of control characters (bytes below 0x20 and 0x7F) before they reach the game, so chat and private messages cannot carry terminal escape sequences to other players. The RNG seed is not logged.

### C. Denial-of-Service Resistance
- **Non-blocking server loop:** Each client has a receive buffer and a send queue (`src/server/server_conn.c`). A slow, silent or half-open connection can never stall the game loop for the other players.
- **Admission control:** Connections that cannot be served (server full, too many pending logins from one address, address locked out) receive `MSG_AUTH_FAIL` and are closed immediately; their descriptors are never leaked.
- **Login limits:** 10 s to authenticate after connecting; 5 failed logins within 60 s lock the address out for 60 s.
- **Back-pressure:** A client that lets more than 2 MiB of output pile up is disconnected.
- **Peer resets:** `SIGPIPE` is ignored and every send uses `MSG_NOSIGNAL`, so a connection closed by the peer is handled as an ordinary disconnect.
- **One session per character:** a second login of a character already online is refused, so two sessions can never save the same character independently (item duplication). TCP keepalive releases the character of a client that vanished without closing the connection within about 90 s.
- **Single disconnect path:** A connection with an error is only marked; the main loop removes it once per iteration (save, notify, close). A rejected login clears its authentication state first, so the reaper never saves a character whose password was wrong.

### D. Persistent State
- **Separate state directory:** the server writes only to its state directory (`--state-dir`, `$DRAGONGL_STATE_DIR`, `$XDG_STATE_HOME/dragongl`, `~/.local/state/dragongl`), created with mode 0700; the JSON data directory is only read. Files are created with mode 0600.
- **Single writer:** the running server holds an `flock` on `server.lock`; a second server on the same directory is refused, and `dragongl-reset-world` refuses to run under a live server.
- **Untrusted files:** every file is a versioned, CRC-checked container whose fields are read one by one; counts, enums and coordinates are range-checked, and the payload must be consumed exactly. A corrupt or foreign file is refused, never half-loaded: an unreadable save refuses the login and is kept, an unreadable world triggers a new one.
- **Durable writes:** temporary file, `fsync`, atomic `rename`, `fsync` of the directory; on failure the previous file is kept and the error is logged.

### E. Concurrency & Thread Safety
Dragon GL separates networking I/O from rendering:
- **Lock-Free Frame Snapshots:** The client render loop works on an immutable `FrameSnapshot` captured each frame, avoiding dirty reads while the network thread receives map chunks and state updates.
- **Granular Mutexes:** Shared visualization pools (Floating Combat Text and particle systems) use dedicated mutexes (`g_particles_mutex`, `g_fct_mutex`) to eliminate data races under heavy combat loads.

### F. Compiler Hardening & Fedora Compliance
Dragon GL enforces Fedora-standard compiler security flags across all builds:
- Position Independent Executable (`-fPIE` / `-pie`).
- Stack smashing protection (`-fstack-protector-strong`).
- Format string vulnerabilities defense (`-Wformat -Wformat-security`).
- Full RELRO and BIND_NOW (`-Wl,-z,relro,-z,now`).
- Source fortification (`-D_FORTIFY_SOURCE=2` in Release builds).
