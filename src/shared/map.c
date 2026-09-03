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

#include "../../include/map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

typedef struct {
    int x, y;
} Point;

typedef struct {
    int x, y, w, h;
    Point center;
    int doors;
} Room;

static Room g_rooms[1000];
static int g_rooms_count = 0;

static void map_fill_rect(Map* map, int x, int y, int w, int h, VoxelType type) {
    for (int iy = y; iy < y + h; iy++) {
        for (int ix = x; ix < x + w; ix++) {
            if (ix >= 0 && ix < MAP_WIDTH && iy >= 0 && iy < MAP_HEIGHT) {
                map->data[0][iy][ix] = type;
            }
        }
    }
}

static void spawn_traps_on_floor(Floor *f) {
    if (f->id == 0) return; // No traps in the city

    f->trap_count = 0;
    int floor_bonus = f->id / 10;
    int target_traps = 10 + (rand() % 15) + (f->id / 5);
    if (target_traps > MAX_TRAPS_PER_FLOOR - 20) target_traps = MAX_TRAPS_PER_FLOOR - 20;

    /*Floor trap pool (excludes wall-based and tile-special)*/
    static const TrapType floor_pool[] = {
        TRAP_PIT, TRAP_SPIKES, TRAP_POISON_NEEDLE, TRAP_GAS, TRAP_ALARM,
        TRAP_TELEPORT, TRAP_ACID, TRAP_WEB, TRAP_BLADE, TRAP_SILENCE,
        TRAP_LIGHTNING, TRAP_QUICKSAND, TRAP_DARKNESS,
        TRAP_FALLING_FLOOR, TRAP_BOULDER, TRAP_PARALYZING_SPORES,
        TRAP_CURSE_RUNE, TRAP_ANTIMAGIC_ZONE, TRAP_SLEEP_GAS,
        TRAP_ILLUSION_MIRROR, TRAP_SWAP_TELEPORT, TRAP_FAKE_DOOR,
        TRAP_FLOOD, TRAP_CEILING_COLLAPSE, TRAP_CRUSHER_WALL,
        TRAP_INVERSION_RUNE, TRAP_STONE_TOMB, TRAP_HUNGER_CURSE, TRAP_BODY_SWAP
    };
    int pool_size = (int)(sizeof(floor_pool) / sizeof(floor_pool[0]));

    for (int i = 0; i < target_traps; i++) {
        int attempts = 0;
        while (attempts < 100) {
            int tx = rand() % MAP_WIDTH;
            int ty = rand() % MAP_HEIGHT;
            VoxelType tv = f->map.data[0][ty][tx];
            bool walkable = (tv == VOXEL_FLOOR || tv == VOXEL_WOOD || tv == VOXEL_COBBLE ||
                             tv == VOXEL_SAND  || tv == VOXEL_MUD  || tv == VOXEL_MARBLE ||
                             tv == VOXEL_ASH);
            if (!walkable) { attempts++; continue; }

            bool already = false;
            for (int j = 0; j < f->trap_count; j++) {
                if (f->traps[j].x == tx && f->traps[j].y == ty) { already = true; break; }
            }
            if (already) { attempts++; continue; }

            Trap *t = &f->traps[f->trap_count++];
            t->x             = tx;
            t->y             = ty;
            t->floor_id      = f->id;
            t->active        = true;
            t->detected      = false;
            t->respawn_timer = 0;
            t->wall_dir      = 0;
            t->type          = floor_pool[rand() % pool_size];

            t->detection_dc  = 10 + (rand() % 5) + floor_bonus;
            t->save_dc       = 10 + (rand() % 5) + floor_bonus;
            t->damage_dice   = 1 + (f->id / 20);
            t->damage_sides  = 6;
            t->damage_type   = DMG_BLUDGEONING;

            /*Type-specific parameters*/
            if (t->type == TRAP_PIT)              t->damage_type = DMG_BLUDGEONING;
            else if (t->type == TRAP_SPIKES)      { t->damage_type = DMG_PIERCING; }
            else if (t->type == TRAP_POISON_NEEDLE|| t->type == TRAP_GAS ||
                     t->type == TRAP_SLEEP_GAS   || t->type == TRAP_PARALYZING_SPORES) {
                t->damage_type  = DMG_POISON;
                t->damage_sides = 4;
            }
            else if (t->type == TRAP_ACID)        t->damage_type = DMG_ACID;
            else if (t->type == TRAP_LIGHTNING)   t->damage_type = DMG_LIGHTNING;
            else if (t->type == TRAP_BOULDER || t->type == TRAP_CRUSHER_WALL) {
                t->damage_type  = DMG_BLUDGEONING;
                t->damage_dice  = 2 + (f->id / 15);
                t->damage_sides = 8;
            }
            else if (t->type == TRAP_FALLING_FLOOR) {
                t->damage_type  = DMG_BLUDGEONING;
                t->damage_sides = 6;
            }
            else if (t->type == TRAP_SPRING_SPEAR) {
                t->damage_type  = DMG_PIERCING;
                t->damage_sides = 8;
            }
            else if (t->type == TRAP_GAS_VEIN) {
                t->damage_type  = DMG_FIRE;
                t->damage_dice  = 2 + (f->id / 20);
                t->damage_sides = 6;
            }
            else if (t->type == TRAP_CRYSTAL_BURST) {
                t->damage_type  = DMG_PIERCING;
                t->damage_sides = 4;
            }
            else if (t->type == TRAP_HUNGER_CURSE || t->type == TRAP_CURSE_RUNE) {
                t->damage_type  = DMG_NECROTIC;
                t->damage_dice  = 1;
                t->damage_sides = 4;
            }
            else if (t->type == TRAP_FLOOD || t->type == TRAP_CEILING_COLLAPSE) {
                t->damage_type  = DMG_BLUDGEONING;
            }
            break;
        }
    }

    // --- Spawn TRAP_DART_WALL on wall tiles adjacent to corridors ---
    int dart_count = 3 + (rand() % 4) + (f->id / 15);
    if (dart_count > 12) dart_count = 12;
    int dart_placed = 0;
    int dart_attempts = 0;
    // Offsets: N, E, S, W
    int wall_dx[4] = {  0,  1,  0, -1 };
    int wall_dy[4] = { -1,  0,  1,  0 };

    while (dart_placed < dart_count && dart_attempts < 5000) {
        dart_attempts++;
        int tx = 5 + rand() % (MAP_WIDTH  - 10);
        int ty = 5 + rand() % (MAP_HEIGHT - 10);
        VoxelType tv = f->map.data[0][ty][tx];
        bool is_wall = (tv == VOXEL_WALL || tv == VOXEL_COBBLE ||
                        tv == VOXEL_ROCK || tv == VOXEL_OBSIDIAN ||
                        tv == VOXEL_ICE  || tv == VOXEL_MARBLE);
        if (!is_wall) continue;

        // Pick a random direction and check if the adjacent tile is walkable floor
        int dir = rand() % 4;
        int nx = tx + wall_dx[dir];
        int ny = ty + wall_dy[dir];
        if (nx < 1 || nx >= MAP_WIDTH-1 || ny < 1 || ny >= MAP_HEIGHT-1) continue;
        VoxelType nv = f->map.data[0][ny][nx];
        bool is_walkable = (nv == VOXEL_FLOOR || nv == VOXEL_WOOD || nv == VOXEL_COBBLE ||
                            nv == VOXEL_SAND  || nv == VOXEL_MUD  || nv == VOXEL_MARBLE ||
                            nv == VOXEL_ASH);
        if (!is_walkable) continue;

        // Avoid duplicate positions
        bool already = false;
        for (int j = 0; j < f->trap_count; j++) {
            if (f->traps[j].x == tx && f->traps[j].y == ty) {
                already = true;
                break;
            }
        }
        if (already) continue;
        if (f->trap_count >= MAX_TRAPS_PER_FLOOR) break;

        Trap *t = &f->traps[f->trap_count++];
        t->type          = TRAP_DART_WALL;
        t->x             = tx;  //The WALL tile containing the hidden hole
        t->y             = ty;
        t->floor_id      = f->id;
        t->active        = true;
        t->detected      = false;
        t->respawn_timer = 0;
        t->wall_dir      = dir; // Direction the wall faces (dart fires into opposite)
        t->damage_type   = DMG_PIERCING;
        int floor_bonus  = f->id / 10;
        t->detection_dc  = 12 + (rand() % 4) + floor_bonus;
        t->save_dc       = 11 + (rand() % 5) + floor_bonus;
        t->damage_dice   = 1 + (f->id / 25);
        t->damage_sides  = 4; // 1d4 piercing + poison
        dart_placed++;
    }

    /* --- SPRING_SPEAR: horizontal spear hidden in walls --- */
    int spear_count = 2 + (rand() % 3) + (f->id / 20);
    if (spear_count > 8) spear_count = 8;
    int spear_placed = 0;
    int spear_attempts = 0;
    while (spear_placed < spear_count && spear_attempts < 3000) {
        spear_attempts++;
        int tx = 5 + rand() % (MAP_WIDTH  - 10);
        int ty = 5 + rand() % (MAP_HEIGHT - 10);
        VoxelType tv = f->map.data[0][ty][tx];
        bool is_wall = (tv == VOXEL_WALL || tv == VOXEL_COBBLE || tv == VOXEL_ROCK ||
                        tv == VOXEL_OBSIDIAN || tv == VOXEL_ICE || tv == VOXEL_MARBLE);
        if (!is_wall) continue;
        int dir = rand() % 4;
        int wall_dx2[4] = {0, 1, 0, -1};
        int wall_dy2[4] = {-1, 0, 1, 0};
        int nx = tx + wall_dx2[dir];
        int ny = ty + wall_dy2[dir];
        if (nx < 1 || nx >= MAP_WIDTH-1 || ny < 1 || ny >= MAP_HEIGHT-1) continue;
        VoxelType nv = f->map.data[0][ny][nx];
        bool is_walkable = (nv == VOXEL_FLOOR || nv == VOXEL_WOOD || nv == VOXEL_SAND ||
                            nv == VOXEL_MUD   || nv == VOXEL_MARBLE || nv == VOXEL_ASH);
        if (!is_walkable) continue;
        bool already = false;
        for (int j = 0; j < f->trap_count; j++) {
            if (f->traps[j].x == tx && f->traps[j].y == ty) { already = true; break; }
        }
        if (already) continue;
        if (f->trap_count >= MAX_TRAPS_PER_FLOOR) break;
        Trap *t      = &f->traps[f->trap_count++];
        t->type          = TRAP_SPRING_SPEAR;
        t->x             = tx;
        t->y             = ty;
        t->floor_id      = f->id;
        t->active        = true;
        t->detected      = false;
        t->respawn_timer = 0;
        t->wall_dir      = dir;
        t->damage_type   = DMG_PIERCING;
        int fb2          = f->id / 10;
        t->detection_dc  = 13 + (rand() % 4) + fb2;
        t->save_dc       = 12 + (rand() % 5) + fb2;
        t->damage_dice   = 1 + (f->id / 20);
        t->damage_sides  = 8;
        spear_placed++;
    }

    /*--- CRYSTAL_BURST: traps on crystals ---*/
    for (int y = 5; y < MAP_HEIGHT-5 && f->trap_count < MAX_TRAPS_PER_FLOOR; y++) {
        for (int x = 5; x < MAP_WIDTH-5 && f->trap_count < MAX_TRAPS_PER_FLOOR; x++) {
            VoxelType tv = f->map.data[0][y][x];
            if ((tv == VOXEL_CRYSTAL_BLUE || tv == VOXEL_CRYSTAL_PURPLE) && rand() % 100 < 40) {
                bool already = false;
                for (int j = 0; j < f->trap_count; j++) {
                    if (f->traps[j].x == x && f->traps[j].y == y) { already = true; break; }
                }
                if (already) continue;
                Trap *t      = &f->traps[f->trap_count++];
                t->type          = TRAP_CRYSTAL_BURST;
                t->x             = x;
                t->y             = y;
                t->floor_id      = f->id;
                t->active        = true;
                t->detected      = false;
                t->respawn_timer = 0;
                t->wall_dir      = 0;
                t->damage_type   = DMG_PIERCING;
                t->detection_dc  = 14 + (f->id / 10);
                t->save_dc       = 12 + (f->id / 10);
                t->damage_dice   = 2 + (f->id / 20);
                t->damage_sides  = 4;
            }
        }
    }

    /*--- GAS_VEIN: ash/mud traps ---*/
    int gas_placed = 0;
    for (int y = 5; y < MAP_HEIGHT-5 && f->trap_count < MAX_TRAPS_PER_FLOOR; y++) {
        for (int x = 5; x < MAP_WIDTH-5 && f->trap_count < MAX_TRAPS_PER_FLOOR; x++) {
            VoxelType tv = f->map.data[0][y][x];
            if ((tv == VOXEL_ASH || tv == VOXEL_MUD) && rand() % 100 < 20 && gas_placed < 8) {
                bool already = false;
                for (int j = 0; j < f->trap_count; j++) {
                    if (f->traps[j].x == x && f->traps[j].y == y) { already = true; break; }
                }
                if (already) continue;
                Trap *t      = &f->traps[f->trap_count++];
                t->type          = TRAP_GAS_VEIN;
                t->x             = x;
                t->y             = y;
                t->floor_id      = f->id;
                t->active        = true;
                t->detected      = false;
                t->respawn_timer = 0;
                t->wall_dir      = 0;
                t->damage_type   = DMG_FIRE;
                t->detection_dc  = 15 + (f->id / 10);
                t->save_dc       = 12 + (f->id / 10);
                t->damage_dice   = 2 + (f->id / 20);
                t->damage_sides  = 6;
                gas_placed++;
            }
        }
    }
}

