# Dragon GL — Security Policy & Threat Model

This document outlines the security architecture, threat model, authentication safeguards, and responsible disclosure procedures for Dragon GL.

---

## 1. Supported Versions

Security fixes and patches are applied to the active development branch and packaged for Fedora Linux:

| Version | Status |
| :--- | :--- |
| `2026.11.x` / current `main` | Supported |
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

Dragon GL is a multi-user client-server game: one server process, reachable over the network, and clients run by players on their own machines. This section says what is protected, from whom, and how; the subsections after it describe each defence.

### Assets

| Asset | Where it lives | What an attacker would want |
| :--- | :--- | :--- |
| Server access password, DM password | server memory (digest / Argon2id hash); the client while typing them | join a private server, run `dm_*` commands |
| Character passwords | Argon2id hashes in `saves/*.save`; in clear only in the client and, for one login, in the server's login worker | take over a character |
| Characters, world, NPCs | the state directory (`world.dat`, `npcs.dat`, `saves/`) | items or gold for free, destroy others' progress |
| Availability of the game loop | the server process | stall or crash the server for everybody |
| The player's machine | the client process | code execution through a malicious server |
| The server's TLS key | `<state dir>/tls/server.key` (mode 0600) or `--tls-key` | impersonate the server |

### Actors and trust boundaries

- **Network attacker** (passive or active, between a client and the server): may read, change, replay, block or redirect any packet. Trust boundary: the TLS channel.
- **Unauthenticated peer**: anyone who can open a TCP connection to the server port. Boundary: the handshake, then `MSG_LOGIN` with the server password.
- **Authenticated player**: has the server password and a character; may send any well-framed message and any text command, in any order, as fast as the network allows. Boundary: the strict decoder and the command handlers; the game state belongs to the server alone (the client only renders what it is told).
- **Malicious server** (for a client): a server the player connects to and does not control. Boundary: the client's strict decoder; the client trusts the server's game state, never its bytes.
- **Local user** of the server host: other accounts on the same machine. Boundary: file modes (state directory 0700, files and key 0600), no secrets on the command line, the environment wiped after start-up, the systemd unit's sandbox (`DynamicUser`, `ProtectSystem=strict`, ...).
- **Out of scope**: an attacker with root on the server host or on the player's machine; a server owner who is malicious toward the players of their own server (they hold the world by design); physical access.

### Threats and mitigations

| Threat | Mitigation | Where |
| :--- | :--- | :--- |
| Eavesdropping on passwords, chat and game state | TLS 1.3 only (OpenSSL 3), forward-secret key exchange; no TLS 1.2 or older, no compression, no renegotiation | `src/shared/tls_common.c` |
| Server impersonation (man in the middle) | the client accepts a server only by a given fingerprint (`--fingerprint`), a certificate an authority the system trusts signed for the host name, or the fingerprint recorded at the first connection (`known_servers`); a changed certificate is refused with a warning | `src/client/client_tls.c`, `known_servers.c` |
| First-connection interception (trust on first use) | the server logs its fingerprint at start-up and the client prints it at the first connection, for the player to compare with the owner's; `--fingerprint` or a CA-signed certificate (`--tls-cert`) removes the window | `PROTOCOL.md` §1, `INSTALL.md` |
| Old or foreign clients misreading the stream | ALPN `dragongl/<version>`: another version fails the handshake; a frame of another version is a protocol error | `tls_common.c`, `server_conn.c` |
| Password guessing | gate password digest compared in constant time; Argon2id per character; 5 failures in 60 s lock the address for 60 s | `auth.c`, `server_conn.c` |
| Memory corruption through packets | one generated codec with strict decoding (lengths before payloads, exact consumption, every count, enum, coordinate and tile range-checked); fuzzed (`tests/fuzz/fuzz_wire.c`) under ASan/UBSan; the same codec in the client | `src/shared/wire.c`, `protocol/messages.json` |
| Corruption through text commands | numbers parsed with overflow checks (`strnum.h`); control characters stripped; every handler fuzzed (`fuzz_text_cmd.c`); usernames restricted to `[A-Za-z0-9_]{1,16}` (they become file names) | `server_commands.c`, `cmd_*.c` |
| Corruption through save files | versioned CRC-checked containers, field-by-field reads, range checks; fuzzed (`fuzz_persist.c`) | `persist.c`, `binio.c` |
| Denial of service by slow or silent peers | non-blocking loop; the TLS handshake is part of the 10 s login timeout; 8 pending connections per address; 2 MiB send queue limit; refused connections are told why from 16 dedicated slots for at most 3 s, then simply closed | `server_conn.c`, `main_server.c` |
| Denial of service by login cost | Argon2id runs in the login worker, off the game loop; the per-address limits bound the work one address can request | `login_worker.c` |
| Item duplication by parallel sessions | one session per character, a character being logged in counts as online | `main_server.c` |
| Secrets exposed to local users | passwords never on the command line; files warned when group/others can read them; environment wiped in place; key file created 0600 in a 0700 directory | `auth.c`, `server_tls.c` |
| Loss of the server identity | the key and certificate live with the world in the state directory; without them clients refuse the new certificate until their entry is removed | `server_tls.h` |

