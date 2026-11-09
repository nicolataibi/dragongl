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
 * Credentials (libsodium).
 *
 *  - Character passwords are stored as Argon2id strings
 *    (crypto_pwhash_str: algorithm, parameters and a random salt are
 *    encoded in the string). They are hashed once, when the character is
 *    created, and verified once per login; the plaintext is wiped right
 *    after.
 *  - The server access ("gate") password is kept only as a keyed BLAKE2b
 *    digest with a random per-process key and compared in constant time:
 *    it is checked on every login and must stay cheap.
 *  - The Dungeon Master password is kept only as an Argon2id string. The
 *    DM account does not exist unless a DM password is configured.
 *
 * Server secrets never come from the command line (visible to every local
 * user in ps and /proc): see auth_load_secret().
 */

#ifndef DRAGONGL_AUTH_H
#define DRAGONGL_AUTH_H

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>

/* Size of an encoded Argon2id string, NUL included
 * (crypto_pwhash_STRBYTES). */
#define AUTH_HASH_LEN 128

/* Server secrets: 8..63 printable, non-blank ASCII characters. 63 is the
 * room a password has in MsgLogin (PROTO_SECRET_LEN, NUL included). */
#define AUTH_SECRET_MIN 8
#define AUTH_SECRET_MAX (PROTO_SECRET_LEN - 1)

bool auth_init(void);

/* Character passwords: 1..63 printable characters. */
bool auth_password_acceptable(const char *pwd);
bool auth_hash_password(const char *pwd, char out[AUTH_HASH_LEN]);
bool auth_verify_password(const char *stored_hash, const char *pwd);

/* Server access password. */
void auth_set_gate_secret(const char *pwd);
bool auth_check_gate(const char *pwd);

/* Dungeon Master password (optional). */
bool auth_set_dm_secret(const char *pwd);
bool auth_dm_enabled(void);
bool auth_check_dm(const char *pwd);

/* Validate a server secret; returns NULL if acceptable, otherwise why not. */
const char *auth_secret_problem(const char *pwd);

/* Read a server secret: first line of `file` if not NULL, otherwise the
 * environment variable `env_name`. The variable's value is wiped in place
 * and removed, so it no longer shows in /proc/<pid>/environ. Returns:
 *   1  secret read into out (NUL-terminated);
 *   0  not configured (no file given and the variable is unset/empty);
 *  -1  error (message in err).
 * A file readable by group or others is accepted with a warning in err
 * (return value 1, err non-empty). */
int auth_load_secret(const char *file, const char *env_name, char *out, size_t out_len, char *err,
                     size_t err_len);

void auth_wipe(void *p, size_t n);

#endif
