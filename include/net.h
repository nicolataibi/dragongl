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

#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>

/* Listening TCP socket on every local address (IPv6 dual-stack, or IPv4
 * where IPv6 is unavailable); -1 on error. */
int net_create_server(int port);
/* Connect to host (name, IPv4 or IPv6 address) within timeout_ms per
 * address tried. The socket is non-blocking, close-on-exec, with TCP
 * keepalive. Returns -1 with a reason in why on failure. */
int net_connect_to_server(const char *host, int port, int timeout_ms, char *why,
                          size_t why_len);
void net_set_nonblocking(int sock);
/* Send all len bytes; false if the peer is gone (or stays full ~2 s). */
bool net_send(int sock, const void *data, int len);
void net_close(int sock);

/*Safe string copy that ALWAYS NUL-terminates: copies at most cap-1 chars
 * from src (empty string if cap < 1) and sets dst[cap-1] = 0.
 * Replaces the many bare strncpy() calls that relied on the destination
 * being zero-initialized to guarantee termination.*/
void copy_str(char *dst, const char *src, size_t cap);

#endif
