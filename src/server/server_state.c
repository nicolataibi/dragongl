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
 * server_state.c - Creation and release of the game state.
 */

#include "game.h"
#include "server.h"

#include <stdlib.h>
#include <string.h>

bool game_create(Game *game) {
    memset(game, 0, sizeof(*game));
    game->next_id = 1;
    game->event.floor_id = 1;
    game->clients = calloc(MAX_CLIENTS, sizeof(Client));
    game->npcs = calloc(MAX_NPCS, sizeof(NPC));
    game->world = calloc(1, sizeof(World));
    if (!game->clients || !game->npcs || !game->world) {
        game_destroy(game);
        return false;
    }
    return true;
}

void game_destroy(Game *game) {
    free(game->clients);
    free(game->npcs);
    free(game->world);
    game->clients = NULL;
    game->npcs = NULL;
    game->world = NULL;
}
