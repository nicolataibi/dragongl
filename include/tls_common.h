/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * TLS helpers shared by the server and the client (OpenSSL 3).
 *
 * Every connection is TLS 1.3, with the ALPN protocol "dragongl/<version>"
 * (protocol.h): a peer of another protocol version fails the handshake
 * with a no_application_protocol alert, before any frame is exchanged.
 * A certificate is identified by the SHA-256 digest of its DER encoding,
 * written "SHA256:AB:CD:..." (the form of openssl x509 -fingerprint).
 */

#ifndef DRAGONGL_TLS_COMMON_H
#define DRAGONGL_TLS_COMMON_H

#include <stdbool.h>
#include <stddef.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>

/* "SHA256:" + 32 bytes as "AB:" groups, NUL included. */
#define TLS_FINGERPRINT_LEN (7 + 32 * 3)
/* 64 lowercase hex digits, NUL included. */
#define TLS_FINGERPRINT_HEX_LEN 65

/* The ALPN wire form of the protocol: length byte + "dragongl/<n>". */
extern const unsigned char TLS_ALPN[];
extern const unsigned int TLS_ALPN_LEN;

/* Fingerprint of a certificate, "SHA256:AB:CD:...". False on error. */
bool tls_cert_fingerprint(X509 *cert, char *out, size_t cap);

/* A fingerprint in any usual spelling ("SHA256:AB:CD:...", "abcd...",
 * with or without colons, any case) as 64 lowercase hex digits. False if
 * it is not 32 bytes of hex. */
bool tls_fingerprint_normalize(const char *in, char hex[TLS_FINGERPRINT_HEX_LEN]);

/* The oldest error of the OpenSSL error queue as text (the queue is then
 * cleared), or `fallback` if the queue is empty. */
void tls_error_text(char *out, size_t cap, const char *fallback);

/* An SSL_CTX for TLS 1.3 only, with the Dragon GL ALPN set on the client
 * side (server: the selection callback accepts only that protocol). */
SSL_CTX *tls_new_context(bool server);

#endif
