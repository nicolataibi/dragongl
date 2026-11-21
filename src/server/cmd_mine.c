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
 * cmd_mine.c - The tunnel command: mining veins, crystals and rock, digging the ground.
 */

#include "server_internal.h"
#include "server_cmd.h"

/*Only a miner's pickaxe or a shovel digs: a war pick is a weapon.*/
bool is_digging_tool(const ItemInstance *it) {
    if (it->template_idx == -1) {
        return false;
    }
    const char *iname = item_database[it->template_idx].name;
    return strcasestr(iname, "Miner's") || strcasestr(iname, "Pickaxe") ||
           strcasestr(iname, "Shovel");
}

const ItemInstance *find_digging_tool(const Client *c) {
    if (is_digging_tool(&c->slot_hand_r)) {
        return &c->slot_hand_r;
    }
    if (is_digging_tool(&c->slot_hand_l)) {
        return &c->slot_hand_l;
    }
    for (int i = 0; i < c->backpack_count; i++) {
        if (is_digging_tool(&c->backpack[i])) {
            return &c->backpack[i];
        }
    }
    for (int i = 0; i < MAX_BELT; i++) {
        if (is_digging_tool(&c->belt[i])) {
            return &c->belt[i];
        }
    }
    return NULL;
}

/*Yields scale with the depth (the city counts as floor 1).*/
static int depth_factor(const Client *c) {
    return c->floor_id > 0 ? c->floor_id : 1;
}

static void mine_gold_vein(Game *game, Client *c, Floor *f, int tx, int ty) {
    int gold_yield = (50 + rand() % 100) * depth_factor(c);
    c->gold += (uint64_t)gold_yield;
    c->xp += 15;
    f->map.data[0][ty][tx] = VOXEL_FLOOR;
    broadcast_spell_vfx(game, tx, ty, tx, ty, 1, 0.8f, 0.7f, 0.1f, c->floor_id); // Gold explosion
    send_text_to_client(
        c->sock,
        "[MINE] You have successfully mined pure gold from the vein! (+%d Gold Coins, +15 XP)",
        gold_yield);
}

/*A mined crystal gives gold and XP, bursts in its colour and grows back
 * after 50 to 100 rounds.*/
static void mine_crystal(Game *game, Client *c, Floor *f, int tx, int ty, VoxelType vt) {
    static const struct {
        VoxelType crystal;
        float r, g, b;
    } k_colours[] = {
        {VOXEL_CRYSTAL_BLUE, 0.3f, 0.7f, 1.0f},   {VOXEL_CRYSTAL_PURPLE, 0.8f, 0.2f, 1.0f},
        {VOXEL_CRYSTAL_RED, 1.0f, 0.1f, 0.1f},    {VOXEL_CRYSTAL_GREEN, 0.1f, 1.0f, 0.2f},
        {VOXEL_CRYSTAL_YELLOW, 1.0f, 0.9f, 0.1f}, {VOXEL_CRYSTAL_ORANGE, 1.0f, 0.5f, 0.0f},
        {VOXEL_CRYSTAL_CYAN, 0.0f, 0.9f, 1.0f},
    };
    int gold_yield = (100 + rand() % 200) * depth_factor(c);
    c->gold += (uint64_t)gold_yield;
    c->xp += 30;
    f->map.data[0][ty][tx] = VOXEL_FLOOR;
    if (f->crystal_respawn_count < 100) {
        f->crystal_respawns[f->crystal_respawn_count].x = tx;
        f->crystal_respawns[f->crystal_respawn_count].y = ty;
        f->crystal_respawns[f->crystal_respawn_count].type = vt;
        f->crystal_respawns[f->crystal_respawn_count].respawn_timer = 50 + (rand() % 50);
        f->crystal_respawn_count++;
    }
    float cr = 1.0f, cg = 1.0f, cb = 1.0f; /*white*/
    for (size_t i = 0; i < sizeof(k_colours) / sizeof(k_colours[0]); i++) {
        if (k_colours[i].crystal == vt) {
            cr = k_colours[i].r;
            cg = k_colours[i].g;
            cb = k_colours[i].b;
        }
    }
    broadcast_spell_vfx(game, tx, ty, tx, ty, 1, cr, cg, cb, c->floor_id);
    send_text_to_client(
        c->sock,
        "[MINE] You have shattered the crystal and extracted precious fragments! (+%d Gold, +30 XP)",
        gold_yield);
}

