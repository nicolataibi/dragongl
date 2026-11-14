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

#include "ai.h"
#include "combat_log.h"
#include "pathfinding.h"
#include "server_internal.h"
#include "server_combat.h"
#include "server_world.h"
#include <stdlib.h>
#include <string.h>

AINodeStatus ai_swarm_behavior(Game *game, NPC *npc, Client *clients, int num_clients,
                               bool new_round);

// Map archetype string -> enum EntityArchetype
static EntityArchetype archetype_from_string(const char *s) {
    if (!s) {
        return ARCH_MELEE;
    }
    if (strcasecmp(s, "boss") == 0) {
        return ARCH_BOSS;
    }
    if (strcasecmp(s, "caster") == 0) {
        return ARCH_CASTER;
    }
    if (strcasecmp(s, "assassin") == 0) {
        return ARCH_ASSASSIN;
    }
    if (strcasecmp(s, "dragon") == 0) {
        return ARCH_DRAGON;
    }
    if (strcasecmp(s, "brute") == 0) {
        return ARCH_BRUTE;
    }
    if (strcasecmp(s, "swarm") == 0) {
        return ARCH_SWARM;
    }
    return ARCH_MELEE;
}

// Attach the behavior tree to the correct function pointer based on archetype
static void attach_behavior_by_archetype(NPC *npc) {
    switch (npc->archetype) {
    case ARCH_BOSS:
        npc->ai_ctx.behavior = AI_BEHAVIOR_VOID_CRAWLER;
        break;
    case ARCH_CASTER:
        npc->ai_ctx.behavior = AI_BEHAVIOR_MAGE;
        break;
    case ARCH_SWARM:
        npc->ai_ctx.behavior = AI_BEHAVIOR_SWARM;
        break;
    default:
        npc->ai_ctx.behavior = AI_BEHAVIOR_DEFAULT;
        break;
    }
}

void ai_init_npc(NPC *npc, const char *monster_name, int floor_id) {
    memset(&npc->ai_ctx, 0, sizeof(AIContext));
    npc->ai_ctx.current_target_id = -1;

    //--- Base stats: JSON template takes priority, floor-scaled fallback ---
    int hp_base;
    if (npc->template && npc->template->hp_avg > 0) {
        hp_base = npc->template->hp_avg;
        npc->ac = npc->template->ac;
        npc->xp_reward = npc->template->xp;
        npc->gold_drop = npc->template->gold;
        npc->damage_dice = npc->template->damage_dice > 0 ? npc->template->damage_dice : 1;
        npc->damage_sides = npc->template->damage_sides > 0 ? npc->template->damage_sides : 6;
    } else {
        hp_base = 20 + (floor_id * 8);
        npc->ac = 10 + (floor_id / 10);
        npc->xp_reward = 50 + (floor_id * 15);
        npc->gold_drop = 0;
        npc->damage_dice = 1 + (floor_id / 20);
        npc->damage_sides = 6;
    }
    npc->attack_bonus = 2 + (floor_id / 4);

    //--- Archetype: JSON template takes priority ---
    const char *arch_str = NULL;
    if (npc->template && npc->template->archetype) {
        arch_str = npc->template->archetype;
    }

    // If the template has no archetype, fall back to a heuristic based on the name
    if (!arch_str && monster_name) {
        if (strstr(monster_name, "Mage") || strstr(monster_name, "Wizard") ||
            strstr(monster_name, "Sorcerer")) {
            arch_str = "caster";
        } else if (strstr(monster_name, "Assassin") || strstr(monster_name, "Rogue") ||
                   strstr(monster_name, "Shadow")) {
            arch_str = "assassin";
        } else if (strstr(monster_name, "Dragon") || strstr(monster_name, "Wyrm") ||
                   strstr(monster_name, "Drake")) {
            arch_str = "dragon";
        } else if (strstr(monster_name, "Golem") || strstr(monster_name, "Giant") ||
                   strstr(monster_name, "Troll")) {
            arch_str = "brute";
        } else if (strstr(monster_name, "Slime") || strstr(monster_name, "Jelly") ||
                   strstr(monster_name, "Mold") || strstr(monster_name, "Spider")) {
            arch_str = "swarm";
        }
    }

    int existing_arch = (int)npc->archetype;
    npc->archetype = archetype_from_string(arch_str);
    if (existing_arch == ARCH_BOSS) {
        npc->archetype = ARCH_BOSS;
    }

    // --- Modify stats based on archetype ---
    switch (npc->archetype) {
    case ARCH_BOSS:
        hp_base = (hp_base * 5) + (floor_id * 50);
        npc->ac += 2;
        npc->attack_bonus += 3;
        npc->damage_sides = 8;
        npc->xp_reward = (npc->xp_reward * 10) + (floor_id * 500);
        npc->gold_drop = (npc->gold_drop * 10) + (floor_id * 100);
        break;
    case ARCH_CASTER:
        hp_base = (int)((float)hp_base * 0.7f);
        npc->ac -= 1;
        npc->spell_slots[1] = 2 + (floor_id / 10);
        break;
    case ARCH_ASSASSIN:
        hp_base = (int)((float)hp_base * 0.8f);
        npc->ac += 2;
        npc->attack_bonus += 2;
        npc->damage_sides = 8;
        break;
    case ARCH_DRAGON:
        hp_base = (int)((float)hp_base * 2.0f);
        npc->ac += 3;
        npc->attack_bonus += 2;
        npc->damage_dice += 1;
        npc->damage_sides = 10;
        break;
    case ARCH_BRUTE:
        hp_base = (int)((float)hp_base * 1.5f);
        npc->ac -= 1;
        npc->damage_dice += 1;
        npc->damage_sides = 8;
        break;
    case ARCH_SWARM:
        hp_base = (int)((float)hp_base * 0.6f);
        npc->ac -= 2;
        npc->damage_sides = 4;
        break;
    default:
        break;
    }

    npc->hp = hp_base;
    npc->max_hp = hp_base;

    // Attach behavior tree
    attach_behavior_by_archetype(npc);
}

