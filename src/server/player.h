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
 * player.h - A character: its numbers (player_stats.c) and its save file
 * (player_store.c).
 */

#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>
#include <stddef.h>

#include "items.h"
#include "persist.h"
#include "server_entities.h"

/* --- player_stats.c --- */

/*A client-provided ability score clamped to the playable range 3..18.*/
int clamp_stat(int v);
/*The six abilities with the bonuses of the equipment and of the effects.*/
void get_total_stats(Client *c, int *ts, int *td, int *tc, int *ti, int *tw, int *th);
int get_player_ac(Client *c);
int get_vision_radius(Client *c);
float get_current_weight(Client *c);
float get_movement_cooldown(Client *c);
int get_equipped_mask(Client *c);
/*"Blessed fine Flaming Longsword +1": the name a player reads.*/
void get_full_item_name(const ItemInstance *inst, char *buf, size_t max_len);
void damage_item(ItemInstance *it, int amt);
int get_xp_threshold(int level);
void check_level_up(Client *c);
void give_starting_gear(Client *c);

/* --- player_store.c --- */

void save_player_data(Client *c);
/*Applies a verified save image to the client; every field is untrusted
 * and range-checked.*/
void apply_player_save(Client *c, const SaveData *sd);
/*Synchronous load (tests and tools; a network login goes through the
 * login worker). 1 loaded, 0 no save (new character), -1 wrong password,
 * -2 unreadable: the login is refused and the file is kept intact.*/
int load_player_data(Client *c, const char *password);
/*The bones file a dead character leaves on its floor.*/
void save_bones(Game *game, Client *c);

#endif /* PLAYER_H */
