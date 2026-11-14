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

/**
 * server_world.c — World tick, NPC respawn and density monitor
 *
 * Contains the world update logic executed on every iteration
 * of the server's main loop:
 *   - Periodic auto-save of players
 *   - Light source and hunger resource consumption
 *   - Periodic effects and conditions (poison, fire, bleeding)
 *   - Saving throws for condition recovery (players and NPCs)
 *   - Speed/Tick System for NPCs (energy and actions)
 *   - NPC respawn with countdown
 *   - Density Monitor: emergency respawn if a floor is too empty
 *   - Persistent AoE cloud updates
 */

#include "server_world.h"
#include "server_internal.h"
#include "ai.h"
#include "aoe.h"
#include "player.h"
#include "server_log.h"
#include "server_tiles.h"
#include "server_combat.h"

/*Energy threshold for NPC action in the Speed/Tick System*/
#define ENERGY_THRESHOLD 10

void get_game_time(Game *game, int *h, int *m) {
    int total_mins = (game->total_turns % 1440);
    *h = (total_mins / 60 + 8) % 24;
    *m = total_mins % 60;
}

/*============================================================================
 * update_world — Major world state update
 *
 * Each iteration of the server's main loop must be called.
 * @clients: Array of connected clients.
 * @npcs: Array of active and inactive NPCs.
 * ==========================================================================*/

/*Respawn a dead player in town: frees the cell on the old floor, tells
 * that floor the player left, registers the player in the town grid and
 * sends the client its new state and the town map. Every death site uses
 * it.*/
void player_respawn_town(Game *game, Client *c) {
    if (!c || !game->world) {
        return;
    }
    int old_floor = c->floor_id;
    if (old_floor >= 0 && old_floor < MAX_FLOORS && c->x >= 0 && c->x < MAP_WIDTH && c->y >= 0 &&
        c->y < MAP_HEIGHT &&
        game->world->floors[old_floor].entity_grid[c->y][c->x] == c->entity_id) {
        game->world->floors[old_floor].entity_grid[c->y][c->x] = 0;
    }
    c->hp = c->max_hp;
    c->floor_id = 0;
    c->x = MAP_CENTER_X + 1;
    c->y = MAP_CENTER_Y + 1;
    if (game->world->floors[0].entity_grid[c->y][c->x] == 0) {
        game->world->floors[0].entity_grid[c->y][c->x] = c->entity_id;
    }
    notify_player_left_floor(game, c, old_floor);
    broadcast_player_state(game, c);
    send_detailed_state(game, c);
    send_map_chunk(c->sock, &game->world->floors[0].map, c->x, c->y, INITIAL_VIEW_RADIUS);
    broadcast_nearby_entities(c, game);
}

static void broadcast_game_time(Game *game, Client *clients) {
    int total_mins = (game->total_turns % 1440);
    int h = (total_mins / 60 + 8) % 24;
    int m = total_mins % 60;

    MsgTimeSync ts;
    ts.game_hour = h;
    ts.game_min = m;
    ts.total_turns = game->total_turns;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].authenticated) {
            send_time_sync(clients[i].sock, &ts);
        }
    }
}

/*Snapshot world.dat and npcs.dat (in this order) for the writer thread,
 * or write them synchronously when it is not running (startup, shutdown).
 * Returns false if either could not be written (the writer logs why); the
 * previous file is kept in that case.*/
/*Paths of the two files of the world; false if the state directory path
 * is too long.*/
static bool world_paths(char *world, char *npcs, size_t n) {
    if (!state_path(world, n, "world.dat") || !state_path(npcs, n, "npcs.dat")) {
        server_log("SAVE", "State directory path too long.");
        return false;
    }
    return true;
}

bool save_world_state(Game *game) {
    char world_path[STATE_DIR_MAX + 32], npcs_path[STATE_DIR_MAX + 32];
    return world_paths(world_path, npcs_path, sizeof(world_path)) &&
           persist_write_world(world_path, game->world) &&
           persist_write_npcs(npcs_path, game->npcs, MAX_NPCS, game->next_id, game->total_turns);
}

