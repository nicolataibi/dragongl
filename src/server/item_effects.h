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
 * item_effects.h - What magic items do to the character who wears, holds
 * or carries them (the "worn" object of data/items.json), and what a
 * consumable does when it is used (the "use" object): saving throws,
 * damage taken, conditions, regeneration, haste. LORE.md, "Magic items".
 */

#ifndef ITEM_EFFECTS_H
#define ITEM_EFFECTS_H

#include <stdbool.h>
#include <stdint.h>

#include "rules.h"
#include "server_entities.h"

/*Damage with no type (hunger, bleeding): only temporary HP stop it.*/
#define DAMAGE_UNTYPED (-1)
/*A monster's weapon, claw or bite: resisted only by who resists
 * bludgeoning, piercing and slashing alike.*/
#define DAMAGE_PHYSICAL (-2)

/*The sum of what the active items of a character give (see
 * player_active_items).*/
typedef struct {
    int ac, ac_unarmored, unarmored_base, saves, ranged_damage;
    int spell_dc, spell_attack, darkvision;
    uint32_t resist, immune, immune_conditions;
    bool haste, displacement, see_invisible, spell_save_advantage, gas_save_advantage;
    bool feather_fall, sustenance, crit_immune;
} WornTotals;

/*Where a saving throw comes from: magic (a spell, a monster's power) and
 * gas give advantage to whoever is protected against them.*/
typedef enum { SAVE_PLAIN, SAVE_MAGIC, SAVE_GAS } SaveSource;

#define EQUIP_SLOTS_ALL  (10 + 10) /*the single slots and the ten rings*/
#define MAX_ACTIVE_ITEMS (EQUIP_SLOTS_ALL + MAX_BACKPACK + MAX_BELT)

/*The items whose worn effects count: every equipped one (a light source
 * only while lit), and each identified "carried" item of the backpack or
 * the belt, one per kind. Returns how many were stored in out.*/
int player_active_items(Client *c, ItemInstance **out);
void player_worn(Client *c, WornTotals *w);

/*The damage types an item resists: its own, and the one rolled for it
 * when it resists "random" (kept in current_charges, as the type + 1).*/
uint32_t item_resist_mask(ItemInstance *it);
/*Rolls the random resistance of a new item (no-op for the others).*/
void item_roll_variant(ItemInstance *it);

/*Modifier of a saving throw: the ability (items included), the
 * proficiency bonus when the class is proficient in it, the save bonuses
 * of the items and of the effects.*/
int player_save_mod(Client *c, Ability a);
/*A saving throw against dc; disadvantage when cursed, advantage from the
 * items against magic or gas. A failed DEX save spends a charge of a
 * worn Ring of Evasion to succeed instead. *roll (may be NULL): the d20.*/
bool player_save(Client *c, Ability a, int dc, SaveSource src, int *roll);

/*Damage taken by a character: none if immune, half if resistant, the
 * temporary hit points first. type: a DamageType, DAMAGE_UNTYPED or
 * DAMAGE_PHYSICAL. Returns the damage after immunity and resistance;
 * death is the caller's (hp <= 0).*/
int player_damage(Client *c, int dmg, int type);

/*True if an item or a magic effect makes the character immune to cond.*/
bool player_condition_immune(Client *c, ConditionType cond);
/*The condition for that many rounds (value: e.g. the charmer), unless the
 * character is immune: then false, after telling him.*/
bool player_add_condition(Client *c, ConditionType cond, int rounds, int value);

/*True if an active item makes the spell (by name) have no effect on him.*/
bool player_spell_immune(Client *c, const char *spell);
bool player_hasted(Client *c);
bool player_sees_invisible(Client *c);
/*Spell save DC and spell attack bonus the worn items add.*/
void player_spell_item_bonus(Client *c, int *dc, int *attack);

/*One round of the items: regeneration.*/
void player_items_round(Game *game, Client *c);

/*What a consumable does when used ("use"): false (nothing spent, after
 * the message) when it cannot be used now.*/
bool item_use_apply(Game *game, Client *c, const ItemTemplate *t);

#endif /* ITEM_EFFECTS_H */
