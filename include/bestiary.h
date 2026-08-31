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
#ifndef BESTIARY_H
#define BESTIARY_H

#include <stdint.h>
#include <stdbool.h>
#include "rules.h"

typedef struct {
    const char* name;
    int hp_avg;
    int ac;
    int xp;
    int gold;
    const char* description;

    //AI type (data-driven, not hard-coded by name)
    const char* archetype;   //"melee", "caster", "swarm", "dragon", "brute", "boss"

    // Speed system (Phase 4 - Speed/Tick System)
    int speed;               // 1=lento, 2=normale, 3=veloce

    //Visual range
    int sight_range;         //default 5 if 0

    //Basic damage die
    int damage_dice;         //number of dice (e.g. 2)
    int damage_sides;        //faces of the die (e.g. 6 → 2d6)

    //Spawning dungeon plan
    int floor_min;           //minimum plan (0 = any)
    int floor_max;           //maximum floor (0 = any)

    //Arrays mapping Damage and Conditions using Enum (Method 1)
    bool damage_resistances[MAX_DAMAGE_TYPES];
    bool damage_immunities[MAX_DAMAGE_TYPES];
    bool damage_vulnerabilities[MAX_DAMAGE_TYPES];
    bool condition_immunities[MAX_CONDITIONS];
} MonsterTemplate;


extern MonsterTemplate* bestiary_data;
extern int bestiary_size;

#endif // BESTIARY_H