bool autosave_step(Client *clients, Game *game, int step) {
    char world_path[STATE_DIR_MAX + 32], npcs_path[STATE_DIR_MAX + 32];
    if (!world_paths(world_path, npcs_path, sizeof(world_path))) {
        return false;
    }
    if (step == 0) {
        int saved_count = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].authenticated) {
                save_player_data(&clients[i]);
                saved_count++;
            }
        }
        bool ok = persist_write_world(world_path, game->world);
        if (ok) {
            server_log("SYS", "Auto-save: %d players and the world.", saved_count);
        }
        return ok;
    }
    return persist_write_npcs(npcs_path, game->npcs, MAX_NPCS, game->next_id, game->total_turns);
}

/*Once a minute, over AUTOSAVE_STEPS consecutive rounds (autosave_step).*/
static void autosave_if_due(Client *clients, Game *game) {
    static time_t last_autosave = 0;
    static int step = -1; /*-1: no autosave in progress*/
    time_t now = time(NULL);
    if (last_autosave == 0) {
        last_autosave = now;
    }
    if (step < 0 && now - last_autosave >= 60) {
        last_autosave = now;
        step = 0;
    }
    if (step >= 0) {
        (void)autosave_step(clients, game, step);
        step = step + 1 < AUTOSAVE_STEPS ? step + 1 : -1;
    }
}

/*The global invasion event: starts every 1500 rounds on a random floor
 * 1-10, ends with a reward when its goal is met or when time runs out.*/
static void update_global_event(Game *game, Client *clients) {
    if (game->event.type == 0) {
        if (game->total_turns % 1500 == 0) {          //Every ~300 seconds (5 min real at 5 tick/s)
            game->event.type = 1;                     // 1 = Skeletons
            game->event.floor_id = (rand() % 10) + 1; //Floor 1-10
            game->event.time_left = 1500;             // 5 minutes
            game->event.progress = 0;
            game->event.goal = 5; // 5 Skeletons

            //Broadcast to everyone
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].authenticated) {
                    send_text_to_client(
                        clients[j].sock,
                        "[GLOBAL EVENT] Invasion on floor %d! Kill %d Skeletons! Time: 5 minutes.",
                        game->event.floor_id, game->event.goal);
                }
            }
        }
    } else {
        game->event.time_left--;
        if (game->event.progress >= game->event.goal) {
            // Victory
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].authenticated) {
                    send_text_to_client(
                        clients[j].sock,
                        "[GLOBAL EVENT] The invasion has been stopped! Defenders are rewarded.");
                    if (clients[j].floor_id == game->event.floor_id) {
                        clients[j].gold += 500;
                        send_text_to_client(clients[j].sock,
                                            "[SYSTEM] You received 500 gold for your bravery!");
                    }
                }
            }
            game->event.type = 0;
        } else if (game->event.time_left <= 0) {
            // Defeat
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].authenticated) {
                    send_text_to_client(
                        clients[j].sock,
                        "[GLOBAL EVENT] Time is up, the monsters retreat into the shadows...");
                }
            }
            game->event.type = 0;
        }
    }
}

/*A death by hunger or by a condition: the bones stay, the character
 * wakes up in town.*/