void world_init(World* world) {
    srand(time(NULL));
    for (int i = 0; i < MAX_FLOORS; i++) {
        world->floors[i].id = i;
        VoxelType base = (i == 0) ? VOXEL_GRASS : VOXEL_ROCK;
        map_fill_rect(&world->floors[i].map, 0, 0, MAP_WIDTH, MAP_HEIGHT, base);
        memset(world->floors[i].entity_grid, 0, sizeof(world->floors[i].entity_grid));
        generate_procedural_dungeon(&world->floors[i].map, i);
        spawn_traps_on_floor(&world->floors[i]);
    }
}

void world_save(World* world, const char* filename) {
    FILE *f = fopen(filename, "wb");
    if (f) {
        fwrite(world, sizeof(World), 1, f);
        fclose(f);
    }
}

bool world_load(World* world, const char* filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return false;
    if (fread(world, sizeof(World), 1, f) != 1) {}
    fclose(f);
    // Re-initialize entity grid after load (volatile state)
    for (int i = 0; i < MAX_FLOORS; i++) {
        memset(world->floors[i].entity_grid, 0, sizeof(world->floors[i].entity_grid));
    }
    return true;
}

void map_dig_room(Map* map, int x, int y, int w, int h) {
    for (int iy = y; iy < y + h; iy++) {
        for (int ix = x; ix < x + w; ix++) {
            if (ix >= 0 && ix < MAP_WIDTH && iy >= 0 && iy < MAP_HEIGHT) {
                bool is_corner = (ix == x || ix == x + w - 1) && (iy == y || iy == y + h - 1);
                if (is_corner) {
                    // Keep corners as ROCK: the miner cannot dig through them,
                    //preventing holes in walls and doors at corner positions.
                    map->data[0][iy][ix] = VOXEL_ROCK;
                } else if (ix == x || ix == x + w - 1 || iy == y || iy == y + h - 1) {
                    if (map->data[0][iy][ix] != VOXEL_FLOOR) {
                        map->data[0][iy][ix] = VOXEL_WALL;
                    }
                } else {
                    map->data[0][iy][ix] = VOXEL_FLOOR;
                }
            }
        }
    }
}