// Reattaches the behavior tree after loading from disk (does not touch stats).
void ai_attach_behavior(NPC *npc) {
    if (!npc || npc->archetype == ARCH_MERCHANT) {
        return;
    }
    attach_behavior_by_archetype(npc);
}

#include "map.h"
#include "player.h"

static int get_light_level(Game *game, Client *target) {
    if (!game->world || target->floor_id < 0 || target->floor_id >= MAX_FLOORS) {
        return 0;
    }
    Map *map = &game->world->floors[target->floor_id].map;
    int tx = target->x;
    int ty = target->y;
    int light = 0;

    for (int y = ty - 2; y <= ty + 2; y++) {
        for (int x = tx - 2; x <= tx + 2; x++) {
            if (x >= 0 && x < MAP_WIDTH && y >= 0 && y < MAP_HEIGHT) {
                VoxelType v = map->data[0][y][x];
                if (v == VOXEL_LAVA) {
                    light += 4;
                } else if (v == VOXEL_MUSHROOM_GLOW || v == VOXEL_CRYSTAL_BLUE ||
                           v == VOXEL_CRYSTAL_PURPLE) {
                    light += 2;
                }
            }
        }
    }
    return light;
}

static Client *find_closest_target(Game *game, NPC *npc, Client *clients, int num_clients) {
    Client *best_target = NULL;
    int min_d = 9999;
    int base_sight =
        (npc->template && npc->template->sight_range > 0) ? npc->template->sight_range : 5;

    for (int i = 0; i < num_clients; i++) {
        if (clients[i].active && clients[i].authenticated && clients[i].floor_id == npc->floor_id) {
            int d = abs(clients[i].x - npc->x) + abs(clients[i].y - npc->y);
            int sight_range = base_sight + get_light_level(game, &clients[i]);
            if (d <= sight_range && d < min_d) {
                min_d = d;
                best_target = &clients[i];
            }
        }
    }
    return best_target;
}

/*Keeps entity_grid consistent with an NPC move (same convention as player
 * movement in server_move.c: clear the old cell if we own it, set the new
 * one). The grid is fully rebuilt every round tick by sync_entity_grid();
 * these incremental writes keep it correct for collision checks made
 * WITHIN the same tick, by other NPCs.*/