static void player_dies(Game *game, Client *c, const char *how) {
    server_log("DEATH", "%s %s.", c->username, how);
    save_bones(game, c);
    player_respawn_town(game, c);
    send_text_to_client(
        c->sock, "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
}

/*Removes the condition from the player; false if he did not have it.*/
static bool player_drop_condition(Client *c, ConditionType cond) {
    const char *name = condition_to_name(cond);
    for (int e = 0; e < c->effect_count; e++) {
        if (strcasecmp(c->effects[e].name, name) == 0) {
            c->effects[e] = c->effects[c->effect_count - 1];
            c->effect_count--;
            return true;
        }
    }
    return false;
}

/*Light sources in the hands and on the belt burn one round.*/
static void player_light_round(Client *c) {
    ItemInstance *ls[] = {&c->slot_hand_r, &c->slot_hand_l, &c->belt[0],
                          &c->belt[1],     &c->belt[2],     &c->belt[3]};
    for (int j = 0; j < 6; j++) {
        if (ls[j]->template_idx == -1 ||
            item_database[ls[j]->template_idx].category != ITEM_LIGHT_SOURCE ||
            ls[j]->durability <= 0) {
            continue;
        }
        ls[j]->durability--;
        if (ls[j]->durability == 0) {
            send_text_to_client(c->sock, "[SYSTEM] Your light source has gone out!");
        }
    }
}

/*Hunger grows every round; a starving character loses 2 HP every other
 * round and gains a level of exhaustion every 100.*/
static void player_hunger_round(Game *game, Client *c) {
    c->hunger_level++;
    if (c->hunger_level < HUNGER_MAX) {
        return;
    }
    if (game->total_turns % 2 == 0) {
        c->hp -= 2;
        send_text_to_client(c->sock, "[SYSTEM] You're starving!");
        if (c->hp <= 0) {
            player_dies(game, c, "starved to death");
        }
    }
    if (game->total_turns % 100 == 0) {
        c->exhaustion_level++;
        send_text_to_client(c->sock, "[SYSTEM] You have gained a level of Exhaustion!");
    }
}

/*Each round a DC 12 saving throw ends a condition: Dexterity against the
 * flames, Strength against stone and ice, Constitution for the others. A
 * cursed character rolls with disadvantage.*/
static void player_condition_saves(Game *game, Client *c) {
    static const ConditionType k_conditions[] = {COND_PARALYZED, COND_STUNNED,  COND_UNCONSCIOUS,
                                                 COND_BURNING,   COND_BLEEDING, COND_PETRIFIED,
                                                 COND_FROZEN};
    for (size_t k = 0; k < sizeof(k_conditions) / sizeof(k_conditions[0]); k++) {
        ConditionType cond = k_conditions[k];
        if (!rules_has_condition_t(c->effects, c->effect_count, cond)) {
            continue;
        }
        int ts, td, tc, ti, tw, th;
        get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
        int score = tc;
        if (cond == COND_BURNING) {
            score = td;
        } else if (cond == COND_PETRIFIED || cond == COND_FROZEN) {
            score = ts;
        }
        int mod = rules_get_modifier(score);
        bool dis = rules_has_condition_t(c->effects, c->effect_count, COND_CURSED);
        int roll_v = 0;
        bool success = rules_roll_save(mod, 12, false, dis, &roll_v);
        clog_save(c->username, condition_to_name(cond), roll_v, mod, 12, success);
        if (success && player_drop_condition(c, cond)) {
            send_text_to_client(c->sock, "[SYSTEM] You have recovered from the %s state!",
                                condition_to_name(cond));
            send_detailed_state(game, c);
        }
    }
}

/*Poison and bleeding cost 1 HP a round, fire 2 (water puts it out).*/
static void player_periodic_damage(Game *game, Client *c) {
    static const struct {
        ConditionType cond;
        int damage;
        const char *feel, *death;
    } k_periodic[] = {
        {COND_POISONED, 1, "[SYSTEM] Feel the poison coursing through your veins...",
         "died from poison"},
        {COND_BURNING, 2, "[SYSTEM] You are burning!", "burned to death"},
        {COND_BLEEDING, 1, "[SYSTEM] You are bleeding...", "bled to death"},
    };
    for (size_t k = 0; k < sizeof(k_periodic) / sizeof(k_periodic[0]); k++) {
        if (!rules_has_condition_t(c->effects, c->effect_count, k_periodic[k].cond)) {
            continue;
        }
        const Floor *fl = &game->world->floors[c->floor_id];
        if (k_periodic[k].cond == COND_BURNING && fl->map.data[0][c->y][c->x] == VOXEL_WATER) {
            if (player_drop_condition(c, COND_BURNING)) {
                send_text_to_client(c->sock, "[SYSTEM] Water extinguishes flames!");
                send_detailed_state(game, c);
            }
            continue;
        }
        c->hp -= k_periodic[k].damage;
        send_text_to_client(c->sock, "%s", k_periodic[k].feel);
        if (c->hp <= 0) {
            player_dies(game, c, k_periodic[k].death);
        }
        send_detailed_state(game, c);
    }
}

/*One round of a player: effects and conditions everywhere; light and
 * hunger only in the dungeon (floor > 0).*/
static void player_round(Game *game, Client *clients, int i) {
    Client *c = &clients[i];
    if (!c->active || !c->authenticated) {
        return;
    }
    if (c->floor_id > 0) {
        player_light_round(c);
        player_hunger_round(game, c);
    }
    if (rules_update_effects(c->effects, &c->effect_count)) {
        send_detailed_state(game, c);
    }
    player_condition_saves(game, c);
    if (game->total_turns % 10 == 0 &&
        rules_has_condition_t(c->effects, c->effect_count, COND_INVISIBLE)) {
        send_text_to_client(c->sock, "[SYSTEM] You are currently invisible.");
    }
    player_periodic_damage(game, c);
}

static void respawn_traps_and_crystals(Game *game) {
    /* --- Trap respawn --- */
    for (int f_id = 1; f_id < MAX_FLOORS; f_id++) {
        Floor *f = &game->world->floors[f_id];
        for (int t_idx = 0; t_idx < f->trap_count; t_idx++) {
            Trap *t = &f->traps[t_idx];
            if (!t->active && t->respawn_timer > 0) {
                t->respawn_timer--;
                if (t->respawn_timer == 0) {
                    t->active = true;
                }
            }
        }
    }

    /* --- Crystal respawn --- */
    for (int f_id = 0; f_id < MAX_FLOORS; f_id++) {
        Floor *f = &game->world->floors[f_id];
        for (int i = 0; i < f->crystal_respawn_count; i++) {
            CrystalRespawn *cr = &f->crystal_respawns[i];
            if (cr->respawn_timer > 0) {
                cr->respawn_timer--;
                if (cr->respawn_timer == 0) {
                    // Restore the crystal in the map
                    f->map.data[0][cr->y][cr->x] = cr->type;

                    // Broadcast VFX to clients
                    float r = 1.0f, g = 1.0f, b = 1.0f;
                    if (cr->type == VOXEL_CRYSTAL_BLUE) {
                        r = 0.3f;
                        g = 0.7f;
                        b = 1.0f;
                    } else if (cr->type == VOXEL_CRYSTAL_PURPLE) {
                        r = 0.8f;
                        g = 0.2f;
                        b = 1.0f;
                    } else if (cr->type == VOXEL_CRYSTAL_RED) {
                        r = 1.0f;
                        g = 0.1f;
                        b = 0.1f;
                    } else if (cr->type == VOXEL_CRYSTAL_GREEN) {
                        r = 0.1f;
                        g = 1.0f;
                        b = 0.2f;
                    } else if (cr->type == VOXEL_CRYSTAL_YELLOW) {
                        r = 1.0f;
                        g = 0.9f;
                        b = 0.1f;
                    } else if (cr->type == VOXEL_CRYSTAL_ORANGE) {
                        r = 1.0f;
                        g = 0.5f;
                        b = 0.0f;
                    } else if (cr->type == VOXEL_CRYSTAL_CYAN) {
                        r = 0.0f;
                        g = 0.9f;
                        b = 1.0f;
                    }

                    broadcast_spell_vfx(game, cr->x, cr->y, cr->x, cr->y, 1, r, g, b, f_id);

                    // Remove from list
                    f->crystal_respawns[i] = f->crystal_respawns[f->crystal_respawn_count - 1];
                    f->crystal_respawn_count--;
                    i--; // re-check this index
                }
            }
        }
    }
}

NPC *npc_alloc(Game *game) {
    /*The search starts where the last one ended: a table that fills up is
     * not walked from the top at every spawn.*/
    NPC *dead = NULL;
    for (int k = 0; k < MAX_NPCS; k++) {
        int i = (game->npc_alloc_hint + k) % MAX_NPCS;
        if (game->npcs[i].active) {
            continue;
        }
        if (game->npcs[i].template == NULL || game->npcs[i].respawn_timer < 0) {
            game->npc_alloc_hint = (i + 1) % MAX_NPCS;
            return &game->npcs[i];
        }
        if (!dead) {
            dead = &game->npcs[i];
        }
    }
    return dead;
}

NPC *npc_spawn_item(Game *game, int floor_id, int x, int y, const ItemInstance *item) {
    NPC *n = npc_alloc(game);
    if (!n) {
        return NULL;
    }
    /*Fields first, activation last (npc_set_active)*/
    n->entity_id = game->next_id++;
    n->floor_id = floor_id;
    n->x = x;
    n->y = y;
    n->archetype = ARCH_TREASURE;
    n->template_idx = item->template_idx; /*the client renders the item from it*/
    n->hp = n->max_hp = 1;
    memset(n->ghost_loot, 0, sizeof(n->ghost_loot));
    n->ghost_loot[0] = *item;
    n->template = NULL;
    /*Nothing of the slot's previous tenant may show through.*/
    n->is_ghost = false;
    n->gold_drop = 0;
    n->custom_name[0] = '\0';
    n->effect_count = 0;
    npc_set_active(game, n, true);
    game->world->floors[floor_id].entity_grid[y][x] = n->entity_id;
    return n;
}

static bool npc_cannot_act(NPC *n) {
    static const ConditionType k_blocking[] = {COND_PARALYZED,   COND_STUNNED,
                                               COND_PETRIFIED,   COND_FROZEN,
                                               COND_UNCONSCIOUS, COND_INCAPACITATED};
    for (size_t i = 0; i < sizeof(k_blocking) / sizeof(k_blocking[0]); i++) {
        if (rules_has_condition_t(n->effects, n->effect_count, k_blocking[i])) {
            return true;
        }
    }
    return false;
}

/*A merchant restocks every 100 rounds.*/
static void merchant_round(NPC *n) {
    if (n->merchant.restock_timer > 0) {
        n->merchant.restock_timer--;
        return;
    }
    for (int j = 0; j < n->merchant.item_count; j++) {
        n->merchant.item_stock[j] = n->merchant.item_stock_max[j];
    }
    n->merchant.restock_timer = 100;
}

/*Poison and bleeding cost an NPC 1 HP a round, fire 2. False if it died
 * (nobody is credited).*/
static bool npc_periodic_damage(Game *game, NPC *n) {
    int damage = 0;
    if (rules_has_condition_t(n->effects, n->effect_count, COND_POISONED)) {
        damage += 1;
    }
    if (rules_has_condition_t(n->effects, n->effect_count, COND_BLEEDING)) {
        damage += 1;
    }
    if (rules_has_condition_t(n->effects, n->effect_count, COND_BURNING)) {
        damage += 2;
    }
    n->hp -= damage;
    if (damage > 0 && n->hp <= 0) {
        n->hp = 0;
        npc_killed(game, NULL, n, NULL);
        return false;
    }
    return true;
}

/*Morale: the first time it falls under a fifth of its hit points an NPC
 * rolls a DC 12 save or is frightened for 5 rounds.*/
static void npc_morale(NPC *n) {
    if (n->hp >= (n->max_hp / 5) || n->morale != 0) {
        return;
    }
    n->morale = 1;
    if (!rules_roll_save(0, 12, false, false, NULL)) {
        ActiveEffect fx;
        memset(&fx, 0, sizeof(fx));
        fx.name = condition_to_name(COND_FRIGHTENED);
        fx.duration_rounds = 5;
        rules_add_effect(n->effects, &n->effect_count, MAX_EFFECTS_PER_ENTITY, &fx);
    }
}

/*Each round a DC 12 save (+2) ends a condition that takes the turn.*/
static void npc_condition_saves(NPC *n) {
    static const ConditionType k_conditions[] = {COND_PARALYZED, COND_STUNNED, COND_UNCONSCIOUS,
                                                 COND_PETRIFIED, COND_FROZEN};
    for (size_t k = 0; k < sizeof(k_conditions) / sizeof(k_conditions[0]); k++) {
        if (!rules_has_condition_t(n->effects, n->effect_count, k_conditions[k])) {
            continue;
        }
        const char *cond_name = condition_to_name(k_conditions[k]);
        bool dis = rules_has_condition_t(n->effects, n->effect_count, COND_CURSED);
        int roll_v = 0;
        bool success = rules_roll_save(2, 12, false, dis, &roll_v);
        clog_save(npc_name(n), cond_name, roll_v, 2, 12, success);
        for (int e = 0; success && e < n->effect_count; e++) {
            if (strcasecmp(n->effects[e].name, cond_name) == 0) {
                n->effects[e] = n->effects[n->effect_count - 1];
                n->effect_count--;
                break;
            }
        }
    }
}

/*A dead monster counts down and comes back at its spawn point (5 rounds
 * later if the tile is taken).*/
static void npc_respawn_round(Game *game, NPC *n) {
    if (n->respawn_timer > 0) {
        n->respawn_timer--;
    }
    if (n->respawn_timer != 0) {
        return;
    }
    /*Bounds: a corrupted npcs.dat can carry an out-of-range floor or spawn
     * point. The slot stays dead (the density monitor skips it too).*/
    if (n->floor_id < 0 || n->floor_id >= MAX_FLOORS || n->spawn_x < 0 || n->spawn_x >= MAP_WIDTH ||
        n->spawn_y < 0 || n->spawn_y >= MAP_HEIGHT) {
        return;
    }
    if (game->world->floors[n->floor_id].entity_grid[n->spawn_y][n->spawn_x] != 0) {
        n->respawn_timer = 5;
        return;
    }
    npc_set_active(game, n, true);
    n->x = n->spawn_x;
    n->y = n->spawn_y;
    n->hp = n->max_hp;
    n->effect_count = 0;
    n->morale = 0;
    n->respawn_timer = 0;
    game->world->floors[n->floor_id].entity_grid[n->y][n->x] = n->entity_id;
    ai_init_npc(n, n->template->name, n->floor_id);
    server_log("SPAWN", "%s respawned on floor %d (%d,%d)", n->template->name, n->floor_id, n->x,
               n->y);
}

/*The floors with at least one character on them. Only there the monsters
 * live their round: with nobody to see them the others sleep (their dead
 * still count down to the respawn, their merchants still restock).*/
static void awake_floors(const Client *clients, bool awake[MAX_FLOORS]) {
    memset(awake, 0, sizeof(bool) * MAX_FLOORS);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        const Client *c = &clients[i];
        if (c->active && c->authenticated && c->floor_id >= 0 && c->floor_id < MAX_FLOORS) {
            awake[c->floor_id] = true;
        }
    }
}

