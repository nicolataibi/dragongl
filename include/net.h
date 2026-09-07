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

int net_create_server(int port);
int net_connect_to_server(const char *ip, int port);
void net_set_nonblocking(int sock);
bool net_send(int sock, const void *data, int len);
int net_receive(int sock, void *buffer, int max_len);
int net_receive_all(int sock, void *buffer, int len);
/*Read EXACTLY len bytes (no partial reads): blocks with short pauses until
 * len bytes arrive, the peer closes the connection (returns -1) or the
 * retry budget is exhausted (returns -1). Use it for protocol headers so a
 * short recv() can never be mistaken for a complete header.*/
int net_receive_exact(int sock, void *buffer, int len);
void net_close(int sock);

/*Safe string copy that ALWAYS NUL-terminates: copies at most cap-1 chars
 * from src (empty string if cap < 1) and sets dst[cap-1] = 0.
 * Replaces the many bare strncpy() calls that relied on the destination
 * being zero-initialized to guarantee termination.*/
void copy_str(char *dst, const char *src, size_t cap);

#endif
