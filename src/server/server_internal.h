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

/*Game clock: one round = 200 ms of real time (5 rounds/s). A full game
 * day is 1440 rounds = 4.8 minutes of real time. The server's main loop
 * drives update_world() through a fixed-step accumulator (TICK_MS, with
 * MAX_TICK_CATCHUP_MS of catch-up) so the simulation speed is the same
 * at 1 client or 64. NOTE: every in-game timer expressed in rounds
 * (hunger, light, trap respawns, ...) inherits this scale — when tuning
 * a constant, convert it: 1 round = 0.2 s real.*/
#define TICK_MS 200LL
#define MAX_TICK_CATCHUP_MS (5LL * TICK_MS)

typedef struct {
    char name[64];
    int x, y;
    uint64_t gold;
    int items[30];
    int amounts[30];
    int w_idx, b_idx, h_idx, s_idx;
} BonesData;

// Global constants
/*Population/respawn tuning — SINGLE source of truth (L3). These used
 * to be #defined in BOTH server_internal.h and main_server.c: two
 * sources of truth that were equal "by luck" and would have silently
 * drifted (identical redefinitions are legal C, so no warning). Do not
 * redefine them anywhere else.*/
#define RESPAWN_TICKS 120       //~2 min at 6s/tick
#define DENSITY_CHECK 50        //every N global rounds
#define DENSITY_MIN_PCT 40      //emergency spawn if < 40% active
#define RESPAWN_TRAPS_TICKS 300 // ~30 min
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
/*Directory containing the JSON data files and the world state files
 * (world.dat, npcs.dat, artifacts.dat). Resolved at start: --data-dir
 * argument > local ./data > /usr/share/dragongl/data (RPM install).*/
#define DATA_DIR_MAX 256
extern char g_data_dir[DATA_DIR_MAX];

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

/*Atomic file write helpers (temp file in the same directory + fsync +
 * rename): a crash mid-write never truncates the target file; on any
 * error the temp file is removed and the last good file is kept.*/
FILE *atomic_write_begin(const char *path, char *tmp, size_t tmp_size);
bool atomic_write_end(FILE *f, const char *path, const char *tmp, bool ok);
/*Delete leftover *.tmp files in dir (stale from a crashed run).*/
void cleanup_stale_tmp_files(const char *dir);

void save_bones(Client* c);
void drop_loot_from_monster(Client *c, NPC *killer);
void check_level_up(Client *c);
int get_xp_threshold(int level);
void damage_item(ItemInstance *it, int amt);
void save_player_data(Client *c);
int load_player_data(Client *c);
void send_text_to_client(int sock, const char *fmt, ...);
/*net_send() wrapper that DROPS the client on the first failed send
 * (save + "left floor" notice + close): a lost message would leave the
 * client in a stale state until a full state that may never come (L2).
 * Returns true if the payload was queued, false if the client was
 * marked for removal.*/
bool net_send_client(int sock, const void *data, int len);
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
/*NULL-safe display name for NPC messages/logs: custom_name (ghosts,
 * summons, dominated) > template name > archetype fallback. NEVER
 * dereference n->template->name directly: gold piles, chests and
 * summoned elementals have template==NULL and crashed the server.*/
const char *npc_name(const NPC *n);
/*Respawn a dead player in town (floor 0, next to center): clears the
 * old floor's grid cell, registers the new one, and re-syncs the
 * client (state + map chunk + nearby entities). Use at EVERY death
 * site instead of setting hp/floor_id/x/y by hand (which leaked the
 * entity_grid and left the client showing the dungeon).*/
void player_respawn_town(Client *c);



void get_game_time(int *h, int *m);
float get_movement_cooldown(Client *c);
int get_equipped_mask(Client *c);
const int *floor_index_for(int floor_id, int *out_count);

#endif // SERVER_INTERNAL_H
