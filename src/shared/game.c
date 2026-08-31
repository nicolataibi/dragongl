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

#include "../../include/game.h"
#include <stdio.h>

static Floor *current_floor = NULL;

void game_init(void) {
    current_floor = NULL;
}

void game_shutdown(void) {
    // Current floor is now managed by master_world in server/main_server.c
}

void game_change_floor(int new_id) {
    (void)new_id;
    // Logic delegated to main_server.c master_world
}

Map* game_get_current_map(void) {
    return current_floor ? &current_floor->map : NULL;
}
