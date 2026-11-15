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
 * cmd_ground.c - Items on the ground: pickup, get, drop, throw.
 */

#include "inventory.h"
#include "strnum.h"
#include "server_internal.h"
#include "player.h"
#include "server_loot.h"
#include "server_combat.h"
#include "spell_cast.h"
#include <ctype.h>

// --- RECOVERY FROM TOMBSTONE ---
bool cmd_pickup(Client *c, const char *cmd, Game *game) {
    (void)cmd;
    (void)game;
    tombstone_pickup(game, c);
    save_player_data(c);
    return true;
}

/*The step of a compass direction (n, ne, e, ... nw), false if word is
 * not one.*/
static bool direction_step(const char *word, int *dx, int *dy) {
    static const struct {
        const char *name;
        int dx, dy;
    } dirs[] = {{"n", 0, -1}, {"ne", 1, -1}, {"e", 1, 0},  {"se", 1, 1},
                {"s", 0, 1},  {"sw", -1, 1}, {"w", -1, 0}, {"nw", -1, -1}};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (strcasecmp(word, dirs[i].name) == 0) {
            *dx = dirs[i].dx;
            *dy = dirs[i].dy;
            return true;
        }
    }
    return false;
}

/*Takes one thing from the ground: gold into the purse, an item into the
 * backpack, or the loot of a closed chest onto the ground.*/
static void collect(Game *game, Client *c, NPC *n) {
    if (n->archetype == ARCH_GOLD) {
        c->gold += (uint64_t)(n->gold_drop > 0 ? n->gold_drop : 0);
        send_text_to_client(c->sock, "[SYSTEM] You have collected %d gold coins!", n->gold_drop);
        send_combat_event(c->sock, COMBAT_EV_GOLD, c->entity_id, n->gold_drop);
        npc_set_active(game, n, false);
    } else if (npc_loot(&game->pools, n) && npc_loot(&game->pools, n)[0].stack_count > 0) {
        const ItemInstance *item = &npc_loot(&game->pools, n)[0];
        if (c->backpack_count >= MAX_BACKPACK) {
            send_text_to_client(c->sock, "[SYSTEM] Backpack full! Unable to collect.");
            return;
        }
        c->backpack[c->backpack_count++] = *item;
        send_text_to_client(c->sock, "[SYSTEM] You have collected: %s",
                            item_database[item->template_idx].name);
        npc_set_active(game, n, false);
        n->respawn_timer = 0;
    } else {
        send_text_to_client(c->sock, "[SYSTEM] Open the chest and find something...");
        drop_loot_from_monster(c, n);
        npc_set_active(game, n, false);
        n->respawn_timer = 100;
    }
    /*The floor counters follow npc_set_active() at each removal.*/
    save_player_data(c);
}

/*get [direction] [n]: what lies on the tile (or the adjacent one in that
 * direction); with several things there, n chooses one of the list.*/
bool cmd_get(Client *c, const char *cmd, Game *game) {
    int target_x = c->x;
    int target_y = c->y;
    int target_idx = 0;
    bool idx_given = false;
    char dir[16] = {0};
    char idx_str[16] = {0};
    const char *args = strchr(cmd, ' ');
    if (args && sscanf(args, "%15s %15s", dir, idx_str) >= 1) {
        int dx = 0, dy = 0;
        if (isdigit((unsigned char)dir[0])) {
            target_idx = str_to_int(dir);
            idx_given = true;
            dir[0] = '\0';
        } else if (direction_step(dir, &dx, &dy)) {
            target_x += dx;
            target_y += dy;
            if (isdigit((unsigned char)idx_str[0])) {
                target_idx = str_to_int(idx_str);
                idx_given = true;
            }
        } else {
            send_text_to_client(c->sock, "[SYSTEM] Usage: get [n|ne|e|se|s|sw|w|nw] [n]");
            return true;
        }
    }

    /*Gold and chests (an item on the ground is a chest that holds it).*/
    int items[64];
    int count = 0;
    FloorIter walk = floor_iter(game, c->floor_id);
    for (int i; (i = floor_iter_next(&walk)) >= 0 && count < 64;) {
        const NPC *n = &game->npcs[i];
        if (n->x == target_x && n->y == target_y &&
            (n->archetype == ARCH_TREASURE || n->archetype == ARCH_GOLD)) {
            items[count++] = i;
        }
    }
    if (count == 0) {
        send_text_to_client(c->sock, "[SYSTEM] There is nothing to collect here.");
        return true;
    }
    if (idx_given && (target_idx < 1 || target_idx > count)) {
        send_text_to_client(c->sock, "[SYSTEM] There is no number %d here: 1 to %d.", target_idx,
                            count);
        return true;
    }
    if (count == 1 || idx_given) {
        collect(game, c, &game->npcs[items[idx_given ? target_idx - 1 : 0]]);
        return true;
    }

    /*Several things and no choice: the list.*/
    send_text_to_client(c->sock, "[SYSTEM] There are more objects:");
    for (int i = 0; i < count; i++) {
        const NPC *n = &game->npcs[items[i]];
        if (n->archetype == ARCH_GOLD) {
            send_text_to_client(c->sock, "  %d. %d gold coins", i + 1, n->gold_drop);
        } else if (npc_loot(&game->pools, n) && npc_loot(&game->pools, n)[0].stack_count > 0) {
            send_text_to_client(c->sock, "  %d. %s", i + 1,
                                item_database[npc_loot(&game->pools, n)[0].template_idx].name);
        } else {
            send_text_to_client(c->sock, "  %d. Treasure chest", i + 1);
        }
    }
    if (dir[0]) {
        send_text_to_client(c->sock, "What do you want to collect? Use 'get %s <n>'.", dir);
    } else {
        send_text_to_client(c->sock, "What do you want to collect? Use 'get <n>'.");
    }
    return true;
}

