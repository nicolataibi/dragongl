/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The client's TLS session (client_tls.h).
 */

#include "client_tls.h"

#include "known_servers.h"
#include "net.h"
#include "protocol.h"
#include "tls_common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/err.h>
#include <openssl/x509v3.h>

struct ClientTls {
    SSL_CTX *ctx;
    SSL *ssl;
    int fd;
    pthread_mutex_t lock;
    bool fatal; /* a fatal TLS error: no close_notify */
    char fingerprint[TLS_FINGERPRINT_LEN];
    char known_path[512];
};

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Wait for the socket direction OpenSSL asked for; false on timeout. */
static bool wait_socket(int fd, int ssl_error, long long deadline) {
    long long left = deadline - now_ms();
    if (left <= 0) {
        return false;
    }
    struct pollfd p = {.fd = fd, .events = ssl_error == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN};
    int r;
    do {
        r = poll(&p, 1, (int)left);
    } while (r < 0 && errno == EINTR);
    return r > 0;
}

static bool is_ip_literal(const char *host) {
    unsigned char addr[16];
    return inet_pton(AF_INET, host, addr) == 1 || inet_pton(AF_INET6, host, addr) == 1;
}

static void free_session(ClientTls *t) {
    SSL_free(t->ssl);
    SSL_CTX_free(t->ctx);
    pthread_mutex_destroy(&t->lock);
    free(t);
}

static bool handshake(ClientTls *t, int timeout_ms, char *why, size_t why_len) {
    long long deadline = now_ms() + timeout_ms;
    for (;;) {
        ERR_clear_error();
        int r = SSL_connect(t->ssl);
        if (r == 1) {
            return true;
        }
        int e = SSL_get_error(t->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (!wait_socket(t->fd, e, deadline)) {
                snprintf(why, why_len, "TLS handshake timed out");
                return false;
            }
            continue;
        }
        char err[160];
        tls_error_text(err, sizeof(err), "connection closed");
        if (strstr(err, "no application protocol")) {
            snprintf(why, why_len,
                     "the server speaks another protocol version (this client: %d); "
                     "update the client or the server",
                     PROTOCOL_VERSION);
        } else {
            snprintf(why, why_len, "TLS handshake failed: %s (is it a Dragon GL %d server?)", err,
                     PROTOCOL_VERSION);
        }
        return false;
    }
}

/* Rules 1-3 of client_tls.h. */
static bool trust_server(ClientTls *t, const char *host, int port, const char *expected_fp,
                         ClientTlsTrust *trust, char *why, size_t why_len) {
    X509 *cert = SSL_get1_peer_certificate(t->ssl);
    bool have_fp = cert && tls_cert_fingerprint(cert, t->fingerprint, sizeof(t->fingerprint));
    X509_free(cert);
    char hex[TLS_FINGERPRINT_HEX_LEN];
    if (!have_fp || !tls_fingerprint_normalize(t->fingerprint, hex)) {
        snprintf(why, why_len, "the server sent no usable certificate");
        return false;
    }
    bool have_path = known_servers_path(t->known_path, sizeof(t->known_path));
    if (!have_path) {
        t->known_path[0] = '\0';
    }
    if (expected_fp) {
        char want[TLS_FINGERPRINT_HEX_LEN];
        if (!tls_fingerprint_normalize(expected_fp, want)) {
            snprintf(why, why_len, "--fingerprint: not a SHA-256 fingerprint: %s", expected_fp);
            return false;
        }
        if (strcmp(want, hex) != 0) {
            snprintf(why, why_len,
                     "the server certificate is not the one given with --fingerprint "
                     "(server: %s)",
                     t->fingerprint);
            return false;
        }
        *trust = TLS_TRUST_GIVEN;
        char err[256];
        if (have_path && !known_servers_set(t->known_path, host, port, hex, err, sizeof(err))) {
            fprintf(stderr, "[NET] WARNING: %s\n", err);
        }
        return true;
    }
    if (SSL_get_verify_result(t->ssl) == X509_V_OK) {
        *trust = TLS_TRUST_AUTHORITY;
        return true;
    }
    if (!have_path) {
        snprintf(why, why_len,
                 "self-signed server and no HOME for known_servers: pass --fingerprint %s",
                 t->fingerprint);
        return false;
    }
    int line = 0;
    switch (known_servers_check(t->known_path, host, port, hex, &line)) {
    case KS_MATCH:
        *trust = TLS_TRUST_KNOWN;
        return true;
    case KS_MISMATCH:
        snprintf(why, why_len,
                 "THE SERVER CERTIFICATE HAS CHANGED (now %s). Someone may be intercepting "
                 "the connection; if the server owner confirms the new fingerprint, remove "
                 "line %d of %s",
                 t->fingerprint, line, t->known_path);
        return false;
    case KS_ERROR:
        snprintf(why, why_len, "cannot read %s: %s", t->known_path, strerror(errno));
        return false;
    case KS_UNKNOWN:
    default: {
        char err[256];
        if (!known_servers_set(t->known_path, host, port, hex, err, sizeof(err))) {
            snprintf(why, why_len, "%s", err);
            return false;
        }
        *trust = TLS_TRUST_FIRST_USE;
        return true;
    }
    }
}

