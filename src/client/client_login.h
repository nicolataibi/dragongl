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
 * client_login.h - What the client asks on the terminal before it
 * connects: the server, the passwords, the character.
 */

#ifndef CLIENT_LOGIN_H
#define CLIENT_LOGIN_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char server_pass[128];
    char username[PROTO_NAME_LEN];
    char password[128];
    /*Used by the server only for a character that does not exist yet.*/
    int race_id;
    int subrace_id; /*-1: none*/
    int class_id;
    int alignment;
} LoginAnswers;

/*Asks everything on standard input. host: the default, replaced by the
 * answer when ask_host. Passwords are read without echo on a terminal.*/
void login_prompt(LoginAnswers *out, char *host, size_t host_cap, bool ask_host);

#endif /* CLIENT_LOGIN_H */
