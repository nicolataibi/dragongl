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

#include "writer.h"
#include "net.h"

#include "server_log.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WRITER_PATH_MAX 1024

typedef struct WriterJob {
    struct WriterJob *next;
    char path[WRITER_PATH_MAX];
    bool unlink_only;
    uint32_t magic;
    uint16_t version;
    BinWriter payload;
} WriterJob;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_work = PTHREAD_COND_INITIALIZER; /* queue not empty / stop */
static pthread_cond_t g_done = PTHREAD_COND_INITIALIZER; /* a job finished */
static pthread_t g_thread;
static bool g_running;
static bool g_stopping;
static WriterJob *g_head, *g_tail;
static char g_busy_path[WRITER_PATH_MAX]; /* job in progress, "" if none */
static unsigned g_failures;
static unsigned g_depth;     /* jobs queued, the one in progress excluded */
static unsigned g_depth_max; /* highest g_depth since the start */

static void job_free(WriterJob *j) {
    if (!j) {
        return;
    }
    bw_free(&j->payload);
    free(j);
}

/* The disk operation itself. Returns true on success. */
static bool job_perform(const WriterJob *j) {
    if (j->unlink_only) {
        if (unlink(j->path) == 0 || errno == ENOENT) {
            return true;
        }
    } else if (binfile_write_atomic(j->path, j->magic, j->version, &j->payload)) {
        return true;
    }
    int saved = errno;
    char msg[128];
    if (strerror_r(saved, msg, sizeof(msg)) != 0) {
        copy_str(msg, "unknown error", sizeof(msg));
    }
    server_log("SAVE", "Failed to %s %s (%s)%s", j->unlink_only ? "remove" : "save", j->path, msg,
               j->unlink_only ? "." : " — previous file kept.");
    errno = saved;
    return false;
}

static void *writer_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!g_head && !g_stopping) {
            pthread_cond_wait(&g_work, &g_lock);
        }
        if (!g_head) {
            break; /* stopping and drained */
        }
        WriterJob *j = g_head;
        g_head = j->next;
        if (!g_head) {
            g_tail = NULL;
        }
        g_depth--;
        copy_str(g_busy_path, j->path, sizeof(g_busy_path));
        pthread_mutex_unlock(&g_lock);

        bool ok = job_perform(j);
        job_free(j);

        pthread_mutex_lock(&g_lock);
        if (!ok) {
            g_failures++;
        }
        g_busy_path[0] = '\0';
        pthread_cond_broadcast(&g_done);
    }
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

bool writer_start(void) {
    pthread_mutex_lock(&g_lock);
    if (g_running) {
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    g_stopping = false;
    int rc = pthread_create(&g_thread, NULL, writer_main, NULL);
    g_running = (rc == 0);
    pthread_mutex_unlock(&g_lock);
    if (rc != 0) {
        server_log("SYS", "Cannot start the writer thread (%s): saves stay synchronous.",
                   strerror(rc));
    }
    return rc == 0;
}

bool writer_stop(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_running) {
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    unsigned before = g_failures;
    g_stopping = true;
    pthread_cond_broadcast(&g_work);
    pthread_mutex_unlock(&g_lock);
    pthread_join(g_thread, NULL);
    pthread_mutex_lock(&g_lock);
    g_running = false;
    g_stopping = false;
    bool ok = (g_failures == before);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool writer_running(void) {
    pthread_mutex_lock(&g_lock);
    bool r = g_running;
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* Queue j, replacing a job for the same path that has not started yet.
 * Called with g_lock held. */
static void enqueue_locked(WriterJob *j) {
    for (WriterJob **pp = &g_head; *pp; pp = &(*pp)->next) {
        WriterJob *old = *pp;
        if (strcmp(old->path, j->path) != 0) {
            continue;
        }
        j->next = old->next;
        *pp = j;
        if (g_tail == old) {
            g_tail = j;
        }
        job_free(old);
        return;
    }
    j->next = NULL;
    if (g_tail) {
        g_tail->next = j;
    } else {
        g_head = j;
    }
    g_tail = j;
    if (++g_depth > g_depth_max) {
        g_depth_max = g_depth;
    }
    pthread_cond_signal(&g_work);
}

static bool submit(WriterJob *j) {
    pthread_mutex_lock(&g_lock);
    if (g_running && !g_stopping) {
        enqueue_locked(j);
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    pthread_mutex_unlock(&g_lock);
    /* No thread: perform it here, after any job for the same path that a
     * stopping thread is still draining. */
    writer_wait_for(j->path);
    bool ok = job_perform(j);
    job_free(j);
    return ok;
}

static WriterJob *job_new(const char *path) {
    if (!path || strlen(path) >= WRITER_PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    WriterJob *j = calloc(1, sizeof(*j));
    if (!j) {
        return NULL;
    }
    copy_str(j->path, path, sizeof(j->path));
    return j;
}

bool writer_submit(const char *path, uint32_t magic, uint16_t version, BinWriter *w) {
    WriterJob *j = job_new(path);
    if (!j) {
        bw_free(w);
        return false;
    }
    j->magic = magic;
    j->version = version;
    j->payload = *w; /* ownership moves to the job */
    bw_init(w);
    return submit(j);
}

bool writer_submit_unlink(const char *path) {
    WriterJob *j = job_new(path);
    if (!j) {
        return false;
    }
    j->unlink_only = true;
    return submit(j);
}

static bool pending_locked(const char *path) {
    if (strcmp(g_busy_path, path) == 0) {
        return true;
    }
    for (const WriterJob *j = g_head; j; j = j->next) {
        if (strcmp(j->path, path) == 0) {
            return true;
        }
    }
    return false;
}

void writer_wait_for(const char *path) {
    if (!path) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    while (pending_locked(path)) {
        pthread_cond_wait(&g_done, &g_lock);
    }
    pthread_mutex_unlock(&g_lock);
}

void writer_queue_depth(unsigned *now, unsigned *highest) {
    pthread_mutex_lock(&g_lock);
    *now = g_depth;
    *highest = g_depth_max;
    pthread_mutex_unlock(&g_lock);
}

unsigned writer_failures(void) {
    pthread_mutex_lock(&g_lock);
    unsigned n = g_failures;
    pthread_mutex_unlock(&g_lock);
    return n;
}