### Residual risks

- **Trust on first use:** a player who connects for the first time through an attacker and does not compare the fingerprint trusts the attacker. Use `--fingerprint`, or a certificate signed by a public or private authority.
- **Self-signed certificate lifetime:** the generated certificate is valid for 10 years and is not revoked if the key leaks: replace both files (clients then need the new fingerprint).
- **Authenticated players are trusted with the server's CPU:** the rate limits (5 steps and 10 text commands per second) bound, but do not remove, the load one player can cause.
- **No end-to-end protection between players:** private messages are readable by the server owner, as everything else in the game.

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
4. **Where secrets come from:** a file (`--password-file`, `--dm-password-file`; a warning is printed if it is readable by group/others) or the environment (`DRAGONGL_SERVER_PASSWORD`, `DRAGONGL_DM_PASSWORD`). Command-line passwords are refused, because every local user can read them with `ps`. The server wipes the environment values in place at start-up, so they disappear from `/proc/<pid>/environ`. Server secrets have 8-63 printable ASCII characters without spaces.
5. **Transport:** every connection is TLS 1.3 (see *Threats and mitigations*): passwords and game traffic are encrypted, and the client authenticates the server before it sends the login.
6. **Off the game loop:** Argon2id (verification, or hashing for a new character) and the reading of the save run in a login worker thread (`src/server/login_worker.c`); the game keeps ticking while a login is checked. The plaintext password exists only in the worker's job and is wiped after use. While a login is being checked, its character name counts as online, so two connections cannot create or load the same character at once. The per-address limits below still bound how much hashing one address can request.

### B. Network Framing & Buffer Bounds
- **Explicit encoding:** every message is serialized field by field, little-endian, by one module shared by server and client (`src/shared/wire.c`, generated from the table `protocol/messages.json`); no struct is sent or received as raw memory. Each frame starts with an 8-byte header (version, type, payload length).
- **Strict decoding:** the length is checked against the limit of the message type before any payload byte is read (256 bytes for anything a client may send); then the payload must decode exactly. Strings are length-prefixed and a string longer than its field (31 bytes for names, 63 for passwords, 255 for commands) is a protocol error, as are trailing bytes, out-of-range counts, enums, coordinates and tile values. The peer is disconnected.
- **Protocol Version Lock:** the ALPN protocol carries the version, so an out-of-date client fails at the TLS handshake with a clear message; any frame with an unexpected `version` is rejected too.
- **Client side:** the client applies the same strict decoding to what the server sends (map chunk sizes and tiles, entity batch counts), ends the session with the reason on any malformed frame, and detects a vanished server with TCP keepalive.

Text commands are stripped of control characters (bytes below 0x20 and 0x7F) before they reach the game, so chat and private messages cannot carry terminal escape sequences to other players. The RNG seed is not logged.

### C. Denial-of-Service Resistance
- **Non-blocking server loop:** Each client has a receive buffer and a send queue (`src/server/server_conn.c`). A slow, silent or half-open connection can never stall the game loop for the other players.
- **Admission control:** Connections that cannot be served (server full, too many pending logins from one address, address locked out) complete the TLS handshake in one of 16 refusal slots, receive `MSG_AUTH_FAIL` and are closed within 3 s; beyond 16 they are closed at once. Their descriptors are never leaked.
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
- **Writer thread:** the game loop only takes the snapshot (in memory); a writer thread performs the disk work (`src/server/writer.c`). A newer snapshot of a file replaces one still waiting in the queue, and a login waits for any pending write of its save before reading it, so a character never comes back older than its last logout. At shutdown the queue is drained, then the final saves are written synchronously.
- **Child processes:** the DM PDF generator runs in the background (`posix_spawn`, reaped with `waitpid(WNOHANG)`); the script is looked up next to the server binary or in `/usr/share/dragongl/tools`, never in the current directory. Every server descriptor (sockets, epoll, state lock, log) is close-on-exec, so the child inherits none of them.

### E. Concurrency & Thread Safety
The server game state belongs to the main loop only. The two server threads (writer, login worker) receive self-contained jobs (an encoded buffer, a copy of the login request) through mutex-protected queues and never touch `Client`, `NPC` or `World`; the login worker reports back through an `eventfd` watched by `epoll`, and a result is applied only if the slot still holds the same connection (generation counter). `server_log()` is thread-safe (`localtime_r`, stream lock).

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
