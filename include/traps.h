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

#ifndef TRAPS_H
#define TRAPS_H

#include <stdbool.h>
#include "rules.h"

typedef enum {
    TRAP_PIT,                //Fall into a hole (DEX save)
    TRAP_SPIKES,             // Spikes from the floor (DEX save)
    TRAP_POISON_NEEDLE,      //Needle from a lock or wall (CON save + Poisoned)
    TRAP_GAS,                //Cloud of poisonous gas (CON save)
    TRAP_ALARM,              // Loud noise (alerts nearby NPCs)
    TRAP_TELEPORT,           // Random teleportation on the same floor
    TRAP_ACID,               // Acid splash trap
    TRAP_WEB,                // Web trap (Restrained)
    TRAP_BLADE,              // Swinging blade (Slashing + Bleeding)
    TRAP_SILENCE,            // Silence trap (Silenced)
    TRAP_LIGHTNING,          // Lightning trap (Lightning dmg)
    TRAP_QUICKSAND,          // Quicksand trap (Prone)
    TRAP_DARKNESS,           // Darkness trap (Blinded)
    TRAP_DART_WALL,          // Poisoned dart fired from wall (DEX save, Piercing+Poisoned)
    //--- New traps ---
    TRAP_FALLING_FLOOR,      //Collapsing floor -> fall to the lower floor (DEX save)
    TRAP_SPRING_SPEAR,       //Spring spear from the wall (DEX save, Piercing + Restrained)
    TRAP_BOULDER,            //Rolling boulder in corridor (DEX save, knockback)
    TRAP_PARALYZING_SPORES,  //Paralyzing Spores (CON save, Paralyzed 2r)
    TRAP_CURSE_RUNE,         //Rune of Curse (WIS save, -2 stat random)
    TRAP_ANTIMAGIC_ZONE,     //Anti-magic field (automatic, Silenced 5r strong)
    TRAP_SLEEP_GAS,          //Sleep Gas (CON save, Unconscious 3r)
    TRAP_ILLUSION_MIRROR,    // ILLUSION MIRROR (WIS save, phantom shield)
    TRAP_SWAP_TELEPORT,      //Swap locations with nearby enemy NPCs
    TRAP_FAKE_DOOR,          //Fake door: teleport into a closed room (INT save)
    TRAP_FLOOD,              //7x7 flooding with VOXEL_WATER (DEX save)
    TRAP_CEILING_COLLAPSE,   //Ceiling Collapse 5x5 with VOXEL_ROCK (DEX save)
    TRAP_CRUSHER_WALL,       //Potato Masher Walls (STR save, massive damage)
    TRAP_GAS_VEIN,           //Explosive vein on ash (WITH save, fire)
    TRAP_CRYSTAL_BURST,      //Exploding Crystal (DEX save, Piercing area)
    TRAP_INVERSION_RUNE,     // INVERSION RUNE (WIS save, Confused 3r)
    TRAP_STONE_TOMB,         //Stone Tomb: VOXEL_WALL walls around (STR save)
    TRAP_HUNGER_CURSE,       //Hunger Curse: Drains HP by healing nearby monsters
    TRAP_BODY_SWAP,          //Body Swap: Exchange HP with nearby NPC
    TRAP_COUNT
} TrapType;

typedef struct {
    TrapType type;
    int x, y;
    int floor_id;
    int detection_dc;
    int save_dc;
    int damage_dice;
    int damage_sides;
    DamageType damage_type;
    int respawn_timer;
    bool active;
    bool detected;
    int wall_dir; //0=N, 1=E, 2=S, 3=W — direction dart fires FROM (dart travels in opposite)
} Trap;

#endif // TRAPS_H