/*drop <name|n>: the item lies on the player's tile until someone takes it.*/
bool cmd_drop(Client *c, const char *cmd, Game *game) {
    const char *input = strchr(cmd, ' ');
    if (!input) {
        return true;
    }
    input++;
    bool in_belt = false;
    int found_idx = find_carried(c, input, &in_belt);
    if (found_idx == -1) {
        send_text_to_client(c->sock, "[SYSTEM] Object '%s' not found.", input);
        return true;
    }
    const ItemInstance *item = in_belt ? &c->belt[found_idx] : &c->backpack[found_idx];
    if (!npc_spawn_item(game, c->floor_id, c->x, c->y, item)) {
        send_text_to_client(c->sock, "[SYSTEM] Unable to drop item: too many items on the ground.");
        return true;
    }
    send_text_to_client(c->sock, "[ACTION] You dropped: %s on the ground",
                        item_database[item->template_idx].name);
    remove_carried(c, found_idx, in_belt);
    return true;
}

/*The flight of a thrown item: up to `range` tiles in a straight line,
 * stopped by rock, walls and doors and by the first hostile creature.
 * (*lx, *ly) is the tile where it lands; returns the creature hit or NULL.*/
static NPC *throw_flight(const Client *c, Game *game, int dx, int dy, int range, int *lx, int *ly) {
    *lx = c->x;
    *ly = c->y;
    for (int step = 1; step <= range; step++) {
        int nx = *lx + dx;
        int ny = *ly + dy;
        if (nx < 0 || nx >= MAP_WIDTH || ny < 0 || ny >= MAP_HEIGHT) {
            break;
        }
        VoxelType voxel = game->world->floors[c->floor_id].map.data[0][ny][nx];
        if (voxel == VOXEL_ROCK || voxel == VOXEL_WALL || voxel == VOXEL_DOOR) {
            break;
        }
        *lx = nx;
        *ly = ny;
        FloorIter walk = floor_iter(game, c->floor_id);
        for (int i; (i = floor_iter_next(&walk)) >= 0;) {
            if (game->npcs[i].x == nx && game->npcs[i].y == ny &&
                spell_can_affect_npc(&game->npcs[i])) {
                return &game->npcs[i];
            }
        }
    }
    return NULL;
}

/*d20 + DEX + the item's attack bonus against AC; the item's dice (1d4 if
 * it has none) on a hit.*/
static void throw_hit(Game *game, Client *c, NPC *target, const ItemTemplate *item) {
    int to_hit = (rand() % 20) + 1 + rules_get_modifier(c->dex) + item->attack_bonus;
    if (to_hit < target->ac) {
        send_text_to_client(c->sock, "[COMBAT] Missed! %s dodges the throw.", npc_name(target));
        return;
    }
    int dice = item->damage_dice_count > 0 ? item->damage_dice_count : 1;
    int sides = item->damage_dice_sides > 0 ? item->damage_dice_sides : 4;
    int dmg = rules_roll_dice(dice, sides);
    target->hp -= dmg;
    send_text_to_client(c->sock, "[COMBAT] Hit %s for %d damage! (HP: %d/%d)", npc_name(target),
                        dmg, target->hp, target->max_hp);
    if (target->hp <= 0) {
        npc_killed(game, c, target, item->name);
    }
}

/*throw <name|n> <n|s|e|w>: 5 tiles (8 for ammunition); the item lands
 * where its flight ends and can be picked up again.*/
bool cmd_throw(Client *c, const char *cmd, Game *game) {
    const char *input = strchr(cmd, ' ');
    if (!input) {
        return true;
    }
    input++;
    const char *last_space = strrchr(input, ' ');
    if (!last_space) {
        send_text_to_client(c->sock, "[SYSTEM] Specifies the direction. Example: throw dart n");
        return true;
    }
    char dir_char = last_space[1];
    char item_query[64];
    size_t len = (size_t)(last_space - input);
    if (len > sizeof(item_query) - 1) {
        len = sizeof(item_query) - 1;
    }
    memcpy(item_query, input, len);
    item_query[len] = '\0';

    int dx = (dir_char == 'e') - (dir_char == 'w');
    int dy = (dir_char == 's') - (dir_char == 'n');
    if (dx == 0 && dy == 0) {
        send_text_to_client(c->sock,
                            "[SYSTEM] Invalid direction. Use: n, s, e, w (e.g. throw dart e)");
        return true;
    }
    bool in_belt = false;
    int found_idx = find_carried(c, item_query, &in_belt);
    if (found_idx == -1) {
        send_text_to_client(c->sock, "[SYSTEM] Item '%s' not found in your inventory.", item_query);
        return true;
    }
    ItemInstance thrown = in_belt ? c->belt[found_idx] : c->backpack[found_idx];
    const ItemTemplate *t_item = &item_database[thrown.template_idx];

    int land_x, land_y;
    NPC *target =
        throw_flight(c, game, dx, dy, t_item->category == ITEM_AMMO ? 8 : 5, &land_x, &land_y);
    send_text_to_client(c->sock, "[ACTION] You throw %s towards %c...", t_item->name, dir_char);
    broadcast_spell_vfx(game, c->x, c->y, land_x, land_y, 0, 1.0f, 0.8f, 0.3f, c->floor_id);
    if (target) {
        throw_hit(game, c, target, t_item);
    }
    npc_spawn_item(game, c->floor_id, land_x, land_y, &thrown);
    remove_carried(c, found_idx, in_belt);
    return true;
}
