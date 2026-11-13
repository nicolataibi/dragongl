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
 * Every connection is TLS 1.3 (server_tls.h): conn_read() and conn_flush()
 * also drive the handshake, which never blocks the loop either, and a
 * connection that does not complete it in LOGIN_TIMEOUT_MS is dropped
 * like one that does not log in.
 *
 * A connection the server refuses at once (server full, address locked
 * out, too many pending logins) gets one of the CONN_REFUSAL_SLOTS
 * slots, beyond the client slots: the handshake, the MSG_AUTH_FAIL with
 * the reason and the close happen there, within CONN_REFUSAL_TIMEOUT_MS.
 * With every refusal slot busy, the socket is just closed.
 *
 * The per-IP guard limits pending (unauthenticated) connections and locks
 * out an address after repeated failed logins.
 */

#ifndef SERVER_CONN_H
#define SERVER_CONN_H

#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
#define LOGIN_FAIL_LIMIT     5
#define LOGIN_FAIL_WINDOW_MS 60000LL
#define LOGIN_LOCKOUT_MS     60000LL

#define CONN_IP_MAX 64

/* Refused connections being told why (see above), and their epoll ids. */
#define CONN_REFUSAL_SLOTS      16
#define CONN_REFUSAL_TIMEOUT_MS 3000LL
#define CONN_REFUSAL_ID_BASE    0x10000u

typedef enum {
    CONN_RX_OK,     /* data read, or nothing available right now */
    CONN_RX_CLOSED, /* orderly shutdown by the peer */
    CONN_RX_ERROR   /* socket error */
} ConnRxStatus;

/* Lifecycle (slot = index in game->clients). conn_open() starts the TLS
 * server side on fd; false if TLS is not initialized or fails. The
 * plaintext slots of conn_open_plain() exist only for the in-process
 * tests (tests/integration/fixture.c). */
bool conn_open(int slot, int fd, const char *peer_ip, long long now_ms);
void conn_open_plain(int slot, int fd, const char *peer_ip, long long now_ms);
void conn_release(int slot); /* frees the buffers; the fd is closed by the caller */
/* Flush what is queued, send close_notify, close the fd, release. */
void conn_close(int slot);
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
/* Decrypted bytes kept by TLS that conn_read() did not take (its buffer
 * was full): epoll will not report them, call conn_read() again. */
bool conn_input_buffered(int slot);

/* Zero the buffer of the last message handed out (credentials). */
void conn_wipe_message(int slot);

/* Send side. */
bool conn_queue(int slot, const void *data, size_t len); /* false = slot is being dropped */
void conn_flush(int slot);                               /* marks the slot on a hard error */
/* Bytes waiting, or TLS waiting for the socket to be writable. */
bool conn_tx_pending(int slot);

/* Deferred removal. */
void conn_mark_drop(int slot, const char *reason);
bool conn_dropping(int slot);
const char *conn_drop_reason(int slot);

/* Refused connections (see above). conn_refuse() takes the socket (it is
 * closed on any failure); events of the ids conn_is_refusal_id() go to
 * conn_refusal_event(); conn_refusals_expire() runs once per loop. */
void conn_refuse(int epoll_fd, int fd, const char *peer_ip, const char *reason, long long now_ms);
bool conn_is_refusal_id(uint32_t id);
void conn_refusal_event(int epoll_fd, uint32_t id, long long now_ms);
void conn_refusals_expire(int epoll_fd, long long now_ms);

/* Per-IP guard. */
bool ipguard_locked(const char *ip, long long now_ms);
void ipguard_login_failed(const char *ip, long long now_ms);
void ipguard_login_succeeded(const char *ip);

#endif
