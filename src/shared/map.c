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

#include "map.h"
#include "map_internal.h"
#include <stdlib.h>
#include <string.h>
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

void map_fill_rect(Map *map, int x, int y, int w, int h, VoxelType type) {
    for (int iy = y; iy < y + h; iy++) {
        for (int ix = x; ix < x + w; ix++) {
            if (ix >= 0 && ix < MAP_WIDTH && iy >= 0 && iy < MAP_HEIGHT) {
                map->data[0][iy][ix] = type;
            }
        }
    }
}

void floor_init(Floor *floor, int floor_id) {
    floor->id = floor_id;
    VoxelType base = (floor_id == 0) ? VOXEL_GRASS : VOXEL_ROCK;
    map_fill_rect(&floor->map, 0, 0, MAP_WIDTH, MAP_HEIGHT, base);
    memset(floor->entity_grid, 0, sizeof(floor->entity_grid));
    generate_procedural_dungeon(&floor->map, floor_id);
    spawn_traps_on_floor(floor);
}

void world_init(World *world) {
    /*No srand() here: the server seeds the RNG once at process start, so
     * dice stay unpredictable across restarts even when a world is loaded
     * from disk.*/
    for (int i = 0; i < MAX_FLOORS; i++) {
        floor_init(&world->floors[i], i);
    }
}

/* World persistence lives in src/server/persist.c (portable format). */

