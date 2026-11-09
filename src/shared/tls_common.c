/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * TLS helpers shared by the server and the client (tls_common.h).
 */

#include "tls_common.h"

#include "protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/evp.h>

#define STR2(x)   #x
#define STR(x)    STR2(x)
#define ALPN_NAME "dragongl/" STR(PROTOCOL_VERSION)

/* ALPN wire form: one length byte, then the name (10 bytes while the
 * version has one digit). */
_Static_assert(sizeof(ALPN_NAME) - 1 == 10, "update the length byte of TLS_ALPN");
const unsigned char TLS_ALPN[] = "\x0a" ALPN_NAME;
const unsigned int TLS_ALPN_LEN = (unsigned int)sizeof(TLS_ALPN) - 1; /* without the NUL */

bool tls_cert_fingerprint(X509 *cert, char *out, size_t cap) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!cert || cap < TLS_FINGERPRINT_LEN || !X509_digest(cert, EVP_sha256(), md, &len) ||
        len != 32) {
        if (cap) {
            out[0] = '\0';
        }
        return false;
    }
    size_t at = (size_t)snprintf(out, cap, "SHA256:");
    for (unsigned int i = 0; i < len; i++) {
        at += (size_t)snprintf(out + at, cap - at, i ? ":%02X" : "%02X", md[i]);
    }
    return true;
}

bool tls_fingerprint_normalize(const char *in, char hex[TLS_FINGERPRINT_HEX_LEN]) {
    hex[0] = '\0';
    if (!in) {
        return false;
    }
    if (strncasecmp(in, "SHA256:", 7) == 0) {
        in += 7;
    }
    size_t n = 0;
    for (const char *p = in; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == ':') {
            continue;
        }
        if (!isxdigit(ch) || n >= TLS_FINGERPRINT_HEX_LEN - 1) {
            hex[0] = '\0';
            return false;
        }
        hex[n++] = (char)tolower(ch);
    }
    hex[n] = '\0';
    if (n != TLS_FINGERPRINT_HEX_LEN - 1) {
        hex[0] = '\0';
        return false;
    }
    return true;
}

void tls_error_text(char *out, size_t cap, const char *fallback) {
    unsigned long e = ERR_get_error();
    if (e == 0) {
        snprintf(out, cap, "%s", fallback);
    } else {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof(buf));
        /* "error:0A00010B:SSL routines::wrong version number" -> the reason */
        const char *reason = ERR_reason_error_string(e);
        snprintf(out, cap, "%s", reason ? reason : buf);
    }
    ERR_clear_error();
}

/* Server side of ALPN: the client must offer exactly our protocol. */
static int select_alpn(SSL *ssl, const unsigned char **out, unsigned char *outlen,
                       const unsigned char *in, unsigned int inlen, void *arg) {
    (void)ssl;
    (void)arg;
    unsigned char *sel = NULL;
    /* Server list first, then the client's: the argument order is right. */
    // NOLINTNEXTLINE(readability-suspicious-call-argument)
    if (SSL_select_next_proto(&sel, outlen, TLS_ALPN, TLS_ALPN_LEN, in, inlen) !=
        OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    *out = sel;
    return SSL_TLSEXT_ERR_OK;
}

SSL_CTX *tls_new_context(bool server) {
    SSL_CTX *ctx = SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
    if (!ctx) {
        return NULL;
    }
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION)) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    /* Non-blocking sockets: a write may be completed later from a buffer
     * that has moved, and may be partial. */
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    /* A peer that closes without close_notify is a closed connection: the
     * frames carry their own length, so a truncated stream is detected by
     * the framing, not by TLS. */
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION |
                                 SSL_OP_IGNORE_UNEXPECTED_EOF);
    if (server) {
        SSL_CTX_set_alpn_select_cb(ctx, select_alpn, NULL);
        /* No session tickets or resumption: every login does a full
         * handshake, and no ticket key has to be kept or rotated. */
        SSL_CTX_set_num_tickets(ctx, 0);
        SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    } else if (SSL_CTX_set_alpn_protos(ctx, TLS_ALPN, TLS_ALPN_LEN) != 0) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}