/*One round of an NPC: its periodic damage, morale and saving throws, then
 * as many AI cycles as its energy allows; a dead monster counts down to
 * its respawn instead. Merchants only restock. On a floor that is not
 * awake a living NPC does nothing at all.*/
static void npc_round(Client *clients, Game *game, NPC *n, const bool awake[MAX_FLOORS]) {
    if (n->respawn_timer < 0) {
        if (n->active && n->archetype == ARCH_MERCHANT) {
            merchant_round(n);
        }
        return;
    }
    if (!n->active) {
        if (n->template != NULL && !n->is_ghost && n->archetype != ARCH_TREASURE &&
            n->archetype != ARCH_GOLD) {
            npc_respawn_round(game, n);
        }
        return;
    }
    if (n->floor_id < 0 || n->floor_id >= MAX_FLOORS || !awake[n->floor_id]) {
        return;
    }
    if (!npc_periodic_damage(game, n)) {
        return;
    }
    npc_morale(n);
    npc_condition_saves(n);
    /*Debilitating conditions take the turn; the effects then age by one
     * round, so an effect of N rounds costs the NPC N turns.*/
    if (!npc_cannot_act(n)) {
        /*The NPC gains energy equal to its speed every tick (1 slow, 2
         * normal, 3 fast) and acts when it reaches ENERGY_THRESHOLD. At most
         * one action (attack, cast) per round: the first energy cycle of
         * the round may act, the extra cycles only move.*/
        n->energy += (n->template && n->template->speed > 0) ? n->template->speed : 2;
        bool first_cycle = true;
        while (n->energy >= ENERGY_THRESHOLD) {
            n->energy -= ENERGY_THRESHOLD;
            ai_update_npc(game, n, clients, MAX_CLIENTS, first_cycle);
            first_cycle = false;
        }
    }
    rules_update_effects(n->effects, &n->effect_count);
}

