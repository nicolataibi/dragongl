/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The client's TLS session with the server.
 *
 * The server is trusted when (in this order):
 *   1. the user gave its fingerprint (--fingerprint) and it matches: it is
 *      recorded in known_servers, replacing an older entry;
 *   2. its certificate is signed by an authority the system trusts and
 *      names the host the user typed;
 *   3. known_servers has its fingerprint (trust on first use: the first
 *      connection records it, every later one must find the same).
 * A server whose certificate changed is refused, with the file and line
 * to remove if the change is expected.
 *
 * The session is used by several threads (network thread: receive; CLI
 * and render threads: send): every call takes the session lock.
 */

#ifndef DRAGONGL_CLIENT_TLS_H
#define DRAGONGL_CLIENT_TLS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct ClientTls ClientTls;

typedef enum {
    TLS_TRUST_GIVEN,     /* rule 1 */
    TLS_TRUST_AUTHORITY, /* rule 2 */
    TLS_TRUST_KNOWN,     /* rule 3, fingerprint already recorded */
    TLS_TRUST_FIRST_USE  /* rule 3, recorded now */
} ClientTlsTrust;

/* Handshake on the connected non-blocking socket fd, within timeout_ms,
 * then the trust check above. host and port name the server as the user
 * typed them; expected_fp may be NULL. NULL on failure, with the reason in
 * why (the socket is left open). */
ClientTls *client_tls_connect(int fd, const char *host, int port, const char *expected_fp,
                              int timeout_ms, ClientTlsTrust *trust, char *why, size_t why_len);

/* "SHA256:AB:CD:..." of the server certificate. */
const char *client_tls_fingerprint(const ClientTls *t);

/* Path of the known_servers file used, "" if none. */
const char *client_tls_known_servers(const ClientTls *t);

/* Send everything; waits up to ~2 s for a full socket. False if the
 * connection is gone. */
bool client_tls_send(ClientTls *t, const void *data, size_t len);

/* Receive what is available: bytes read, 0 = nothing now, -1 = closed by
 * the server, -2 = error (reason in why). */
long client_tls_recv(ClientTls *t, void *buf, size_t cap, char *why, size_t why_len);

/* Decrypted bytes waiting inside TLS (poll() would not report them). */
bool client_tls_pending(ClientTls *t);

/* close_notify and free; the caller closes the socket. */
void client_tls_close(ClientTls *t);

#endif
