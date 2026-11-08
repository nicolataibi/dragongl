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

/*
 * Non-blocking client connections.
 *
 * Every client slot owns a receive buffer and a send queue. The main loop
 * never waits on a socket:
 *   - conn_read() drains what the kernel has, conn_next_message() hands
 *     out complete, validated client->server messages one at a time;
 *   - conn_queue() appends outgoing bytes, conn_flush() writes as much as
 *     the kernel accepts and keeps the rest for the next EPOLLOUT.
 * A connection that misbehaves (protocol error, peer gone, send queue
 * overflow) is only MARKED for removal: the main loop reaps it once per
 * iteration (save, notify, close), so no send path ever recurses into a
 * disconnect.
 *
 * The per-IP guard limits pending (unauthenticated) connections and locks
 * out an address after repeated failed logins.
 */

#ifndef SERVER_CONN_H
#define SERVER_CONN_H

#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>

/* Largest client->server message: header + MsgTextCmd (268 bytes). */
#define CONN_RX_CAP 1024
/* A client that lets this much output pile up is not reading: drop it. */
#define CONN_TX_LIMIT ((size_t)2 * 1024 * 1024)
/* Time allowed between TCP accept and a successful MSG_LOGIN. */
#define LOGIN_TIMEOUT_MS 10000LL
/* Unauthenticated connections accepted from a single address. */
#define MAX_PENDING_PER_IP 8
/* Failed logins from one address within LOGIN_FAIL_WINDOW_MS that trigger
 * a lockout of LOGIN_LOCKOUT_MS. */
#define LOGIN_FAIL_LIMIT 5
#define LOGIN_FAIL_WINDOW_MS 60000LL
#define LOGIN_LOCKOUT_MS 60000LL

#define CONN_IP_MAX 64

typedef enum {
    CONN_RX_OK,     /* data read, or nothing available right now */
    CONN_RX_CLOSED, /* orderly shutdown by the peer */
    CONN_RX_ERROR   /* socket error */
} ConnRxStatus;

/* Lifecycle (slot = index in g_clients). */
void conn_open(int slot, int fd, const char *peer_ip, long long now_ms);
void conn_release(int slot);     /* frees the buffers; the fd is closed by the caller */
bool conn_is_open(int slot);
int conn_fd(int slot);
int conn_slot_for_sock(int sock); /* -1 if the socket belongs to no live slot */
const char *conn_peer_ip(int slot);
long long conn_opened_ms(int slot);

/* Receive side. */
ConnRxStatus conn_read(int slot);
/* 1 = a message is ready (*payload points to hdr->length bytes, valid
 * until the next call); 0 = need more bytes; -1 = protocol violation. */
int conn_next_message(int slot, MsgHeader *hdr, const void **payload);

/* Bytes received but not yet handed out by conn_next_message(). */
bool conn_rx_pending(int slot);

/* Zero the buffer of the last message handed out (credentials). */
void conn_wipe_message(int slot);

/* Send side. */
bool conn_queue(int slot, const void *data, size_t len); /* false = slot is being dropped */
void conn_flush(int slot);                                /* marks the slot on a hard error */
bool conn_tx_pending(int slot);

/* Deferred removal. */
void conn_mark_drop(int slot, const char *reason);
bool conn_dropping(int slot);
const char *conn_drop_reason(int slot);

/* Best-effort MSG_AUTH_FAIL on a socket that never got a slot. */
void conn_send_auth_fail_now(int fd, const char *reason);

/* Per-IP guard. */
bool ipguard_locked(const char *ip, long long now_ms);
void ipguard_login_failed(const char *ip, long long now_ms);
void ipguard_login_succeeded(const char *ip);

#endif
