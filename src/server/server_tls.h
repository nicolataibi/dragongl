/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The server's TLS identity: certificate, private key and the SSL_CTX of
 * every client connection (server_conn.c).
 *
 * Without --tls-cert/--tls-key the server uses <state dir>/tls/server.crt
 * and server.key, and creates them at the first start: an ECDSA P-256
 * key and a self-signed certificate for the host name, valid 10 years.
 * Clients pin its fingerprint on the first connection (known_servers), so
 * keep the tls/ directory with the world: a new certificate makes every
 * client refuse the server until its old entry is removed.
 */

#ifndef DRAGONGL_SERVER_TLS_H
#define DRAGONGL_SERVER_TLS_H

#include <stdbool.h>
#include <stddef.h>

#include <openssl/ssl.h>

/* Load the certificate and key (both given, or both NULL for the state
 * directory pair, created if missing). False with the reason in err. A
 * key file readable by group or others is used, with a warning in warn
 * (empty otherwise), as the password files are. */
bool server_tls_init(const char *cert_file, const char *key_file, char *err, size_t err_len,
                     char *warn, size_t warn_len);

/* The context of the client connections, or NULL before server_tls_init()
 * (only the in-process tests run without TLS). */
SSL_CTX *server_tls_ctx(void);

/* "SHA256:AB:CD:..." of the certificate in use, "" before init. */
const char *server_tls_fingerprint(void);

/* Path of the certificate in use, "" before init. */
const char *server_tls_cert_path(void);

void server_tls_cleanup(void);

#endif
