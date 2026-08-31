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
#include "pathfinding.h"
#include <stdlib.h>
#include <string.h>

// Map archetype string -> enum EntityArchetype
static EntityArchetype archetype_from_string(const char* s) {
    if (!s)                                return ARCH_MELEE;
    if (strcasecmp(s, "boss")    == 0)     return ARCH_BOSS;
    if (strcasecmp(s, "caster")  == 0)     return ARCH_CASTER;
    if (strcasecmp(s, "assassin")== 0)     return ARCH_ASSASSIN;
    if (strcasecmp(s, "dragon")  == 0)     return ARCH_DRAGON;
    if (strcasecmp(s, "brute")   == 0)     return ARCH_BRUTE;
    if (strcasecmp(s, "swarm")   == 0)     return ARCH_SWARM;
    return ARCH_MELEE;
}

// Attach the behavior tree to the correct function pointer based on archetype
static void attach_behavior_by_archetype(NPC* npc) {
    extern AINodeStatus ai_swarm_behavior(NPC*, Client*, int, bool, NPC*);
    switch (npc->archetype) {
        case ARCH_BOSS:     npc->ai_ctx.behavior_tree_root = (void*)ai_void_crawler_behavior; break;
        case ARCH_CASTER:   npc->ai_ctx.behavior_tree_root = (void*)ai_mage_behavior;         break;
        case ARCH_SWARM:    npc->ai_ctx.behavior_tree_root = (void*)ai_swarm_behavior;        break;
        default:            npc->ai_ctx.behavior_tree_root = NULL;                            break;
    }
}

void ai_init_npc(NPC* npc, const char* monster_name, int floor_id) {
    memset(&npc->ai_ctx, 0, sizeof(AIContext));
    npc->ai_ctx.current_target_id = -1;

    //--- Base stats: JSON template takes priority, floor-scaled fallback ---
    int hp_base;
    if (npc->template && npc->template->hp_avg > 0) {
        hp_base            = npc->template->hp_avg;
        npc->ac            = npc->template->ac;
        npc->xp_reward     = npc->template->xp;
        npc->gold_drop     = npc->template->gold;
        npc->damage_dice   = npc->template->damage_dice  > 0 ? npc->template->damage_dice  : 1;
        npc->damage_sides  = npc->template->damage_sides > 0 ? npc->template->damage_sides : 6;
    } else {
        hp_base            = 20 + (floor_id * 8);
        npc->ac            = 10 + (floor_id / 10);
        npc->xp_reward     = 50 + (floor_id * 15);
        npc->gold_drop     = 0;
        npc->damage_dice   = 1  + (floor_id / 20);
        npc->damage_sides  = 6;
    }
    npc->attack_bonus      = 2  + (floor_id / 4);

    //--- Archetype: JSON template takes priority ---
    const char* arch_str = NULL;
    if (npc->template && npc->template->archetype) {
        arch_str = npc->template->archetype;
    }

    // If the template has no archetype, fall back to a heuristic based on the name
    if (!arch_str && monster_name) {
        if      (strstr(monster_name,"Mage")||strstr(monster_name,"Wizard")||strstr(monster_name,"Sorcerer"))     arch_str = "caster";
        else if (strstr(monster_name,"Assassin")||strstr(monster_name,"Rogue")||strstr(monster_name,"Shadow"))    arch_str = "assassin";
        else if (strstr(monster_name,"Dragon")||strstr(monster_name,"Wyrm")||strstr(monster_name,"Drake"))        arch_str = "dragon";
        else if (strstr(monster_name,"Golem")||strstr(monster_name,"Giant")||strstr(monster_name,"Troll"))        arch_str = "brute";
        else if (strstr(monster_name,"Slime")||strstr(monster_name,"Jelly")||strstr(monster_name,"Mold")||strstr(monster_name,"Spider")) arch_str = "swarm";
    }


    int existing_arch = npc->archetype;
    npc->archetype = archetype_from_string(arch_str);
    if (existing_arch == ARCH_BOSS) {
        npc->archetype = ARCH_BOSS;
    }

    // --- Modify stats based on archetype ---
    switch (npc->archetype) {
        case ARCH_BOSS:
            hp_base          = (hp_base * 5) + (floor_id * 50);
            npc->ac          += 2;
            npc->attack_bonus+= 3;
            npc->damage_sides = 8;
            npc->xp_reward   = (npc->xp_reward * 10) + (floor_id * 500);
            npc->gold_drop   = (npc->gold_drop * 10) + (floor_id * 100);
            break;
        case ARCH_CASTER:
            hp_base           = (int)(hp_base * 0.7f);
            npc->ac          -= 1;
            npc->spell_slots[1] = 2 + (floor_id / 10);
            break;
        case ARCH_ASSASSIN:
            hp_base           = (int)(hp_base * 0.8f);
            npc->ac          += 2;
            npc->attack_bonus+= 2;
            npc->damage_sides = 8;
            break;
        case ARCH_DRAGON:
            hp_base           = (int)(hp_base * 2.0f);
            npc->ac          += 3;
            npc->attack_bonus+= 2;
            npc->damage_dice += 1;
            npc->damage_sides = 10;
            break;
        case ARCH_BRUTE:
            hp_base           = (int)(hp_base * 1.5f);
            npc->ac          -= 1;
            npc->damage_dice += 1;
            npc->damage_sides = 8;
            break;
        case ARCH_SWARM:
            hp_base           = (int)(hp_base * 0.6f);
            npc->ac          -= 2;
            npc->damage_sides = 4;
            break;
        default:
            break;
    }

    npc->hp     = hp_base;
    npc->max_hp = hp_base;

    // Attach behavior tree
    attach_behavior_by_archetype(npc);
}

