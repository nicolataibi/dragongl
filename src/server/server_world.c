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

/*Single point for NPC alive/dead transitions (see the comment in
 * server_world.h). The stats update mirrors floor_stats_rebuild() EXACTLY
 * (template != NULL, not a merchant, respawn_timer >= 0, floor in range):
 * a slot that the rebuild would not count must not move the counters
 * either, otherwise every summon/drop/corpse-reuse would leak one count
 * per transition. Callers must invoke it with the NPC's FINAL fields
 * already set (template, floor_id, archetype), not before.*/
void npc_set_active(NPC *n, bool active) {
    if (!n || n->active == active) {
        return;
    }
    n->active = active;
    if (n->archetype == ARCH_MERCHANT) {
        return; /*excluded from the cache, like in floor_stats_rebuild*/
    }
    if (n->template == NULL || n->respawn_timer < 0) {
        return; /*not counted by the rebuild: nothing to keep in sync*/
    }
    if (n->floor_id < 0 || n->floor_id >= MAX_FLOORS) {
        return;
    }
    if (active) {
        floor_stats_npc_spawned(n->floor_id);
    } else {
        floor_stats_npc_died(n->floor_id);
    }
}

/*============================================================================
 * Per-floor active NPC index
 *
 * Local queries used to scan all MAX_NPCS (50,000) slots on every player
 * step (entity broadcast) and on every attack (aggro) — tens of thousands
 * of wasted comparisons each. Instead we keep one index array per floor
 * with the slots of the active NPCs and rebuild it once per world tick
 * (5/s: 50k simple checks = negligible).
 *
 * The index is deliberately dumb (no incremental insert/remove): NPC
 * spawn/death/move happen in many places, and a full rebuild every tick
 * is simpler and strictly cheaper than keeping bookkeeping at every
 * mutation site. Between ticks a consumer validates each entry
 * (active && same floor), so the worst case is a few stale checks.*/
#define FLOOR_INDEX_CAP 4096
static int  g_floor_idx[MAX_FLOORS][FLOOR_INDEX_CAP];
static int  g_floor_idx_n[MAX_FLOORS];
static bool g_floor_idx_overflow[MAX_FLOORS];

void floor_index_rebuild(const NPC *npcs) {
    for (int f = 0; f < MAX_FLOORS; f++) {
        g_floor_idx_n[f] = 0;
        g_floor_idx_overflow[f] = false;
    }
    for (int i = 0; i < MAX_NPCS; i++) {
        const NPC *n = &npcs[i];
        if (!n->active) {
            continue;
        }
        int f = n->floor_id;
        if (f < 0 || f >= MAX_FLOORS) {
            continue;
        }
        if (g_floor_idx_overflow[f]) {
            continue;
        }
        if (g_floor_idx_n[f] >= FLOOR_INDEX_CAP) {
            /* Pathologically crowded floor: mark it and let consumers
             * fall back to the legacy full scan (correctness first). */
            g_floor_idx_overflow[f] = true;
            continue;
        }
        g_floor_idx[f][g_floor_idx_n[f]++] = i;
    }
}

/*Returns the index of the floor's active NPCs, or NULL when the floor
 * overflowed the cap (the caller must then do the full scan).*/
const int *floor_index_for(int floor_id, int *count_out) {
    if (floor_id < 0 || floor_id >= MAX_FLOORS) {
        if (count_out) *count_out = 0;
        return NULL;
    }
    if (g_floor_idx_overflow[floor_id]) {
        if (count_out) *count_out = -1;
        return NULL;
    }
    if (count_out) *count_out = g_floor_idx_n[floor_id];
    return g_floor_idx[floor_id];
}

/*============================================================================
 * update_world — Major world state update
 *
 * Each iteration of the server's main loop must be called.
 * @clients: Array of connected clients.
 * @npcs: Array of active and inactive NPCs.
 * ==========================================================================*/

/*Respawn a dead player in town, keeping entity_grid consistent and the
 * client in sync.
 *
 * The old code (six death sites: starvation/poison/burn/bleed in
 * update_world, bleed during MSG_MOVE, perform_attack_npc and the mage
 * AI) set hp/floor_id/x/y by hand and did NOT:
 *   - clear the player's cell on the OLD floor (it stayed "occupied"
 *     forever, blocking that tile for other entities);
 *   - register the player in the NEW floor's grid (invisible to
 *     collision detection until the next sync);
 *   - send the respawned player anything (his client kept showing the
 *     dungeon until the next move, and the town map was never sent,
 *     so the first view after death was pure rock).
 * This helper does all of it in one place.*/
