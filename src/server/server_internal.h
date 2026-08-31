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

#ifndef SERVER_INTERNAL_H
#define SERVER_INTERNAL_H

#include "../../include/net.h"
#include "server_entities.h"
#include "server.h"
#include "items.h"
#include "rules.h"
#include "combat_log.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

long long get_time_ms(void);

typedef struct {
    char name[64];
    int x, y;
    uint64_t gold;
    int items[30];
    int amounts[30];
    int w_idx, b_idx, h_idx, s_idx;
} BonesData;

// Global constants
#define RESPAWN_TICKS 120
#define DENSITY_CHECK 50
#define DENSITY_MIN_PCT 40
#define RESPAWN_TRAPS_TICKS 300
/*% chance that a kill will generate a loot item*/
#define LOOT_DROP_CHANCE 60

// Tombstone system
extern Tombstone g_tombstones[MAX_TOMBSTONES];
void tombstone_create(Client *c);
void tombstone_list(Client *c);
bool tombstone_pickup(Client *c);
void tombstone_save(const Tombstone *t);
void tombstone_load_all(void);

// Shared globals
extern World *master_world;
extern Client *g_clients;
extern NPC *g_npcs;
extern int global_total_turns;
extern int next_id;

// World Events
extern int active_event_type;
extern int event_floor_id;
extern int event_time_left;
extern int event_progress;
extern int event_goal;

// Internal helpers from main_server.c made public
void get_total_stats(Client *c, int *ts, int *td, int *tc, int *ti, int *tw, int *th);
int get_player_ac(Client *c);
int get_vision_radius(Client *c);
void sync_entity_grid(NPC *npcs);

void save_bones(Client* c);
void drop_loot_from_monster(Client *c, NPC *killer);
void check_level_up(Client *c);
int get_xp_threshold(int level);
void damage_item(ItemInstance *it, int amt);
void save_player_data(Client *c);
int load_player_data(Client *c);
void send_text_to_client(int sock, const char *fmt, ...);
void send_detailed_state(Client *c);
void server_log(const char *cat, const char *fmt, ...);
void broadcast_spell_vfx(int sx, int sy, int tx, int ty, int vfx_type, float r, float g, float b, int floor_id);
void send_map_chunk(int sock, Map *map, int cx, int cy, int size);
void get_full_item_name(const ItemInstance *inst, char *buf, size_t max_len);
float get_current_weight(Client *c);
void print_merchant_inventory(Client *c, NPC *merchant);
void update_city_doors(void);
void check_tile_events(Client *c, NPC *npcs);
void check_traps(Client *c, NPC *npcs);
/* Update the character's deepest floor ever reached (innate transit magic).
* Call it every time a client's floor_id changes.*/
void client_track_explored_floor(Client *c);
void broadcast_nearby_entities(Client *c, NPC *npcs);
/* Tell all clients on old_floor that c left it (hp<=0 removal message).
* Call it whenever a player's floor_id changes (stairs, teleport, ...).*/
void notify_player_left_floor(Client *c, int old_floor);
/* Broadcast c's state to all clients on c->floor_id (arrival).*/
void broadcast_player_state(Client *c);
void handle_boss_death(Client *c, NPC *boss);
void sync_entity_grid(NPC *npcs);
void give_starting_gear(Client *c);


#endif // SERVER_INTERNAL_H