// Reattaches the behavior tree after loading from disk (does not touch stats).
void ai_attach_behavior(NPC* npc) {
    if (!npc || npc->archetype == ARCH_MERCHANT) return;
    attach_behavior_by_archetype(npc);
}


#include "../../include/map.h"
extern World *master_world;
extern int next_id; /*unique entity ID counter (defined in main_server.c)*/

static int get_light_level(Client* target) {
    if (!master_world || target->floor_id < 0 || target->floor_id >= MAX_FLOORS) return 0;
    Map* map = &master_world->floors[target->floor_id].map;
    int tx = target->x;
    int ty = target->y;
    int light = 0;
    
    for (int y = ty - 2; y <= ty + 2; y++) {
        for (int x = tx - 2; x <= tx + 2; x++) {
            if (x >= 0 && x < MAP_WIDTH && y >= 0 && y < MAP_HEIGHT) {
                VoxelType v = map->data[0][y][x];
                if (v == VOXEL_LAVA) light += 4;
                else if (v == VOXEL_MUSHROOM_GLOW || v == VOXEL_CRYSTAL_BLUE || v == VOXEL_CRYSTAL_PURPLE) light += 2;
            }
        }
    }
    return light;
}

static Client* find_closest_target(NPC* npc, Client* clients, int num_clients) {
    Client* best_target = NULL;
    int min_d = 9999;
    int base_sight = (npc->template && npc->template->sight_range > 0) ? npc->template->sight_range : 5;
    
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].active && clients[i].authenticated && clients[i].floor_id == npc->floor_id) {
            int d = abs(clients[i].x - npc->x) + abs(clients[i].y - npc->y);
            int sight_range = base_sight + get_light_level(&clients[i]);
            if (d <= sight_range && d < min_d) {
                min_d = d;
                best_target = &clients[i];
            }
        }
    }
    return best_target;
}

/*Returns true if the cell (x,y) on the NPC's floor is out of bounds or
 * already occupied by a player or by another active NPC.
 * Used to make sure the AI never moves a monster onto an occupied cell:
 * stacking entities would corrupt the entity_grid (collision) and
 * break melee combat.*/
static bool cell_occupied(int x, int y, NPC* npc, Client* clients,
                          int num_clients, NPC* all_npcs) {
    if (x < 0 || x >= MAP_WIDTH || y < 0 || y >= MAP_HEIGHT) {
        return true;
    }
    for (int i = 0; i < num_clients; i++) {
        if (clients[i].active && clients[i].authenticated &&
            clients[i].floor_id == npc->floor_id &&
            clients[i].x == x && clients[i].y == y) {
            return true;
        }
    }
    for (int i = 0; i < MAX_NPCS; i++) {
        if (&all_npcs[i] != npc && all_npcs[i].active &&
            all_npcs[i].floor_id == npc->floor_id &&
            all_npcs[i].x == x && all_npcs[i].y == y) {
            return true;
        }
    }
    return false;
}

