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

#ifndef SPELLS_H
#define SPELLS_H

#include <stdint.h>
#include "rules.h"

/*Maximum number of spells in the database (used for the known_spells bitfield).
 * Must be >= spell_database_size at runtime. 512 abundantly covers the
 * SRD dataset (395 spells) with room for future expansion.
 * The bitfield occupies 512/64 = 8 uint64_t = 64 bytes per character.*/
#define MAX_SPELL_DB_SIZE 512

// -------------------------------------------------------
//AoE projection types
// -------------------------------------------------------
typedef enum {
    SPELL_TARGET_SINGLE,        //Single target
    SPELL_TARGET_SELF,          //Self-target
    SPELL_TARGET_AOE_CIRCLE,    //Spherical explosion (fireball)
    SPELL_TARGET_AOE_CONE,      //Cone (dragon breath, flame jet)
    SPELL_TARGET_AOE_LINE,      // Linea (fulmine, sputo acido)
    SPELL_TARGET_AOE_CLOUD      //Multi-round persistent cloud (poisonous gas)
} SpellTargetType;

typedef enum {
    SPELL_EFFECT_DAMAGE,
    SPELL_EFFECT_HEAL,
    SPELL_EFFECT_BUFF,
    SPELL_EFFECT_DEBUFF
} SpellEffectType;

// -------------------------------------------------------
//Persistent gas cloud (remains on map N rounds)
// -------------------------------------------------------
#define MAX_CLOUDS 32

typedef struct {
    bool active;
    int  cx, cy;         //Center of the cloud
    int  radius;         //Radius in cells
    int  floor_id;
    int  rounds_left;    //Residual rounds before dissolving
    int  dice_count;
    int  dice_sides;
    DamageType dmg_type;
    int  save_dc;        //Saving Throw DC (Constitution)
    char name[32];       // Es. "Nube Acida", "Gas Velenoso"
} PersistentCloud;

typedef struct {
    const char*     name;
    int             level;
    SpellTargetType target_type;
    SpellEffectType effect_type;
    int             range;
    int             radius;         /*For AoE: radius or cone/line length*/
    int             dice_count;
    int             dice_sides;
    DamageType      damage_type;    /*Damage type for future resistances*/
    int             cloud_rounds;   /*> 0 → create PersistentCloud*/
    ActiveEffect    status_effect;
    bool            has_status_effect;
    int             vfx_type;
    float           vfx_r;
    float           vfx_g;
    float           vfx_b;
    /*Bitmask of the classes that can learn this magic.
     * Bit N corresponds to the Nth value of ClassType (0=Barbarian … 11=Wizard).
     * 0 = no class (magic not available to players).*/
    uint32_t        class_mask;
    /*Innate class magic: known automatically by every character of a
     * matching class (no book required to cast, always in the grimoire).
     * Used for the 12 class transit cantrips.*/
    bool            innate;
} SpellTemplate;

extern SpellTemplate*   spell_database;
extern int              spell_database_size;
extern PersistentCloud  g_clouds[MAX_CLOUDS];

#endif // SPELLS_H