static void npc_set_cell(Game *game, NPC *npc, int x, int y) {
    if (!game->world) {
        return;
    }
    if (npc->floor_id < 0 || npc->floor_id >= MAX_FLOORS) {
        return;
    }
    if (npc->x >= 0 && npc->x < MAP_WIDTH && npc->y >= 0 && npc->y < MAP_HEIGHT) {
        Floor *fl = &game->world->floors[npc->floor_id];
        if (fl->entity_grid[npc->y][npc->x] == npc->entity_id) {
            fl->entity_grid[npc->y][npc->x] = 0;
        }
    }
    npc->x = x;
    npc->y = y;
    if (x >= 0 && x < MAP_WIDTH && y >= 0 && y < MAP_HEIGHT) {
        game->world->floors[npc->floor_id].entity_grid[y][x] = npc->entity_id;
    }
}

/*Returns true if the cell (x,y) on the NPC's floor is out of bounds or
 * already occupied by a player or by another active NPC, so the AI never
 * stacks two entities on one cell (collisions and melee rely on the
 * entity_grid). O(1) through the entity_grid: this runs for every step
 * attempt of every monster.*/
static bool cell_occupied(Game *game, int x, int y, NPC *npc, Client *clients, int num_clients) {
    (void)clients;
    (void)num_clients;
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) {
        return true;
    }
    if (!game->world || npc->floor_id < 0 || npc->floor_id >= MAX_FLOORS) {
        return true;
    }
    int id = game->world->floors[npc->floor_id].entity_grid[y][x];
    return id != 0 && id != npc->entity_id;
}

static void default_ai_update(Game *game, NPC *npc, Client *clients, int num_clients,
                              bool new_round) {
    Client *target = find_closest_target(game, npc, clients, num_clients);
    if (!target) {
        return;
    }

    int d = abs(target->x - npc->x) + abs(target->y - npc->y);
    if (d <= 1) {
        if (new_round) {
            perform_attack_npc(npc, target, game);
        }
        return;
    }

    /*A* movement — recalculate every 3 ticks or when the target has moved*/
    npc->ai_ctx.state_timer++;
    if (npc->ai_ctx.state_timer < 3) {
        return; /* wait for next tick */
    }
    npc->ai_ctx.state_timer = 0;

    if (!game->world) {
        return;
    }
    Map *map = &game->world->floors[npc->floor_id].map;

    PathNode path[MAX_ASTAR_PATH];
    int steps = pathfind_astar(map, npc->x, npc->y, target->x, target->y, path, MAX_ASTAR_PATH);

    if (steps >= 2) {
        /* path[0] = current position, path[1] = first step.
         * A* only sees the map: if the first step is occupied by a
         * player or another NPC, wait for the next tick. */
        if (!cell_occupied(game, path[1].x, path[1].y, npc, clients, num_clients)) {
            npc_set_cell(game, npc, path[1].x, path[1].y);
        }
    } else if (steps == 0) {
        /* A* found no path: direct fallback (e.g. target is adjacent).
         * One axis per step (4-way) and never onto an occupied cell. */
        int nx = npc->x, ny = npc->y;
        if (npc->x < target->x) {
            nx++;
        } else if (npc->x > target->x) {
            nx--;
        } else if (npc->y < target->y) {
            ny++;
        } else if (npc->y > target->y) {
            ny--;
        }
        if (!cell_occupied(game, nx, ny, npc, clients, num_clients)) {
            npc_set_cell(game, npc, nx, ny);
        }
    }
}

/*Living NPCs on a floor over which its swarms stop splitting.*/
#define SWARM_SPLIT_FLOOR_CAP 500

