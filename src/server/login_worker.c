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

#include "login_worker.h"

#include "server_log.h"
#include "writer.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static bool g_running;
static bool g_stopping;
static int g_efd = -1;
static LoginJob *g_todo_head, *g_todo_tail;
static LoginJob *g_done_head, *g_done_tail;
static unsigned g_backlog; /* jobs submitted and not verified yet */

static void push(LoginJob **head, LoginJob **tail, LoginJob *j) {
    j->next = NULL;
    if (*tail) {
        (*tail)->next = j;
    } else {
        *head = j;
    }
    *tail = j;
}

static LoginJob *pop(LoginJob **head, LoginJob **tail) {
    LoginJob *j = *head;
    if (j) {
        *head = j->next;
        if (!*head) {
            *tail = NULL;
        }
        j->next = NULL;
    }
    return j;
}

void login_job_free(LoginJob *job) {
    if (!job) {
        return;
    }
    auth_wipe(job, sizeof(*job));
    free(job);
}

void login_job_run(LoginJob *job) {
    job->outcome = LOGIN_SERVER_ERROR;
    job->why[0] = '\0';
    if (job->is_dm && !auth_check_dm(job->ml.password)) {
        job->outcome = LOGIN_BAD_DM;
        goto out;
    }
    /* A save queued by the previous session (logout, autosave) must be on
     * disk before it is read, or the character would come back older. */
    writer_wait_for(job->save_path);
    BinFileStatus st = persist_read_player(job->save_path, &job->sd, job->why, sizeof(job->why));
    if (st == BINFILE_MISSING) {
        job->outcome =
            auth_hash_password(job->ml.password, job->new_hash) ? LOGIN_NEW : LOGIN_SERVER_ERROR;
    } else if (st != BINFILE_OK) {
        job->outcome = LOGIN_UNREADABLE;
    } else if (auth_verify_password(job->sd.password_hash, job->ml.password)) {
        job->outcome = LOGIN_LOADED;
    } else {
        auth_wipe(&job->sd, sizeof(job->sd));
        job->outcome = LOGIN_BAD_PASSWORD;
    }
out:
    auth_wipe(job->ml.password, sizeof(job->ml.password));
}

static void signal_done(void) {
    uint64_t one = 1;
    while (write(g_efd, &one, sizeof(one)) < 0 && errno == EINTR) {
    }
}

static void *worker_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!g_todo_head && !g_stopping) {
            pthread_cond_wait(&g_work, &g_lock);
        }
        if (g_stopping) {
            break;
        }
        LoginJob *j = pop(&g_todo_head, &g_todo_tail);
        pthread_mutex_unlock(&g_lock);

        login_job_run(j);

        pthread_mutex_lock(&g_lock);
        g_backlog--;
        push(&g_done_head, &g_done_tail, j);
        signal_done();
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

bool login_worker_start(void) {
    if (g_running) {
        return true;
    }
    g_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_efd < 0) {
        return false;
    }
    g_stopping = false;
    if (pthread_create(&g_thread, NULL, worker_main, NULL) != 0) {
        close(g_efd);
        g_efd = -1;
        return false;
    }
    g_running = true;
    return true;
}

void login_worker_stop(void) {
    if (!g_running) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_stopping = true;
    pthread_cond_broadcast(&g_work);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thread, NULL);
    g_running = false;
    LoginJob *j;
    while ((j = pop(&g_todo_head, &g_todo_tail)) != NULL) {
        login_job_free(j);
    }
    g_backlog = 0;
    while ((j = pop(&g_done_head, &g_done_tail)) != NULL) {
        login_job_free(j);
    }
    close(g_efd);
    g_efd = -1;
}

int login_worker_eventfd(void) {
    return g_efd;
}

void login_worker_submit(LoginJob *job) {
    pthread_mutex_lock(&g_lock);
    push(&g_todo_head, &g_todo_tail, job);
    g_backlog++;
    pthread_cond_signal(&g_work);
    pthread_mutex_unlock(&g_lock);
}

unsigned login_worker_backlog(void) {
    pthread_mutex_lock(&g_lock);
    unsigned n = g_backlog;
    pthread_mutex_unlock(&g_lock);
    return n;
}

LoginJob *login_worker_take(void) {
    pthread_mutex_lock(&g_lock);
    LoginJob *j = pop(&g_done_head, &g_done_tail);
    if (!g_done_head) {
        /* Reset the counter while holding the lock: a job finishing after
         * this point writes it again, so no completion is ever missed. */
        uint64_t v;
        while (read(g_efd, &v, sizeof(v)) < 0 && errno == EINTR) {
        }
    }
    pthread_mutex_unlock(&g_lock);
    return j;
}

void log_login_outcome(const LoginJob *job) {
    switch (job->outcome) {
    case LOGIN_BAD_PASSWORD:
        server_log("AUTH", "Incorrect password for: %s", job->ml.username);
        break;
    case LOGIN_UNREADABLE:
        /*Never half-loaded, never treated as a new character (the next save
     * would overwrite it): refused, file kept.*/
        server_log("SAVE", "%s.save is unreadable (%s) — login refused (file kept).",
                   job->ml.username, job->why);
        break;
    case LOGIN_BAD_DM:
        server_log("AUTH", "Invalid DM password from slot %d.", job->slot);
        break;
    case LOGIN_SERVER_ERROR:
        server_log("AUTH", "Password hashing failed for: %s", job->ml.username);
        break;
    default:
        break;
    }
}
