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

#ifndef CLIENT_PARTICLES_H
#define CLIENT_PARTICLES_H

#include <stdbool.h>
#include <pthread.h>

#define MAX_PARTICLES 2000

typedef struct {
    float x, y, z;
    float vx, vy, vz;
    float r, g, b, a;
    float life, max_life;
    float size;
    int type; // 0=spark, 1=smoke/explosion, 2=trail/aura, 3=magic_missile, etc.
    bool active;
} Particle;

extern Particle g_particles[MAX_PARTICLES];

/*THREADING (B1): the pool is shared by TWO threads — the NET thread fills
 * it (spawn_vfx, called from net_thread.c on MSG_SPELL_VFX) and the RENDER
 * thread drains/reads it (particles_update + the per-backend draw pass). It
 * used to be accessed with no synchronization at all (TSan-flaggable).
 * RULE: every access to g_particles[] — spawn, update OR draw — must hold
 * g_particles_mutex (spawn_vfx/particles_update take it internally; the
 * renderers take it around their draw passes). This mutex is NEVER nested
 * with g_state_mutex, so there is no lock-ordering hazard.*/
extern pthread_mutex_t g_particles_mutex;

void particles_init(void);
void particles_update(float dt);
void spawn_vfx(int type, float sx, float sy, float tx, float ty, float r, float g, float b);

#endif // CLIENT_PARTICLES_H