typedef struct {
    int x, y;
    int dir_idx;
    int dirs[4];
} DfsNode;

static void draw_line(Map* map, int x0, int y0, int x1, int y1, VoxelType type, int* out_x, int* out_y, int* out_count) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;

    while (1) {
        if (x0 >= 1 && x0 < MAP_WIDTH - 1 && y0 >= 1 && y0 < MAP_HEIGHT - 1) {
            map->data[0][y0][x0] = type;
            if (out_x && out_y && out_count) {
                out_x[*out_count] = x0;
                out_y[*out_count] = y0;
                (*out_count)++;
            }
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void __attribute__((unused)) decorate_room(Map* map, Room* r, int type) {
    int ix1 = r->x + 1;
    int iy1 = r->y + 1;
    int ix2 = r->x + r->w - 2;
    int iy2 = r->y + r->h - 2;
    
    if (ix2 <= ix1 || iy2 <= iy1) return;
    
    if (type == 1 && r->w >= 9 && r->h >= 9) { // Vault
        for (int y = iy1 + 2; y <= iy2 - 2; y++) {
            for (int x = ix1 + 2; x <= ix2 - 2; x++) {
                if (x == ix1 + 2 || x == ix2 - 2 || y == iy1 + 2 || y == iy2 - 2) {
                    map->data[0][y][x] = VOXEL_WALL;
                }
            }
        }
        map->data[0][r->center.y][ix1 + 2] = VOXEL_FLOOR; // Inner door
    } else if (type == 2 && r->w >= 7 && r->h >= 7) { // Pillars
        for (int y = iy1 + 1; y <= iy2 - 1; y += 2) {
            for (int x = ix1 + 1; x <= ix2 - 1; x += 2) {
                map->data[0][y][x] = VOXEL_WALL;
            }
        }
    } else if (type == 3 && r->w >= 9 && r->h >= 9) { // Cross shape
        int cw = r->w / 3;
        int ch = r->h / 3;
        for (int y = iy1; y <= iy2; y++) {
            for (int x = ix1; x <= ix2; x++) {
                if ((x < ix1 + cw || x > ix2 - cw) && (y < iy1 + ch || y > iy2 - ch)) {
                    map->data[0][y][x] = VOXEL_ROCK;
                }
            }
        }
        for (int y = iy1; y <= iy2; y++) {
            for (int x = ix1; x <= ix2; x++) {
                if (map->data[0][y][x] == VOXEL_ROCK) {
                    for (int dy=-1; dy<=1; dy++) {
                        for (int dx=-1; dx<=1; dx++) {
                            if (y+dy>=iy1 && y+dy<=iy2 && x+dx>=ix1 && x+dx<=ix2) {
                                if (map->data[0][y+dy][x+dx] == VOXEL_FLOOR) {
                                    map->data[0][y][x] = VOXEL_WALL;
                                }
                            }
                        }
                    }
                }
            }
        }
    } else if (type == 4 && r->w >= 7 && r->h >= 7) { // Cellular Automata Cave
        for (int y = iy1; y <= iy2; y++) {
            for (int x = ix1; x <= ix2; x++) {
                if (rand() % 100 < 40) map->data[0][y][x] = VOXEL_WALL;
            }
        }
        for (int p = 0; p < 3; p++) {
            VoxelType temp[30][30];
            for (int y = iy1; y <= iy2; y++) {
                for (int x = ix1; x <= ix2; x++) {
                    int neighbors = 0;
                    for (int dy=-1; dy<=1; dy++) {
                        for (int dx=-1; dx<=1; dx++) {
                            if (dy==0 && dx==0) continue;
                            int ny = y+dy; int nx = x+dx;
                            if (ny < iy1 || ny > iy2 || nx < ix1 || nx > ix2) neighbors++;
                            else if (map->data[0][ny][nx] == VOXEL_WALL || map->data[0][ny][nx] == VOXEL_ROCK) neighbors++;
                        }
                    }
                    if (map->data[0][y][x] == VOXEL_WALL || map->data[0][y][x] == VOXEL_ROCK) {
                        temp[y-iy1][x-ix1] = (neighbors >= 4) ? VOXEL_WALL : VOXEL_FLOOR;
                    } else {
                        temp[y-iy1][x-ix1] = (neighbors >= 5) ? VOXEL_WALL : VOXEL_FLOOR;
                    }
                }
            }
            for (int y = iy1; y <= iy2; y++) {
                for (int x = ix1; x <= ix2; x++) {
                    map->data[0][y][x] = temp[y-iy1][x-ix1];
                }
            }
        }
        // Free path to doors
        for (int x = ix1; x <= ix2; x++) { map->data[0][r->center.y][x] = VOXEL_FLOOR; map->data[0][r->center.y+1][x] = VOXEL_FLOOR; }
        for (int y = iy1; y <= iy2; y++) { map->data[0][y][r->center.x] = VOXEL_FLOOR; map->data[0][y][r->center.x+1] = VOXEL_FLOOR; }
    }
}

static bool point_in_polygon(int x, int y, int* px, int* py, int sides) {
    bool c = false;
    for (int i = 0, j = sides - 1; i < sides; j = i++) {
        if (((py[i] > y) != (py[j] > y)) &&
            (x < (px[j] - px[i]) * (float)(y - py[i]) / (float)(py[j] - py[i]) + px[i]))
            c = !c;
    }
    return c;
}

static void generate_blob(Map* map, VoxelType type, int count, int min_size, int max_size) {
    for (int i = 0; i < count; i++) {
        int x = rand() % MAP_WIDTH;
        int y = rand() % MAP_HEIGHT;
        int size = min_size + rand() % (max_size - min_size + 1);
        for (int j = 0; j < size; j++) {
            if (x >= 1 && x < MAP_WIDTH-1 && y >= 1 && y < MAP_HEIGHT-1) {
                map->data[0][y][x] = type;
            }
            int dir = rand() % 4;
            if (dir == 0) y--;
            else if (dir == 1) y++;
            else if (dir == 2) x--;
            else x++;
        }
    }
}

static void place_polygon_rooms(Map* map) {
    g_rooms_count = 0;
    int max_attempts = 100000;
    
    for (int i = 0; i < max_attempts; i++) {
        double r = 10.0 + (rand() % 11); // radius between 10 and 20
        double cx = 25.0 + (rand() % (MAP_WIDTH - 50));
        double cy = 25.0 + (rand() % (MAP_HEIGHT - 50));
        
        bool overlap = false;
        for (int j = 0; j < g_rooms_count; j++) {
            double cx2 = g_rooms[j].center.x;
            double cy2 = g_rooms[j].center.y;
            double r2 = g_rooms[j].w; // Using w to store radius
            double dist = sqrt((cx - cx2) * (cx - cx2) + (cy - cy2) * (cy - cy2));
            
            // Distance must be at least R1 + R2 + 2 tiles to avoid overlapping/touching walls
            if (dist < (r + r2 + 2.0)) {
                overlap = true; 
                break;
            }
        }
        
        if (!overlap) {
            int sides = 3 + (rand() % 23); // 3 to 25 sides
            double phase = ((double)rand() / RAND_MAX) * 2.0 * M_PI;
            
            int px[30], py[30];
            for (int s = 0; s < sides; s++) {
                double angle = phase + (s * 2.0 * M_PI / sides);
                px[s] = (int)(cx + r * cos(angle));
                py[s] = (int)(cy + r * sin(angle));
            }
            px[sides] = px[0];
            py[sides] = py[0];
            
            // Draw walls and collect wall points
            int wall_x[1000], wall_y[1000];
            int wall_count = 0;
            
            for (int s = 0; s < sides; s++) {
                draw_line(map, px[s], py[s], px[s+1], py[s+1], VOXEL_WALL, wall_x, wall_y, &wall_count);
            }
            
            // Choose Theme
            int theme = rand() % 21; 
            // 0: Empty/Normal, 1: Library, 2: Crypt/Bones, 3: Slime Pool, 4: Armory
            // 5: Treasure Vault, 6: Mirror Maze, 7: Overgrown Garden
            // 8: Summoning Circle, 9: Alchemist Lab, 10: Arcane Observatory
            // 11: Dwarven Forge, 12: Frozen Tomb, 13: Spider Nest, 14: Crystal Cave
            // 15: Deserted Arena, 16: Sunken Temple, 17: Fungal Forest
            // 18: Lava Caldera, 19: Throne Room, 20: Chaos Rift
            
            // Fill Polygon
            int min_x = cx - r - 1; int max_x = cx + r + 1;
            int min_y = cy - r - 1; int max_y = cy + r + 1;
            for (int y = min_y; y <= max_y; y++) {
                for (int x = min_x; x <= max_x; x++) {
                    if (y >= 1 && y < MAP_HEIGHT-1 && x >= 1 && x < MAP_WIDTH-1) {
                        if (map->data[0][y][x] != VOXEL_WALL) { // don't overwrite walls
                            if (point_in_polygon(x, y, px, py, sides)) {
                                if (theme == 1) { // Library
                                    map->data[0][y][x] = VOXEL_WOOD;
                                    if (rand() % 100 < 15) map->data[0][y][x] = VOXEL_WALL; // Bookshelves
                                } else if (theme == 2) { // Crypt
                                    map->data[0][y][x] = VOXEL_MUD;
                                    if (rand() % 100 < 5) map->data[0][y][x] = VOXEL_ROCK; // Graves
                                } else if (theme == 3) { // Slime Pool
                                    double d = sqrt((x - cx)*(x - cx) + (y - cy)*(y - cy));
                                    if (d < r * 0.6) map->data[0][y][x] = VOXEL_WATER;
                                    else map->data[0][y][x] = VOXEL_MUD;
                                } else if (theme == 4) { // Armory
                                    map->data[0][y][x] = VOXEL_COBBLE;
                                    if (rand() % 100 < 8) map->data[0][y][x] = VOXEL_OBSIDIAN; // Weapon racks
                                } else if (theme == 5) { // Treasure Vault
                                    map->data[0][y][x] = VOXEL_MARBLE;
                                    int chance = rand() % 100;
                                    if (chance < 5) map->data[0][y][x] = VOXEL_GOLD_VEIN; // Gold pillars
                                    else if (chance < 7) map->data[0][y][x] = VOXEL_CRYSTAL_PURPLE; // Gems
                                } else if (theme == 6) { // Mirror Maze
                                    map->data[0][y][x] = VOXEL_ICE;
                                    if (rand() % 100 < 25) map->data[0][y][x] = VOXEL_CRYSTAL_BLUE; // Crystal mirrors
                                } else if (theme == 7) { // Overgrown Garden
                                    map->data[0][y][x] = VOXEL_GRASS;
                                    int chance = rand() % 100;
                                    if (chance < 10) map->data[0][y][x] = VOXEL_MUSHROOM_GLOW; // Bio-luminescence
                                    else if (chance < 15) map->data[0][y][x] = VOXEL_WOOD; // Roots/Trunks
                                    else if (chance < 20) map->data[0][y][x] = VOXEL_WATER; // Puddles
                                } else if (theme == 8) { // Summoning Circle
                                    double d = sqrt((x - cx)*(x - cx) + (y - cy)*(y - cy));
                                    if (d < r * 0.3) map->data[0][y][x] = VOXEL_OBSIDIAN; // Altar
                                    else if (d >= r * 0.3 && d < r * 0.4) map->data[0][y][x] = VOXEL_CRYSTAL_PURPLE; // Ring of power
                                    else map->data[0][y][x] = VOXEL_ASH; // Burnt ground
                                } else if (theme == 9) { // Alchemist Lab
                                    map->data[0][y][x] = VOXEL_MARBLE;
                                    int chance = rand() % 100;
                                    if (chance < 4) map->data[0][y][x] = VOXEL_LAVA; // Spilled acid/fire
                                    else if (chance < 8) map->data[0][y][x] = VOXEL_MUSHROOM_GLOW; // Mutated spores
                                    else if (chance < 12) map->data[0][y][x] = VOXEL_WATER; // Potion spills
                                    else if (chance < 15) map->data[0][y][x] = VOXEL_WALL; // Workbenches
                                } else if (theme == 10) { // Arcane Observatory
                                    // Checkerboard pattern
                                    if ((x + y) % 2 == 0) map->data[0][y][x] = VOXEL_MARBLE;
                                    else map->data[0][y][x] = VOXEL_OBSIDIAN;
                                    if (rand() % 100 < 3) map->data[0][y][x] = VOXEL_CRYSTAL_BLUE; // Lenses/Telescopes
                                } else if (theme == 11) { // Dwarven Forge
                                    map->data[0][y][x] = VOXEL_COBBLE;
                                    if (x % 7 == 0 || y % 7 == 0) map->data[0][y][x] = VOXEL_LAVA; // Lava channels
                                    else if (rand() % 100 < 5) map->data[0][y][x] = VOXEL_OBSIDIAN; // Anvils
                                } else if (theme == 12) { // Frozen Tomb
                                    map->data[0][y][x] = VOXEL_ICE;
                                    int chance = rand() % 100;
                                    if (chance < 8) map->data[0][y][x] = VOXEL_ROCK; // Frozen graves
                                    else if (chance < 12) map->data[0][y][x] = VOXEL_CRYSTAL_BLUE; // Ice spikes
                                } else if (theme == 13) { // Spider Nest
                                    map->data[0][y][x] = VOXEL_ASH; // Dusty webs
                                    if (rand() % 100 < 15) map->data[0][y][x] = VOXEL_WALL; // Thick web cocoons
                                    else if (rand() % 100 < 5) map->data[0][y][x] = VOXEL_MUD; // Filth
                                } else if (theme == 14) { // Crystal Cave
                                    map->data[0][y][x] = VOXEL_FLOOR;
                                    int chance = rand() % 100;
                                    if (chance < 15) map->data[0][y][x] = VOXEL_CRYSTAL_BLUE;
                                    else if (chance < 30) map->data[0][y][x] = VOXEL_CRYSTAL_PURPLE;
                                } else if (theme == 15) { // Deserted Arena
                                    double d = sqrt((x - cx)*(x - cx) + (y - cy)*(y - cy));
                                    if (d >= r * 0.8 && d < r * 0.9) map->data[0][y][x] = VOXEL_WOOD; // Wooden stakes ring
                                    else map->data[0][y][x] = VOXEL_SAND;
                                } else if (theme == 16) { // Sunken Temple
                                    if (rand() % 100 < 60) map->data[0][y][x] = VOXEL_WATER;
                                    else map->data[0][y][x] = VOXEL_MARBLE; // Stepping stones
                                } else if (theme == 17) { // Fungal Forest
                                    map->data[0][y][x] = VOXEL_MUD;
                                    if (rand() % 100 < 30) map->data[0][y][x] = VOXEL_MUSHROOM_GLOW; // Dense mushrooms
                                } else if (theme == 18) { // Lava Caldera
                                    double d = sqrt((x - cx)*(x - cx) + (y - cy)*(y - cy));
                                    if (d < r * 0.5) map->data[0][y][x] = VOXEL_LAVA;
                                    else map->data[0][y][x] = VOXEL_OBSIDIAN;
                                } else if (theme == 19) { // Royal Throne Room
                                    if (fabsf((float)(x - cx)) <= 1) map->data[0][y][x] = VOXEL_ASH; // Carpet
                                    else map->data[0][y][x] = VOXEL_MARBLE;
                                    if (x == cx && y == cy - (r/2)) map->data[0][y][x] = VOXEL_GOLD_VEIN; // Throne
                                } else if (theme == 20) { // Chaos Rift
                                    int v = rand() % 10;
                                    if (v == 0) map->data[0][y][x] = VOXEL_LAVA;
                                    else if (v == 1) map->data[0][y][x] = VOXEL_WATER;
                                    else if (v == 2) map->data[0][y][x] = VOXEL_ICE;
                                    else if (v == 3) map->data[0][y][x] = VOXEL_MUSHROOM_GLOW;
                                    else if (v == 4) map->data[0][y][x] = VOXEL_GOLD_VEIN;
                                    else if (v == 5) map->data[0][y][x] = VOXEL_ASH;
                                    else map->data[0][y][x] = VOXEL_FLOOR;
                                }
                            }
                        }
                    }
                }
            }

            // Place 1 to 3 doors on the walls
            int num_doors = 1 + rand() % 3;
            for(int d = 0; d < num_doors && wall_count > 0; d++) {
                int door_idx = rand() % wall_count;
                map->data[0][wall_y[door_idx]][wall_x[door_idx]] = VOXEL_DOOR;
            }
            
            Room room = {(int)cx, (int)cy, (int)r, (int)r, {(int)cx, (int)cy}, num_doors};
            g_rooms[g_rooms_count++] = room;
            
            // We want to fill the map, so we don't break early, let max_attempts run its course
            //or break if we're satisfied. A 300x300 map can fit hundreds of rooms.
            if (g_rooms_count >= 800) break; 
        }
    }
}

static void apply_biome(Map* map, int floor_id) {
    VoxelType b_rock = VOXEL_ROCK;
    VoxelType b_wall = VOXEL_WALL;
    VoxelType b_floor = VOXEL_FLOOR;
    
    if (floor_id >= 1 && floor_id <= 10) {
        b_rock = VOXEL_ROCK; b_wall = VOXEL_WALL; b_floor = VOXEL_WOOD;
    } else if (floor_id >= 11 && floor_id <= 25) {
        b_rock = VOXEL_ROCK; b_wall = VOXEL_WALL; b_floor = VOXEL_MUD;
    } else if (floor_id >= 26 && floor_id <= 40) {
        b_rock = VOXEL_ROCK; b_wall = VOXEL_WALL; b_floor = VOXEL_FLOOR;
    } else if (floor_id >= 41 && floor_id <= 55) {
        b_rock = VOXEL_ROCK; b_wall = VOXEL_WALL; b_floor = VOXEL_SAND;
    } else if (floor_id >= 56 && floor_id <= 75) {
        b_rock = VOXEL_ROCK; b_wall = VOXEL_OBSIDIAN; b_floor = VOXEL_ASH;
    } else if (floor_id >= 76 && floor_id <= 90) {
        b_rock = VOXEL_ROCK; b_wall = VOXEL_WALL; b_floor = VOXEL_MARBLE;
    } else if (floor_id >= 91 && floor_id <= 100) {
        b_rock = VOXEL_ROCK; b_wall = VOXEL_OBSIDIAN; b_floor = VOXEL_ASH;
    }
    
    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            if (map->data[0][y][x] == VOXEL_ROCK) map->data[0][y][x] = b_rock;
            else if (map->data[0][y][x] == VOXEL_WALL) map->data[0][y][x] = b_wall;
            else if (map->data[0][y][x] == VOXEL_FLOOR) map->data[0][y][x] = b_floor;
        }
    }
}