void player_respawn_town(Client *c) {
  if (!c || !master_world)
    return;
  int old_floor = c->floor_id;
  if (old_floor >= 0 && old_floor < MAX_FLOORS &&
      c->x >= 0 && c->x < MAP_WIDTH && c->y >= 0 && c->y < MAP_HEIGHT &&
      master_world->floors[old_floor].entity_grid[c->y][c->x] == c->entity_id) {
    master_world->floors[old_floor].entity_grid[c->y][c->x] = 0;
  }
  c->hp = c->max_hp;
  c->floor_id = 0;
  c->x = MAP_CENTER_X + 1;
  c->y = MAP_CENTER_Y + 1;
  if (master_world->floors[0].entity_grid[c->y][c->x] == 0)
    master_world->floors[0].entity_grid[c->y][c->x] = c->entity_id;
  notify_player_left_floor(c, old_floor);
  broadcast_player_state(c);
  send_detailed_state(c);
  send_map_chunk(c->sock, &master_world->floors[0].map, c->x, c->y,
                 INITIAL_VIEW_RADIUS);
  broadcast_nearby_entities(c, g_npcs);
}

static void broadcast_game_time(Client *clients) {
  int total_mins = (global_total_turns % 1440);
  int h = (total_mins / 60 + 8) % 24;
  int m = total_mins % 60;

  MsgHeader hdr = msg_hdr(MSG_TIME_SYNC, (int)sizeof(MsgTimeSync));
  MsgTimeSync ts;
  ts.game_hour = h;
  ts.game_min = m;
  ts.total_turns = global_total_turns;

  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active && clients[i].authenticated) {
      /*net_send_client: a failed time-sync send means a stuck/dead
       * client — drop it instead of letting the message vanish (L2).*/
      net_send_client(clients[i].sock, &hdr, sizeof(hdr));
      net_send_client(clients[i].sock, &ts, sizeof(ts));
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
      char world_path[DATA_DIR_MAX + 32], npcs_path[DATA_DIR_MAX + 32];
      snprintf(world_path, sizeof(world_path), "%s/world.dat", g_data_dir);
      snprintf(npcs_path, sizeof(npcs_path), "%s/npcs.dat", g_data_dir);
      world_save(master_world, world_path);
      /*Atomic write (see atomic_write_begin): a crash mid-autosave must
       * not leave a truncated npcs.dat that the next boot would treat
       * as "world/NPCs inconsistent -> fresh world" (total state loss).*/
      char tmp[DATA_DIR_MAX + 64];
      FILE *fn = atomic_write_begin(npcs_path, tmp, sizeof(tmp));
      if (fn) {
        /*Compact format: [magic:uint32][count:int][NPC * count][next_id:int][turns:int]
         * Replaces the old format that always wrote 50,000 slots.*/
        const uint32_t magic = 0xDEAD7ECC;
        int used = 0;
        bool ok = true;
        for (int ni = 0; ni < MAX_NPCS; ni++) {
          if (npcs[ni].template != NULL || npcs[ni].active) {
            used++;
          }
        }
        ok = ok && (fwrite(&magic, sizeof(uint32_t), 1, fn) == 1);
        ok = ok && (fwrite(&used, sizeof(int), 1, fn) == 1);
        for (int ni = 0; ni < MAX_NPCS; ni++) {
          if (npcs[ni].template != NULL || npcs[ni].active) {
            ok = ok && (fwrite(&npcs[ni], sizeof(NPC), 1, fn) == 1);
          }
        }
        ok = ok && (fwrite(&next_id, sizeof(int), 1, fn) == 1);
        ok = ok && (fwrite(&global_total_turns, sizeof(int), 1, fn) == 1);
        if (!atomic_write_end(fn, npcs_path, tmp, ok))
          server_log("SYS", "WARNING: autosave of npcs.dat failed — previous file kept.");
      }
      server_log("SYS", "Auto-save: %d players, world and %d NPCs.", saved_count, MAX_NPCS);
    }
  }

  /*--- Phase 4: Advanced Speed/Tick (TICK_MS per tick) ---
   * The main loop already calls update_world() on the fixed-step clock,
   * so this gate opens on (almost) every call; it stays as the
   * authoritative definition of "one round passed".*/
  if (last_tick_ms == 0) last_tick_ms = now_ms;
  bool new_round = (now_ms - last_tick_ms >= TICK_MS);
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
            player_respawn_town(&clients[i]);
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
        bool dis = rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_CURSED);

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
      if (rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_INVISIBLE)) {
        if (global_total_turns % 10 == 0) {
          send_text_to_client(clients[i].sock,
                              "[SYSTEM] You are currently invisible.");
        }
      }

      /*--- Player condition periodic effects ---*/
      if (rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_POISONED)) {
        clients[i].hp -= 1;
        send_text_to_client(
            clients[i].sock,
            "[SYSTEM] Feel the poison coursing through your veins...");
        if (clients[i].hp <= 0) {
          server_log("DEATH", "%s died from poison.", clients[i].username);
            save_bones(&clients[i]);
            player_respawn_town(&clients[i]);
            send_text_to_client(clients[i].sock, "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
        }
        send_detailed_state(&clients[i]);
      }

      if (rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_BURNING)) {
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
            player_respawn_town(&clients[i]);
            send_text_to_client(clients[i].sock, "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
          }
          send_detailed_state(&clients[i]);
        }
      }

      if (rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_BLEEDING)) {
        clients[i].hp -= 1;
        send_text_to_client(clients[i].sock,
                            "[SYSTEM] You are bleeding...");
        if (clients[i].hp <= 0) {
          server_log("DEATH", "%s bled to death.",
                     clients[i].username);
            save_bones(&clients[i]);
            player_respawn_town(&clients[i]);
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
  /*Round state captured for the WHOLE loop.
   * The old code let the first NPC clear 'new_round' mid-loop, which broke
   * three things at once:
   *  - only the FIRST NPC of the tick decayed its effects (poison that
   *    should last 5 rounds lasted dozens more);
   *  - only ONE NPC per whole tick (5 ticks/s) could attack: a pack of 10
   *    monsters hit once per second instead of 10 times;
   *  - dead NPCs after the first acting one in the array did not count
   *    down their respawn timer on that tick.
   * Per-NPC "one action per round" is now enforced locally with
   * first_cycle below, so this flag must never be cleared mid-loop.*/
  bool was_new_round = new_round;
  /*Kept for the post-loop logic (entity grid sync, density monitor,
   * AoE clouds): true on exactly the ticks in which monsters may have
   * moved, so the grid gets re-aligned on the right ticks.*/
  bool round_active = was_new_round;

  for (int i = 0; i < MAX_NPCS; i++) {
    NPC *n = &npcs[i];

    /*Merchants with respawn_timer == -1: Always skipped*/
    if (n->respawn_timer < 0) {
      continue;
    }

    if (n->active) {
      /* Living NPC: update effects and AI */
      if (was_new_round) {
        rules_update_effects(n->effects, &n->effect_count);

        /* --- Periodic effects: NPC poison --- */
        if (rules_has_condition_t(n->effects, n->effect_count, COND_POISONED)) {
          n->hp -= 1;
          if (n->hp <= 0) {
            n->hp = 0;
            npc_set_active(n, false);
            n->respawn_timer = 60;
            if (n->archetype == ARCH_BOSS) {
              handle_boss_death(NULL, n);
            }
          }
        }

        /* --- NPC Morale: below 20% HP rolls a saving throw vs Frightened --- */
        if (n->hp < (n->max_hp / 5) && n->morale == 0) {
          n->morale = 1;
          int roll_v = 0;
          bool success = rules_roll_save(0, 12, false, false, &roll_v);
          if (!success) {
            if (n->effect_count < MAX_EFFECTS_PER_ENTITY) {
              n->effects[n->effect_count].name = "Frightened";
              n->effects[n->effect_count].duration_rounds = 5;
              n->effect_count++;
              // clog_save(n->template->name, "Morale", roll_v, 0, 12, false);
            }
          }
        }

        /*--- NPC saving throws for condition recovery ---*/
        /*Compared BY VALUE (ConditionType): the display name comes from
         *the canonical table, so a renamed string can't break the logic.*/
        const ConditionType npc_conditions[] = {
            COND_PARALYZED, COND_STUNNED, COND_UNCONSCIOUS,
            COND_PETRIFIED, COND_FROZEN
        };
        for (int c_idx = 0; c_idx < 5; c_idx++) {
          if (!rules_has_condition_t(n->effects, n->effect_count,
                                     npc_conditions[c_idx])) {
            continue;
          }
          bool dis = rules_has_condition_t(n->effects, n->effect_count, COND_CURSED);
          int roll_v = 0;
          /*Base modifier +2 for NPCs*/
          bool success = rules_roll_save(2, 12, false, dis, &roll_v);
          /*npc_name(): an NPC with effects but template==NULL (a
           * summoned elemental under a cloud) must not deref the
           * template.*/
          clog_save(npc_name(n), condition_to_name(npc_conditions[c_idx]),
                    roll_v, 2, 12, success);
          if (success) {
            const char *cond_name = condition_to_name(npc_conditions[c_idx]);
            for (int e_idx = 0; e_idx < n->effect_count; e_idx++) {
              if (strcasecmp(n->effects[e_idx].name, cond_name) == 0) {
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
      if (rules_has_condition_t(n->effects, n->effect_count, COND_PARALYZED) ||
          rules_has_condition_t(n->effects, n->effect_count, COND_STUNNED)    ||
          rules_has_condition_t(n->effects, n->effect_count, COND_PETRIFIED)  ||
          rules_has_condition_t(n->effects, n->effect_count, COND_FROZEN)     ||
          rules_has_condition_t(n->effects, n->effect_count, COND_UNCONSCIOUS)) {
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

      /*Each NPC gets at most ONE action (attack/cast) per round: the
       * first energy cycle of THIS round runs with was_new_round, extra
       * cycles (fast NPCs with surplus energy) only move.*/
      bool first_cycle = true;
      while (n->energy >= ENERGY_THRESHOLD) {
        n->energy -= ENERGY_THRESHOLD;
        ai_update_npc(n, clients, MAX_CLIENTS,
                      was_new_round && first_cycle, npcs);
        first_cycle = false;
      }

    } else if (n->template != NULL && !n->is_ghost &&
               n->archetype != ARCH_TREASURE && n->archetype != ARCH_GOLD) {
      /* Dead NPC: respawn countdown */
      if (was_new_round && n->respawn_timer > 0) {
        n->respawn_timer--;
      }
      if (n->respawn_timer == 0) {
        /*Bounds: a corrupted npcs.dat can carry an out-of-range floor
         * or spawn point. Leave the slot dead (the density monitor
         * skips it too) instead of indexing the world out of bounds.*/
        if (n->floor_id < 0 || n->floor_id >= MAX_FLOORS ||
            n->spawn_x < 0 || n->spawn_x >= MAP_WIDTH ||
            n->spawn_y < 0 || n->spawn_y >= MAP_HEIGHT) {
            continue;
        }
        /* Ensure the spawn tile is free, otherwise delay */
        if (master_world->floors[n->floor_id].entity_grid[n->spawn_y][n->spawn_x] != 0) {
            n->respawn_timer = 5;
            continue;
        }
        /*Revives at the spawn location (npc_set_active updates the
         * O(1) floor cache in the same step — see A2).*/
        npc_set_active(n, true);
        n->x = n->spawn_x;
        n->y = n->spawn_y;
        n->hp = n->max_hp;
        n->effect_count = 0;
        n->morale = 0;
        n->respawn_timer = 0;
        master_world->floors[n->floor_id].entity_grid[n->y][n->x] = n->entity_id;
        ai_init_npc(n, n->template->name, n->floor_id);
        server_log("SPAWN", "%s respawned on floor %d (%d,%d)",
                   n->template->name, n->floor_id, n->x, n->y);
      }
    }
  } /*end of NPC loop*/

  /*Rebuild the per-floor active-NPC index (spawn/death/split changed
   * during the loop). 5/s * 50k cheap checks — the price that turns
   * every local query (broadcast, aggro, swarm split) from O(50k) into
   * O(entities on the floor).*/
  floor_index_rebuild(npcs);

  /*After each tick AI, realigns the entity_grid with the updated positions
   * of the NPCs. Without this, the player passes through the monsters because
   * the grid still holds the positions prior to the AI movement.*/
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
        if (n->spawn_x < 0 || n->spawn_x >= MAP_WIDTH ||
            n->spawn_y < 0 || n->spawn_y >= MAP_HEIGHT) {
          continue; /*corrupted spawn point: never index the grid OOB*/
        }
        if (master_world->floors[f].entity_grid[n->spawn_y][n->spawn_x] != 0) {
            continue; /* Skip and try another NPC if this spawn spot is blocked */
        }
        npc_set_active(n, true); /* Updates the O(1) cache (A2) */
        n->x            = n->spawn_x;
        n->y            = n->spawn_y;
        n->hp           = n->max_hp;
        n->effect_count = 0;
        n->morale       = 0;
        n->respawn_timer = 0;
        master_world->floors[f].entity_grid[n->y][n->x] = n->entity_id;
        ai_init_npc(n, n->template->name, n->floor_id);
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