/*A floor whose living NPCs fall under DENSITY_MIN_PCT gets one of its dead
 * NPCs back at its spawn point (one per floor per check).*/
static void rebalance_floor_density(Game *game) {
    for (int f = 1; f < MAX_FLOORS; f++) {
        int total = 0;
        int active = 0;
        floor_stats_get(game, f, &total, &active);

        if (total == 0) {
            continue;
        }
        int pct = (active * 100) / total;
        if (pct >= DENSITY_MIN_PCT) {
            continue;
        }

        /* Density too low: force respawn of the first dead NPC on
     * this floor. O(N) scan — but performed rarely and only
     * on floors in emergency, not on all of them. */
        for (int i = 0; i < MAX_NPCS; i++) {
            NPC *n = &game->npcs[i];
            if (n->active) {
                continue;
            }
            if (n->template == NULL) {
                continue;
            }
            if (n->archetype == ARCH_MERCHANT) {
                continue;
            }
            if (n->floor_id != f) {
                continue;
            }
            if (n->respawn_timer < 0) {
                continue;
            }
            if (n->spawn_x < 0 || n->spawn_x >= MAP_WIDTH || n->spawn_y < 0 ||
                n->spawn_y >= MAP_HEIGHT) {
                continue; /*corrupted spawn point: never index the grid OOB*/
            }
            if (game->world->floors[f].entity_grid[n->spawn_y][n->spawn_x] != 0) {
                continue; /* Skip and try another NPC if this spawn spot is blocked */
            }
            npc_set_active(game, n, true); /* Updates the floor caches */
            n->x = n->spawn_x;
            n->y = n->spawn_y;
            n->hp = n->max_hp;
            n->effect_count = 0;
            n->morale = 0;
            n->respawn_timer = 0;
            game->world->floors[f].entity_grid[n->y][n->x] = n->entity_id;
            ai_init_npc(n, n->template->name, n->floor_id);
            server_log("SPAWN", "[DENSITY] Floor %d (%d%%): emergency %s", f, pct,
                       n->template->name);
            break; /*One at a time so as not to overload*/
        }
    }
}

void update_world(Client *clients, Game *game) {
    autosave_if_due(clients, game);

    game->total_turns++;
    if (game->total_turns % 5 == 0) {
        broadcast_game_time(game, clients);
    }
    update_global_event(game, clients);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        player_round(game, clients, i);
    }
    respawn_traps_and_crystals(game);

    /*Each NPC decays its effects and acts once per round (first_cycle in
     * npc_round); every dead one counts down.*/
    bool awake[MAX_FLOORS];
    awake_floors(clients, awake);
    for (int i = 0; i < MAX_NPCS; i++) {
        npc_round(clients, game, &game->npcs[i], awake);
    }

    /*Spawns, deaths and splits of this round.*/
    floor_index_rebuild(game);
    /*The AI moved the monsters: realign the entity grid, or players would
     * walk through them.*/
    sync_entity_grid(game);
    if (game->total_turns % DENSITY_CHECK == 0) {
        rebalance_floor_density(game);
    }
    aoe_update_clouds(game, clients, MAX_CLIENTS);
}

/*pickup_tombstone_items has been replaced by tombstone_pickup() in tombstone.c*/
