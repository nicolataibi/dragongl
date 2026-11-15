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

/*A new, identified, ordinary instance of the template: one unit, full
 * durability (a lantern full of oil), every charge of a wand.*/
void item_instance_new(ItemInstance *out, int template_idx);

/*True if the item casts a spell from its charges (a wand); a scroll casts
 * one too, but once, and has no charges.*/
bool item_is_wand(const ItemInstance *it);

/*The ten single equipment slots, in the order of 'inventory', with the
 * names 'remove' and 'repair' accept for each ("head", "arm_r|right_arm|arm"...).*/
#define EQUIP_SLOT_COUNT 10
ItemInstance *equip_slot(Client *c, int i);
const char *equip_slot_names(int i);
/*The equipped item (a single slot, then a ring) whose slot name is word
 * or whose name (or artifact name) contains it; NULL if none.*/
ItemInstance *find_equipped(Client *c, const char *word);

#endif /* INVENTORY_H */