static void spawn_veins(Map* map, int floor_id) {
    int num_veins = 10 + rand() % 10;
    for (int v = 0; v < num_veins; v++) {
        VoxelType vein_type = VOXEL_GOLD_VEIN;
        if (floor_id >= 1 && floor_id <= 10) vein_type = VOXEL_WATER;
        else if (floor_id >= 11 && floor_id <= 25) vein_type = (rand()%2==0) ? VOXEL_MUSHROOM_GLOW : VOXEL_WATER;
        else if (floor_id >= 26 && floor_id <= 40) vein_type = (rand()%2==0) ? VOXEL_GOLD_VEIN : VOXEL_WATER;
        else if (floor_id >= 41 && floor_id <= 55) vein_type = VOXEL_CRYSTAL_BLUE;
        else if (floor_id >= 56 && floor_id <= 75) vein_type = (rand()%2==0) ? VOXEL_LAVA : VOXEL_CRYSTAL_PURPLE;
        else if (floor_id >= 76 && floor_id <= 90) vein_type = VOXEL_ASH;
        else if (floor_id >= 91 && floor_id <= 100) vein_type = (rand()%2==0) ? VOXEL_LAVA : VOXEL_CRYSTAL_PURPLE;

        float x = (float)(rand() % MAP_WIDTH);
        float y = (float)(rand() % MAP_HEIGHT);
        int length = 30 + rand() % 100;
        float dir = (rand() % 360) * (float)(M_PI / 180.0f);
        
        for (int i = 0; i < length; i++) {
            x += cosf(dir) * 1.5f;
            y += sinf(dir) * 1.5f;
            dir += ((rand() % 100) / 100.0f - 0.5f) * 1.0f; // Wander
            
            int ix = (int)x;
            int iy = (int)y;
            if (ix >= 2 && ix < MAP_WIDTH-2 && iy >= 2 && iy < MAP_HEIGHT-2) {
                for(int dy=-1; dy<=1; dy++) {
                    for(int dx=-1; dx<=1; dx++) {
                        if (rand()%100 < 60) {
                            VoxelType current = map->data[0][iy+dy][ix+dx];
                            bool is_liquid = (vein_type == VOXEL_WATER || vein_type == VOXEL_LAVA || vein_type == VOXEL_MUD || vein_type == VOXEL_ASH);
                            if (current != VOXEL_DOOR && current != VOXEL_STAIRS_DOWN && current != VOXEL_STAIRS_UP) {
                                if (is_liquid) {
                                    map->data[0][iy+dy][ix+dx] = vein_type;
                                } else {
                                    // Solid veins only replace walls/rocks
                                    if (current != VOXEL_FLOOR && current != VOXEL_WOOD && current != VOXEL_SAND) {
                                        map->data[0][iy+dy][ix+dx] = vein_type;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void generate_procedural_dungeon(Map* map, int floor_id) {
    if (floor_id == 0) {
        int cx = MAP_CENTER_X;
        int cy = MAP_CENTER_Y;
        
        //General grass background
        map_fill_rect(map, 0, 0, MAP_WIDTH, MAP_HEIGHT, VOXEL_GRASS);

        //Paths to the cardinal points (leaving the square)
        map_fill_rect(map, cx - 3, 0, 7, MAP_HEIGHT, VOXEL_MARBLE);
        map_fill_rect(map, 0, cy - 3, MAP_WIDTH, 7, VOXEL_MARBLE);
        map_fill_rect(map, cx - 2, 0, 5, MAP_HEIGHT, VOXEL_COBBLE);
        map_fill_rect(map, 0, cy - 2, MAP_WIDTH, 5, VOXEL_COBBLE);

        //Let's draw a magnificent circular square
        int R_MARBLE = 36;
        int R_COBBLE = 34;
        int R_WATER = 9;
        
        for (int y = cy - R_MARBLE; y <= cy + R_MARBLE; y++) {
            for (int x = cx - R_MARBLE; x <= cx + R_MARBLE; x++) {
                float dx = (float)(x - cx);
                float dy = (float)(y - cy);
                float dist = sqrtf(dx*dx + dy*dy);
                
                if (dist <= R_WATER) {
                    map->data[0][y][x] = VOXEL_WATER; // Fontana centrale
                } else if (dist <= R_WATER + 2.0f) {
                    map->data[0][y][x] = VOXEL_ICE; //Fountain edge in blue ice
                } else if (dist <= R_COBBLE) {
                    //Mixed pattern for the square
                    if (((int)dist) % 4 == 0) {
                        map->data[0][y][x] = VOXEL_MARBLE;
                    } else {
                        map->data[0][y][x] = VOXEL_COBBLE;
                    }
                } else if (dist <= R_MARBLE) {
                    map->data[0][y][x] = VOXEL_MARBLE; //External edge of the square
                }
            }
        }

        //Decorations: Crystals at the corners of the fountain
        map_fill_rect(map, cx - 11, cy - 11, 2, 2, VOXEL_CRYSTAL_BLUE);
        map_fill_rect(map, cx + 10, cy - 11, 2, 2, VOXEL_CRYSTAL_BLUE);
        map_fill_rect(map, cx - 11, cy + 10, 2, 2, VOXEL_CRYSTAL_BLUE);
        map_fill_rect(map, cx + 10, cy + 10, 2, 2, VOXEL_CRYSTAL_BLUE);

        //Temple of the Wizard (North) - BLUE Crystal
        map_fill_rect(map, 145, 101, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, 146, 102, 9, 9, VOXEL_MARBLE);
        map_fill_rect(map, 149, 105, 3, 3, VOXEL_CRYSTAL_BLUE);
        map->data[0][111][150] = VOXEL_DOOR;

        //Paladin Temple (Northeast) - YELLOW Crystal
        map_fill_rect(map, 176, 113, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, 177, 114, 9, 9, VOXEL_MARBLE);
        map_fill_rect(map, 180, 117, 3, 3, VOXEL_CRYSTAL_YELLOW);
        map->data[0][123][181] = VOXEL_DOOR;

        //Cleric Temple (East) - WHITE Crystal
        map_fill_rect(map, 189, 145, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, 190, 146, 9, 9, VOXEL_MARBLE);
        map_fill_rect(map, 193, 149, 3, 3, VOXEL_CRYSTAL_WHITE);
        map->data[0][150][189] = VOXEL_DOOR;

        // Tempio dello Stregone (Sud-Est) - Cristallo ROSSO
        map_fill_rect(map, 176, 176, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, 177, 177, 9, 9, VOXEL_MARBLE);
        map_fill_rect(map, 180, 180, 3, 3, VOXEL_CRYSTAL_RED);
        map->data[0][181][176] = VOXEL_DOOR;

        //Warlock Temple (South) - PURPLE Crystal
        map_fill_rect(map, 145, 189, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, 146, 190, 9, 9, VOXEL_MARBLE);
        map_fill_rect(map, 149, 193, 3, 3, VOXEL_CRYSTAL_PURPLE);
        map->data[0][189][150] = VOXEL_DOOR;

        // Bard's Temple (South-West) - ORANGE Crystal
        map_fill_rect(map, 113, 176, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, 114, 177, 9, 9, VOXEL_WOOD);
        map_fill_rect(map, 117, 180, 3, 3, VOXEL_CRYSTAL_ORANGE);
        map->data[0][181][123] = VOXEL_DOOR;

        // Druid's Temple (West) - GREEN Crystal
        map_fill_rect(map, 101, 145, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, 102, 146, 9, 9, VOXEL_GRASS);
        map_fill_rect(map, 105, 149, 3, 3, VOXEL_CRYSTAL_GREEN);
        map->data[0][150][111] = VOXEL_DOOR;

        // Ranger's Temple (Northwest) - CYAN Crystal
        map_fill_rect(map, 113, 113, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, 114, 114, 9, 9, VOXEL_WOOD);
        map_fill_rect(map, 117, 117, 3, 3, VOXEL_CRYSTAL_CYAN);
        map->data[0][118][123] = VOXEL_DOOR;

        // === MARTIAL TEMPLES (Outer Ring, r=78) ===
        // Positioned on the 4 cardinal directions, outside the magic temples ring (r=44).
        // Each dojo: 11x11 temple with its class crystal altar.
        // The dedicated fountain of each class lives on the extra outer
        // ring defined below (fountains r=97); the statue ring (r=90)
        // remains the outermost crystal circle of the dojos.

        // Gladiator's Arena (Fighter, North, r=78) - COBBLE Floor, ORANGE Crystal
        // Centered on the north axis x=150 (temple x145..155), in symmetry
        // with the other three outer-ring dojos.
        map_fill_rect(map, 145,  67, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, 146,  68,  9,  9, VOXEL_COBBLE);
        map_fill_rect(map, 149,  71,  3,  3, VOXEL_CRYSTAL_ORANGE);
        map->data[0][77][150] = VOXEL_DOOR; // Door on the south wall (perimeter)
        // Fighter's Statue (first outer ring, r=90)
        map_fill_rect(map, 149,  59,  3,  3, VOXEL_CRYSTAL_ORANGE);

        // Fighting Pit (Barbarian, East, r=78) - GRASS floor, RED crystal
        map_fill_rect(map, 223, 145, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, 224, 146,  9,  9, VOXEL_GRASS);
        map_fill_rect(map, 227, 149,  3,  3, VOXEL_CRYSTAL_RED);
        map->data[0][150][223] = VOXEL_DOOR; // Door on the west wall (perimeter)
        // Barbarian's Statue (first outer ring, r=90)
        map_fill_rect(map, 239, 149,  3,  3, VOXEL_CRYSTAL_RED);

        // Den of Shadows (Rogue, South, r=78) - WOOD floor, PURPLE crystal
        map_fill_rect(map, 145, 223, 11, 11, VOXEL_ROCK);
        map_fill_rect(map, 146, 224,  9,  9, VOXEL_WOOD);
        map_fill_rect(map, 149, 227,  3,  3, VOXEL_CRYSTAL_PURPLE);
        map->data[0][223][150] = VOXEL_DOOR; // Door on the north wall (perimeter)
        // Rogue's Statue (first outer ring, r=90)
        map_fill_rect(map, 149, 239,  3,  3, VOXEL_CRYSTAL_PURPLE);

        // Lotus Dojo (Monk, West, r=78) - WOOD floor, WHITE crystal
        map_fill_rect(map,  67, 145, 11, 11, VOXEL_ROCK);
        map_fill_rect(map,  68, 146,  9,  9, VOXEL_WOOD);
        map_fill_rect(map,  71, 149,  3,  3, VOXEL_CRYSTAL_WHITE);
        map->data[0][150][77] = VOXEL_DOOR; // Door on the east wall (perimeter)
        // Monk's Statue (first outer ring, r=90)
        map_fill_rect(map,  59, 149,  3,  3, VOXEL_CRYSTAL_WHITE);

        // === MARTIAL FOUNTAINS (r=97) ===
        // One extra outer ring, one fountain per cardinal direction,
        // aligned with the dojos (r=78) and the statue ring (r=90).
        // Fountain: 5x5 thematic border + 3x3 water.
        //
        //   Fighter (N):   fountain (150,53)   ORANGE
        //   Barbarian (E): fountain (247,150)  RED
        //   Rogue (S):     fountain (150,247)  PURPLE
        //   Monk (W):      fountain (53,150)   WHITE

        // Fighter's Fountain (North)
        map_fill_rect(map, 148,  51,  5,  5, VOXEL_MARBLE);
        map_fill_rect(map, 149,  52,  3,  3, VOXEL_WATER);

        // Barbarian's Fountain (East)
        map_fill_rect(map, 245, 148,  5,  5, VOXEL_COBBLE);
        map_fill_rect(map, 246, 149,  3,  3, VOXEL_WATER);

        // Rogue's Fountain (South)
        map_fill_rect(map, 148, 245,  5,  5, VOXEL_ASH);
        map_fill_rect(map, 149, 246,  3,  3, VOXEL_WATER);

        // Monk's Fountain (West)
        map_fill_rect(map,  51, 148,  5,  5, VOXEL_MARBLE);
        map_fill_rect(map,  52, 149,  3,  3, VOXEL_WATER);

        // === FOUNTAINS AND STATUES === //
        // Wizard — fountain (150,91) statue (150,85) OK
        map_fill_rect(map, 148, 89, 5, 5, VOXEL_MARBLE);
        map_fill_rect(map, 149, 90, 3, 3, VOXEL_WATER);
        map_fill_rect(map, 149, 84, 3, 3, VOXEL_CRYSTAL_BLUE);

        // Paladin — fountain (191,108) statue (195,104) OK
        map_fill_rect(map, 189, 106, 5, 5, VOXEL_COBBLE);
        map_fill_rect(map, 190, 107, 3, 3, VOXEL_WATER);
        map_fill_rect(map, 194, 103, 3, 3, VOXEL_CRYSTAL_YELLOW);

        // Cleric — fountain (209,150) statue (215,150) OK
        map_fill_rect(map, 207, 148, 5, 5, VOXEL_ICE);
        map_fill_rect(map, 208, 149, 3, 3, VOXEL_WATER);
        map_fill_rect(map, 214, 149, 3, 3, VOXEL_CRYSTAL_WHITE);

        // Sorcerer — fountain (191,191) statue (195,195) OK
        map_fill_rect(map, 189, 189, 5, 5, VOXEL_OBSIDIAN);
        map_fill_rect(map, 190, 190, 3, 3, VOXEL_WATER);
        map_fill_rect(map, 194, 194, 3, 3, VOXEL_CRYSTAL_RED);

        // Warlock — fountain (150,209) statue (150,215) OK
        map_fill_rect(map, 148, 207, 5, 5, VOXEL_ASH);
        map_fill_rect(map, 149, 208, 3, 3, VOXEL_WATER);
        map_fill_rect(map, 149, 214, 3, 3, VOXEL_CRYSTAL_PURPLE);

        // Bard — fountain (108,191) statue (104,195) OK
        map_fill_rect(map, 106, 189, 5, 5, VOXEL_WOOD);
        map_fill_rect(map, 107, 190, 3, 3, VOXEL_WATER);
        map_fill_rect(map, 103, 194, 3, 3, VOXEL_CRYSTAL_ORANGE);

        // Druid — fountain (91,150) statue (85,150) OK
        map_fill_rect(map, 89, 148, 5, 5, VOXEL_SAND);
        map_fill_rect(map, 90, 149, 3, 3, VOXEL_WATER);
        map_fill_rect(map, 84, 149, 3, 3, VOXEL_CRYSTAL_GREEN);

        // Ranger — fountain (108,108) statue (104,104) OK
        map_fill_rect(map, 106, 106, 5, 5, VOXEL_GOLD_VEIN);
        map_fill_rect(map, 107, 107, 3, 3, VOXEL_WATER);
        map_fill_rect(map, 103, 103, 3, 3, VOXEL_CRYSTAL_CYAN);


        //Positioning of the 11 city stores in a circle (radius 26).
        //Slot i sits at angle i * (360/11) degrees, so the ring stays
        //perfectly symmetric. Slot 10 is The Archive of a Thousand Battles
        //(martial bookshop, see spawn_martial_archive in server_spawn.c).
        int shop_coords[11][2];
        for (int i = 0; i < 11; i++) {
            float angle = (i * (360.0f / 11.0f)) * (M_PI / 180.0f);
            shop_coords[i][0] = cx + (int)(cosf(angle) * 26.0f);
            shop_coords[i][1] = cy + (int)(sinf(angle) * 26.0f);
        }

        // Construction of the 11 buildings
        for (int i = 0; i < 11; i++) {
            int sx = shop_coords[i][0];
            int sy = shop_coords[i][1];
            
            // 9x9 external walls (rocks)
            map_fill_rect(map, sx - 4, sy - 4, 9, 9, VOXEL_ROCK);
            //Internal floor 7x7
            map_fill_rect(map, sx - 3, sy - 3, 7, 7, VOXEL_WOOD);
            //Merchant's chest (Decorative ash block inside)
            map_fill_rect(map, sx - 1, sy - 1, 3, 2, VOXEL_ASH);
            
            //Door looking towards the center (cx, cy)
            float dx = cx - sx;
            float dy = cy - sy;
            if (fabsf(dx) > fabsf(dy)) {
                if (dx > 0) map->data[0][sy][sx + 4] = VOXEL_DOOR; // Est
                else        map->data[0][sy][sx - 4] = VOXEL_DOOR; // Ovest
            } else {
                if (dy > 0) map->data[0][sy + 4][sx] = VOXEL_DOOR; // Sud
                else        map->data[0][sy - 4][sx] = VOXEL_DOOR; // Nord
            }
        }

        //Central island safe for stairs
        map_fill_rect(map, cx - 2, cy - 2, 5, 5, VOXEL_MARBLE);
        map_fill_rect(map, cx - 1, cy - 1, 3, 3, VOXEL_COBBLE);
        
        //Stairs in the exact center of the map
        map->data[0][cy][cx] = VOXEL_STAIRS_DOWN;
        return;
    }
    if (floor_id == 20) { /*... BOSS ...*/ return; }

    map_fill_rect(map, 0, 0, MAP_WIDTH, MAP_HEIGHT, VOXEL_ROCK);
    
    //"Mixed" Generation: Global caves generated before artificial rooms
    int cave_chance = 30; // 30% chance for a normal floor to be heavily cavernous
    if ((floor_id >= 11 && floor_id <= 25) || (floor_id >= 41 && floor_id <= 55)) cave_chance = 80;
    
    if (rand() % 100 < cave_chance) {
        for (int y = 5; y < MAP_HEIGHT-5; y++) {
            for (int x = 5; x < MAP_WIDTH-5; x++) {
                if (rand() % 100 < 48) map->data[0][y][x] = VOXEL_FLOOR;
            }
        }
        for (int p = 0; p < 5; p++) {
            VoxelType* temp = (VoxelType*)calloc(MAP_HEIGHT * MAP_WIDTH, sizeof(VoxelType));
            for (int y = 5; y < MAP_HEIGHT-5; y++) {
                for (int x = 5; x < MAP_WIDTH-5; x++) {
                    int neighbors = 0;
                    for(int dy=-1; dy<=1; dy++){
                        for(int dx=-1; dx<=1; dx++){
                            if(dy==0 && dx==0) continue;
                            if(map->data[0][y+dy][x+dx] == VOXEL_ROCK) neighbors++;
                        }
                    }
                    if (map->data[0][y][x] == VOXEL_ROCK) {
                        temp[y * MAP_WIDTH + x] = (neighbors >= 4) ? VOXEL_ROCK : VOXEL_FLOOR;
                    } else {
                        temp[y * MAP_WIDTH + x] = (neighbors >= 5) ? VOXEL_ROCK : VOXEL_FLOOR;
                    }
                }
            }
            for (int y = 5; y < MAP_HEIGHT-5; y++) {
                for (int x = 5; x < MAP_WIDTH-5; x++) {
                    map->data[0][y][x] = temp[y * MAP_WIDTH + x];
                }
            }
            free(temp);
        }
    }
    
    // Prepare the platform (whole map is walkable floor)
    map_fill_rect(map, 0, 0, MAP_WIDTH, MAP_HEIGHT, VOXEL_FLOOR);
    
    // Add impassable border
    for (int x = 0; x < MAP_WIDTH; x++) { map->data[0][0][x] = VOXEL_ROCK; map->data[0][MAP_HEIGHT-1][x] = VOXEL_ROCK; }
    for (int y = 0; y < MAP_HEIGHT; y++) { map->data[0][y][0] = VOXEL_ROCK; map->data[0][y][MAP_WIDTH-1] = VOXEL_ROCK; }

    //Outdoor generation (forests, lakes, deserts)
    generate_blob(map, VOXEL_GRASS, 50, 50, 400);
    generate_blob(map, VOXEL_WOOD, 25, 20, 150); // Trees
    generate_blob(map, VOXEL_WATER, 15, 100, 600); // Lakes/Rivers
    generate_blob(map, VOXEL_SAND, 30, 80, 500); // Deserts
    generate_blob(map, VOXEL_MUSHROOM_GLOW, 15, 20, 100); // Magic shrooms
    generate_blob(map, VOXEL_LAVA, 5, 50, 300); // Lava pools

    //STEP 1: Creating and placing all rooms (polygons)
    //The polygons will overwrite the outdoor perfectly.
    place_polygon_rooms(map);


    //PHASE 3: Stairs (Start and End)
    if (g_rooms_count > 0) map->data[0][g_rooms[0].center.y][g_rooms[0].center.x] = VOXEL_STAIRS_UP;
    if (g_rooms_count > 1) map->data[0][g_rooms[g_rooms_count-1].center.y][g_rooms[g_rooms_count-1].center.x] = VOXEL_STAIRS_DOWN;

    // PHASE 4: Biomes and Geological Veins (Streamers) Application
    apply_biome(map, floor_id);
    spawn_veins(map, floor_id);

    int real_connected = 0;
    for (int i=0; i<g_rooms_count; i++) {
        if (g_rooms[i].doors > 0 || i == 0) real_connected++;
    }

    printf("[DungeonGen] Floor %d: Generated %d rooms (actually connected: %d).\n", floor_id, g_rooms_count, real_connected);
    for (int i = 0; i < g_rooms_count; i++) {
        printf("  -> Room %3d: pos(%3d, %3d), dim(%2d x %2d), center(%3d, %3d), doors: %d\n", 
               i, g_rooms[i].x, g_rooms[i].y, g_rooms[i].w, g_rooms[i].h, 
               g_rooms[i].center.x, g_rooms[i].center.y, g_rooms[i].doors);
    }
}
