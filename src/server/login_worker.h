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
 * Login worker: the slow half of a login, off the game loop.
 *
 * Argon2id is deliberately expensive (tens of milliseconds per hash, see
 * auth.h) and reading a save touches the disk: neither may run inside a
 * game tick. A login is split in two:
 *
 *   main loop   login_begin: cheap checks (gate password, username
 *               policy, one session per character, DM account enabled),
 *               then a LoginJob is submitted and the slot is marked
 *               login_pending.
 *   worker      DM password check, writer_wait_for(save) so a logout's
 *               save is on disk, persist_read_player, Argon2id verify (or
 *               hash, for a new character). The plaintext password is
 *               wiped as soon as it is no longer needed.
 *   main loop   login_worker_eventfd() becomes readable; login_finish
 *               applies the result to the slot — if the slot still holds
 *               the same connection (generation check: the client may
 *               have gone away, and the slot may have been reused).
 */

#ifndef DRAGONGL_LOGIN_WORKER_H
#define DRAGONGL_LOGIN_WORKER_H

#include "auth.h"
#include "persist.h"
#include "protocol.h"
#include "server_entities.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LOGIN_LOADED = 1, /* existing character, password verified: sd is valid */
    LOGIN_NEW = 0,    /* no save: new_hash holds the new character's hash */
    LOGIN_BAD_PASSWORD = -1,
    LOGIN_UNREADABLE = -2, /* save exists but is not valid: kept, login refused */
    LOGIN_SERVER_ERROR = -3,
    LOGIN_BAD_DM = -4
} LoginOutcome;

typedef struct LoginJob {
    struct LoginJob *next;
    /* request */
    int slot;
    uint64_t gen;
    MsgLogin ml; /* server_pass already wiped; password wiped by the worker */
    bool is_dm;
    char save_path[STATE_DIR_MAX + 64];
    /* result */
    LoginOutcome outcome;
    char why[96];
    SaveData sd;
    char new_hash[AUTH_HASH_LEN];
} LoginJob;

/* Start the thread and its eventfd (EFD_NONBLOCK | EFD_CLOEXEC). */
bool login_worker_start(void);
/* Discard queued jobs (wiped), stop the thread, close the eventfd. */
void login_worker_stop(void);
int login_worker_eventfd(void);

/* Takes ownership of job (heap-allocated with calloc). */
void login_worker_submit(LoginJob *job);
/* Logins submitted whose password has not been verified yet. */
unsigned login_worker_backlog(void);

/* Next finished job, or NULL. Resets the eventfd when the queue is empty.
 * The caller frees it with login_job_free(). */
LoginJob *login_worker_take(void);

/* Wipe (credentials, hashes, save image) and free. */
void login_job_free(LoginJob *job);

/* The worker's part, also usable synchronously (tests). */
void login_job_run(LoginJob *job);

/*The log line of a refused login.*/
void log_login_outcome(const LoginJob *job);

#endif
