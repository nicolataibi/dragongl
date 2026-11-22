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
#include "ground_journal.h"
#include "strnum.h"
#include "server_internal.h"
#include "player.h"
#include "server_loot.h"
#include "server_combat.h"
#include "spell_cast.h"
#include "server_cmd.h"
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
    ground_journal_taken(game, n); /*before the save of the character*/
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
        send_text_to_client(c->sock, "[SYSTEM] You open the chest and find something...");
        drop_loot_from_monster(game, c, n);
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
    send_text_to_client(c->sock, "[ACTION] You dropped %s on the ground.",
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

/*The direction of the nearest creature that a throw of `range` tiles
 * would hit, among n, s, e and w; false if none.*/
static bool throw_auto_aim(const Client *c, Game *game, int range, int *dx, int *dy) {
    static const int k_dx[4] = {0, 0, 1, -1}, k_dy[4] = {-1, 1, 0, 0};
    int best = range + 1;
    for (int d = 0; d < 4; d++) {
        int lx, ly;
        NPC *hit = throw_flight(c, game, k_dx[d], k_dy[d], range, &lx, &ly);
        int dist = abs(lx - c->x) + abs(ly - c->y);
        if (hit && dist < best) {
            best = dist;
            *dx = k_dx[d];
            *dy = k_dy[d];
        }
    }
    return best <= range;
}

/*throw <n|item> [n|s|e|w]: one unit of the item flies 5 tiles (8 for
 * ammunition), towards the nearest creature in a straight line when no
 * direction is given; it lands where its flight ends and can be picked up
 * again.*/
bool cmd_throw(Client *c, const char *cmd, Game *game) {
    const char *input = strchr(cmd, ' ');
    if (!input) {
        return true;
    }
    input += strspn(input, " ");
    char item_query[64];
    copy_str(item_query, input, sizeof(item_query));
    /*Trailing spaces, then an optional last word that is a direction.*/
    size_t qlen = strlen(item_query);
    while (qlen > 0 && item_query[qlen - 1] == ' ') {
        item_query[--qlen] = '\0';
    }
    int dx = 0, dy = 0;
    bool aimed = false;
    char *last_space = strrchr(item_query, ' ');
    if (last_space && parse_direction4(last_space + 1, &dx, &dy)) {
        aimed = true;
        while (last_space > item_query && last_space[-1] == ' ') {
            last_space--;
        }
        *last_space = '\0';
    }
    bool in_belt = false;
    int found_idx = find_carried(c, item_query, &in_belt);
    if (found_idx == -1) {
        send_text_to_client(c->sock, "[SYSTEM] Item '%s' not found in your inventory.", item_query);
        return true;
    }
    ItemInstance *carried = in_belt ? &c->belt[found_idx] : &c->backpack[found_idx];
    const ItemTemplate *t_item = &item_database[carried->template_idx];
    int range = t_item->category == ITEM_AMMO ? 8 : 5;
    if (!aimed && !throw_auto_aim(c, game, range, &dx, &dy)) {
        send_text_to_client(c->sock,
                            "[SYSTEM] No creature in a straight line within %d tiles: give a "
                            "direction (e.g. throw %s n).",
                            range, item_query);
        return true;
    }
    /*One unit flies; the rest of the stack stays in hand.*/
    ItemInstance thrown = *carried;
    thrown.stack_count = 1;

    int land_x, land_y;
    NPC *target = throw_flight(c, game, dx, dy, range, &land_x, &land_y);
    static const char *const k_names[3][3] = {
        {"", "west", ""}, {"north", "", "south"}, {"", "east", ""}};
    send_text_to_client(c->sock, "[ACTION] You throw %s towards the %s...", t_item->name,
                        k_names[dx + 1][dy + 1]);
    broadcast_spell_vfx(game, c->x, c->y, land_x, land_y, 0, 1.0f, 0.8f, 0.3f, c->floor_id);
    if (target) {
        throw_hit(game, c, target, t_item);
    }
    npc_spawn_item(game, c->floor_id, land_x, land_y, &thrown);
    if (carried->stack_count > 1) {
        carried->stack_count--;
    } else {
        remove_carried(c, found_idx, in_belt);
    }
    send_detailed_state(game, c);
    return true;
}

/*shoot [n|s|e|w]: the ranged weapon in the hands (a bow, a crossbow, a
 * sling, a blowgun) shoots the first creature in that direction within its
 * range and the player's sight, or the nearest one in a straight line
 * without a direction. A creature next to the player is shot point-blank,
 * with disadvantage; no ammunition is spent when nothing is in range.*/
bool cmd_shoot(Client *c, const char *cmd, Game *game) {
    const ItemInstance *weapon =
        (c->slot_hand_r.template_idx != -1) ? &c->slot_hand_r : &c->slot_hand_l;
    const ItemTemplate *w =
        weapon->template_idx != -1 ? &item_database[weapon->template_idx] : NULL;
    if (!w || w->category != ITEM_WEAPON || w->ammo == AMMO_NONE) {
        send_text_to_client(c->sock, "[ERROR] You hold no ranged weapon (a bow, a crossbow, a "
                                     "sling or a blowgun).");
        return true;
    }
    int range = w->range;
    int sight = get_vision_radius(c);
    if (sight < range) {
        range = sight;
    }
    if (range < 1) {
        send_text_to_client(c->sock, "[ERROR] You cannot see where to shoot.");
        return true;
    }
    int dx = 0, dy = 0;
    const char *arg = strchr(cmd, ' ');
    if (arg && arg[strspn(arg, " ")] != '\0') {
        if (!parse_direction4(arg + strspn(arg, " "), &dx, &dy)) {
            send_text_to_client(c->sock, "[SYSTEM] Usage: shoot [n|s|e|w]");
            return true;
        }
    } else if (!throw_auto_aim(c, game, range, &dx, &dy)) {
        send_text_to_client(c->sock,
                            "[SYSTEM] No creature in a straight line within %d tiles: give a "
                            "direction (e.g. shoot n).",
                            range);
        return true;
    }
    int lx, ly;
    NPC *target = throw_flight(c, game, dx, dy, range, &lx, &ly);
    if (!target) {
        send_text_to_client(c->sock, "[SYSTEM] No creature that way within %d tiles.", range);
        return true;
    }
    broadcast_spell_vfx(game, c->x, c->y, target->x, target->y, 0, 1.0f, 0.9f, 0.6f, c->floor_id);
    if (abs(target->x - c->x) <= 1 && abs(target->y - c->y) <= 1) {
        perform_attack(c, target, game); /*point-blank: disadvantage*/
    } else {
        perform_ranged_attack(c, target, game);
    }
    send_detailed_state(game, c);
    return true;
}
