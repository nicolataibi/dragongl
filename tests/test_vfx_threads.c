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

/**
 * test_vfx_threads.c — Concurrency regression test for the shared VFX and
 * FCT pools (B1).
 *
 * In the client, the NET thread spawns (spawn_vfx on MSG_SPELL_VFX,
 * fct_combat_event on MSG_COMBAT_EVENT) while the RENDER thread updates and reads
 * (particles_update / fct_update + the per-backend draw passes). Before
 * the fix the two threads touched g_particles[] / g_fct[] with no
 * synchronization at all (ThreadSanitizer-flaggable).
 *
 * This test hammers the REAL production pools from two threads:
 *   - "net" thread:    spawn_vfx() (all 5 types) + fct_combat_event()
 *   - "render" thread: particles_update() + fct_update() + draw-side
 *                      scans that read every field the renderers read
 *                      and DEACTIVATE out-of-range particles exactly
 *                      like draw_particles() does
 * then, once both threads are joined, checks the pool invariants a race
 * would corrupt, and proves LIVENESS: the reader must have actually
 * observed spawned particles and FCTs (a deadlock would make it 0).
 *
 * For the race itself, build and run this test under ThreadSanitizer:
 *   cmake -B build-tests-tsan -S tests \
 *         -DCMAKE_C_FLAGS=-fsanitize=thread \
 *         -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
 *   cmake --build build-tests-tsan && ./build-tests-tsan/test_vfx_threads
 * (before the fix, TSan reports multiple data races within ~1 second).
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "client_particles.h"
#include "client_fct.h"
#include "protocol.h"

#define RUN_SECONDS 2

/*Both threads print: serialize with a log mutex (TSan-clean output).*/
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
#define TLOG(...) do { \
    pthread_mutex_lock(&g_log_mutex); \
    printf(__VA_ARGS__); \
    fflush(stdout); \
    pthread_mutex_unlock(&g_log_mutex); \
} while (0)

static volatile bool g_stop = false;

/*The "net thread": the exact calls net_thread.c makes when the server
 * sends MSG_SPELL_VFX / MSG_TEXT.*/
static void *net_thread(void *arg) {
    (void)arg;
    int type = 0;
    while (!g_stop) {
        spawn_vfx(type, 0.0f, 0.0f, 3.0f, 4.0f, 1.0f, 0.5f, 0.2f);
        type = (type + 1) % 5;
        fct_combat_event(COMBAT_EV_DAMAGE_TAKEN, 7);
        fct_combat_event(COMBAT_EV_CRITICAL, 12);
        fct_combat_event(0, 0); /* unknown kind: no FCT */
        usleep(100); /*~10k spawn rounds/s — plenty of contention*/
    }
    return NULL;
}

static volatile long g_seen_particles = 0;
static volatile long g_seen_fct = 0;

/*The "render thread": particles_update + fct_update (exactly what the
 * frame loop calls) plus a draw_particles()-style scan of the pool,
 * including its DEACTIVATION of out-of-range particles.
 *
 * IMPORTANT: the scans mirror the REAL draw passes, which run under
 * g_particles_mutex / g_fct_mutex (B1) — draw_particles(),
 * draw_floating_combat_text() (GL) and the VBO mapping pass (VK).
 * An unlocked scan here would be a race in the TEST, not in the client.*/
static void *render_thread(void *arg) {
    (void)arg;
    while (!g_stop) {
        particles_update(1.0f / 60.0f);
        fct_update(1.0f / 60.0f);

        const float vr = 4.0f; /*a small vision radius, like the default*/
        pthread_mutex_lock(&g_particles_mutex);
        for (int i = 0; i < MAX_PARTICLES; i++) {
            Particle *p = &g_particles[i];
            if (!p->active) continue;
            /*Every field the draw passes read:*/
            float rx = p->x, ry = p->y, rz = p->z;
            float rr = p->r, rg = p->g, rb = p->b, ra = p->a;
            (void)rx; (void)ry; (void)rz; (void)rr; (void)rg; (void)rb; (void)ra;
            g_seen_particles++;
            /*draw_particles() deactivates particles beyond the radius —
             * a WRITE to the shared pool, part of the original race.*/
            if (rx * rx + rz * rz > vr * vr) {
                p->active = false;
            }
        }
        pthread_mutex_unlock(&g_particles_mutex);

        pthread_mutex_lock(&g_fct_mutex);
        for (int i = 0; i < FCT_MAX_ENTRIES; i++) {
            const FloatingText *f = &g_fct[i];
            if (!f->active) continue;
            (void)f->offset_y; (void)f->alpha; (void)f->scale;
            (void)f->world_x;  (void)f->world_z; (void)f->text;
            g_seen_fct++;
        }
        pthread_mutex_unlock(&g_fct_mutex);
        usleep(20);
    }
    return NULL;
}

/*Single-threaded (both workers joined): any violation here means a race
 * corrupted the pool while it ran.*/
static int check_invariants(void) {
    int bad = 0;
    for (int i = 0; i < MAX_PARTICLES; i++) {
        const Particle *p = &g_particles[i];
        if (p->active &&
            (p->max_life <= 0.0f || p->life > p->max_life || p->life < 0.0f)) {
            TLOG("  BAD particle %d: active with life=%.3f max_life=%.3f\n",
                 i, p->life, p->max_life);
            bad++;
        }
    }
    for (int i = 0; i < FCT_MAX_ENTRIES; i++) {
        const FloatingText *f = &g_fct[i];
        if (f->active &&
            (f->max_life <= 0.0f || f->life > f->max_life || f->life < 0.0f ||
             f->text[FCT_TEXT_LEN - 1] != '\0')) {
            TLOG("  BAD fct %d: active with life=%.3f max_life=%.3f text=\"%s\"\n",
                 i, f->life, f->max_life, f->text);
            bad++;
        }
    }
    return bad;
}

int main(void) {
    particles_init();
    memset(g_fct, 0, sizeof(g_fct));

    pthread_t net_t, rend_t;
    if (pthread_create(&net_t, NULL, net_thread, NULL) != 0 ||
        pthread_create(&rend_t, NULL, render_thread, NULL) != 0) {
        fprintf(stderr, "FAIL: pthread_create\n");
        return 1;
    }

    struct timespec ts = { RUN_SECONDS, 0 };
    nanosleep(&ts, NULL);
    g_stop = true;
    pthread_join(net_t, NULL);
    pthread_join(rend_t, NULL);

    int bad = check_invariants();
    TLOG("reader observed %ld active particles and %ld active FCTs\n",
         (long)g_seen_particles, (long)g_seen_fct);

    if (bad > 0) {
        printf("FAIL: %d corrupted pool entries after %ds of concurrent access\n",
               bad, RUN_SECONDS);
        return 1;
    }
    if (g_seen_particles == 0 || g_seen_fct == 0) {
        printf("FAIL: the reader never observed any spawned particle/FCT "
               "(liveness — likely deadlock)\n");
        return 1;
    }
    printf("PASS: vfx/fct pools survived %ds of concurrent spawn/update/draw\n",
           RUN_SECONDS);
    return 0;
}
