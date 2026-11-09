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
 * Must be >= spell_database_size at runtime: the shipped dataset already
 * contains 551 spells (the last 12 are the class transit cantrips), more
 * than 512. 1024 covers the current dataset with ample room for expansion.
 * The bitfield occupies 1024/64 = 16 uint64_t = 128 bytes per character.
 * Every access to known_spells[] MUST still guard `idx < MAX_SPELL_DB_SIZE`
 * as a defensive measure (see spell_spell_slot_get/set in server_internal).*/
#define MAX_SPELL_DB_SIZE 1024

/*-------------------------------------------------------
 * The effect of a spell, read from the "effect" object of its
 * data/spells.json record (data/schema/spells.schema.json documents every
 * field). Nothing is deduced from the description any more: the record
 * says what the spell does, the engine does exactly that.
 * -------------------------------------------------------*/

/*Where the effect lands.*/
typedef enum {
    SPELL_TARGET_SINGLE,     /*one creature within range*/
    SPELL_TARGET_SELF,       /*the caster*/
    SPELL_TARGET_AOE_CIRCLE, /*every creature within radius of the origin*/
    SPELL_TARGET_AOE_CONE,   /*a 90-degree cone of length radius from the caster*/
    SPELL_TARGET_AOE_LINE,   /*a line of length radius from the caster*/
    SPELL_TARGET_AOE_CLOUD,  /*a circle that stays `rounds` rounds*/
    SPELL_TARGET_COUNT
} SpellTargetType;

typedef enum {
    SPELL_KIND_DAMAGE,    /*dice of damage_type, with an optional condition rider*/
    SPELL_KIND_HEAL,      /*the caster regains dice hit points*/
    SPELL_KIND_CONDITION, /*a condition for `rounds` rounds*/
    SPELL_KIND_BUFF,      /*+value to stat for `rounds` rounds, on the caster*/
    SPELL_KIND_UTILITY,   /*no combat mechanic (exploration, social, ...)*/
    SPELL_KIND_SPECIAL,   /*a dedicated handler of spell_router.c*/
    SPELL_KIND_COUNT
} SpellKind;

/*How a target resists.*/
typedef enum {
    SPELL_ROLL_AUTO,   /*it does not: the effect lands*/
    SPELL_ROLL_ATTACK, /*spell attack roll against its AC*/
    SPELL_ROLL_SAVE,   /*saving throw against the spell DC*/
    SPELL_ROLL_COUNT
} SpellRoll;

typedef enum { SPELL_BUFF_NONE, SPELL_BUFF_AC, SPELL_BUFF_ATTACK, SPELL_BUFF_DAMAGE } SpellBuffStat;

typedef enum {
    SPELL_HANDLER_NONE,
    SPELL_HANDLER_ANIMATE_DEAD,
    SPELL_HANDLER_BLINK,
    SPELL_HANDLER_CHARM_PERSON,
    SPELL_HANDLER_CLASS_TRANSIT,
    SPELL_HANDLER_DOMINATE_MONSTER,
    SPELL_HANDLER_INVISIBILITY,
    SPELL_HANDLER_SUMMON_ELEMENTAL,
    SPELL_HANDLER_TELEPORT,
    SPELL_HANDLER_COUNT
} SpellHandler;

/*Terrain the spell leaves at its origin, after the effect.*/
typedef enum {
    SPELL_TERRAIN_NONE,
    SPELL_TERRAIN_WALL,
    SPELL_TERRAIN_ICE,
    SPELL_TERRAIN_MUD
} SpellTerrain;

typedef struct {
    SpellKind kind;
    SpellTargetType target;
    int range;  /*cells (5 feet each) from the caster to the target or origin*/
    int radius; /*cells: area radius, cone or line length*/
    SpellRoll roll;
    /*SPELL_ROLL_SAVE: the save that resists the whole effect. With an
     * attack or no roll, the save that resists only the condition rider
     * (ABILITY_NONE: the rider lands with the hit).*/
    Ability save;
    bool save_halves; /*damage: a successful save halves it (else none)*/
    int dc;           /*fixed DC (martial maneuvers); 0 = 8 + proficiency + ability*/
    int dice_count;
    int dice_sides;
    int dice_bonus;
    Ability dice_add;     /*ability modifier added to the roll (ABILITY_NONE: none)*/
    bool cantrip_scaling; /*the dice multiply at caster level 5, 11 and 17*/
    DamageType damage_type;
    ConditionType condition; /*MAX_CONDITIONS = none*/
    /*Duration of the condition or buff; for a cloud, its lifetime (the
     * condition a cloud gives lasts one round and is renewed each round).*/
    int rounds;
    SpellBuffStat stat;
    int value;
    SpellHandler handler;
    SpellTerrain terrain;
} SpellEffect;

typedef struct SpellTemplate {
    const char *name;
    int level;    /*0 = cantrip*/
    bool martial; /*school "martial": a maneuver, not a spell*/
    SpellEffect effect;
    int vfx_type;
    float vfx_r;
    float vfx_g;
    float vfx_b;
    /*Bitmask of the classes that can learn this magic.
     * Bit N corresponds to the Nth value of ClassType (0=Barbarian … 11=Wizard).
     * 0 = no class (magic not available to players).*/
    uint32_t class_mask;
} SpellTemplate;

extern SpellTemplate *spell_database;
extern int spell_database_size;

#endif // SPELLS_H
