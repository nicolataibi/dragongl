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
 * aoe.h - Areas of effect: which cells a spell covers, what its damage
 * does to the terrain there, and the clouds that stay on the map.
 */

#ifndef AOE_H
#define AOE_H

#include <stdbool.h>
#include "server_entities.h"
#include "spell_rules.h"
#include "spells.h"

/*A spell area on the map: centred on (ox, oy) for a circle or a cloud;
 * from (ox, oy) towards (tx, ty) for a cone or a line.*/
typedef struct {
    SpellTargetType shape;
    int radius;
    int ox, oy;
    int tx, ty;
} AoeArea;

/*True if the cell (x, y) lies in the area.*/
bool aoe_contains(const AoeArea *a, int x, int y);

/*The voxel chemistry of a damage type over the area: fire melts ice and
 * burns wood, cold freezes water, acid eats doors, force breaks walls.*/
void aoe_terraform(Game *game, const AoeArea *a, int floor_id, DamageType type);

/*The line the caster sees when the area spell goes off.*/
void aoe_announce(const Client *caster, const SpellTemplate *sp);

#define MAX_CLOUDS 32

/*A cloud spell that stays on the map: each round it resolves its effect
 * again against every creature inside it.*/
typedef struct {
    bool active;
    int cx, cy;
    int floor_id;
    int rounds_left;
    const SpellTemplate *spell;
    SpellCaster caster; /*the caster's numbers at cast time*/
    char owner[32];     /*caster's username: kills by the cloud credit him*/
} PersistentCloud;

void aoe_init_clouds(Game *game);
/*A new cloud of sp at (cx, cy); false when all MAX_CLOUDS are in use.*/
bool aoe_spawn_cloud(Game *game, const SpellTemplate *sp, const SpellCaster *caster,
                     const char *owner, int floor_id, int cx, int cy);
/*One round of every cloud: its effect on the NPCs inside, then the
 * countdown.*/
void aoe_update_clouds(Game *game, Client *clients, int client_count);

#endif /* AOE_H */
