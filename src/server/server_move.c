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
 * server_move.c - One step of a player (MSG_MOVE).
 *
 * The step first meets whatever stands on the next tile: a merchant
 * (greeting), loot (picked up), a monster (attacked) or another player.
 * When nothing blocks it the player walks onto the tile, which can trigger
 * bleeding, tile events and traps.
 */

#include "server_move.h"
#include "server_internal.h"
#include "server_combat.h"
#include "server_world.h"
#include "rules.h"
#include <math.h>
#include <stdlib.h>

/*The shop ring of floor 0, with the formula of map.c and update_city_doors():
 * 11 shops on a circle of radius 26, door 4 tiles from the centre of the
 * building towards the centre of the city.*/
static bool is_inside_shop(int x, int y) {
    int cx = MAP_CENTER_X;
    int cy = MAP_CENTER_Y;
    for (int i = 0; i < 11; i++) {
        float angle = city_shop_angle(i);
        int sx = cx + (int)(cosf(angle) * 26.0f);
        int sy = cy + (int)(sinf(angle) * 26.0f);
        if (x >= sx - 3 && x <= sx + 3 && y >= sy - 3 && y <= sy + 3) {
            return true;
        }
    }
    return false;
}

static bool is_shop_door(int x, int y) {
    int cx = MAP_CENTER_X;
    int cy = MAP_CENTER_Y;
    for (int i = 0; i < 11; i++) {
        float angle = city_shop_angle(i);
        int sx = cx + (int)(cosf(angle) * 26.0f);
        int sy = cy + (int)(sinf(angle) * 26.0f);
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
        if (x == tx && y == ty) {
            return true;
        }
    }
    return false;
}

static bool cannot_move(Client *c) {
    return rules_has_condition_t(c->effects, c->effect_count, COND_PARALYZED) ||
           rules_has_condition_t(c->effects, c->effect_count, COND_STUNNED) ||
           rules_has_condition_t(c->effects, c->effect_count, COND_PETRIFIED) ||
           rules_has_condition_t(c->effects, c->effect_count, COND_FROZEN) ||
           rules_has_condition_t(c->effects, c->effect_count, COND_UNCONSCIOUS);
}

/*The loot NPC is gone from the tile: the player can step there.*/
static void loot_taken(NPC *n, Floor *fl, int nx, int ny, int respawn_timer) {
    npc_set_active(n, false);
    n->respawn_timer = respawn_timer;
    fl->entity_grid[ny][nx] = 0;
}

/*A gold pile, the ghost of a dead player, a dropped item or a chest.
 * Returns true when something stays on the tile and blocks the step.*/
static bool pick_up_loot(Client *c, NPC *n, Floor *fl, int nx, int ny) {
    if (n->archetype == ARCH_GOLD && !n->is_ghost) {
        c->gold += n->gold_drop;
        send_text_to_client(c->sock, "[SYSTEM] You pick up a stack of %d gold coins!",
                            n->gold_drop);
        send_combat_event(c->sock, COMBAT_EV_GOLD, c->entity_id, n->gold_drop);
        loot_taken(n, fl, nx, ny, 0);
        save_player_data(c);
        return false;
    }
    if (n->is_ghost) {
        /*Gold and every item of the ghost; what does not fit stays.*/
        bool any = false;
        bool full = false;
        if (n->gold_drop > 0) {
            c->gold += n->gold_drop;
            n->gold_drop = 0;
            any = true;
        }
        for (int gi = 0; gi < 30; gi++) {
            if (n->ghost_loot[gi].template_idx >= 0 && n->ghost_loot[gi].stack_count > 0) {
                if (c->backpack_count >= MAX_BACKPACK) {
                    full = true;
                    break;
                }
                c->backpack[c->backpack_count++] = n->ghost_loot[gi];
                n->ghost_loot[gi].template_idx = -1;
                n->ghost_loot[gi].stack_count = 0;
                any = true;
            }
        }
        bool blocked = full;
        if (full) {
            send_text_to_client(
                c->sock, "[SYSTEM] Backpack full! You have only recovered part of the loot.");
        } else {
            if (any) {
                send_text_to_client(c->sock, "[SYSTEM] Recover everything you were carrying!");
            }
            loot_taken(n, fl, nx, ny, 0);
        }
        save_player_data(c);
        return blocked;
    }
    if (n->ghost_loot[0].stack_count > 0) {
        /*A single dropped item.*/
        if (c->backpack_count >= MAX_BACKPACK) {
            send_text_to_client(c->sock, "[SYSTEM] Backpack full! You cannot pick up the item.");
            return true;
        }
        c->backpack[c->backpack_count++] = n->ghost_loot[0];
        send_text_to_client(c->sock, "[SYSTEM] You picked up: %s",
                            item_database[n->ghost_loot[0].template_idx].name);
        loot_taken(n, fl, nx, ny, 0);
        save_player_data(c);
        return false;
    }
    /*A chest with nothing stored: random loot.*/
    send_text_to_client(c->sock,
                        "[SYSTEM] You open the treasure chest and find something inside...");
    drop_loot_from_monster(c, n);
    loot_taken(n, fl, nx, ny, 100);
    save_player_data(c);
    return false;
}

