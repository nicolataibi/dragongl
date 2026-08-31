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

void particles_init(void);
void particles_update(float dt);
void spawn_vfx(int type, float sx, float sy, float tx, float ty, float r, float g, float b);

#endif // CLIENT_PARTICLES_H
