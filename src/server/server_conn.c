/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "server_conn.h"
#include "auth.h"
#include "net.h"
#include "server_entities.h"
#include "server_internal.h"
#include "server_tls.h"
#include "tls_common.h"
#include "wire.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>

typedef struct {
    bool open;
    int fd;
    /* TLS session; NULL only for the plaintext slots of the in-process
     * tests (conn_open_plain). */
    SSL *ssl;
    bool handshake_done;
    bool want_write; /* TLS can only progress once the socket is writable */
    bool tls_fatal;  /* a fatal TLS error: no close_notify may follow */
    char peer_ip[CONN_IP_MAX];
    long long opened_ms;

    uint8_t rx[CONN_RX_CAP];
    size_t rx_len;
    uint8_t msg[CONN_RX_CAP]; /* payload of the last message handed out */

    uint8_t *tx;
    size_t tx_len;
    size_t tx_cap;

    bool dropping;
    char drop_reason[160];

    /* Refusal slots (MAX_CLIENTS..): the reason sent after the handshake. */
    char refusal[PROTO_REASON_LEN];
    bool refusal_queued;
    uint32_t refusal_events; /* epoll interest as registered */
} Conn;

_Static_assert(CONN_RX_CAP >= WIRE_HEADER_SIZE + WIRE_MAX_CLIENT_PAYLOAD,
               "the receive buffer must hold the largest client message");

#define CONN_SLOTS (MAX_CLIENTS + CONN_REFUSAL_SLOTS)

static Conn g_conns[CONN_SLOTS];

static Conn *slot_conn(int slot) {
    if (slot < 0 || slot >= CONN_SLOTS || !g_conns[slot].open) {
        return NULL;
    }
    return &g_conns[slot];
}

/* --- Lifecycle ----------------------------------------------------- */

static void conn_init(int slot, int fd, const char *peer_ip, long long now_ms) {
    conn_release(slot);
    Conn *c = &g_conns[slot];
    c->open = true;
    c->fd = fd;
    copy_str(c->peer_ip, peer_ip ? peer_ip : "?", sizeof(c->peer_ip));
    c->opened_ms = now_ms;
}

bool conn_open(int slot, int fd, const char *peer_ip, long long now_ms) {
    if (slot < 0 || slot >= CONN_SLOTS || !server_tls_ctx()) {
        return false;
    }
    SSL *ssl = SSL_new(server_tls_ctx());
    if (!ssl || SSL_set_fd(ssl, fd) != 1) {
        SSL_free(ssl);
        ERR_clear_error();
        return false;
    }
    SSL_set_accept_state(ssl);
    conn_init(slot, fd, peer_ip, now_ms);
    g_conns[slot].ssl = ssl;
    return true;
}

void conn_open_plain(int slot, int fd, const char *peer_ip, long long now_ms) {
    if (slot >= 0 && slot < MAX_CLIENTS) {
        conn_init(slot, fd, peer_ip, now_ms);
        g_conns[slot].handshake_done = true;
    }
}

void conn_release(int slot) {
    if (slot < 0 || slot >= CONN_SLOTS) {
        return;
    }
    Conn *c = &g_conns[slot];
    SSL_free(c->ssl);
    free(c->tx);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

void conn_close(int slot) {
    Conn *c = slot_conn(slot);
    if (!c) {
        return;
    }
    conn_flush(slot); /* best effort: a queued MSG_AUTH_FAIL */
    if (c->ssl && c->handshake_done && !c->tls_fatal) {
        (void)SSL_shutdown(c->ssl); /* close_notify, without waiting for the answer */
        ERR_clear_error();
    }
    close(c->fd); /* also removes it from the epoll set */
    conn_release(slot);
}

bool conn_is_open(int slot) {
    return slot_conn(slot) != NULL;
}

int conn_fd(int slot) {
    Conn *c = slot_conn(slot);
    return c ? c->fd : -1;
}

int conn_slot_for_sock(int sock) {
    if (sock < 0) {
        return -1;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_conns[i].open && g_conns[i].fd == sock) {
            return i;
        }
    }
    return -1;
}

const char *conn_peer_ip(int slot) {
    Conn *c = slot_conn(slot);
    return c ? c->peer_ip : "?";
}

long long conn_opened_ms(int slot) {
    Conn *c = slot_conn(slot);
    return c ? c->opened_ms : 0;
}

/* --- TLS ------------------------------------------------------------ */

/* A failed TLS operation: log it, mark the slot. */
static void tls_failed(int slot, Conn *c, int ssl_error, const char *what) {
    char why[128];
    tls_error_text(why, sizeof(why),
                   ssl_error == SSL_ERROR_SYSCALL ? "connection closed" : "protocol error");
    c->tls_fatal = true;
    char reason[160];
    snprintf(reason, sizeof(reason), "TLS %s failed: %s", what, why);
    server_log("NET", "TLS %s with %s failed: %s", what, c->peer_ip, why);
    conn_mark_drop(slot, reason);
}

