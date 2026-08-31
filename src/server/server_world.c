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
#include "combat_log.h"
#include "rules.h"
#include "../../include/protocol.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*Energy threshold for NPC action in the Speed/Tick System*/
#define ENERGY_THRESHOLD 10

/*============================================================================
 * Floor Stats Cache — updated incrementally to O(1)
 *
 * Instead of scanning all 50,000 NPCs for each floor on every check
 * density scans (O(N×M)), we maintain per-floor counters updated in O(1)
 * at each NPC event (death, respawn, spawn).
 * ==========================================================================*/
typedef struct {
    int total;  /*Total NPCs allocated on this floor (non-merchants)*/
    int active; /*Active (living) NPCs on this floor*/
} FloorStats;

/*Global cache: one counter per floor + one slot for excluded merchants*/
static FloorStats g_floor_stats[MAX_FLOORS];
static bool       g_floor_stats_dirty = true; /* Force cache rebuild on the first tick */

/*Completely rebuilds cache (called only once at startup
* and after a load from file). Cost: O(N) one-off.*/
void floor_stats_rebuild(NPC *npcs) {
    for (int f = 0; f < MAX_FLOORS; f++) {
        g_floor_stats[f].total  = 0;
        g_floor_stats[f].active = 0;
    }
    for (int i = 0; i < MAX_NPCS; i++) {
        if (npcs[i].template == NULL) {
            continue;
        }
        if (npcs[i].archetype == ARCH_MERCHANT) {
            continue;
        }
        if (npcs[i].respawn_timer < 0) {
            continue;
        }
        int f = npcs[i].floor_id;
        if (f < 0 || f >= MAX_FLOORS) {
            continue;
        }
        g_floor_stats[f].total++;
        if (npcs[i].active) {
            g_floor_stats[f].active++;
        }
    }
    g_floor_stats_dirty = false;
}

/*Update the counter when an NPC dies (decrement active).*/
void floor_stats_npc_died(int floor_id) {
    if (floor_id < 0 || floor_id >= MAX_FLOORS) {
        return;
    }
    if (g_floor_stats[floor_id].active > 0) {
        g_floor_stats[floor_id].active--;
    }
}

/*Updates the counter when an NPC respawns or is spawned.*/
void floor_stats_npc_spawned(int floor_id) {
    if (floor_id < 0 || floor_id >= MAX_FLOORS) {
        return;
    }
    g_floor_stats[floor_id].active++;
}

/*============================================================================
 * update_world — Major world state update
 *
 * Each iteration of the server's main loop must be called.
 * @clients: Array of connected clients.
 * @npcs: Array of active and inactive NPCs.
 * ==========================================================================*/

static void broadcast_game_time(Client *clients) {
  int total_mins = (global_total_turns % 1440);
  int h = (total_mins / 60 + 8) % 24;
  int m = total_mins % 60;

  MsgHeader hdr = {MSG_TIME_SYNC, sizeof(MsgTimeSync)};
  MsgTimeSync ts;
  ts.game_hour = h;
  ts.game_min = m;
  ts.total_turns = global_total_turns;

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active && clients[i].authenticated) {
      net_send(clients[i].sock, &hdr, sizeof(hdr));
      net_send(clients[i].sock, &ts, sizeof(ts));
    }
  }
}

