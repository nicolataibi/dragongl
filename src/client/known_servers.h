/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Servers the client trusts by certificate fingerprint (trust on first
 * use, as SSH's known_hosts).
 *
 * One line per server: "<host> <port> <sha256 in hex>"; '#' starts a
 * comment. The file is $XDG_CONFIG_HOME/dragongl/known_servers, else
 * ~/.config/dragongl/known_servers; the client creates it (mode 0600, in
 * a 0700 directory) at the first connection to a server whose certificate
 * is not signed by a trusted authority.
 */

#ifndef DRAGONGL_KNOWN_SERVERS_H
#define DRAGONGL_KNOWN_SERVERS_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    KS_UNKNOWN,  /* no line for host and port */
    KS_MATCH,    /* the line has this fingerprint */
    KS_MISMATCH, /* the line has another fingerprint: *line is its number */
    KS_ERROR     /* the file exists but cannot be read */
} KnownServerResult;

/* The path described above. False if neither variable gives a home. */
bool known_servers_path(char *out, size_t cap);

/* Look host:port up (host compared without case). fp_hex: 64 lowercase
 * hex digits (tls_fingerprint_normalize). A missing file is KS_UNKNOWN. */
KnownServerResult known_servers_check(const char *path, const char *host, int port,
                                      const char *fp_hex, int *line);

/* Record host:port with fp_hex, replacing a line for the same server.
 * Creates the directory and the file. False with the reason in why. */
bool known_servers_set(const char *path, const char *host, int port, const char *fp_hex, char *why,
                       size_t why_len);

#endif