AINodeStatus ai_swarm_behavior(Game *game, NPC *npc, Client *clients, int num_clients,
                               bool new_round) {
    if (new_round && npc->hp > 1) {
        /*Adjacent swarms, through the entity grid: 8 lookups instead of a
         * walk of the whole floor for every swarm.*/
        int counter = 0;
        const Floor *fl = &game->world->floors[npc->floor_id];
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int ax = npc->x + dx, ay = npc->y + dy;
                if ((dx == 0 && dy == 0) || ax < 0 || ax >= MAP_WIDTH || ay < 0 ||
                    ay >= MAP_HEIGHT || fl->entity_grid[ay][ax] == 0) {
                    continue;
                }
                const NPC *near = npc_by_entity_id(game, fl->entity_grid[ay][ax]);
                counter += near && near->active && near->archetype == ARCH_SWARM;
            }
        }

        if (counter == 0) {
            counter = 1; // to prevent div by 0 and allow growth
        }

        if (counter < 4 && (rand() % (counter * 7)) == 0) {
            // Try to split
            int try_dx = (rand() % 3) - 1;
            int try_dy = (rand() % 3) - 1;
            if (try_dx != 0 || try_dy != 0) {
                int nx = npc->x + try_dx;
                int ny = npc->y + try_dy;

                // Check if map tile is empty
                Map *m = &game->world->floors[npc->floor_id].map;
                if (nx > 0 && nx < MAP_WIDTH - 1 && ny > 0 && ny < MAP_HEIGHT - 1 &&
                    m->data[0][ny][nx] == VOXEL_FLOOR) {
                    // Check if occupied by another npc or a player
                    bool occupied = cell_occupied(game, nx, ny, npc, clients, num_clients);
                    if (!occupied) {
                        /*A floor already crowded gets no more swarms: they
                         * would end up owning the whole NPC table.*/
                        int total = 0, alive = 0;
                        floor_stats_get(game, npc->floor_id, &total, &alive);
                        NPC *clone = alive < SWARM_SPLIT_FLOOR_CAP ? npc_alloc(game) : NULL;
                        if (clone) {
                            // Split!
                            npc->hp = (npc->hp / 2) > 0 ? (npc->hp / 2) : 1;

                            *clone = *npc; //copy all stats
                            /*The copy also copied active=true: undo it so
                             * the activation below goes through
                             * npc_set_active (floor counters and index).*/
                            clone->active = false;
                            /*A truly unique ID: the slot index could
                             * collide with an already-assigned entity_id
                             * (NPC or player) and corrupt the
                             * entity_grid collision lookups.*/
                            clone->entity_id = game->next_id++;
                            /*Register the clone in the grid (the parent's
                             * cell keeps the parent's ID).*/
                            npc_set_cell(game, clone, nx, ny);
                            clone->hp = npc->hp;
                            /*The clone respawns where it split, not at the
                             * parent's original spawn: with the inherited
                             * spawn_x/y both halves reappeared at the same
                             * spot after dying in different places.*/
                            clone->spawn_x = nx;
                            clone->spawn_y = ny;
                            /*All fields are final: activate the clone
                             * through npc_set_active so the floor caches
                             * count it.*/
                            npc_set_active(game, clone, true);

                            broadcast_spell_vfx(game, npc->x, npc->y, nx, ny, 1, 0.0f, 1.0f, 0.0f,
                                                npc->floor_id); // Green explosion
                            return AI_SUCCESS;
                        }
                    }
                }
            }
        }
    }

    //Default melee behavior
    default_ai_update(game, npc, clients, num_clients, new_round);
    return AI_SUCCESS;
}

void ai_update_npc(Game *game, NPC *npc, Client *clients, int num_clients, bool new_round) {
    /*Merchants have no AI; TREASURE and GOLD have NO AI at all: they are
     * pickable loot with template==NULL. Running them through the default
     * behavior made them walk to the player and call
     * perform_attack_npc -> n->template->name -> NULL deref (server crash).
     * Summoned elementals keep the melee AI (they DO fight).*/
    if (!npc || !npc->active) {
        return;
    }
    if (npc->archetype == ARCH_MERCHANT || npc->archetype == ARCH_TREASURE ||
        npc->archetype == ARCH_GOLD) {
        return;
    }

    switch (npc->ai_ctx.behavior) {
    case AI_BEHAVIOR_VOID_CRAWLER:
        (void)ai_void_crawler_behavior(game, npc, clients, num_clients, new_round);
        break;
    case AI_BEHAVIOR_MAGE:
        (void)ai_mage_behavior(game, npc, clients, num_clients, new_round);
        break;
    case AI_BEHAVIOR_SWARM:
        (void)ai_swarm_behavior(game, npc, clients, num_clients, new_round);
        break;
    default:
        default_ai_update(game, npc, clients, num_clients, new_round);
        break;
    }
}

