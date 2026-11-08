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

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

typedef struct {
    bool open;
    int fd;
    char peer_ip[CONN_IP_MAX];
    long long opened_ms;

    uint8_t rx[CONN_RX_CAP];
    size_t rx_len;
    uint8_t msg[CONN_RX_CAP]; /* last message handed out */

    uint8_t *tx;
    size_t tx_len;
    size_t tx_cap;

    bool dropping;
    char drop_reason[96];
} Conn;

static Conn g_conns[MAX_CLIENTS];

static Conn *slot_conn(int slot) {
    if (slot < 0 || slot >= MAX_CLIENTS || !g_conns[slot].open)
        return NULL;
    return &g_conns[slot];
}

/* --- Lifecycle ----------------------------------------------------- */

void conn_open(int slot, int fd, const char *peer_ip, long long now_ms) {
    if (slot < 0 || slot >= MAX_CLIENTS)
        return;
    conn_release(slot);
    Conn *c = &g_conns[slot];
    c->open = true;
    c->fd = fd;
    copy_str(c->peer_ip, peer_ip ? peer_ip : "?", sizeof(c->peer_ip));
    c->opened_ms = now_ms;
}

void conn_release(int slot) {
    if (slot < 0 || slot >= MAX_CLIENTS)
        return;
    Conn *c = &g_conns[slot];
    free(c->tx);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

bool conn_is_open(int slot) {
    return slot_conn(slot) != NULL;
}

int conn_fd(int slot) {
    Conn *c = slot_conn(slot);
    return c ? c->fd : -1;
}

int conn_slot_for_sock(int sock) {
    if (sock < 0)
        return -1;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_conns[i].open && g_conns[i].fd == sock)
            return i;
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

/* --- Receive ------------------------------------------------------- */

ConnRxStatus conn_read(int slot) {
    Conn *c = slot_conn(slot);
    if (!c)
        return CONN_RX_ERROR;
    /* A full buffer is not an error: the caller consumes the complete
     * messages and the level-triggered epoll reports the rest again. */
    while (c->rx_len < sizeof(c->rx)) {
        ssize_t n = recv(c->fd, c->rx + c->rx_len, sizeof(c->rx) - c->rx_len, 0);
        if (n > 0) {
            c->rx_len += (size_t)n;
        } else if (n == 0) {
            return CONN_RX_CLOSED;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        } else {
            return CONN_RX_ERROR;
        }
    }
    return CONN_RX_OK;
}

bool conn_rx_pending(int slot) {
    Conn *c = slot_conn(slot);
    return c && c->rx_len > 0;
}

/* Exact payload size of every message a client may send. */
static int client_msg_size(MsgType type) {
    switch (type) {
    case MSG_LOGIN:    return (int)sizeof(MsgLogin);
    case MSG_MOVE:     return (int)sizeof(MsgMove);
    case MSG_TEXT_CMD: return (int)sizeof(MsgTextCmd);
    default:           return -1;
    }
}

int conn_next_message(int slot, MsgHeader *hdr, const void **payload) {
    Conn *c = slot_conn(slot);
    if (!c || c->dropping)
        return 0;
    if (c->rx_len < sizeof(MsgHeader))
        return 0;
    MsgHeader h;
    memcpy(&h, c->rx, sizeof(h));
    /* Validate before waiting for the payload: an unknown type or a wrong
     * length can never become valid, and the length drives the read. */
    int expected = client_msg_size(h.type);
    if (h.version != PROTOCOL_VERSION || h.reserved != 0 || expected < 0 ||
        h.length != expected)
        return -1;
    size_t total = sizeof(MsgHeader) + (size_t)h.length;
    if (c->rx_len < total)
        return 0;
    memcpy(c->msg, c->rx + sizeof(MsgHeader), (size_t)h.length);
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
    if (!c || c->dropping)
        return false;
    if (len == 0)
        return true;
    if (c->tx_len + len > CONN_TX_LIMIT) {
        conn_mark_drop(slot, "send queue overflow (client not reading)");
        return false;
    }
    if (c->tx_len + len > c->tx_cap) {
        size_t cap = c->tx_cap ? c->tx_cap : 16384;
        while (cap < c->tx_len + len)
            cap *= 2;
        if (cap > CONN_TX_LIMIT)
            cap = CONN_TX_LIMIT;
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

void conn_flush(int slot) {
    Conn *c = slot_conn(slot);
    if (!c)
        return;
    size_t sent = 0;
    while (sent < c->tx_len) {
        ssize_t n = send(c->fd, c->tx + sent, c->tx_len - sent,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            c->tx_len = 0;
            conn_mark_drop(slot, "send failed (peer gone)");
            return;
        }
    }
    if (sent > 0) {
        memmove(c->tx, c->tx + sent, c->tx_len - sent);
        c->tx_len -= sent;
    }
}

void conn_wipe_message(int slot) {
    Conn *c = slot_conn(slot);
    if (c)
        auth_wipe(c->msg, sizeof(c->msg));
}

bool conn_tx_pending(int slot) {
    Conn *c = slot_conn(slot);
    return c && c->tx_len > 0;
}

/* --- Deferred removal ------------------------------------------------- */

void conn_mark_drop(int slot, const char *reason) {
    Conn *c = slot_conn(slot);
    if (!c || c->dropping)
        return;
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

void conn_send_auth_fail_now(int fd, const char *reason) {
    struct {
        MsgHeader h;
        MsgAuthFail m;
    } pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.h = msg_hdr(MSG_AUTH_FAIL, (int)sizeof(MsgAuthFail));
    copy_str(pkt.m.reason, reason, sizeof(pkt.m.reason));
    /* One attempt: a freshly accepted socket has an empty send buffer. */
    (void)send(fd, &pkt, sizeof(pkt), MSG_NOSIGNAL | MSG_DONTWAIT);
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
        if (g_ipguard[i].ip[0] && strcmp(g_ipguard[i].ip, ip) == 0)
            return &g_ipguard[i];
        if (g_ipguard[i].last_seen_ms < oldest->last_seen_ms)
            oldest = &g_ipguard[i];
    }
    if (!create)
        return NULL;
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
        server_log("AUTH", "Too many failed logins from %s: locked out for %lld s",
                   ip, LOGIN_LOCKOUT_MS / 1000);
    }
}

void ipguard_login_succeeded(const char *ip) {
    IpRecord *r = ipguard_find(ip, false, 0);
    if (r)
        r->failures = 0;
}