static void draw_line(Map *map, int x0, int y0, int x1, int y1, VoxelType type, int *out_x,
                      int *out_y, int *out_count) {
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
        if (x0 == x1 && y0 == y1) {
            break;
        }
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static bool point_in_polygon(int x, int y, const int *px, const int *py, int sides) {
    bool c = false;
    for (int i = 0, j = sides - 1; i < sides; j = i++) {
        if (((py[i] > y) != (py[j] > y)) &&
            ((float)x <
             (float)(px[j] - px[i]) * (float)(y - py[i]) / (float)(py[j] - py[i]) + (float)px[i])) {
            c = !c;
        }
    }
    return c;
}

static void generate_blob(Map *map, VoxelType type, int count, int min_size, int max_size) {
    for (int i = 0; i < count; i++) {
        int x = rand() % MAP_WIDTH;
        int y = rand() % MAP_HEIGHT;
        int size = min_size + rand() % (max_size - min_size + 1);
        for (int j = 0; j < size; j++) {
            if (x >= 1 && x < MAP_WIDTH - 1 && y >= 1 && y < MAP_HEIGHT - 1) {
                map->data[0][y][x] = type;
            }
            int dir = rand() % 4;
            if (dir == 0) {
                y--;
            } else if (dir == 1) {
                y++;
            } else if (dir == 2) {
                x--;
            } else {
                x++;
            }
        }
    }
}

/*Room themes. 0: empty, 1: library, 2: crypt, 3: slime pool, 4: armory,
 * 5: treasure vault, 6: mirror maze, 7: overgrown garden, 8: summoning
 * circle, 9: alchemist lab, 10: arcane observatory, 11: dwarven forge,
 * 12: frozen tomb, 13: spider nest, 14: crystal cave, 15: deserted arena,
 * 16: sunken temple, 17: fungal forest, 18: lava caldera, 19: throne room,
 * 20: chaos rift.*/
#define ROOM_THEMES 21

/*Themes that are a base voxel with scattered features: one d100 per tile,
 * the first feature whose threshold is above the roll replaces the base.*/
typedef struct {
    VoxelType base;
    struct {
        int below;
        VoxelType voxel;
    } feature[4];
} ScatterTheme;

static const ScatterTheme k_scatter_themes[ROOM_THEMES] = {
    [1] = {VOXEL_WOOD, {{15, VOXEL_WALL}}},                                  /*bookshelves*/
    [2] = {VOXEL_MUD, {{5, VOXEL_ROCK}}},                                    /*graves*/
    [4] = {VOXEL_COBBLE, {{8, VOXEL_OBSIDIAN}}},                             /*weapon racks*/
    [5] = {VOXEL_MARBLE, {{5, VOXEL_GOLD_VEIN}, {7, VOXEL_CRYSTAL_PURPLE}}}, /*gold pillars, gems*/
    [6] = {VOXEL_ICE, {{25, VOXEL_CRYSTAL_BLUE}}},                           /*crystal mirrors*/
    [7] = {VOXEL_GRASS, {{10, VOXEL_MUSHROOM_GLOW}, {15, VOXEL_WOOD}, {20, VOXEL_WATER}}},
    [9] = {VOXEL_MARBLE, /*spilled fire, spores, potions, workbenches*/
           {{4, VOXEL_LAVA}, {8, VOXEL_MUSHROOM_GLOW}, {12, VOXEL_WATER}, {15, VOXEL_WALL}}},
    [12] = {VOXEL_ICE, {{8, VOXEL_ROCK}, {12, VOXEL_CRYSTAL_BLUE}}}, /*frozen graves, ice spikes*/
    [14] = {VOXEL_FLOOR, {{15, VOXEL_CRYSTAL_BLUE}, {30, VOXEL_CRYSTAL_PURPLE}}},
    [16] = {VOXEL_MARBLE, {{60, VOXEL_WATER}}},      /*stepping stones in the water*/
    [17] = {VOXEL_MUD, {{30, VOXEL_MUSHROOM_GLOW}}}, /*dense mushrooms*/
};

static VoxelType scatter_theme_voxel(const ScatterTheme *t) {
    int chance = rand() % 100;
    for (int i = 0; i < 4 && t->feature[i].below > 0; i++) {
        if (chance < t->feature[i].below) {
            return t->feature[i].voxel;
        }
    }
    return t->base;
}

/*The themes drawn by position: rings around the centre, patterns, chaos.*/
static VoxelType shaped_theme_voxel(int theme, int x, int y, double cx, double cy, double r) {
    static const VoxelType k_chaos[10] = {
        VOXEL_LAVA, VOXEL_WATER, VOXEL_ICE,   VOXEL_MUSHROOM_GLOW, VOXEL_GOLD_VEIN,
        VOXEL_ASH,  VOXEL_FLOOR, VOXEL_FLOOR, VOXEL_FLOOR,         VOXEL_FLOOR};
    double d = sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
    VoxelType v;
    switch (theme) {
    case 3: /*slime pool*/
        return d < r * 0.6 ? VOXEL_WATER : VOXEL_MUD;
    case 8: /*summoning circle: altar, ring of power, burnt ground*/
        if (d < r * 0.3) {
            return VOXEL_OBSIDIAN;
        }
        return d < r * 0.4 ? VOXEL_CRYSTAL_PURPLE : VOXEL_ASH;
    case 10: /*arcane observatory: checkerboard with lenses*/
        v = (x + y) % 2 == 0 ? VOXEL_MARBLE : VOXEL_OBSIDIAN;
        return rand() % 100 < 3 ? VOXEL_CRYSTAL_BLUE : v;
    case 11: /*dwarven forge: lava channels and anvils*/
        if (x % 7 == 0 || y % 7 == 0) {
            return VOXEL_LAVA;
        }
        return rand() % 100 < 5 ? VOXEL_OBSIDIAN : VOXEL_COBBLE;
    case 13: /*spider nest: dusty webs, cocoons, filth*/
        if (rand() % 100 < 15) {
            return VOXEL_WALL;
        }
        return rand() % 100 < 5 ? VOXEL_MUD : VOXEL_ASH;
    case 15: /*deserted arena: a ring of wooden stakes*/
        return (d >= r * 0.8 && d < r * 0.9) ? VOXEL_WOOD : VOXEL_SAND;
    case 18: /*lava caldera*/
        return d < r * 0.5 ? VOXEL_LAVA : VOXEL_OBSIDIAN;
    case 19: /*throne room: a carpet to the throne*/
        if (x == cx && y == cy - (r / 2)) {
            return VOXEL_GOLD_VEIN;
        }
        return fabsf((float)(x - cx)) <= 1 ? VOXEL_ASH : VOXEL_MARBLE;
    case 20: /*chaos rift*/
        return k_chaos[rand() % 10];
    default:
        return VOXEL_FLOOR;
    }
}

typedef struct {
    double cx, cy, r;
    int sides;
    int px[30], py[30];
} Polygon;

/*Paints the inside of the polygon with the theme (theme 0 leaves it).*/
static void fill_polygon_theme(Map *map, const Polygon *p, int theme) {
    if (theme == 0) {
        return;
    }
    const ScatterTheme *scatter = &k_scatter_themes[theme];
    for (int y = (int)(p->cy - p->r - 1); y <= (int)(p->cy + p->r + 1); y++) {
        for (int x = (int)(p->cx - p->r - 1); x <= (int)(p->cx + p->r + 1); x++) {
            if (y < 1 || y >= MAP_HEIGHT - 1 || x < 1 || x >= MAP_WIDTH - 1 ||
                map->data[0][y][x] == VOXEL_WALL || /*don't overwrite walls*/
                !point_in_polygon(x, y, p->px, p->py, p->sides)) {
                continue;
            }
            map->data[0][y][x] = scatter->feature[0].below > 0
                                     ? scatter_theme_voxel(scatter)
                                     : shaped_theme_voxel(theme, x, y, p->cx, p->cy, p->r);
        }
    }
}

/*True if a room of radius r at (cx, cy) would touch one already placed
 * (2 tiles between the walls at least).*/
static bool room_overlaps(double cx, double cy, double r) {
    for (int j = 0; j < g_rooms_count; j++) {
        double cx2 = g_rooms[j].center.x;
        double cy2 = g_rooms[j].center.y;
        double r2 = g_rooms[j].w; // Using w to store radius
        double dist = sqrt((cx - cx2) * (cx - cx2) + (cy - cy2) * (cy - cy2));
        if (dist < (r + r2 + 2.0)) {
            return true;
        }
    }
    return false;
}

/*Fills the map with polygonal rooms (3 to 25 sides, radius 10 to 20) that
 * do not touch, each with a theme and 1 to 3 doors.*/
static void place_polygon_rooms(Map *map) {
    g_rooms_count = 0;
    int max_attempts = 100000;

    /*A 300x300 map fits hundreds of rooms: keep trying until 800.*/
    for (int i = 0; i < max_attempts && g_rooms_count < 800; i++) {
        Polygon p = {0};
        p.r = 10.0 + (rand() % 11);
        p.cx = 25.0 + (rand() % (MAP_WIDTH - 50));
        p.cy = 25.0 + (rand() % (MAP_HEIGHT - 50));
        if (room_overlaps(p.cx, p.cy, p.r)) {
            continue;
        }
        p.sides = 3 + (rand() % 23);
        double phase = ((double)rand() / RAND_MAX) * 2.0 * M_PI;
        for (int s = 0; s < p.sides; s++) {
            double angle = phase + (s * 2.0 * M_PI / p.sides);
            p.px[s] = (int)(p.cx + p.r * cos(angle));
            p.py[s] = (int)(p.cy + p.r * sin(angle));
        }
        p.px[p.sides] = p.px[0];
        p.py[p.sides] = p.py[0];

        // Draw walls and collect wall points
        int wall_x[1000], wall_y[1000];
        int wall_count = 0;
        for (int s = 0; s < p.sides; s++) {
            draw_line(map, p.px[s], p.py[s], p.px[s + 1], p.py[s + 1], VOXEL_WALL, wall_x, wall_y,
                      &wall_count);
        }
        fill_polygon_theme(map, &p, rand() % ROOM_THEMES);

        // Place 1 to 3 doors on the walls
        int num_doors = 1 + rand() % 3;
        for (int d = 0; d < num_doors && wall_count > 0; d++) {
            int door_idx = rand() % wall_count;
            map->data[0][wall_y[door_idx]][wall_x[door_idx]] = VOXEL_DOOR;
        }
        Room room = {(int)p.cx, (int)p.cy, (int)p.r, (int)p.r, {(int)p.cx, (int)p.cy}, num_doors};
        g_rooms[g_rooms_count++] = room;
    }
}

static void apply_biome(Map *map, int floor_id) {
    VoxelType b_rock = VOXEL_ROCK;
    VoxelType b_wall = VOXEL_WALL;
    VoxelType b_floor = VOXEL_FLOOR;

    if (floor_id >= 1 && floor_id <= 10) {
        b_rock = VOXEL_ROCK;
        b_wall = VOXEL_WALL;
        b_floor = VOXEL_WOOD;
    } else if (floor_id >= 11 && floor_id <= 25) {
        b_rock = VOXEL_ROCK;
        b_wall = VOXEL_WALL;
        b_floor = VOXEL_MUD;
    } else if (floor_id >= 26 && floor_id <= 40) {
        b_rock = VOXEL_ROCK;
        b_wall = VOXEL_WALL;
        b_floor = VOXEL_FLOOR;
    } else if (floor_id >= 41 && floor_id <= 55) {
        b_rock = VOXEL_ROCK;
        b_wall = VOXEL_WALL;
        b_floor = VOXEL_SAND;
    } else if ((floor_id >= 56 && floor_id <= 75) || (floor_id >= 91 && floor_id <= 100)) {
        b_rock = VOXEL_ROCK;
        b_wall = VOXEL_OBSIDIAN;
        b_floor = VOXEL_ASH;
    } else if (floor_id >= 76 && floor_id <= 90) {
        b_rock = VOXEL_ROCK;
        b_wall = VOXEL_WALL;
        b_floor = VOXEL_MARBLE;
    }

    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            if (map->data[0][y][x] == VOXEL_ROCK) {
                map->data[0][y][x] = b_rock;
            } else if (map->data[0][y][x] == VOXEL_WALL) {
                map->data[0][y][x] = b_wall;
            } else if (map->data[0][y][x] == VOXEL_FLOOR) {
                map->data[0][y][x] = b_floor;
            }
        }
    }
}