AINodeStatus ai_mage_behavior(Game *game, NPC *npc, Client *clients, int num_clients,
                              bool new_round) {
    Client *target = find_closest_target(game, npc, clients, num_clients);
    if (!target) {
        return AI_FAILURE;
    }

    int d = abs(target->x - npc->x) + abs(target->y - npc->y);

    if (new_round) {
        // Try to cast a spell if spell slots are available
        if (npc->spell_slots[1] > 0) {
            send_text_to_client(target->sock, "[DANGER] The Mage casts 'Magic Missile' at you!");
            /*Magic Missile: 3 homing missiles, 1d4+1 force damage each,
             * always hits (no attack roll, no AC).*/
            int dmg = 0;
            for (int m = 0; m < 3; m++) {
                dmg += rules_roll_dice(1, 4) + 1;
            }
            target->hp -= dmg;
            send_text_to_client(target->sock,
                                "[COMBAT] The missiles strike you for %d force damage! (HP: %d/%d)",
                                dmg, target->hp > 0 ? target->hp : 0, target->max_hp);
            if (target->hp <= 0) {
                target->hp = 0;
                clog_death(target->username, npc_name(npc), npc->floor_id);
                save_bones(game, target);
                player_respawn_town(game, target);
                send_text_to_client(
                    target->sock,
                    "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
            }
            npc->spell_slots[1]--;
            return AI_SUCCESS;
        }

        //No spells left: attack in melee or back away
        if (d <= 1) {
            perform_attack_npc(npc, target, game);
            return AI_SUCCESS;
        }
    }

    // Movement (maintains distance): one axis per step, and never
    // onto a cell occupied by a player or another NPC.
    if (d > 3 && rand() % 2 == 0) {
        int nx = npc->x, ny = npc->y;
        if (npc->x < target->x) {
            nx++;
        } else if (npc->x > target->x) {
            nx--;
        } else if (npc->y < target->y) {
            ny++;
        } else if (npc->y > target->y) {
            ny--;
        }
        if (!cell_occupied(game, nx, ny, npc, clients, num_clients)) {
            npc_set_cell(game, npc, nx, ny);
        }
    }

    return AI_RUNNING;
}

AINodeStatus ai_void_crawler_behavior(Game *game, NPC *npc, Client *clients, int num_clients,
                                      bool new_round) {
    Client *target = find_closest_target(game, npc, clients, num_clients);
    if (!target) {
        return AI_FAILURE;
    }

    int d = abs(target->x - npc->x) + abs(target->y - npc->y);

    if (new_round) {
        // Phase 1: Enslave
        if (d > 1 && d <= 5) {
            if (rand() % 4 == 0) {
                int save_mod = rules_get_modifier(target->wis);
                bool saved = rules_roll_save(save_mod, 14, false, false, NULL);
                if (!saved) {
                    send_text_to_client(target->sock,
                                        "[DANGER] The Boss enters your mind! You are Charmed!");
                    ActiveEffect charm_effect = {"Enslave", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 3,
                                                 false};
                    if (target->effect_count < MAX_EFFECTS_PER_ENTITY) {
                        target->effects[target->effect_count++] = charm_effect;
                    }
                    return AI_SUCCESS;
                }
                send_text_to_client(target->sock,
                                    "[DANGER] The Boss tries to control you, but you resist!");
            }
        }

        // Phase 2: Mucous Cloud / Melee
        if (d <= 1) {
            if (rand() % 3 == 0) {
                perform_attack_npc(npc, target, game);
            } else {
                /*Mucus only: this option replaces the melee attack.*/
                int save_mod = rules_get_modifier(target->con);
                bool saved = rules_roll_save(save_mod, 14, false, false, NULL);
                if (!saved) {
                    send_text_to_client(target->sock,
                                        "[DANGER] You are engulfed by Mucus! You are Poisoned!");
                    ActiveEffect poison_effect = {
                        "Mucous Cloud", EVENT_ON_TURN_START, MOD_ADDITIVE, -2, 3, false};
                    if (target->effect_count < MAX_EFFECTS_PER_ENTITY) {
                        target->effects[target->effect_count++] = poison_effect;
                    }
                } else {
                    send_text_to_client(target->sock, "[DANGER] You dodge the Mucous Cloud!");
                }
            }
            return AI_SUCCESS;
        }
    }

    // Phase 3: Approach (continuous movement between rounds):
    // one axis per step, and never onto an occupied cell.
    if (rand() % 20 == 0) {
        int nx = npc->x, ny = npc->y;
        if (npc->x < target->x) {
            nx++;
        } else if (npc->x > target->x) {
            nx--;
        } else if (npc->y < target->y) {
            ny++;
        } else if (npc->y > target->y) {
            ny--;
        }
        if (!cell_occupied(game, nx, ny, npc, clients, num_clients)) {
            npc_set_cell(game, npc, nx, ny);
        }
    }

    return AI_RUNNING;
}