/* 1 = handshake complete, 0 = in progress, -1 = failed (slot marked). */
static int conn_handshake(int slot, Conn *c) {
    if (!c->ssl || c->handshake_done) {
        return 1;
    }
    c->want_write = false;
    ERR_clear_error();
    int r = SSL_do_handshake(c->ssl);
    if (r == 1) {
        c->handshake_done = true;
        /* The ALPN callback refuses another protocol; a client that offers
         * none at all is refused here. */
        const unsigned char *alpn = NULL;
        unsigned int alpn_len = 0;
        SSL_get0_alpn_selected(c->ssl, &alpn, &alpn_len);
        if (alpn_len == 0) {
            server_log("NET", "TLS client %s offered no application protocol", c->peer_ip);
            conn_mark_drop(slot, "no ALPN protocol (not a Dragon GL client)");
            return -1;
        }
        return 1;
    }
    int e = SSL_get_error(c->ssl, r);
    if (e == SSL_ERROR_WANT_READ) {
        return 0;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
        c->want_write = true;
        return 0;
    }
    tls_failed(slot, c, e, "handshake");
    return -1;
}

/* --- Receive ------------------------------------------------------- */

/* One read into the free part of rx: bytes read, 0 = nothing more now,
 * -1 = closed by the peer, -2 = error (slot marked for TLS errors). */
static long read_some(int slot, Conn *c) {
    size_t room = sizeof(c->rx) - c->rx_len;
    if (!c->ssl) {
        for (;;) {
            ssize_t n = recv(c->fd, c->rx + c->rx_len, room, 0);
            if (n > 0) {
                return (long)n;
            }
            if (n == 0) {
                return -1;
            }
            if (errno == EINTR) {
                continue;
            }
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -2;
        }
    }
    ERR_clear_error();
    int n = SSL_read(c->ssl, c->rx + c->rx_len, (int)room);
    if (n > 0) {
        return n;
    }
    int e = SSL_get_error(c->ssl, n);
    switch (e) {
    case SSL_ERROR_WANT_READ:
        return 0;
    case SSL_ERROR_WANT_WRITE:
        c->want_write = true;
        return 0;
    case SSL_ERROR_ZERO_RETURN:
        return -1;
    case SSL_ERROR_SYSCALL:
        /* EOF or a reset without close_notify: the peer is gone. */
        c->tls_fatal = true;
        ERR_clear_error();
        return -1;
    default:
        tls_failed(slot, c, e, "read");
        return -2;
    }
}

ConnRxStatus conn_read(int slot) {
    Conn *c = slot_conn(slot);
    if (!c) {
        return CONN_RX_ERROR;
    }
    int hs = conn_handshake(slot, c);
    if (hs <= 0) {
        return hs < 0 ? CONN_RX_ERROR : CONN_RX_OK;
    }
    /* A full buffer is not an error: the caller consumes the complete
     * messages and reads again (conn_input_buffered), or the
     * level-triggered epoll reports the rest. */
    while (c->rx_len < sizeof(c->rx)) {
        long n = read_some(slot, c);
        if (n > 0) {
            c->rx_len += (size_t)n;
        } else if (n == 0) {
            break;
        } else {
            return n == -1 ? CONN_RX_CLOSED : CONN_RX_ERROR;
        }
    }
    return CONN_RX_OK;
}

bool conn_input_buffered(int slot) {
    Conn *c = slot_conn(slot);
    return c && c->ssl && c->handshake_done && SSL_pending(c->ssl) > 0;
}

bool conn_rx_pending(int slot) {
    Conn *c = slot_conn(slot);
    return c && (c->rx_len > 0 || conn_input_buffered(slot));
}

int conn_next_message(int slot, MsgHeader *hdr, const void **payload) {
    Conn *c = slot_conn(slot);
    if (!c || c->dropping) {
        return 0;
    }
    if (c->rx_len < WIRE_HEADER_SIZE) {
        return 0;
    }
    MsgHeader h;
    wire_read_header(c->rx, &h);
    /* Validate before waiting for the payload: a wrong version, a type a
     * client may not send or an oversized length can never become valid,
     * and the length drives the read. The payload itself is decoded (and
     * checked field by field) by the caller. */
    uint32_t max = wire_client_payload_max(h.type);
    if (h.version != PROTOCOL_VERSION || max == 0 || h.length > max) {
        return -1;
    }
    size_t total = WIRE_HEADER_SIZE + (size_t)h.length;
    if (c->rx_len < total) {
        return 0;
    }
    memcpy(c->msg, c->rx + WIRE_HEADER_SIZE, (size_t)h.length);
    memmove(c->rx, c->rx + total, c->rx_len - total);
    c->rx_len -= total;
    /*The consumed bytes (a MSG_LOGIN carries passwords) must not linger
     * past the end of the buffer.*/
    auth_wipe(c->rx + c->rx_len, total);
    *hdr = h;
    *payload = c->msg;
    return 1;
}