static void spawn_veins(Map *map, int floor_id) {
    int num_veins = 10 + rand() % 10;
    for (int v = 0; v < num_veins; v++) {
        VoxelType vein_type = VOXEL_GOLD_VEIN;
        if (floor_id >= 1 && floor_id <= 10) {
            vein_type = VOXEL_WATER;
        } else if (floor_id >= 11 && floor_id <= 25) {
            vein_type = (rand() % 2 == 0) ? VOXEL_MUSHROOM_GLOW : VOXEL_WATER;
        } else if (floor_id >= 26 && floor_id <= 40) {
            vein_type = (rand() % 2 == 0) ? VOXEL_GOLD_VEIN : VOXEL_WATER;
        } else if (floor_id >= 41 && floor_id <= 55) {
            vein_type = VOXEL_CRYSTAL_BLUE;
        } else if ((floor_id >= 56 && floor_id <= 75) || (floor_id >= 91 && floor_id <= 100)) {
            vein_type = (rand() % 2 == 0) ? VOXEL_LAVA : VOXEL_CRYSTAL_PURPLE;
        } else if (floor_id >= 76 && floor_id <= 90) {
            vein_type = VOXEL_ASH;
        }

        float x = (float)(rand() % MAP_WIDTH);
        float y = (float)(rand() % MAP_HEIGHT);
        int length = 30 + rand() % 100;
        float dir = (float)(rand() % 360) * (float)(M_PI / 180.0f);

        for (int i = 0; i < length; i++) {
            x += cosf(dir) * 1.5f;
            y += sinf(dir) * 1.5f;
            dir += ((float)(rand() % 100) / 100.0f - 0.5f) * 1.0f; // Wander

            int ix = (int)x;
            int iy = (int)y;
            if (ix >= 2 && ix < MAP_WIDTH - 2 && iy >= 2 && iy < MAP_HEIGHT - 2) {
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (rand() % 100 < 60) {
                            VoxelType current = map->data[0][iy + dy][ix + dx];
                            bool is_liquid = (vein_type == VOXEL_WATER || vein_type == VOXEL_LAVA ||
                                              vein_type == VOXEL_MUD || vein_type == VOXEL_ASH);
                            if (current != VOXEL_DOOR && current != VOXEL_STAIRS_DOWN &&
                                current != VOXEL_STAIRS_UP) {
                                if (is_liquid) {
                                    map->data[0][iy + dy][ix + dx] = vein_type;
                                } else {
                                    // Solid veins only replace walls/rocks
                                    if (current != VOXEL_FLOOR && current != VOXEL_WOOD &&
                                        current != VOXEL_SAND) {
                                        map->data[0][iy + dy][ix + dx] = vein_type;
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

void generate_procedural_dungeon(Map *map, int floor_id) {
    if (floor_id == 0) {
        generate_city(map);
        return;
    }
    // Prepare the platform (whole map is walkable floor)
    map_fill_rect(map, 0, 0, MAP_WIDTH, MAP_HEIGHT, VOXEL_FLOOR);

    // Add impassable border
    for (int x = 0; x < MAP_WIDTH; x++) {
        map->data[0][0][x] = VOXEL_ROCK;
        map->data[0][MAP_HEIGHT - 1][x] = VOXEL_ROCK;
    }
    for (int y = 0; y < MAP_HEIGHT; y++) {
        map->data[0][y][0] = VOXEL_ROCK;
        map->data[0][y][MAP_WIDTH - 1] = VOXEL_ROCK;
    }

    //Outdoor generation (forests, lakes, deserts)
    generate_blob(map, VOXEL_GRASS, 50, 50, 400);
    generate_blob(map, VOXEL_WOOD, 25, 20, 150);          // Trees
    generate_blob(map, VOXEL_WATER, 15, 100, 600);        // Lakes/Rivers
    generate_blob(map, VOXEL_SAND, 30, 80, 500);          // Deserts
    generate_blob(map, VOXEL_MUSHROOM_GLOW, 15, 20, 100); // Magic shrooms
    generate_blob(map, VOXEL_LAVA, 5, 50, 300);           // Lava pools

    //STEP 1: Creating and placing all rooms (polygons)
    //The polygons will overwrite the outdoor perfectly.
    place_polygon_rooms(map);

    //PHASE 3: Stairs (Start and End)
    if (g_rooms_count > 0) {
        map->data[0][g_rooms[0].center.y][g_rooms[0].center.x] = VOXEL_STAIRS_UP;
    }
    if (g_rooms_count > 1) {
        map->data[0][g_rooms[g_rooms_count - 1].center.y][g_rooms[g_rooms_count - 1].center.x] =
            VOXEL_STAIRS_DOWN;
    }

    // PHASE 4: Biomes and Geological Veins (Streamers) Application
    apply_biome(map, floor_id);
    spawn_veins(map, floor_id);
}
