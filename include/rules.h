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

#ifndef RULES_H
#define RULES_H

#include <stdint.h>
#include <stdbool.h>

//Types of damage
typedef enum {
    DMG_SLASHING,
    DMG_PIERCING,
    DMG_BLUDGEONING,
    DMG_POISON,
    DMG_ACID,
    DMG_FIRE,
    DMG_COLD,
    DMG_RADIANT,
    DMG_NECROTIC,
    DMG_LIGHTNING,
    DMG_THUNDER,
    DMG_FORCE,
    DMG_PSYCHIC,
    MAX_DAMAGE_TYPES
} DamageType;

//State conditions
typedef enum {
    COND_BLINDED,
    COND_CHARMED,
    COND_DEAFENED,
    COND_FRIGHTENED,
    COND_GRAPPLED,
    COND_INCAPACITATED,
    COND_INVISIBLE,
    COND_PARALYZED,
    COND_PETRIFIED,
    COND_POISONED,
    COND_PRONE,
    COND_RESTRAINED,
    COND_STUNNED,
    COND_UNCONSCIOUS,
    COND_BURNING,
    COND_BLEEDING,
    COND_CURSED,
    COND_FROZEN,
    COND_SILENCED,
    MAX_CONDITIONS
} ConditionType;

//Types of events that can trigger effects or reactions
typedef enum {
    EVENT_ON_ATTACK,      //Before the attack roll
    EVENT_ON_HIT,         //After a successful shot
    EVENT_ON_DAMAGE,      //During damage calculation
    EVENT_ON_SAVE,        //During a saving throw
    EVENT_ON_TURN_START,
    EVENT_ON_TURN_END
} RuleEventType;

//Applicable modifier types
typedef enum {
    MOD_ADDITIVE,         //Add a fixed value (e.g. +1)
    MOD_MULTIPLIER,       // Multiplies (e.g. x2 for vulnerability)
    MOD_ADVANTAGE,        // Garantisce vantaggio
    MOD_DISADVANTAGE      // Garantisce svantaggio
} ModifierType;

//Definition of an active effect
typedef struct {
    const char* name;
    RuleEventType trigger;
    ModifierType mod_type;
    int value;
    int duration_rounds;
    bool is_persistent;
} ActiveEffect;

//Context of an event for calculating modifiers
typedef struct {
    RuleEventType type;
    int base_value;
    int final_value;
    bool has_advantage;
    bool has_disadvantage;
    
    // References to involved entities (IDs or pointers)
    int source_id;
    int target_id;
} RuleContext;

// New structures for Resistance and Vulnerability
typedef enum {
    DMG_MOD_NORMAL = 0,
    DMG_MOD_RESISTANCE = 1,
    DMG_MOD_VULNERABILITY = 2,
    DMG_MOD_IMMUNITY = 3
} DamageModifier;

//Framework for share economics
typedef struct {
    int speed_total;
    int speed_remaining;
    bool has_action;
    bool has_bonus_action;
    bool has_reaction;
    int legendary_actions_total;
    int legendary_actions_remaining;
} ActionEconomy;

//Rules engine functions
void rules_apply_modifiers(RuleContext* ctx, ActiveEffect* effects, int effect_count);
int rules_roll_dice(int count, int sides);
bool rules_update_effects(ActiveEffect* effects, int* effect_count);
int rules_get_modifier(int score);
bool rules_has_condition(ActiveEffect* effects, int effect_count, const char* condition_name);

//Extended functions
int rules_roll_d20(bool advantage, bool disadvantage);
bool rules_roll_attack(int bonus, int ac, bool advantage, bool disadvantage, bool *is_crit);
bool rules_roll_attack_detailed(int bonus, int ac, bool advantage, bool disadvantage, bool *is_crit, int *out_roll);
bool rules_roll_save(int modifier, int dc, bool advantage, bool disadvantage, int* out_roll);
int rules_calculate_damage(int raw_damage, DamageModifier dmg_mod);

#endif // RULES_H
