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

#include "net.h"
#include "server_entities.h"
#include "server_net.h"
#include "server_world.h"
#include "server.h"
#include "items.h"
#include "rules.h"
#include "combat_log.h"
#include "game.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/*Game clock: one round = 200 ms of real time (5 rounds/s). A full game
 * day is 1440 rounds = 4.8 minutes of real time. The server's main loop
 * drives update_world() through a fixed-step accumulator (TICK_MS, with
 * MAX_TICK_CATCHUP_MS of catch-up) so the simulation speed is the same
 * at 1 client or 64. NOTE: every in-game timer expressed in rounds
 * (hunger, light, trap respawns, ...) inherits this scale — when tuning
 * a constant, convert it: 1 round = 0.2 s real.*/
#define TICK_MS             200LL
#define MAX_TICK_CATCHUP_MS (5LL * TICK_MS)

// Global constants
/*Population/respawn tuning, defined only here: do not redefine them
 * anywhere else.*/
#define RESPAWN_TICKS       120 //~2 min at 6s/tick
#define DENSITY_CHECK       50  //every N global rounds
#define DENSITY_MIN_PCT     40  //emergency spawn if < 40% active
#define RESPAWN_TRAPS_TICKS 300 // ~30 min
/*% chance that a kill will generate a loot item*/
#define LOOT_DROP_CHANCE 60

// Tombstone system
void tombstone_create(Game *game, Client *c);
void tombstone_list(Game *game, Client *c);
bool tombstone_pickup(Game *game, Client *c);
void tombstone_save(const Tombstone *t);
void tombstone_load_all(Game *game);

/*Queue a MSG_AUTH_FAIL for the client on `sock`.*/
void send_auth_fail(int sock, const char *reason);
/*Structured events for the floating combat text (CombatEventKind).*/
void send_combat_event(int sock, int kind, int entity_id, int amount);
bool send_welcome(int sock, const MsgWelcome *m);
void send_time_sync(int sock, const MsgTimeSync *m);
void send_tombstone_remove(int sock, int entity_id);
void print_merchant_inventory(Client *c, NPC *merchant);
void handle_boss_death(Game *game, Client *c, NPC *boss);
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
void player_respawn_town(Game *game, Client *c);

#endif // SERVER_INTERNAL_H