/*Rock and walls open; 15% of the tiles hide some silver.*/
static void mine_rock(Game *game, Client *c, Floor *f, int tx, int ty) {
    f->map.data[0][ty][tx] = VOXEL_FLOOR;
    broadcast_spell_vfx(game, tx, ty, tx, ty, 1, 0.6f, 0.6f, 0.6f,
                        c->floor_id); // Grey rock explosion
    unsigned int hash = (unsigned int)(tx * 73 + ty * 37 + c->floor_id * 19 + 17);
    if (hash % 100 < 15) {
        int bonus = 20 * depth_factor(c);
        c->gold += (uint64_t)bonus;
        send_text_to_client(
            c->sock, "[MINE] While digging the rock you found a small supply of silver! (+%d Gold)",
            bonus);
    } else {
        send_text_to_client(c->sock, "[SYSTEM] You have carved out the rock, opening a passage!");
    }
}

/*Digging the ground underfoot turns it to rubble; 20% of the tiles hide
 * a few coins.*/
static void dig_ground(Client *c, Floor *f, int tx, int ty) {
    unsigned int hash = (unsigned int)(tx * 31 + ty * 47 + c->floor_id * 13 + 5);
    f->map.data[0][ty][tx] = VOXEL_COBBLE;
    if (hash % 100 < 20) {
        int bonus = 10 * depth_factor(c);
        c->gold += (uint64_t)bonus;
        send_text_to_client(
            c->sock, "[MINE] While digging in the loose soil you found some lost coins! (+%d Gold)",
            bonus);
    } else {
        send_text_to_client(c->sock,
                            "[MINE] You disturbed the ground, but found nothing of value.");
    }
}

/*tunnel <n|s|e|w|d>: mines the tile in that direction, or digs underfoot.
 * "t " (lowercase) is the short alias: 't n' = tunnel north.*/
bool cmd_tunnel(Client *c, const char *cmd, Game *game) {
    const char *dir_str = strchr(cmd, ' ');
    if (!dir_str) {
        send_text_to_client(c->sock,
                            "[SYSTEM] Use: tunnel <n|s|e|w> or 'tunnel d' (down/under your feet)");
        return true;
    }
    dir_str += strspn(dir_str, " ");
    int dx = 0, dy = 0;
    size_t len = strcspn(dir_str, " ");
    bool digging_down =
        dir_str[len + strspn(dir_str + len, " ")] == '\0' &&
        ((len == 1 && dir_str[0] == 'd') || (len == 4 && strncasecmp(dir_str, "down", 4) == 0));
    if (!digging_down && !parse_direction4(dir_str, &dx, &dy)) {
        send_text_to_client(c->sock, "[SYSTEM] Invalid direction. Use: n, s, e, w, or d (down)");
        return true;
    }
    int tx = c->x + dx, ty = c->y + dy;
    /*The town is built, not dug: its walls, statues and paving stay.*/
    if (c->floor_id == 0) {
        send_text_to_client(c->sock, "[MINE] The town is not a mine: dig in the dungeon.");
        return true;
    }
    /*The outermost ring of rock is the edge of the world: it is never dug.*/
    if (tx < 1 || tx >= MAP_WIDTH - 1 || ty < 1 || ty >= MAP_HEIGHT - 1) {
        send_text_to_client(c->sock, "[SYSTEM] Beyond the edge of the map.");
        return true;
    }
    if (!find_digging_tool(c)) {
        send_text_to_client(c->sock,
                            "[MINE] You need a pickaxe or a shovel to dig (in a hand, on the belt "
                            "or in the backpack).");
        return true;
    }
    Floor *f = &game->world->floors[c->floor_id];
    VoxelType vt = f->map.data[0][ty][tx];
    if (vt == VOXEL_GOLD_VEIN) {
        mine_gold_vein(game, c, f, tx, ty);
    } else if (vt >= VOXEL_CRYSTAL_BLUE && vt <= VOXEL_CRYSTAL_WHITE) {
        mine_crystal(game, c, f, tx, ty, vt);
    } else if (vt == VOXEL_MUSHROOM_GLOW) {
        c->hp = (c->hp + 15 > c->max_hp) ? c->max_hp : c->hp + 15;
        f->map.data[0][ty][tx] = VOXEL_FLOOR;
        send_detailed_state(game, c);
        send_text_to_client(
            c->sock,
            "[MINE] You extract the glowing mushrooms and absorb their life essence (+15 HP).");
    } else if (vt == VOXEL_WALL || vt == VOXEL_ROCK) {
        mine_rock(game, c, f, tx, ty);
    } else if (digging_down && (vt == VOXEL_MUD || vt == VOXEL_SAND || vt == VOXEL_ASH ||
                                vt == VOXEL_GRASS || vt == VOXEL_FLOOR)) {
        dig_ground(c, f, tx, ty);
    } else {
        send_text_to_client(c->sock, "[SYSTEM] There is nothing to dig or mine there.");
        return true;
    }
    send_map_chunk(c->sock, &f->map, c->x, c->y, INITIAL_VIEW_RADIUS);
    return true;
}
