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

int net_create_server(int port);
int net_connect_to_server(const char *ip, int port);
void net_set_nonblocking(int sock);
bool net_send(int sock, const void *data, int len);
int net_receive(int sock, void *buffer, int max_len);
int net_receive_all(int sock, void *buffer, int len);
void net_close(int sock);

#endif
