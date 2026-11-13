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
 * server_startup.h - Start of the server.
 */

#ifndef SERVER_STARTUP_H
#define SERVER_STARTUP_H

#include <stdbool.h>

#include "server_entities.h"

/*Seeds rand() from the system entropy.*/
void seed_game_rng(void);
/*Sets game->data_dir: --data-dir, $DRAGONGL_DATA_DIR, <exe>/../data,
 * DRAGONGL_PKGDATADIR/data.*/
void resolve_data_dir(Game *game);
/*Loads the JSON datasets. 0, or the exit status: 2 without game data, 1
 * for unreadable data.*/
int load_game_data(Game *game);
/*world.dat and npcs.dat are a pair: both load, or a new world is generated
 * and saved. False if the new world cannot be saved.*/
bool load_or_generate_world(Game *game);

#endif /* SERVER_STARTUP_H */