static void default_ai_update(NPC* npc, Client* clients, int num_clients, bool new_round, NPC* all_npcs) {
    Client* target = find_closest_target(npc, clients, num_clients);
    if (!target) return;

    int d = abs(target->x - npc->x) + abs(target->y - npc->y);
    if (d <= 1) {
        if (new_round) {
            perform_attack_npc(npc, target, all_npcs);
        }
        return;
    }

    /*A* movement — recalculate every 3 ticks or when the target has moved*/
    npc->ai_ctx.state_timer++;
    if (npc->ai_ctx.state_timer < 3) {
        return; /* wait for next tick */
    }
    npc->ai_ctx.state_timer = 0;

    if (!master_world) {
        return;
    }
    Map *map = &master_world->floors[npc->floor_id].map;

    PathNode path[MAX_ASTAR_PATH];
    int steps = pathfind_astar(map,
                               npc->x, npc->y,
                               target->x, target->y,
                               path, MAX_ASTAR_PATH);

    if (steps >= 2) {
        /* path[0] = current position, path[1] = first step.
         * A* only sees the map: if the first step is occupied by a
         * player or another NPC, wait for the next tick. */
        if (!cell_occupied(path[1].x, path[1].y, npc, clients, num_clients, all_npcs)) {
            npc->x = path[1].x;
            npc->y = path[1].y;
        }
    } else if (steps == 0) {
        /* A* found no path: direct fallback (e.g. target is adjacent).
         * One axis per step (4-way) and never onto an occupied cell. */
        int nx = npc->x, ny = npc->y;
        if (npc->x < target->x)      nx++;
        else if (npc->x > target->x) nx--;
        else if (npc->y < target->y) ny++;
        else if (npc->y > target->y) ny--;
        if (!cell_occupied(nx, ny, npc, clients, num_clients, all_npcs)) {
            npc->x = nx;
            npc->y = ny;
        }
    }
}


extern void broadcast_spell_vfx(int sx, int sy, int tx, int ty, int vfx_type, float r, float g, float b, int floor_id);

AINodeStatus ai_swarm_behavior(NPC* npc, Client* clients, int num_clients, bool new_round, NPC* all_npcs) {
    if (new_round && npc->hp > 1) {
        // Count adjacent swarms
        int counter = 0;
        for (int i = 0; i < MAX_NPCS; i++) {
            if (all_npcs[i].active && all_npcs[i].floor_id == npc->floor_id && all_npcs[i].archetype == ARCH_SWARM) {
                int dx = abs(all_npcs[i].x - npc->x);
                int dy = abs(all_npcs[i].y - npc->y);
                if (dx <= 1 && dy <= 1 && (dx > 0 || dy > 0)) {
                    counter++;
                }
            }
        }
        
        if (counter == 0) counter = 1; // to prevent div by 0 and allow growth
        
        if (counter < 4 && (rand() % (counter * 7)) == 0) {
            // Try to split
            int try_dx = (rand() % 3) - 1;
            int try_dy = (rand() % 3) - 1;
            if (try_dx != 0 || try_dy != 0) {
                int nx = npc->x + try_dx;
                int ny = npc->y + try_dy;
                
                // Check if map tile is empty
                Map* m = &master_world->floors[npc->floor_id].map;
                if (nx > 0 && nx < MAP_WIDTH - 1 && ny > 0 && ny < MAP_HEIGHT - 1 && m->data[0][ny][nx] == VOXEL_FLOOR) {
                    // Check if occupied by another npc or a player
                    bool occupied = cell_occupied(nx, ny, npc, clients, num_clients, all_npcs);
                    if (!occupied) {
                        //Find empty slots
                        int slot = -1;
                        for (int i = 0; i < MAX_NPCS; i++) {
                            if (!all_npcs[i].active) { slot = i; break; }
                        }
                        if (slot != -1) {
                            // Split!
                            npc->hp = (npc->hp / 2) > 0 ? (npc->hp / 2) : 1;
                            
                            NPC* clone = &all_npcs[slot];
                            *clone = *npc; //copy all stats
                            /*A truly unique ID: the slot index could
                             * collide with an already-assigned entity_id
                             * (NPC or player) and corrupt the
                             * entity_grid collision lookups.*/
                            clone->entity_id = next_id++;
                            clone->x = nx;
                            clone->y = ny;
                            clone->hp = npc->hp;
                            
                            broadcast_spell_vfx(npc->x, npc->y, nx, ny, 1, 0.0f, 1.0f, 0.0f, npc->floor_id); // Green explosion
                            return AI_SUCCESS;
                        }
                    }
                }
            }
        }
    }
    
    //Default melee behavior
    default_ai_update(npc, clients, num_clients, new_round, all_npcs);
    return AI_SUCCESS;
}

