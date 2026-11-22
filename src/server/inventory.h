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
 * inventory.h - Finding and removing the items a character carries.
 */

#ifndef INVENTORY_H
#define INVENTORY_H

#include <stdbool.h>

#include "server_entities.h"

/*The number a player types for a line of a 1-based list ("3"), as a
 * 0-based index; -1 when s does not start with a digit or the number is 0.*/
int list_index(const char *s);
/*A carried item by number (backpack) or by name (backpack, then belt): its
 * index, with *in_belt set when it is a belt slot; -1 if there is none.*/
int find_carried(const Client *c, const char *query, bool *in_belt);
/*Takes the item out of the backpack (closing the gap) or off the belt.*/
void remove_carried(Client *c, int idx, bool in_belt);

/*Takes backpack[idx] out and closes the gap with the last item; the slot
 * that frees up is emptied. Nothing past backpack_count may hold an item:
 * the remains of a character copy the backpack, and a stale copy left
 * there came back to its owner as a second item.*/
void backpack_take_swap_last(Client *c, int idx);
/*Empties the slot past the last item (after backpack_count--).*/
void backpack_clear_tail(Client *c);
/*True for food: the rations ("Ring of Regeneration" and "Sonatas of
 * Inspiration" also hold the letters "ration").*/
bool item_is_food(const ItemTemplate *t);
/*The food to eat ('eat', 'eat <name>'): as find_carried, among the food
 * only; -1 if the character carries none.*/
int find_food(const Client *c, const char *query, bool *in_belt);

/*A new, identified, ordinary instance of the template: one unit, full
 * durability (a lantern full of oil), every charge of a wand, the damage
 * type of a ring of resistance.*/
void item_instance_new(ItemInstance *out, int template_idx);

/*True if the item has charges that come back with a rest in town (a wand,
 * a staff, a ring of evasion; not the beads of a necklace of fireballs).*/
bool item_recharges(const ItemInstance *it);

/*True if a weapon is magic (an artifact, an elemental or enchanted one, a
 * magic item of items.json): it hurts monsters that only magic hurts.*/
bool item_is_magic(const ItemInstance *it);
/*The damage type of a weapon's element (MAX_DAMAGE_TYPES: none).*/
DamageType element_damage_type(ItemElement e);

/*The ten single equipment slots, in the order of 'inventory', with the
 * names 'remove' and 'repair' accept for each ("head", "arm_r|right_arm|arm"...).*/
#define EQUIP_SLOT_COUNT 10
ItemInstance *equip_slot(Client *c, int i);
const char *equip_slot_names(int i);
/*The equipped item (a single slot, then a ring) whose slot name is word
 * or whose name (or artifact name) contains it; NULL if none.*/
ItemInstance *find_equipped(Client *c, const char *word);

#endif /* INVENTORY_H */