void update_world(Client *clients, NPC *npcs) {
  static long long last_tick_ms = 0;
  static time_t last_autosave = 0;
  time_t now = time(NULL);
  long long now_ms = get_time_ms();

  if (last_autosave == 0) {
    last_autosave = now;
  }

  /*-------------------------------------------------------
   * Full auto-save every minute (60 seconds)
   * -------------------------------------------------------*/
  if (now - last_autosave >= 60) {
    last_autosave = now;
    int saved_count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (clients[i].active && clients[i].authenticated) {
        save_player_data(&clients[i]);
        saved_count++;
      }
    }
    
    /* --- Phase 3: Save Full State (Persistent Voxels and NPCs) --- */
    if (master_world) {
      world_save(master_world, "data/world.dat");
      FILE *fn = fopen("data/npcs.dat", "wb");
      if (fn) {
        /*Compact format: [magic:uint32][count:int][NPC * count][next_id:int][turns:int]
         * Replaces the old format that always wrote 50,000 slots.*/
        const uint32_t magic = 0xDEAD7ECC;
        int used = 0;
        for (int ni = 0; ni < MAX_NPCS; ni++) {
          if (npcs[ni].template != NULL || npcs[ni].active) {
            used++;
          }
        }
        fwrite(&magic, sizeof(uint32_t), 1, fn);
        fwrite(&used, sizeof(int), 1, fn);
        for (int ni = 0; ni < MAX_NPCS; ni++) {
          if (npcs[ni].template != NULL || npcs[ni].active) {
            fwrite(&npcs[ni], sizeof(NPC), 1, fn);
          }
        }
        fwrite(&next_id, sizeof(int), 1, fn);
        fwrite(&global_total_turns, sizeof(int), 1, fn);
        fclose(fn);
      }
      server_log("SYS", "Auto-save: %d players, world and %d NPCs.", saved_count, MAX_NPCS);
    }
  }

  /*--- Phase 4: Advanced Speed/Tick (200ms per tick) ---*/
  if (last_tick_ms == 0) last_tick_ms = now_ms;
  bool new_round = (now_ms - last_tick_ms >= 200);
  if (new_round) {
    last_tick_ms = now_ms;
    global_total_turns++;
    if (global_total_turns % 5 == 0) {
      broadcast_game_time(clients);
    }
    /* -------------------------------------------------------
     * World Events: Global event processing (Phase 5 MMO)
     * ------------------------------------------------------- */
    if (active_event_type == 0) {
        if (global_total_turns % 1500 == 0) { //Every ~300 seconds (5 min real at 5 tick/s)
            active_event_type = 1; // 1 = Skeletons
            event_floor_id = (rand() % 10) + 1; //Floor 1-10
            event_time_left = 1500; // 5 minutes
            event_progress = 0;
            event_goal = 5; // 5 Skeletons
            
            //Broadcast to everyone
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].authenticated) {
                    send_text_to_client(clients[j].sock, "[GLOBAL EVENT] Invasion on floor %d! Kill %d Skeletons! Time: 5 minutes.", event_floor_id, event_goal);
                }
            }
        }
    } else {
        event_time_left--;
        if (event_progress >= event_goal) {
            // Victory
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].authenticated) {
                    send_text_to_client(clients[j].sock, "[GLOBAL EVENT] The invasion has been stopped! Defenders are rewarded.");
                    if (clients[j].floor_id == event_floor_id) {
                        clients[j].gold += 500;
                        send_text_to_client(clients[j].sock, "[SYSTEM] You received 500 gold for your bravery!");
                    }
                }
            }
            active_event_type = 0;
        } else if (event_time_left <= 0) {
            // Defeat
            for (int j = 0; j < MAX_CLIENTS; j++) {
                if (clients[j].active && clients[j].authenticated) {
                    send_text_to_client(clients[j].sock, "[GLOBAL EVENT] Time is up, the monsters retreat into the shadows...");
                }
            }
            active_event_type = 0;
        }
    }

    /*-------------------------------------------------------
* Updates per round: players (dungeon floors)
     * -------------------------------------------------------*/
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (!clients[i].active || !clients[i].authenticated) {
        continue;
      }
      if (clients[i].floor_id <= 0) {
        continue;
      }

      /*--- Light source consumption ---*/
      ItemInstance *ls[] = {
          &clients[i].slot_hand_r, &clients[i].slot_hand_l,
          &clients[i].belt[0],     &clients[i].belt[1],
          &clients[i].belt[2],     &clients[i].belt[3]
      };
      for (int j = 0; j < 6; j++) {
        if (ls[j]->template_idx == -1) {
          continue;
        }
        const ItemTemplate *it = &item_database[ls[j]->template_idx];
        if (it->category != ITEM_LIGHT_SOURCE) {
          continue;
        }
        if (ls[j]->durability > 0) {
          ls[j]->durability--;
          if (ls[j]->durability == 0) {
            send_text_to_client(
                clients[i].sock,
                "[SYSTEM] Your light source has gone out!");
          }
        }
      }

      /* --- Hunger and Exhaustion System --- */
      clients[i].hunger_level++;
      if (clients[i].hunger_level >= HUNGER_MAX) {
        if (global_total_turns % 2 == 0) {
          clients[i].hp -= 2;
          send_text_to_client(clients[i].sock,
                              "[SYSTEM] You're starving!");
          if (clients[i].hp <= 0) {
            server_log("DEATH", "%s starved to death.", clients[i].username);
            save_bones(&clients[i]);
            clients[i].hp = clients[i].max_hp;
            clients[i].floor_id = 0;
            clients[i].x = MAP_CENTER_X + 1;
            clients[i].y = MAP_CENTER_Y + 1;
            send_text_to_client(clients[i].sock, "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
          }
        }
        /*Exhaustion accumulates while you stay hungry*/
        if (global_total_turns % 100 == 0) {
          clients[i].exhaustion_level++;
          send_text_to_client(
              clients[i].sock,
              "[SYSTEM] You have gained a level of Exhaustion!");
        }
      }

      /* --- Update effects and conditions --- */
      if (rules_update_effects(clients[i].effects, &clients[i].effect_count)) {
        send_detailed_state(&clients[i]);
      }

      /*--- Saving throws to recover player condition ---*/
      const char *conditions_to_check[] = {
          "Paralyzed", "Stunned",   "Unconscious",
          "Burning",   "Bleeding",  "Petrified", "Frozen"
      };
      for (int c_idx = 0; c_idx < 7; c_idx++) {
        if (!rules_has_condition(clients[i].effects,
                                 clients[i].effect_count,
                                 conditions_to_check[c_idx])) {
          continue;
        }
        int ts, td, tc, ti, tw, th;
        get_total_stats(&clients[i], &ts, &td, &tc, &ti, &tw, &th);

        /*Attribute used for the saving throw varies by condition*/
        int mod = 0;
        if (strcmp(conditions_to_check[c_idx], "Burning") == 0) {
          mod = rules_get_modifier(td); /*Dexterity: flame reflexes*/
        } else if (strcmp(conditions_to_check[c_idx], "Bleeding") == 0) {
          mod = rules_get_modifier(tc); /* Constitution: stop the bleeding */
        } else if (strcmp(conditions_to_check[c_idx], "Petrified") == 0) {
          mod = rules_get_modifier(ts); /* Strength: free yourself from the stone */
        } else if (strcmp(conditions_to_check[c_idx], "Frozen") == 0) {
          mod = rules_get_modifier(ts); /*Strength: Break the ice*/
        } else {
          mod = rules_get_modifier(tc); /* Default: Constitution */
        }

        /*Disadvantage on saving throws if cursed*/
        bool dis = rules_has_condition(clients[i].effects,
                                       clients[i].effect_count, "Cursed");

        int roll_v = 0;
        bool success = rules_roll_save(mod, 12, false, dis, &roll_v);
        clog_save(clients[i].username, conditions_to_check[c_idx],
                  roll_v, mod, 12, success);

        if (success) {
          for (int e_idx = 0; e_idx < clients[i].effect_count; e_idx++) {
            if (strcasecmp(clients[i].effects[e_idx].name,
                           conditions_to_check[c_idx]) == 0) {
              clients[i].effects[e_idx] =
                  clients[i].effects[clients[i].effect_count - 1];
              clients[i].effect_count--;
              send_text_to_client(
                  clients[i].sock,
                  "[SYSTEM] You have recovered from the %s state!",
                  conditions_to_check[c_idx]);
              send_detailed_state(&clients[i]);
              break;
            }
          }
        }
      }

      /* --- Invisibility notification (every 10 rounds) --- */
      if (rules_has_condition(clients[i].effects,
                              clients[i].effect_count, "Invisible")) {
        if (global_total_turns % 10 == 0) {
          send_text_to_client(clients[i].sock,
                              "[SYSTEM] You are currently invisible.");
        }
      }

      /*--- Player condition periodic effects ---*/
      if (rules_has_condition(clients[i].effects,
                              clients[i].effect_count, "Poisoned")) {
        clients[i].hp -= 1;
        send_text_to_client(
            clients[i].sock,
            "[SYSTEM] Feel the poison coursing through your veins...");
        if (clients[i].hp <= 0) {
          server_log("DEATH", "%s died from poison.", clients[i].username);
            save_bones(&clients[i]);
            clients[i].hp = clients[i].max_hp;
            clients[i].floor_id = 0;
            clients[i].x = MAP_CENTER_X + 1;
            clients[i].y = MAP_CENTER_Y + 1;
            send_text_to_client(clients[i].sock, "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
        }
        send_detailed_state(&clients[i]);
      }

      if (rules_has_condition(clients[i].effects,
                              clients[i].effect_count, "Burning")) {
        /*If in water, turn off immediately*/
        Floor *fl = &master_world->floors[clients[i].floor_id];
        if (fl->map.data[0][clients[i].y][clients[i].x] == VOXEL_WATER) {
          for (int e_idx = 0; e_idx < clients[i].effect_count; e_idx++) {
            if (strcasecmp(clients[i].effects[e_idx].name, "Burning") == 0) {
              clients[i].effects[e_idx] =
                  clients[i].effects[clients[i].effect_count - 1];
              clients[i].effect_count--;
              send_text_to_client(clients[i].sock,
                                  "[SYSTEM] Water extinguishes flames!");
              send_detailed_state(&clients[i]);
              break;
            }
          }
        } else {
          clients[i].hp -= 2;
          send_text_to_client(clients[i].sock, "[SYSTEM] You are burning!");
          if (clients[i].hp <= 0) {
            server_log("DEATH", "%s burned to death.", clients[i].username);
            save_bones(&clients[i]);
            clients[i].hp = clients[i].max_hp;
            clients[i].floor_id = 0;
            clients[i].x = MAP_CENTER_X + 1;
            clients[i].y = MAP_CENTER_Y + 1;
            send_text_to_client(clients[i].sock, "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
          }
          send_detailed_state(&clients[i]);
        }
      }

      if (rules_has_condition(clients[i].effects,
                              clients[i].effect_count, "Bleeding")) {
        clients[i].hp -= 1;
        send_text_to_client(clients[i].sock,
                            "[SYSTEM] You are bleeding...");
        if (clients[i].hp <= 0) {
          server_log("DEATH", "%s bled to death.",
                     clients[i].username);
            save_bones(&clients[i]);
            clients[i].hp = clients[i].max_hp;
            clients[i].floor_id = 0;
            clients[i].x = MAP_CENTER_X + 1;
            clients[i].y = MAP_CENTER_Y + 1;
            send_text_to_client(clients[i].sock, "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
        }
        send_detailed_state(&clients[i]);
      }
    } /*end loop players*/

    /* --- Trap respawn --- */
    for (int f_id = 1; f_id < MAX_FLOORS; f_id++) {
      Floor *f = &master_world->floors[f_id];
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
      Floor *f = &master_world->floors[f_id];
      for (int i = 0; i < f->crystal_respawn_count; i++) {
        CrystalRespawn *cr = &f->crystal_respawns[i];
        if (cr->respawn_timer > 0) {
          cr->respawn_timer--;
          if (cr->respawn_timer == 0) {
            // Restore the crystal in the map
            f->map.data[0][cr->y][cr->x] = cr->type;
            
            // Broadcast VFX to clients
            float r = 1.0f, g = 1.0f, b = 1.0f;
            if (cr->type == VOXEL_CRYSTAL_BLUE) { r = 0.3f; g = 0.7f; b = 1.0f; }
            else if (cr->type == VOXEL_CRYSTAL_PURPLE) { r = 0.8f; g = 0.2f; b = 1.0f; }
            else if (cr->type == VOXEL_CRYSTAL_RED) { r = 1.0f; g = 0.1f; b = 0.1f; }
            else if (cr->type == VOXEL_CRYSTAL_GREEN) { r = 0.1f; g = 1.0f; b = 0.2f; }
            else if (cr->type == VOXEL_CRYSTAL_YELLOW) { r = 1.0f; g = 0.9f; b = 0.1f; }
            else if (cr->type == VOXEL_CRYSTAL_ORANGE) { r = 1.0f; g = 0.5f; b = 0.0f; }
            else if (cr->type == VOXEL_CRYSTAL_CYAN) { r = 0.0f; g = 0.9f; b = 1.0f; }
            
            broadcast_spell_vfx(cr->x, cr->y, cr->x, cr->y, 1, r, g, b, f_id);
            
            // Remove from list
            f->crystal_respawns[i] = f->crystal_respawns[f->crystal_respawn_count - 1];
            f->crystal_respawn_count--;
            i--; // re-check this index
          }
        }
      }
    }
  } /*end new_round block*/

  /* -------------------------------------------------------
   * 1. NPC loop: AI, effects, and respawn timer
   * ------------------------------------------------------- */
  /*Capture the round state before the AI loop: 'new_round' is cleared by
   *the first AI cycle inside the loop (to limit per-tick actions), so the
   *post-loop logic (entity grid sync, density monitor) must use this flag.
   *Otherwise the grid would NOT be re-aligned on exactly the ticks where
   *monsters moved, and the player would pass through them.*/
  bool round_active = new_round;

  for (int i = 0; i < MAX_NPCS; i++) {
    NPC *n = &npcs[i];

    /*Merchants with respawn_timer == -1: Always skipped*/
    if (n->respawn_timer < 0) {
      continue;
    }

    if (n->active) {
      /* Living NPC: update effects and AI */
      if (new_round) {
        rules_update_effects(n->effects, &n->effect_count);

        /* --- Periodic effects: NPC poison --- */
        if (rules_has_condition(n->effects, n->effect_count, "Poisoned")) {
          n->hp -= 1;
          if (n->hp <= 0) {
            n->hp = 0;
            n->active = false;
            n->respawn_timer = 60;
            floor_stats_npc_died(n->floor_id);
            if (n->archetype == ARCH_BOSS) {
              handle_boss_death(NULL, n);
            }
          }
        }

        /* --- NPC Morale: below 20% HP rolls a saving throw vs Frightened --- */
        if (n->hp < (n->max_hp / 5) &&
            !rules_has_condition(n->effects, n->effect_count, "Frightened")) {
          int roll_v = 0;
          bool success = rules_roll_save(0, 12, false, false, &roll_v);
          if (!success) {
            if (n->effect_count < MAX_EFFECTS_PER_ENTITY) {
              n->effects[n->effect_count].name = "Frightened";
              n->effects[n->effect_count].duration_rounds = 5;
              n->effect_count++;
              clog_save(n->template->name, "Morale", roll_v, 0, 12, false);
            }
          }
        }

        /*--- NPC saving throws for condition recovery ---*/
        const char *npc_conditions[] = {
            "Paralyzed", "Stunned", "Unconscious", "Petrified", "Frozen"
        };
        for (int c_idx = 0; c_idx < 5; c_idx++) {
          if (!rules_has_condition(n->effects, n->effect_count,
                                   npc_conditions[c_idx])) {
            continue;
          }
          bool dis = rules_has_condition(n->effects, n->effect_count, "Cursed");
          int roll_v = 0;
          /*Base modifier +2 for NPCs*/
          bool success = rules_roll_save(2, 12, false, dis, &roll_v);
          clog_save(n->template->name, npc_conditions[c_idx],
                    roll_v, 2, 12, success);
          if (success) {
            for (int e_idx = 0; e_idx < n->effect_count; e_idx++) {
              if (strcasecmp(n->effects[e_idx].name,
                             npc_conditions[c_idx]) == 0) {
                n->effects[e_idx] = n->effects[n->effect_count - 1];
                n->effect_count--;
                break;
              }
            }
          }
        }

        /*--- Merchant store replenishment management ---*/
        if (n->archetype == ARCH_MERCHANT) {
          if (n->merchant.restock_timer > 0) {
            n->merchant.restock_timer--;
          } else {
            for (int j = 0; j < n->merchant.item_count; j++) {
              n->merchant.item_stock[j] = n->merchant.item_stock_max[j];
            }
            n->merchant.restock_timer = 100; /*Restock every 100 rounds ≈ 10 min*/
          }
        }
      } /*end new_round for NPC*/

      /*--- Block actions for debilitating conditions ---*/
      if (rules_has_condition(n->effects, n->effect_count, "Paralyzed") ||
          rules_has_condition(n->effects, n->effect_count, "Stunned")    ||
          rules_has_condition(n->effects, n->effect_count, "Petrified")  ||
          rules_has_condition(n->effects, n->effect_count, "Frozen")     ||
          rules_has_condition(n->effects, n->effect_count, "Unconscious")) {
        continue;
      }

      /* --- Speed/Tick System (Phase 4) ---
       * The NPC accumulates energy every tick equal to its speed.
       * It only acts when it has enough energy (threshold = ENERGY_THRESHOLD).
       * Speed 1 = slow (Golem), 2 = normal, 3 = fast (Spider). */
      int npc_speed = (n->template && n->template->speed > 0)
                          ? n->template->speed
                          : 2;
      n->energy += npc_speed;

      while (n->energy >= ENERGY_THRESHOLD) {
        n->energy -= ENERGY_THRESHOLD;
        ai_update_npc(n, clients, MAX_CLIENTS, new_round, npcs);
        new_round = false; /* Only the first cycle is "new_round" */
      }

    } else if (n->template != NULL && !n->is_ghost &&
               n->archetype != ARCH_TREASURE && n->archetype != ARCH_GOLD) {
      /* Dead NPC: respawn countdown */
      if (new_round && n->respawn_timer > 0) {
        n->respawn_timer--;
      }
      if (n->respawn_timer == 0) {
        /* Ensure the spawn tile is free, otherwise delay */
        if (master_world->floors[n->floor_id].entity_grid[n->spawn_y][n->spawn_x] != 0) {
            n->respawn_timer = 5;
            continue;
        }
        /*Revives at the spawn location*/
        n->active = true;
        n->x = n->spawn_x;
        n->y = n->spawn_y;
        n->hp = n->max_hp;
        n->effect_count = 0;
        n->respawn_timer = 0;
        master_world->floors[n->floor_id].entity_grid[n->y][n->x] = n->entity_id;
        ai_init_npc(n, n->template->name, n->floor_id);
        floor_stats_npc_spawned(n->floor_id);
        server_log("SPAWN", "%s respawned on floor %d (%d,%d)",
                   n->template->name, n->floor_id, n->x, n->y);
      }
    }
  } /*end of NPC loop*/

  /*After each tick AI, realigns the entity_grid with the updated positions
   * of the NPCs. Without this, the player passes through the monsters because
   * the grid still holds the positions prior to the AI movement.
   * NOTE: 'round_active' must be used here — 'new_round' is cleared by the
   * first AI cycle in the loop above, which would skip the sync on exactly
   * the tick in which the monsters moved.*/
  if (round_active) {
    sync_entity_grid(npcs);
  }


  /*-------------------------------------------------------
   * 2. Density Monitor: every global DENSITY_CHECK round
   * Optimized with O(1) per-floor cache instead of O(N*M)
   * -------------------------------------------------------*/
  if (round_active && (global_total_turns % DENSITY_CHECK == 0)) {

    /*Rebuild the cache only if reported dirty (first boot or load)*/
    if (g_floor_stats_dirty) {
      floor_stats_rebuild(npcs);
    }

    for (int f = 1; f < MAX_FLOORS; f++) {
      int total  = g_floor_stats[f].total;
      int active = g_floor_stats[f].active;

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
        NPC *n = &npcs[i];
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
        if (master_world->floors[f].entity_grid[n->spawn_y][n->spawn_x] != 0) {
            continue; /* Skip and try another NPC if this spawn spot is blocked */
        }
        n->active       = true;
        n->x            = n->spawn_x;
        n->y            = n->spawn_y;
        n->hp           = n->max_hp;
        n->effect_count = 0;
        n->respawn_timer = 0;
        master_world->floors[f].entity_grid[n->y][n->x] = n->entity_id;
        ai_init_npc(n, n->template->name, n->floor_id);
        floor_stats_npc_spawned(f); /* Update O(1) cache */
        server_log("SPAWN",
                   "[DENSITY] Floor %d (%d%%): emergency %s",
                   f, pct, n->template->name);
        break; /*One at a time so as not to overload*/
      }
    }
  }

  /*-------------------------------------------------------
* 3. AoE persistent clouds update (every round)
   * -------------------------------------------------------*/
  /*'round_active': 'new_round' is cleared by the first AI cycle above.*/
  if (round_active) {
    aoe_update_clouds(npcs, MAX_NPCS, clients, MAX_CLIENTS);
  }
}


/*pickup_tombstone_items has been replaced by tombstone_pickup() in tombstone.c*/