/* --- Send ---------------------------------------------------------- */

bool conn_queue(int slot, const void *data, size_t len) {
    Conn *c = slot_conn(slot);
    if (!c || c->dropping) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    if (c->tx_len + len > CONN_TX_LIMIT) {
        conn_mark_drop(slot, "send queue overflow (client not reading)");
        return false;
    }
    if (c->tx_len + len > c->tx_cap) {
        size_t cap = c->tx_cap ? c->tx_cap : 16384;
        while (cap < c->tx_len + len) {
            cap *= 2;
        }
        if (cap > CONN_TX_LIMIT) {
            cap = CONN_TX_LIMIT;
        }
        uint8_t *grown = realloc(c->tx, cap);
        if (!grown) {
            conn_mark_drop(slot, "out of memory for the send queue");
            return false;
        }
        c->tx = grown;
        c->tx_cap = cap;
    }
    memcpy(c->tx + c->tx_len, data, len);
    c->tx_len += len;
    return true;
}

/* One write from tx: bytes written, 0 = the socket is full, -1 = the
 * peer is gone (slot marked). */
static long write_some(int slot, Conn *c, size_t sent) {
    size_t left = c->tx_len - sent;
    if (!c->ssl) {
        for (;;) {
            ssize_t n = send(c->fd, c->tx + sent, left, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0) {
                return (long)n;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return 0;
            }
            conn_mark_drop(slot, "send failed (peer gone)");
            return -1;
        }
    }
    ERR_clear_error();
    int n = SSL_write(c->ssl, c->tx + sent, (int)left);
    if (n > 0) {
        return n;
    }
    int e = SSL_get_error(c->ssl, n);
    if (e == SSL_ERROR_WANT_WRITE) {
        c->want_write = true;
        return 0;
    }
    if (e == SSL_ERROR_WANT_READ) {
        return 0;
    }
    if (e == SSL_ERROR_SYSCALL) {
        c->tls_fatal = true;
        ERR_clear_error();
        conn_mark_drop(slot, "send failed (peer gone)");
    } else {
        tls_failed(slot, c, e, "write");
    }
    return -1;
}

void conn_flush(int slot) {
    Conn *c = slot_conn(slot);
    if (!c || conn_handshake(slot, c) <= 0) {
        return;
    }
    c->want_write = false;
    size_t sent = 0;
    while (sent < c->tx_len) {
        long n = write_some(slot, c, sent);
        if (n < 0) {
            c->tx_len = 0;
            return;
        }
        if (n == 0) {
            break;
        }
        sent += (size_t)n;
    }
    if (sent > 0) {
        memmove(c->tx, c->tx + sent, c->tx_len - sent);
        c->tx_len -= sent;
    }
}

void conn_wipe_message(int slot) {
    Conn *c = slot_conn(slot);
    if (c) {
        auth_wipe(c->msg, sizeof(c->msg));
    }
}

bool conn_tx_pending(int slot) {
    Conn *c = slot_conn(slot);
    return c && (c->tx_len > 0 || c->want_write);
}

/* --- Deferred removal ------------------------------------------------- */

void conn_mark_drop(int slot, const char *reason) {
    Conn *c = slot_conn(slot);
    if (!c || c->dropping) {
        return;
    }
    c->dropping = true;
    copy_str(c->drop_reason, reason ? reason : "disconnected", sizeof(c->drop_reason));
}

bool conn_dropping(int slot) {
    Conn *c = slot_conn(slot);
    return c && c->dropping;
}

const char *conn_drop_reason(int slot) {
    Conn *c = slot_conn(slot);
    return c ? c->drop_reason : "";
}

/* --- Refused connections ------------------------------------------------ */

static uint32_t refusal_interest(int slot) {
    return EPOLLIN | EPOLLRDHUP | (conn_tx_pending(slot) ? EPOLLOUT : 0u);
}

void conn_refuse(int epoll_fd, int fd, const char *peer_ip, const char *reason, long long now_ms) {
    int slot = -1;
    for (int k = 0; k < CONN_REFUSAL_SLOTS; k++) {
        if (!g_conns[MAX_CLIENTS + k].open) {
            slot = MAX_CLIENTS + k;
            break;
        }
    }
    if (slot < 0 || !conn_open(slot, fd, peer_ip, now_ms)) {
        close(fd); /* every refusal slot busy: no reason, just the close */
        return;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.u32 = CONN_REFUSAL_ID_BASE + (uint32_t)(slot - MAX_CLIENTS);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        conn_close(slot);
        return;
    }
    Conn *c = &g_conns[slot];
    c->refusal_events = ev.events;
    copy_str(c->refusal, reason, sizeof(c->refusal));
}

