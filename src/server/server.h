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

#ifndef SERVER_H
#define SERVER_H

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "items.h"

#define MAX_TOMBSTONES 100
#define TOMBSTONE_SYMBOL 'T'
#define TOMBSTONE_BACKPACK_SIZE 32
#define TOMBSTONE_RINGS 10
#define TOMBSTONE_EXPIRE_HOURS 24

/** Tombstone — Tombstone left at the player's death point.
 * Contains all inventory (backpack + equipped slots) and gold.
 * Only the owner player can recover items.
 * It is saved to disk in saves/tombstone_<owner>.dat*/
typedef struct {
    bool           active;
    int            x;
    int            y;
    int            floor_id;
    char           owner[32];        /*username of the dead player*/
    uint64_t       gold;
    time_t         death_time;

    /*Backpack*/
    ItemInstance   backpack[TOMBSTONE_BACKPACK_SIZE];
    int            backpack_count;

    /*Belt*/
    ItemInstance   belt[4];

    /* Equipment slots */
    ItemInstance   slot_head;
    ItemInstance   slot_neck;
    ItemInstance   slot_body;
    ItemInstance   slot_back;
    ItemInstance   slot_hand_r;
    ItemInstance   slot_hand_l;
    ItemInstance   slot_hands;
    ItemInstance   slot_arm_r;
    ItemInstance   slot_arm_l;
    ItemInstance   slot_feet;
    ItemInstance   slot_rings[TOMBSTONE_RINGS];
} Tombstone;

#endif // SERVER_H
