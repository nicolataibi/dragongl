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
 * server_tiles.c - Tiles under the players: where a character may stand, the entity grid,
 * the city doors, and what happens on the tile a player steps on.
 */

#include "server_tiles.h"
#include <math.h>

#include "server_internal.h"

#include "player.h"
#include "server_log.h"

void sync_entity_grid(Game *game) {
    if (!game->world) {
        return;
    }
    for (int f = 0; f < MAX_FLOORS; f++) {
        memset(game->world->floors[f].entity_grid, 0, sizeof(game->world->floors[f].entity_grid));
    }
    if (game->clients) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!game->clients[i].active || !game->clients[i].authenticated) {
                continue;
            }
            /*Bounds: an out-of-range floor or coordinate (corrupted save, bad
       * dm_place) must never index the world out of bounds — this runs
       * on every round tick and would corrupt the heap.*/
            int f = game->clients[i].floor_id;
            if (f < 0 || f >= MAX_FLOORS) {
                continue;
            }
            if (game->clients[i].x < 0 || game->clients[i].x >= MAP_WIDTH ||
                game->clients[i].y < 0 || game->clients[i].y >= MAP_HEIGHT) {
                continue;
            }
            game->world->floors[f].entity_grid[game->clients[i].y][game->clients[i].x] =
                game->clients[i].entity_id;
        }
    }
    if (game->npcs) {
        for (int i = 0; i < MAX_NPCS; i++) {
            if (!game->npcs[i].active) {
                continue;
            }
            int f = game->npcs[i].floor_id;
            if (f < 0 || f >= MAX_FLOORS) {
                continue;
            }
            if (game->npcs[i].x < 0 || game->npcs[i].x >= MAP_WIDTH || game->npcs[i].y < 0 ||
                game->npcs[i].y >= MAP_HEIGHT) {
                continue;
            }
            game->world->floors[f].entity_grid[game->npcs[i].y][game->npcs[i].x] =
                game->npcs[i].entity_id;
        }
    }
}

bool tile_walkable(const Floor *fl, int x, int y) {
    if (x < 1 || y < 1 || x >= MAP_WIDTH - 1 || y >= MAP_HEIGHT - 1) {
        return false;
    }
    switch (fl->map.data[0][y][x]) {
    case VOXEL_FLOOR:
    case VOXEL_COBBLE:
    case VOXEL_MARBLE:
    case VOXEL_WOOD:
    case VOXEL_GRASS:
    case VOXEL_SAND:
    case VOXEL_STAIRS_UP:
    case VOXEL_STAIRS_DOWN:
    case VOXEL_DOOR:
        return true;
    default:
        return false; /*rock, wall, water, lava, traps... */
    }
}

bool tile_free(const Floor *fl, int x, int y) {
    return tile_walkable(fl, x, y) && fl->entity_grid[y][x] == 0;
}

/*Find a walkable tile near (ax,ay) — expanding rings up to radius 4.
 * ALWAYS returns in-bounds coordinates (worst case the clamped anchor),
 * so callers can never push a player out of the map.*/
void find_walkable_near(const Floor *fl, int ax, int ay, int *ox, int *oy) {
    if (ax < 1) {
        ax = 1;
    }
    if (ax > MAP_WIDTH - 2) {
        ax = MAP_WIDTH - 2;
    }
    if (ay < 1) {
        ay = 1;
    }
    if (ay > MAP_HEIGHT - 2) {
        ay = MAP_HEIGHT - 2;
    }
    if (tile_walkable(fl, ax, ay)) {
        *ox = ax;
        *oy = ay;
        return;
    }
    for (int r = 1; r <= 4; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) + abs(dy) != r) {
                    continue;
                }
                int x = ax + dx, y = ay + dy;
                if (tile_walkable(fl, x, y)) {
                    *ox = x;
                    *oy = y;
                    return;
                }
            }
        }
    }
    /*No walkable ring found (fully walled pocket): clamped anchor is still
     * in-bounds — safe even if it means standing in rock.*/
    *ox = ax;
    *oy = ay;
}

/*Opens/closes the city shop doors by day/night.
 *
 * The door positions MUST follow the formula of the shop ring built in
 * map.c (generate_city): 11 shops, radius 26, slot i at angle i*(360/11)
 * degrees, door on the dominant axis 4 tiles out from the building
 * centre, facing the city centre. server_move.c uses it too.*/
void update_city_doors(Game *game) {
    int h, m;
    get_game_time(game, &h, &m);
    VoxelType state = (h >= 6 && h < 20) ? VOXEL_DOOR : VOXEL_WALL;
    Map *city = &game->world->floors[0].map;
    int cx = MAP_CENTER_X;
    int cy = MAP_CENTER_Y;

    for (int i = 0; i < 11; i++) {
        float angle = city_shop_angle(i);
        int sx = cx + (int)(cosf(angle) * 26.0f);
        int sy = cy + (int)(sinf(angle) * 26.0f);
        /*Door faces the center: dominant axis, 4 tiles from the shop center
     * (same rule as map.c).*/
        int dx = cx - sx, dy = cy - sy;
        int tx = sx, ty = sy;
        if (abs(dx) > abs(dy)) {
            if (dx > 0) {
                tx = sx + 4;
            } else {
                tx = sx - 4;
            }
        } else {
            if (dy > 0) {
                ty = sy + 4;
            } else {
                ty = sy - 4;
            }
        }
        if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) {
            continue;
        }
        /*Only flip tiles that are actually a door or a closed wall: never
     * turn floor/rock into a door (that would seal corridors).*/
        if (city->data[0][ty][tx] == VOXEL_DOOR || city->data[0][ty][tx] == VOXEL_WALL) {
            city->data[0][ty][tx] = state;
        }
    }
}