void ai_update_npc(NPC* npc, Client* clients, int num_clients, bool new_round, NPC* all_npcs) {
    if (!npc || !npc->active || npc->archetype == ARCH_MERCHANT) return;

    AINodeFunc root = (AINodeFunc)npc->ai_ctx.behavior_tree_root;
    if (root) {
        root(npc, clients, num_clients, new_round, all_npcs);
    } else {
        default_ai_update(npc, clients, num_clients, new_round, all_npcs);
    }
}

AINodeStatus ai_mage_behavior(NPC* npc, Client* clients, int num_clients, bool new_round, NPC* all_npcs) {
    Client* target = find_closest_target(npc, clients, num_clients);
    if (!target) return AI_FAILURE;

    int d = abs(target->x - npc->x) + abs(target->y - npc->y);

    if (new_round) {
        // Try to cast a spell if spell slots are available
        if (npc->spell_slots[1] > 0) {
            send_text_to_client(target->sock, "[DANGER] The Mage casts 'Magic Missile' at you!");
            // Spell damage application logic would go here
            npc->spell_slots[1]--;
            return AI_SUCCESS;
        }
        
        //No spells left: attack in melee or back away
        if (d <= 1) {
            perform_attack_npc(npc, target, all_npcs);
            return AI_SUCCESS;
        }
    }

    // Movement (maintains distance): one axis per step, and never
    // onto a cell occupied by a player or another NPC.
    if (d > 3 && rand() % 2 == 0) {
        int nx = npc->x, ny = npc->y;
        if (npc->x < target->x)      nx++;
        else if (npc->x > target->x) nx--;
        else if (npc->y < target->y) ny++;
        else if (npc->y > target->y) ny--;
        if (!cell_occupied(nx, ny, npc, clients, num_clients, all_npcs)) {
            npc->x = nx;
            npc->y = ny;
        }
    }

    return AI_RUNNING;
}

AINodeStatus ai_void_crawler_behavior(NPC* npc, Client* clients, int num_clients, bool new_round, NPC* all_npcs) {
    Client* target = find_closest_target(npc, clients, num_clients);
    if (!target) return AI_FAILURE;

    int d = abs(target->x - npc->x) + abs(target->y - npc->y);

    if (new_round) {
        // Phase 1: Enslave
        if (d > 1 && d <= 5) {
            if (rand() % 4 == 0) {
                int save_mod = rules_get_modifier(target->wis);
                bool saved = rules_roll_save(save_mod, 14, false, false, NULL);
                if (!saved) {
                    send_text_to_client(target->sock, "[DANGER] The Boss enters your mind! You are Charmed!");
                    ActiveEffect charm_effect = { "Enslave", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 3, false };
                    if (target->effect_count < MAX_EFFECTS_PER_ENTITY) {
                        target->effects[target->effect_count++] = charm_effect;
                    }
                    return AI_SUCCESS;
                } else {
                    send_text_to_client(target->sock, "[DANGER] The Boss tries to control you, but you resist!");
                }
            }
        }

        // Phase 2: Mucous Cloud / Melee
        if (d <= 1) {
            if (rand() % 3 == 0) {
                perform_attack_npc(npc, target, all_npcs);
            } else {
                int save_mod = rules_get_modifier(target->con);
                bool saved = rules_roll_save(save_mod, 14, false, false, NULL);
                if (!saved) {
                    send_text_to_client(target->sock, "[DANGER] You are engulfed by Mucus! You are Poisoned!");
                    ActiveEffect poison_effect = { "Mucous Cloud", EVENT_ON_TURN_START, MOD_ADDITIVE, -2, 3, false };
                    if (target->effect_count < MAX_EFFECTS_PER_ENTITY) {
                        target->effects[target->effect_count++] = poison_effect;
                    }
                } else {
                    send_text_to_client(target->sock, "[DANGER] You dodge the Mucous Cloud!");
                }
                perform_attack_npc(npc, target, all_npcs);
            }
            return AI_SUCCESS;
        }
    }

    // Phase 3: Approach (continuous movement between rounds):
    // one axis per step, and never onto an occupied cell.
    if (rand() % 20 == 0) {
        int nx = npc->x, ny = npc->y;
        if (npc->x < target->x)      nx++;
        else if (npc->x > target->x) nx--;
        else if (npc->y < target->y) ny++;
        else if (npc->y > target->y) ny--;
        if (!cell_occupied(nx, ny, npc, clients, num_clients, all_npcs)) {
            npc->x = nx;
            npc->y = ny;
        }
    }

    return AI_RUNNING;
}
