#include <math.h>
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

#include "client_particles.h"
#include <stdlib.h>
#include <string.h>

/*Helper: random float in [0, 1] without triggering RAND_MAX int->float warning*/
static inline float randf(void) {
    return (float)rand() / (float)RAND_MAX;
}

Particle g_particles[MAX_PARTICLES];

void particles_init(void) {
    memset(g_particles, 0, sizeof(g_particles));
}

void particles_update(float dt) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!g_particles[i].active) continue;
        Particle *p = &g_particles[i];
        
        p->x += p->vx * dt * 60.0f;
        p->y += p->vy * dt * 60.0f;
        p->z += p->vz * dt * 60.0f;
        
        if (p->type == 0 || p->type == 1) p->vy -= 0.01f * dt * 60.0f; // gravity
        
        p->life -= 1.0f * dt * 60.0f;
        p->a = p->life / p->max_life;
        
        if (p->life <= 0.0f || p->y < 0.0f) {
            p->active = false;
        }
    }
}

void spawn_vfx(int type, float sx, float sy, float tx, float ty, float r, float g, float b) {
    int count = 50; 
    if (type == 1) count = 200; // explosion / fireball
    if (type == 2) count = 100; // aura / heal
    if (type == 3) count = 30;  // magic missile moving ray
    if (type == 4) count = 150; // ice storm / blizzard

    for (int i = 0; i < count; i++) {
        int idx = -1;
        for (int j = 0; j < MAX_PARTICLES; j++) {
            if (!g_particles[j].active) { idx = j; break; }
        }
        if (idx == -1) break;

        Particle *p = &g_particles[idx];
        p->active = true;
        p->type = type;
        p->r = r; p->g = g; p->b = b; p->a = 1.0f;
        
        float rv1 = randf() * 2.0f - 1.0f;
        float rv2 = randf() * 2.0f - 1.0f;
        float rv3 = randf() * 2.0f - 1.0f;

        if (type == 0) { // Arrow / Projectile (moving)
            p->x = sx; p->y = 1.0f; p->z = sy;
            float dx = tx - sx;
            float dz = ty - sy;
            float dist = sqrtf(dx*dx + dz*dz);
            if (dist > 0.001f) { dx /= dist; dz /= dist; }
            float speed = 0.6f + randf() * 0.1f;
            p->vx = dx * speed + rv1 * 0.02f;
            p->vy = rv2 * 0.02f;
            p->vz = dz * speed + rv3 * 0.02f;
            p->life = (dist / 0.65f) + randf() * 2.0f;
            p->max_life = p->life;
            p->size = 0.12f;
            p->r = 0.9f; p->g = 0.9f; p->b = 0.9f;
        } else if (type == 1) { // Explosion (Fireball)
            p->x = tx; p->y = 1.0f; p->z = ty;
            p->vx = rv1 * 0.3f; p->vy = rv2 * 0.3f; p->vz = rv3 * 0.3f;
            p->life = randf() * 30.0f + 20.0f;
            p->max_life = p->life;
            p->size = 0.3f;
        } else if (type == 2) { // Aura (Cure wounds)
            p->x = tx + rv1*0.5f; p->y = 0.5f; p->z = ty + rv3*0.5f;
            p->vx = 0;
            p->vy = randf() * 0.1f + 0.05f;
            p->vz = 0;
            p->life = randf() * 40.0f + 20.0f;
            p->max_life = p->life;
            p->size = 0.15f;
        } else if (type == 3) { // Magic Missile (Moving ray)
            p->x = sx;
            p->y = 1.0f + rv2 * 0.1f;
            p->z = sy;
            float dx = tx - sx;
            float dz = ty - sy;
            float dist = sqrtf(dx*dx + dz*dz);
            if (dist > 0.001f) { dx /= dist; dz /= dist; }
            float speed = 0.3f + randf() * 0.3f;
            p->vx = dx * speed + rv1 * 0.02f;
            p->vy = rv2 * 0.02f;
            p->vz = dz * speed + rv3 * 0.02f;
            p->life = (dist / 0.45f) + randf() * 5.0f;
            p->max_life = p->life;
            p->size = 0.15f;
            p->r = r; p->g = g; p->b = b; // Use the spell's actual color instead of forced purple
        } else if (type == 4) { // Ice Blizzard
            p->x = tx + rv1 * 2.0f; p->y = 3.0f + rv2; p->z = ty + rv3 * 2.0f;
            p->vx = rv1 * 0.1f;
            p->vy = -0.1f - randf() * 0.2f;
            p->vz = rv3 * 0.1f;
            p->life = randf() * 50.0f + 20.0f;
            p->max_life = p->life;
            p->size = 0.1f;
            p->r = 0.7f; p->g = 0.9f; p->b = 1.0f; // Ice blue
        }
    }
}