bool conn_is_refusal_id(uint32_t id) {
    return id >= CONN_REFUSAL_ID_BASE && id < CONN_REFUSAL_ID_BASE + CONN_REFUSAL_SLOTS;
}

/* Handshake, then MSG_AUTH_FAIL, then close; or close after the timeout. */
static void refusal_step(int epoll_fd, int slot, long long now_ms) {
    Conn *c = slot_conn(slot);
    if (!c) {
        return;
    }
    (void)conn_read(slot);
    c->rx_len = 0; /* whatever the client sent is ignored */
    if (c->handshake_done && !c->refusal_queued && !c->dropping) {
        BinWriter w;
        bw_init(&w);
        MsgAuthFail m;
        copy_str(m.reason, c->refusal, sizeof(m.reason));
        if (wire_put_auth_fail(&w, &m)) {
            (void)conn_queue(slot, w.data, w.len);
        }
        bw_free(&w);
        c->refusal_queued = true;
    }
    conn_flush(slot);
    bool done = c->dropping || (c->refusal_queued && !conn_tx_pending(slot)) ||
                now_ms - c->opened_ms > CONN_REFUSAL_TIMEOUT_MS;
    if (done) {
        conn_close(slot);
        return;
    }
    uint32_t want = refusal_interest(slot);
    if (want != c->refusal_events) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = want;
        ev.data.u32 = CONN_REFUSAL_ID_BASE + (uint32_t)(slot - MAX_CLIENTS);
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) == 0) {
            c->refusal_events = want;
        }
    }
}

void conn_refusal_event(int epoll_fd, uint32_t id, long long now_ms) {
    if (conn_is_refusal_id(id)) {
        refusal_step(epoll_fd, MAX_CLIENTS + (int)(id - CONN_REFUSAL_ID_BASE), now_ms);
    }
}

void conn_refusals_expire(int epoll_fd, long long now_ms) {
    for (int k = 0; k < CONN_REFUSAL_SLOTS; k++) {
        Conn *c = &g_conns[MAX_CLIENTS + k];
        if (c->open && now_ms - c->opened_ms > CONN_REFUSAL_TIMEOUT_MS) {
            refusal_step(epoll_fd, MAX_CLIENTS + k, now_ms);
        }
    }
}

/* --- Per-IP guard ------------------------------------------------------ */

#define IPGUARD_SLOTS 128

typedef struct {
    char ip[CONN_IP_MAX];
    int failures;
    long long window_start_ms;
    long long locked_until_ms;
    long long last_seen_ms;
} IpRecord;

static IpRecord g_ipguard[IPGUARD_SLOTS];

static IpRecord *ipguard_find(const char *ip, bool create, long long now_ms) {
    IpRecord *oldest = &g_ipguard[0];
    for (int i = 0; i < IPGUARD_SLOTS; i++) {
        if (g_ipguard[i].ip[0] && strcmp(g_ipguard[i].ip, ip) == 0) {
            return &g_ipguard[i];
        }
        if (g_ipguard[i].last_seen_ms < oldest->last_seen_ms) {
            oldest = &g_ipguard[i];
        }
    }
    if (!create) {
        return NULL;
    }
    /* Reuse the least recently seen record (an empty one is oldest). */
    memset(oldest, 0, sizeof(*oldest));
    copy_str(oldest->ip, ip, sizeof(oldest->ip));
    oldest->window_start_ms = now_ms;
    oldest->last_seen_ms = now_ms;
    return oldest;
}

bool ipguard_locked(const char *ip, long long now_ms) {
    IpRecord *r = ipguard_find(ip, false, now_ms);
    return r && r->locked_until_ms > now_ms;
}

void ipguard_login_failed(const char *ip, long long now_ms) {
    IpRecord *r = ipguard_find(ip, true, now_ms);
    r->last_seen_ms = now_ms;
    if (now_ms - r->window_start_ms > LOGIN_FAIL_WINDOW_MS) {
        r->window_start_ms = now_ms;
        r->failures = 0;
    }
    if (++r->failures >= LOGIN_FAIL_LIMIT) {
        r->locked_until_ms = now_ms + LOGIN_LOCKOUT_MS;
        r->failures = 0;
        r->window_start_ms = now_ms;
        server_log("AUTH", "Too many failed logins from %s: locked out for %lld s", ip,
                   LOGIN_LOCKOUT_MS / 1000);
    }
}

void ipguard_login_succeeded(const char *ip) {
    IpRecord *r = ipguard_find(ip, false, 0);
    if (r) {
        r->failures = 0;
    }
}
