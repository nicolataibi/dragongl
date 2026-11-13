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
 * server_login.h - Connections before they are players.
 */

#ifndef SERVER_LOGIN_H
#define SERVER_LOGIN_H

#include <stdbool.h>
#include <stdint.h>

#include "login_worker.h"
#include "server_entities.h"

/*Server access password (required) and DM password (optional), from a
 * file or the environment; false (after the message) without the first.*/
bool load_server_secrets(const char *gate_file, const char *dm_file);
/*Accepts every queued connection; one that cannot be served is told why
 * and closed at once.*/
void accept_pending_connections(int listen_fd, int epoll_fd, Client *clients);
/*Drops the connections that did not log in within LOGIN_TIMEOUT_MS.*/
void expire_pending_logins(Client *clients);
/*MSG_LOGIN of slot i: the cheap checks, then the login worker.*/
void handle_login(Client *clients, int i, const void *payload, uint32_t len);
/*The worker's verdict on a login: the character enters the world or the
 * connection is refused.*/
void login_finish(Client *clients, LoginJob *job, Game *game);

#endif /* SERVER_LOGIN_H */