static bool is_loot(const NPC *n) {
    return (n->archetype == ARCH_BOSS && n->is_ghost) || n->archetype == ARCH_TREASURE ||
           n->archetype == ARCH_GOLD;
}

/*Meets the entity standing on (nx, ny). Returns true if the step is blocked.*/
static bool bump_into(Client *c, Floor *fl, int ent_id, int nx, int ny, Client *clients,
                      NPC *npcs) {
    NPC *n = npc_by_entity_id(npcs, ent_id);
    if (n) {
        if (n->archetype == ARCH_MERCHANT) {
            send_text_to_client(c->sock,
                                "[%s] Welcome! Use 'buy <n>', 'sell <n>', 'haggle "
                                "buy <n>', 'identify' or 'cure'.",
                                n->merchant.shop_name);
            print_merchant_inventory(c, n);
            return true;
        }
        if (is_loot(n)) {
            return pick_up_loot(c, n, fl, nx, ny);
        }
        perform_attack(c, n, npcs);
        return true;
    }
    for (int p = 0; p < MAX_CLIENTS; p++) {
        if (clients[p].active && clients[p].authenticated && clients[p].entity_id == ent_id) {
            send_text_to_client(c->sock, "[SOCIAL] You see %s in front of you.",
                                clients[p].username);
            /*Players never block a staircase.*/
            VoxelType target_vt = fl->map.data[0][ny][nx];
            return !(target_vt == VOXEL_STAIRS_UP || target_vt == VOXEL_STAIRS_DOWN);
        }
    }
    return false;
}

/*Walks onto (nx, ny) when the terrain allows it. Walls of floor 0 open for
 * a player inside a shop who steps through its door.*/
static void step_onto(Client *c, int nx, int ny, int old_x, int old_y, NPC *npcs) {
    Floor *fl = &master_world->floors[c->floor_id];
    VoxelType vt = fl->map.data[0][ny][nx];
    bool can_pass = (vt != VOXEL_WALL && vt != VOXEL_ROCK);
    if (!can_pass && vt == VOXEL_WALL && c->floor_id == 0 && is_shop_door(nx, ny) &&
        is_inside_shop(old_x, old_y)) {
        can_pass = true;
    }
    if (!can_pass) {
        return;
    }

    if (fl->entity_grid[c->y][c->x] == c->entity_id) {
        fl->entity_grid[c->y][c->x] = 0;
    }
    c->x = nx;
    c->y = ny;
    fl->entity_grid[ny][nx] = c->entity_id;
    /*A step does not advance global_total_turns: the game clock runs at a
   * fixed 5 ticks/s in update_world(), whatever the players do.*/

    if (rules_has_condition_t(c->effects, c->effect_count, COND_BLEEDING)) {
        c->hp -= 2;
        send_text_to_client(c->sock, "[DANGER] Moving reopens your wounds! You bleed heavily...");
        if (c->hp <= 0) {
            server_log("DEATH", "%s bled to death during the movement.", c->username);
            save_bones(c);
            player_respawn_town(c);
            send_text_to_client(
                c->sock,
                "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
        }
    }
    update_city_doors();
    check_tile_events(c, npcs);
    check_traps(c, npcs);
}

void player_move(Client *c, const MsgMove *m, Client *clients, NPC *npcs) {
    /*Where the view was: the map chunk is sent again only if it moved.*/
    int old_x = c->x, old_y = c->y;
    int old_floor = c->floor_id;

    long long now_ms = get_time_ms();
    if (now_ms - c->last_action_ms < 200) {
        return; /*at most 5 actions per second*/
    }
    c->last_action_ms = now_ms;

    if (cannot_move(c)) {
        send_text_to_client(c->sock, "[SYSTEM] You can't move in this state!");
        return;
    }

    int dx = (m->dx > 0) - (m->dx < 0); /*-1, 0 or 1*/
    int dy = (m->dy > 0) - (m->dy < 0);
    int nx = c->x + dx, ny = c->y + dy;
    if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
        Floor *fl = &master_world->floors[c->floor_id];
        int ent_id = fl->entity_grid[ny][nx];
        bool blocked = ent_id > 0 && bump_into(c, fl, ent_id, nx, ny, clients, npcs);
        if (!blocked) {
            step_onto(c, nx, ny, old_x, old_y, npcs);
        }
    }

    /*The state goes out even after a blocked step: an attack can end a
   * fight, and the NPC positions would otherwise stay a tick behind.*/
    broadcast_nearby_entities(c, npcs);
    send_detailed_state(c);
    broadcast_player_state(c);
    /*Tile events and traps can change the floor (stairs, teleports).*/
    if (c->x != old_x || c->y != old_y || c->floor_id != old_floor) {
        send_map_chunk(c->sock, &master_world->floors[c->floor_id].map, c->x, c->y,
                       INITIAL_VIEW_RADIUS);
    }
}
