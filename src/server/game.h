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
 * game.h - The state of a running game.
 *
 * One Game is created by main() (and one by each test fixture) and passed
 * to every module that reads or changes the world: the server keeps no
 * global game state.
 */

#ifndef DRAGONGL_GAME_H
#define DRAGONGL_GAME_H

#include "aoe.h"
#include "map.h"
#include "server.h"
#include "server_entities.h"

/*Length of the path of the data directory, terminator included.*/
#define DATA_DIR_MAX 256

/*The world event in progress on one floor (type 0: none).*/
typedef struct {
    int type;
    int floor_id;
    int time_left;
    int progress;
    int goal;
} WorldEvent;

struct Game {
    World *world;    /*Every floor of the dungeon and the town*/
    Client *clients; /*MAX_CLIENTS slots*/
    NPC *npcs;       /*MAX_NPCS slots*/
    int total_turns; /*Rounds played since the world was created*/
    int next_id;     /*Next entity id to hand out*/
    WorldEvent event;
    Tombstone tombstones[MAX_TOMBSTONES];
    PersistentCloud clouds[MAX_CLOUDS];
    /*Read-only directory of the JSON datasets. Resolved at start:
     * --data-dir, $DRAGONGL_DATA_DIR, <exe>/../data (build tree),
     * DRAGONGL_PKGDATADIR/data. Everything the server writes lives in the
     * state directory (persist.h).*/
    char data_dir[DATA_DIR_MAX];
};

/*A zeroed Game with its client and NPC arrays allocated; false when out
 * of memory.*/
bool game_create(Game *game);
/*Releases what game_create() and the world loader allocated.*/
void game_destroy(Game *game);

#endif
