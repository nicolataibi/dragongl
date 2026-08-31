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

#ifndef AOE_H
#define AOE_H

#include <stdbool.h>
#include "server_entities.h"
#include "spells.h"

// -------------------------------------------------------
//Narrative texts by AoE type - used in send_text
// -------------------------------------------------------
typedef struct {
    const char *cast_msg;   //Launch message
    const char *hit_msg;    //Message for each target hit
    const char *save_msg;   //Message when target resists (saves)
    const char *kill_msg;   //Kill message
    const char *cloud_msg;  //Persistent cloud creation message
} AoeNarrative;

// -------------------------------------------------------
//AoE engine features
// -------------------------------------------------------

//Initialize the cloud pool
void aoe_init_clouds(void);

//Process all AoE spells
//Returns the total number of targets hit
int aoe_resolve_spell(
    SpellTemplate *sp,
    Client        *caster,
    NPC           *npcs,
    int            npc_count,
    int            origin_x,
    int            origin_y
);

//Per-tick update: persistent cloud damage, countdown
void aoe_update_clouds(NPC *npcs, int npc_count, Client *clients, int client_count);

//Helper: Apply damage to a single NPC with narrative message
void aoe_apply_damage_to_npc(
    NPC           *npc,
    Client        *caster,
    int            dmg,
    bool           saved,
    const char    *effect_name,
    const char    *kill_msg
);

#endif // AOE_H