/* Innate transit magic: remember the deepest floor this character has ever
* stood on. Called every time a client's floor_id changes.*/
void client_track_explored_floor(Client *c) {
    if (c && c->floor_id > c->max_floor_explored) {
        c->max_floor_explored = c->floor_id;
    }
}

void check_tile_events(Client *c, Game *game) {
    VoxelType vt = game->world->floors[c->floor_id].map.data[0][c->y][c->x];
    // --- EVENTI AMBIENTALI ---
    if (vt == VOXEL_ICE) {
        /*Slippery ground (ice, not water): Prone for 1 round.*/
        if (c->effect_count < MAX_EFFECTS_PER_ENTITY) {
            c->effects[c->effect_count] =
                (ActiveEffect){"Prone", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 1, false};
            c->effect_count++;
            send_text_to_client(c->sock,
                                "[DANGER] The ground is slippery! You fell to the ground!");
        }
    } else if (vt == VOXEL_STAIRS_DOWN) {
        /*Floor boundary: there is no world below the last floor (the
     * falling floor trap has the same guard).*/
        if (c->floor_id >= MAX_FLOORS - 1) {
            send_text_to_client(
                c->sock, "[SYSTEM] The dungeon ends here: there is no floor below this one.");
            return;
        }
        if (c->floor_id > 0 && c->floor_id % 10 == 0) {
            int boss_index = (c->floor_id / 10) - 1; /*>= 0: floor_id is a positive multiple of 10*/
            // cppcheck-suppress [shiftTooManyBits,signConversionCond]
            if (!(c->bosses_defeated & (1u << boss_index))) {
                bool boss_alive = false;
                if (game->npcs) {
                    FloorIter walk = floor_iter(game, c->floor_id);
                    for (int i; (i = floor_iter_next(&walk)) >= 0;) {
                        if (game->npcs[i].archetype == ARCH_BOSS) {
                            boss_alive = true;
                            break;
                        }
                    }
                }

                if (boss_alive) {
                    send_text_to_client(
                        c->sock,
                        "[SYSTEM] The Boss' energy seals the stairs. Defeat him before proceeding!");
                } else {
                    send_text_to_client(
                        c->sock,
                        "[SYSTEM] Only those who have proven their worth by defeating the Boss can go down. Wait for his return and face him!");
                }
                return;
            }
        }
        game->world->floors[c->floor_id].entity_grid[c->y][c->x] = 0;
        int old_floor = c->floor_id;
        c->floor_id++;
        notify_player_left_floor(game, c, old_floor);
        // Global search for the nearest up-staircase (or the first one found)
        bool found = false;
        int best_x = MAP_CENTER_X, best_y = MAP_CENTER_Y;
        float min_dist = 999999.0f;

        for (int iy = 0; iy < MAP_HEIGHT; iy++) {
            for (int ix = 0; ix < MAP_WIDTH; ix++) {
                if (game->world->floors[c->floor_id].map.data[0][iy][ix] == VOXEL_STAIRS_UP) {
                    float d = sqrtf(powf((float)(ix - c->x), 2) + powf((float)(iy - c->y), 2));
                    if (d < min_dist) {
                        min_dist = d;
                        best_x = ix;
                        best_y = iy;
                        found = true;
                    }
                }
            }
        }
        if (found) {
            c->x = best_x;
            c->y = best_y;
            game->world->floors[c->floor_id].entity_grid[best_y][best_x] = c->entity_id;
        }
        client_track_explored_floor(c);
        /*Arrival broadcast AFTER the landing position is set, so clients on
    * the new floor see the player where he actually is*/
        broadcast_player_state(game, c);
        send_detailed_state(game, c);
        send_map_chunk(c->sock, &game->world->floors[c->floor_id].map, c->x, c->y,
                       INITIAL_VIEW_RADIUS);
        broadcast_nearby_entities(c, game);
        save_player_data(c);
    } else if (vt == VOXEL_STAIRS_UP && c->floor_id > 0) {
        game->world->floors[c->floor_id].entity_grid[c->y][c->x] = 0;
        int old_floor = c->floor_id;
        c->floor_id--;
        notify_player_left_floor(game, c, old_floor);
        bool found = false;
        int best_x = MAP_CENTER_X, best_y = MAP_CENTER_Y;
        float min_dist = 999999.0f;

        for (int iy = 0; iy < MAP_HEIGHT; iy++) {
            for (int ix = 0; ix < MAP_WIDTH; ix++) {
                if (game->world->floors[c->floor_id].map.data[0][iy][ix] == VOXEL_STAIRS_DOWN) {
                    float d = sqrtf(powf((float)(ix - c->x), 2) + powf((float)(iy - c->y), 2));
                    if (d < min_dist) {
                        min_dist = d;
                        best_x = ix;
                        best_y = iy;
                        found = true;
                    }
                }
            }
        }
        if (found) {
            c->x = best_x;
            c->y = best_y;
            game->world->floors[c->floor_id].entity_grid[best_y][best_x] = c->entity_id;
        }
        /*Arrival broadcast AFTER the landing position is set, so clients on
    * the new floor see the player where he actually is*/
        broadcast_player_state(game, c);
        send_detailed_state(game, c);
        send_map_chunk(c->sock, &game->world->floors[c->floor_id].map, c->x, c->y,
                       INITIAL_VIEW_RADIUS);
        broadcast_nearby_entities(c, game);
        save_player_data(c);
    }
}