ClientTls *client_tls_connect(int fd, const char *host, int port, const char *expected_fp,
                              int timeout_ms, ClientTlsTrust *trust, char *why, size_t why_len) {
    ClientTls *t = calloc(1, sizeof(*t));
    if (!t) {
        snprintf(why, why_len, "out of memory");
        return NULL;
    }
    t->fd = fd;
    pthread_mutex_init(&t->lock, NULL);
    t->ctx = tls_new_context(false);
    t->ssl = t->ctx ? SSL_new(t->ctx) : NULL;
    bool ok =
        t->ssl && SSL_CTX_set_default_verify_paths(t->ctx) == 1 && SSL_set_fd(t->ssl, fd) == 1;
    if (ok) {
        /* The verdict on the chain is read after the handshake (rule 2);
         * a self-signed server must still be able to complete it. */
        SSL_set_verify(t->ssl, SSL_VERIFY_NONE, NULL);
        X509_VERIFY_PARAM *param = SSL_get0_param(t->ssl);
        if (is_ip_literal(host)) {
            ok = X509_VERIFY_PARAM_set1_ip_asc(param, host) == 1;
        } else {
            ok = SSL_set_tlsext_host_name(t->ssl, host) == 1 &&
                 X509_VERIFY_PARAM_set1_host(param, host, 0) == 1;
        }
    }
    if (!ok) {
        tls_error_text(why, why_len, "cannot set up TLS");
        free_session(t);
        return NULL;
    }
    if (!handshake(t, timeout_ms, why, why_len)) {
        free_session(t);
        return NULL;
    }
    const unsigned char *alpn = NULL;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(t->ssl, &alpn, &alpn_len);
    if (alpn_len + 1 != TLS_ALPN_LEN || memcmp(alpn, TLS_ALPN + 1, alpn_len) != 0) {
        snprintf(why, why_len, "the server did not agree on protocol %d", PROTOCOL_VERSION);
        free_session(t);
        return NULL;
    }
    if (!trust_server(t, host, port, expected_fp, trust, why, why_len)) {
        free_session(t);
        return NULL;
    }
    return t;
}

const char *client_tls_fingerprint(const ClientTls *t) {
    return t->fingerprint;
}

const char *client_tls_known_servers(const ClientTls *t) {
    return t->known_path;
}

bool client_tls_send(ClientTls *t, const void *data, size_t len) {
    const unsigned char *p = data;
    long long deadline = now_ms() + 2000;
    bool ok = true;
    pthread_mutex_lock(&t->lock);
    while (len > 0) {
        ERR_clear_error();
        int n = SSL_write(t->ssl, p, len > 65536 ? 65536 : (int)len);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        int e = SSL_get_error(t->ssl, n);
        if ((e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) &&
            wait_socket(t->fd, e, deadline)) {
            continue;
        }
        t->fatal = e == SSL_ERROR_SYSCALL || e == SSL_ERROR_SSL;
        ERR_clear_error();
        ok = false;
        break;
    }
    pthread_mutex_unlock(&t->lock);
    return ok;
}

long client_tls_recv(ClientTls *t, void *buf, size_t cap, char *why, size_t why_len) {
    pthread_mutex_lock(&t->lock);
    ERR_clear_error();
    int n = SSL_read(t->ssl, buf, cap > 65536 ? 65536 : (int)cap);
    long result = n;
    if (n <= 0) {
        int e = SSL_get_error(t->ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            result = 0;
        } else if (e == SSL_ERROR_ZERO_RETURN) {
            result = -1;
        } else {
            t->fatal = true;
            if (e == SSL_ERROR_SYSCALL) {
                snprintf(why, why_len, "%s", errno ? strerror(errno) : "connection closed");
                ERR_clear_error();
            } else {
                tls_error_text(why, why_len, "TLS error");
            }
            result = -2;
        }
    }
    pthread_mutex_unlock(&t->lock);
    return result;
}

bool client_tls_pending(ClientTls *t) {
    pthread_mutex_lock(&t->lock);
    bool pending = SSL_pending(t->ssl) > 0;
    pthread_mutex_unlock(&t->lock);
    return pending;
}

void client_tls_close(ClientTls *t) {
    if (!t) {
        return;
    }
    pthread_mutex_lock(&t->lock);
    if (!t->fatal) {
        (void)SSL_shutdown(t->ssl);
        ERR_clear_error();
    }
    pthread_mutex_unlock(&t->lock);
    free_session(t);
}
