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
 * cmd_world.c - Exploration commands: look, fountain, statue, search, disarm, examine,
 * tunnel, open, close.
 *
 * Every handler has the CmdFn signature (server_cmd.h): it returns true
 * when it handled the command. The table in server_commands.c decides
 * which handler sees which command.
 */

#include "server_cmd.h"
#include "server_internal.h"
#include "server_world.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

const TempleInfo TEMPLES[] = {
  { CLASS_WIZARD,   145,155, 101,111, "Arcane Academy",          "study"      },
  { CLASS_PALADIN,  176,186, 113,123, "Cathedral of Justice","celebrate"  },
  { CLASS_CLERIC,   189,199, 145,155, "Great Temple",             "pray"       },
  { CLASS_SORCERER, 176,186, 176,186, "Spire of Blood",         "pronounce"  },
  { CLASS_WARLOCK,  145,155, 189,199, "Altar of Pacts",          "invoke"     },
  { CLASS_BARD,     113,123, 176,186, "Conservatory",             "intone"     },
  { CLASS_DRUID,    101,111, 145,155, "Sacred Grove",           "awaken"     },
  { CLASS_RANGER,   113,123, 113,123, "Hunter's Outpost",  "practice"   },
  { CLASS_FIGHTER,  145,155,  67, 77, "Gladiator's Arena",      "drill"      },
  { CLASS_BARBARIAN,223,233, 145,155, "Fighting Pit",   "rage"       },
  { CLASS_ROGUE,    145,155, 223,233, "Den of Shadows",          "sneak"      },
  { CLASS_MONK,      67, 77, 145,155, "Lotus Dojo",             "meditate"   },
};
const int TEMPLE_COUNT = (int)(sizeof(TEMPLES) / sizeof(TEMPLES[0]));

static const char *get_voxel_name_it(VoxelType vt) {
  switch (vt) {
  case VOXEL_FLOOR:
    return "Stone floor";
  case VOXEL_COBBLE:
    return "Cobblestone street";
  case VOXEL_MARBLE:
    return "Marble floor";
  case VOXEL_WOOD:
    return "Wooden plank";
  case VOXEL_GRASS:
    return "Grassy Terrain";
  case VOXEL_SAND:
    return "Sand";
  case VOXEL_MUD:
    return "Mud";
  case VOXEL_WATER:
    return "Clear Water";
  case VOXEL_ICE:
    return "Icy Surface";
  case VOXEL_LAVA:
    return "Glowing Lava";
  case VOXEL_STAIRS_DOWN:
    return "Stairs to go down ( > )";
  case VOXEL_STAIRS_UP:
    return "Stairs to go up ( < )";
  case VOXEL_DOOR:
    return "Door";
  case VOXEL_TRAP:
    return "Hidden Trap";
  case VOXEL_ROCK:
    return "Solid Rock";
  case VOXEL_WALL:
    return "Stone Wall";
  case VOXEL_ASH:
    return "Ash";
  case VOXEL_OBSIDIAN:
    return "Obsidian";
  case VOXEL_MUSHROOM_GLOW:
    return "Bioluminescent Mushrooms";
  case VOXEL_CRYSTAL_BLUE:
  case VOXEL_CRYSTAL_PURPLE:
  case VOXEL_CRYSTAL_RED:
  case VOXEL_CRYSTAL_GREEN:
  case VOXEL_CRYSTAL_YELLOW:
  case VOXEL_CRYSTAL_ORANGE:
  case VOXEL_CRYSTAL_CYAN:
  case VOXEL_CRYSTAL_WHITE:
    return "Magic Crystal";
  default:
    return "Unknown Terrain";
  }
}

static void evaluate_digging_potential(int nx, int ny, int f, VoxelType vt, char *buf, size_t buf_size) {
  if (vt == VOXEL_GOLD_VEIN) {
    snprintf(buf, buf_size, "⚡ VERY RICH: Pure Gold Vein! Highly recommended dig!");
  } else if (vt >= VOXEL_CRYSTAL_BLUE && vt <= VOXEL_CRYSTAL_WHITE) {
    snprintf(buf, buf_size, "✨ PRECIOUS: Raw magic crystal! Extraction recommended!");
  } else if (vt == VOXEL_WALL || vt == VOXEL_ROCK) {
    unsigned int hash = (unsigned int)(nx * 73 + ny * 37 + f * 19 + 17);
    int chance = hash % 100;
    if (chance < 15) {
      snprintf(buf, buf_size, "💎 VERY PROSPEROUS: Silver and Quartz veins detected in the wall!");
    } else if (chance < 35) {
      snprintf(buf, buf_size, "🪙 PROSPEROUS: Traces of metallic minerals and pyrite.");
    } else {
      snprintf(buf, buf_size, "🪨 STERILE: Ordinary hard rock. Risk of blunting the tool.");
    }
  } else if (vt == VOXEL_MUD || vt == VOXEL_SAND || vt == VOXEL_ASH || vt == VOXEL_GRASS) {
    unsigned int hash = (unsigned int)(nx * 31 + ny * 47 + f * 13 + 5);
    int chance = hash % 100;
    if (chance < 20) {
      snprintf(buf, buf_size, "🏺 PROSPEROUS: Sediment rich in raw nuggets or finds!");
    } else {
      snprintf(buf, buf_size, "🌱 SOIL: Soft soil, easy but poor excavation.");
    }
  } else {
    snprintf(buf, buf_size, "➖ REGULAR: Non-mining surface.");
  }
}

bool cmd_look(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  int vis_rad = get_vision_radius(c);
  int px = c->x;
  int py = c->y;
  int f = c->floor_id;

  if (vis_rad <= 0) {
    send_text_to_client(c->sock, "[INSPECTION] You can't see anything: you are surrounded by darkness or blinded!");
    return true;
  }

  //--- 1. PRECISE INSPECTION UNDERFOOT ---
  VoxelType feet_vt = master_world->floors[f].map.data[0][py][px];
  char feet_buf[512];
  snprintf(feet_buf, sizeof(feet_buf), "%s", get_voxel_name_it(feet_vt));

  //Temple on floor 0
  if (f == 0) {
    for (int i = 0; i < TEMPLE_COUNT; i++) {
      const TempleInfo *t = &TEMPLES[i];
      if (px >= t->x0 && px <= t->x1 && py >= t->y0 && py <= t->y1) {
        strncat(feet_buf, " | Temple: ", sizeof(feet_buf) - strlen(feet_buf) - 1);
        strncat(feet_buf, t->name, sizeof(feet_buf) - strlen(feet_buf) - 1);
        break;
      }
    }
  }

  //Tombstone underfoot or adjacent
  for (int i = 0; i < MAX_TOMBSTONES; i++) {
    if (g_tombstones[i].active &&
        g_tombstones[i].floor_id == f &&
        abs(g_tombstones[i].x - px) + abs(g_tombstones[i].y - py) <= 1) {
      char tbuf[96];
      snprintf(tbuf, sizeof(tbuf), "| [⚰ Tombstone of %s — type 'list' to inspect]",
               g_tombstones[i].owner);
      strncat(feet_buf, tbuf, sizeof(feet_buf) - strlen(feet_buf) - 1);
    }
  }

  //Objects / Entities underfoot
  FloorIter walk = floor_iter(npcs, f);
  for (int i; (i = floor_iter_next(&walk)) >= 0;) {
    if (npcs[i].x == px && npcs[i].y == py) {

      if (npcs[i].archetype == ARCH_GOLD) {
        strncat(feet_buf, "| [Golden Bag]", sizeof(feet_buf) - strlen(feet_buf) - 1);
      } else if (npcs[i].archetype == ARCH_TREASURE) {
        strncat(feet_buf, "| [Treasury Chest]", sizeof(feet_buf) - strlen(feet_buf) - 1);
      } else if (npcs[i].is_ghost) {
        char gbuf[96]; /*room for a 63-char name + decoration*/
        snprintf(gbuf, sizeof(gbuf), "| [%s remains]", npcs[i].custom_name);
        strncat(feet_buf, gbuf, sizeof(feet_buf) - strlen(feet_buf) - 1);
      } else if (npcs[i].archetype == ARCH_MERCHANT) {
        char mbuf[96];
        snprintf(mbuf, sizeof(mbuf), " | [%s]", npcs[i].merchant.shop_name);
        strncat(feet_buf, mbuf, sizeof(feet_buf) - strlen(feet_buf) - 1);
      } else if (npcs[i].template) {
        char nbuf[64];
        snprintf(nbuf, sizeof(nbuf), " | [%s HP:%d/%d]", npcs[i].template->name, npcs[i].hp, npcs[i].max_hp);
        strncat(feet_buf, nbuf, sizeof(feet_buf) - strlen(feet_buf) - 1);
      }
    }
  }

  //Other players underfoot
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].active && g_clients[i].authenticated && &g_clients[i] != c &&
        g_clients[i].floor_id == f && g_clients[i].x == px && g_clients[i].y == py) {
      char pbuf[64];
      snprintf(pbuf, sizeof(pbuf), "| [Player: %s]", g_clients[i].username);
      strncat(feet_buf, pbuf, sizeof(feet_buf) - strlen(feet_buf) - 1);
    }
  }

  //--- 2. SECTOR INSPECTION (N, NE, E, SE, S, SW, W, NW) ---
  //Sector mapping: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
  const char *sector_names[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  char sec_buf[8][256];
  int sec_counts[8] = {0};
  for (int s = 0; s < 8; s++) {
    sec_buf[s][0] = '\0';
  }

  //Grid scan in line of sight
  for (int dy = -vis_rad; dy <= vis_rad; dy++) {
    for (int dx = -vis_rad; dx <= vis_rad; dx++) {
      if (dx == 0 && dy == 0) continue;
      int nx = px + dx;
      int ny = py + dy;
      if (nx < 0 || nx >= MAP_WIDTH || ny < 0 || ny >= MAP_HEIGHT) continue;

      float dist = sqrtf((float)(dx * dx + dy * dy));
      if (dist > (float)vis_rad) continue;
      int dist_steps = (int)roundf(dist);

      //Angular sector calculation (negative dy = North)
      float dy_map = -(float)dy;
      float deg = atan2f(dy_map, (float)dx) * (180.0f / 3.1415926535f);
      int sector = 2; // E
      if (deg >= 67.5f && deg < 112.5f) sector = 0;       // N
      else if (deg >= 22.5f && deg < 67.5f) sector = 1;   // NE
      else if (deg >= -22.5f && deg < 22.5f) sector = 2;  // E
      else if (deg >= -67.5f && deg < -22.5f) sector = 3; // SE
      else if (deg >= -112.5f && deg < -67.5f) sector = 4;// S
      else if (deg >= -157.5f && deg < -112.5f) sector = 5;// SW
      else if (deg >= 157.5f || deg < -157.5f) sector = 6;// W
      else if (deg >= 112.5f && deg < 157.5f) sector = 7; // NW

      if (strlen(sec_buf[sector]) > 180) continue;

      //A) Check monsters/NPCs in cell (nx, ny)
      FloorIter walk = floor_iter(npcs, f);
      for (int i; (i = floor_iter_next(&walk)) >= 0;) {
        if (npcs[i].x == nx && npcs[i].y == ny) {
          char entry[96];
          if (npcs[i].archetype == ARCH_MERCHANT) {
            snprintf(entry, sizeof(entry), "%s (%dp)", npcs[i].merchant.shop_name, dist_steps);
          } else if (npcs[i].archetype == ARCH_GOLD) {
            snprintf(entry, sizeof(entry), "Gold (%dp)", dist_steps);
          } else if (npcs[i].archetype == ARCH_TREASURE) {
            snprintf(entry, sizeof(entry), "Treasure (%dp)", dist_steps);
          } else if (npcs[i].is_ghost) {
            snprintf(entry, sizeof(entry), "Remains-%s (%dp)", npcs[i].custom_name, dist_steps);
          } else if (npcs[i].template) {
            snprintf(entry, sizeof(entry), "%s (%dp)", npcs[i].template->name, dist_steps);
          } else {
            snprintf(entry, sizeof(entry), "Entity (%dp)", dist_steps);
          }
          if (sec_counts[sector] > 0) strncat(sec_buf[sector], ", ", sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
          strncat(sec_buf[sector], entry, sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
          sec_counts[sector]++;
        }
      }

      //B) Control other players in the cell (nx, ny)
      for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].authenticated && &g_clients[i] != c &&
            g_clients[i].floor_id == f && g_clients[i].x == nx && g_clients[i].y == ny) {
          char entry[96];
          snprintf(entry, sizeof(entry), "Player-%s (%dp)", g_clients[i].username, dist_steps);
          if (sec_counts[sector] > 0) strncat(sec_buf[sector], ", ", sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
          strncat(sec_buf[sector], entry, sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
          sec_counts[sector]++;
        }
      }

      //C) Check notable elements of the map (nx, ny)
      VoxelType vt = master_world->floors[f].map.data[0][ny][nx];
      if (vt == VOXEL_STAIRS_DOWN) {
        char entry[64];
        snprintf(entry, sizeof(entry), "Stairs-Down (%dp)", dist_steps);
        if (sec_counts[sector] > 0) strncat(sec_buf[sector], ", ", sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
        strncat(sec_buf[sector], entry, sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
        sec_counts[sector]++;
      } else if (vt == VOXEL_STAIRS_UP) {
        char entry[64];
        snprintf(entry, sizeof(entry), "Stairs-Up (%dp)", dist_steps);
        if (sec_counts[sector] > 0) strncat(sec_buf[sector], ", ", sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
        strncat(sec_buf[sector], entry, sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
        sec_counts[sector]++;
      } else if (f == 0 && (vt == VOXEL_WATER || vt == VOXEL_ICE)) {
        char entry[64];
        snprintf(entry, sizeof(entry), "Sacred-Fountain (%dp)", dist_steps);
        if (sec_counts[sector] > 0) strncat(sec_buf[sector], ", ", sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
        strncat(sec_buf[sector], entry, sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
        sec_counts[sector]++;
      } else if (vt >= VOXEL_CRYSTAL_BLUE && vt <= VOXEL_CRYSTAL_WHITE) {
        char entry[64];
        snprintf(entry, sizeof(entry), "Crystal (%dp)", dist_steps);
        if (sec_counts[sector] > 0) strncat(sec_buf[sector], ", ", sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
        strncat(sec_buf[sector], entry, sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
        sec_counts[sector]++;
      } else if (vt == VOXEL_TRAP) {
        char entry[64];
        snprintf(entry, sizeof(entry), "Trap (%dp)", dist_steps);
        if (sec_counts[sector] > 0) strncat(sec_buf[sector], ", ", sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
        strncat(sec_buf[sector], entry, sizeof(sec_buf[sector]) - strlen(sec_buf[sector]) - 1);
        sec_counts[sector]++;
      }
    }
  }

  //--- 3. TABLE OUTPUT TO CLIENT ---
  send_text_to_client(c->sock, "[INSPECTION] Illuminated View Range: %d Steps (%d Floor)", vis_rad, f);
  send_text_to_client(c->sock, "[UNDER FEET] Position (%d, %d): %s", px, py, feet_buf);
  send_text_to_client(c->sock, "+--------+-------------------------------------------------------------+");
  send_text_to_client(c->sock, "| SECTOR| DETECTIONS IN THE ILLUMINATED AREA |");
  send_text_to_client(c->sock, "+--------+-------------------------------------------------------------+");

  for (int s = 0; s < 8; s++) {
    if (sec_counts[s] == 0) {
      send_text_to_client(c->sock, "| %-6s | Free                                                        |", sector_names[s]);
    } else {
      send_text_to_client(c->sock, "| %-6s | %-59.59s |", sector_names[s], sec_buf[s]);
    }
  }
  send_text_to_client(c->sock, "+--------+-------------------------------------------------------------+");
  return true;
}

bool cmd_fountain(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  if (c->floor_id == 0) {
    bool near_fountain = false;
    for (int dy = -3; dy <= 3; dy++) {
      for (int dx = -3; dx <= 3; dx++) {
        int nx = c->x + dx;
        int ny = c->y + dy;
        if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
          VoxelType vt = master_world->floors[0].map.data[0][ny][nx];
          if (vt == VOXEL_WATER || vt == VOXEL_ICE) near_fountain = true;
        }
      }
    }
    if (near_fountain) {
      c->hp = c->max_hp;
      send_detailed_state(c); //Sync HP with the HUD client
      send_text_to_client(c->sock, "[SYSTEM] Drink the water from the sacred fountain and feel your wounds magically heal!");
    } else {
      send_text_to_client(c->sock, "[ERROR] You are not near any healing fountains.");
    }
  } else {
    send_text_to_client(c->sock, "[ERROR] Healing fountains are only found in the Magic Sanctuary.");
  }
  return true;
}

bool cmd_statue(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  if (c->floor_id == 0) {
    bool near_crystal = false;
    for (int dy = -3; dy <= 3; dy++) {
      for (int dx = -3; dx <= 3; dx++) {
        int nx = c->x + dx;
        int ny = c->y + dy;
        if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
          VoxelType vt = master_world->floors[0].map.data[0][ny][nx];
          if ((vt >= VOXEL_CRYSTAL_BLUE && vt <= VOXEL_CRYSTAL_PURPLE) ||
              (vt >= VOXEL_CRYSTAL_RED && vt <= VOXEL_CRYSTAL_WHITE)) near_crystal = true;
        }
      }
    }
    if (near_crystal) {
      for (int sl = 1; sl <= 9; sl++) {
        c->spell_slots[sl] = c->spell_slots_max[sl];
      }
      c->hunger_level = 0;
      c->exhaustion_level = 0;
      send_detailed_state(c); //Sync spell slots, hunger and exhaustion with the HUD client
      send_text_to_client(c->sock, "[SYSTEM] Meditate at the foot of the crystal monument. Your spirit is renewed, chasing away fatigue, hunger and recharging your spells!");
    } else {
      send_text_to_client(c->sock, "[ERROR] You are not near any crystal statues.");
    }
  } else {
    send_text_to_client(c->sock, "[ERROR] Restoration statues are only found in the Magic Sanctuary.");
  }
  return true;
}

bool cmd_search(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  (void)npcs;
  int roll = (rand() % 20) + 1;
  int perception = roll + rules_get_modifier(c->wis);
  bool found_something = false;

  Floor *f = &master_world->floors[c->floor_id];
  for (int i = 0; i < f->trap_count; i++) {
    Trap *t = &f->traps[i];
    if (!t->active || t->detected) continue;

    int check_x = t->x, check_y = t->y;
    if (t->type == 1) { // TRAP_DART_WALL
        int wall_dx[4] = {0, 1, 0, -1};
        int wall_dy[4] = {-1, 0, 1, 0};
        check_x += wall_dx[t->wall_dir];
        check_y += wall_dy[t->wall_dir];
    }

    if (abs(c->x - check_x) <= 2 && abs(c->y - check_y) <= 2) {
      if (perception >= t->detection_dc) {
        t->detected = true;
        found_something = true;
        send_text_to_client(c->sock, "[PERCEPTION] You have discovered a hidden trap! (Roll: %d vs DC: %d)", perception, t->detection_dc);
      }
    }
  }

  if (!found_something) {
    send_text_to_client(c->sock, "[SYSTEM] You examine the area carefully, but you don't notice anything unusual.");
  }
  return true;
}

bool cmd_disarm(Client *c, const char *cmd, NPC *npcs) {
  Floor *f = &master_world->floors[c->floor_id];
  Trap *target_trap = NULL;
  int min_dist = 999;

  // Sub-mode parsing
  bool is_salvage = (strstr(cmd, "salvage") != NULL || strstr(cmd, " r") != NULL);
  bool is_magic = (strstr(cmd, "magic") != NULL || strstr(cmd, "magic") != NULL || strstr(cmd, " m") != NULL);

  //Search for adjacent or nearby traps (distance <= 2)
  for (int i = 0; i < f->trap_count; i++) {
    Trap *t = &f->traps[i];
    if (!t->active) continue;

    int tx = t->x, ty = t->y;
    if (t->type == TRAP_DART_WALL || t->type == TRAP_SPRING_SPEAR) {
      int wall_dx[4] = {0, 1, 0, -1};
      int wall_dy[4] = {-1, 0, 1, 0};
      tx += wall_dx[t->wall_dir];
      ty += wall_dy[t->wall_dir];
    }

    int dist = abs(c->x - tx) + abs(c->y - ty);
    if (dist <= 2) {
      if (t->detected && dist < min_dist) {
        min_dist = dist;
        target_trap = t;
      } else if (!target_trap && dist < min_dist) {
        min_dist = dist;
        target_trap = t;
      }
    }
  }

  if (!target_trap) {
    send_text_to_client(c->sock, "[ERROR] No traps detected in range. Use 'search' to find one.");
    return true;
  }

  target_trap->detected = true;

  //--- DRAGON GL RULES AND MODIFIERS ---
  int roll = (rand() % 20) + 1;
  int dc = target_trap->detection_dc > 0 ? target_trap->detection_dc : 12;
  int total_mod = 0;
  const char *method_name = "Manual Dexterity";

  //I'll check if it's a magic trap
  bool is_trap_magical = (target_trap->type == TRAP_CURSE_RUNE || target_trap->type == TRAP_ANTIMAGIC_ZONE ||
                          target_trap->type == TRAP_ILLUSION_MIRROR || target_trap->type == TRAP_INVERSION_RUNE ||
                          target_trap->type == TRAP_DARKNESS || target_trap->type == TRAP_SILENCE ||
                          target_trap->type == TRAP_TELEPORT || target_trap->type == TRAP_CRYSTAL_BURST);

  if (is_magic || is_trap_magical) {
    method_name = "Arcane Dissipation";
    int main_stat = (c->intel > c->wis) ? c->intel : c->wis;
    total_mod = rules_get_modifier(main_stat);

    //Consume 1 spell slot for Arcane Focus (+4)
    if (c->spell_slots[1] > 0) {
      c->spell_slots[1]--;
      total_mod += 4;
      send_text_to_client(c->sock, "[MAGIC] You channel your arcane energy (-1 Slot Lv.1, Focus +4)!");
    }
  } else {
    total_mod = rules_get_modifier(c->dex);
    int prof_bonus = 2 + (c->level - 1) / 4;

    //Mastery Bonus for Thieves or Precision Kits
    if (c->class_id == CLASS_ROGUE) {
      total_mod += prof_bonus * 2; // Tactical Mastery
    } else {
      bool has_tools = false;
      for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx != -1) {
          const char *iname = item_database[c->backpack[i].template_idx].name;
          if (strcasestr(iname, "Tool") || strcasestr(iname, "Lockpick") || strcasestr(iname, "Kit")) {
            has_tools = true;
            break;
          }
        }
      }
      if (has_tools) total_mod += prof_bonus;
    }
  }

  if (is_salvage) {
    dc += 3; //Tactical Recovery Mode: +3 DC to extract intact materials
  }

  int total_check = roll + total_mod;

  // --- OPERATION OUTCOME ---
  if (roll == 20 || total_check >= dc) {
    target_trap->active = false;
    target_trap->detected = false;
    target_trap->respawn_timer = RESPAWN_TRAPS_TICKS;

    if (f->map.data[0][target_trap->y][target_trap->x] == VOXEL_TRAP) {
      f->map.data[0][target_trap->y][target_trap->x] = VOXEL_FLOOR;
    }

    int xp_reward = 25 + c->floor_id * 5;
    c->xp += xp_reward;

    if (roll == 20) {
      send_text_to_client(c->sock, "[DISARMS] CRITICAL SUCCESS! [%s] (Roll: 20 Nat + Mod: %d = %d vs DC: %d)", method_name, total_mod, total_check, dc);
      send_text_to_client(c->sock, "[DISARMS] With extraordinary skill you dismantle the trap safely (+%d XP)!", xp_reward * 2);
      c->xp += xp_reward;
    } else {
      send_text_to_client(c->sock, "[DISARMS] SUCCESS! [%s] (Roll: %d + Mod: %d = %d vs DC: %d)", method_name, roll, total_mod, total_check, dc);
      send_text_to_client(c->sock, "[DISARMS] You successfully neutralize the trap (+%d XP).", xp_reward);
    }

    //Extra reward if in Tactical Recovery mode
    if (is_salvage) {
      uint64_t gold_salvage = 40 + c->floor_id * 15 + (rand() % 30);
      c->gold += gold_salvage;
      send_text_to_client(c->sock, "[RECOVERY] Extract valuable materials and alchemical resources from the mechanism (+%lu Gold Coins)!", (unsigned long)gold_salvage);
    }
  } else if (roll == 1 || total_check <= dc - 5) {
    send_text_to_client(c->sock, "[DISARMS] CATASTROPHIC FAILURE! [%s] (Roll: %d + Mod: %d = %d vs DC: %d)", method_name, roll, total_mod, total_check, dc);
    send_text_to_client(c->sock, "[DANGER] An execution error triggers the trap!");

    //Shield Protection: If a shield is equipped, it absorbs physical impact damage
    bool has_shield = ((c->slot_hand_r.template_idx != -1 && item_database[c->slot_hand_r.template_idx].category == ITEM_SHIELD) ||
                       (c->slot_hand_l.template_idx != -1 && item_database[c->slot_hand_l.template_idx].category == ITEM_SHIELD));
    if (has_shield && !is_trap_magical) {
      send_text_to_client(c->sock, "[DEFENSE] You instinctively raise your shield to protect yourself from the trap's blow!");
    }

    int orig_x = c->x, orig_y = c->y;
    c->x = target_trap->x;
    c->y = target_trap->y;
    check_traps(c, npcs);
    c->x = orig_x;
    c->y = orig_y;
  } else {
    send_text_to_client(c->sock, "[DISARMS] FAILURE! [%s] (Roll: %d + Mod: %d = %d vs DC: %d)", method_name, roll, total_mod, total_check, dc);
    send_text_to_client(c->sock, "[DISARMS] The attempt fails, but you retreat in time before triggering the mechanism.");
  }
  return true;
}

bool cmd_examine(Client *c, const char *cmd, NPC *npcs) {
  (void)cmd;
  send_text_to_client(c->sock, "[DETAILED EXAMINATION] Area 1 step (Position %d, %d - Floor %d)", c->x, c->y, c->floor_id);
  send_text_to_client(c->sock, "+--------+-----------------------+-------------------------------------------------------+");
  send_text_to_client(c->sock, "| SQUARE. | TYPE GROUND / WALL | MINING EVALUATION AND ATTENDANCE |");
  send_text_to_client(c->sock, "+--------+-----------------------+-------------------------------------------------------+");

  int qx[] = {0, 0, 1, 1, 1, 0, -1, -1, -1};
  int qy[] = {0, -1, -1, 0, 1, 1, 1, 0, -1};
  const char *qnames[] = {"FEET", "N", "NE", "E", "SE", "S", "SW", "W", "NW"};

  Floor *f = &master_world->floors[c->floor_id];

  for (int i = 0; i < 9; i++) {
    int nx = c->x + qx[i];
    int ny = c->y + qy[i];
    if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
      VoxelType vt = f->map.data[0][ny][nx];
      const char *vname = get_voxel_name_it(vt);

      char v_min[128];
      evaluate_digging_potential(nx, ny, c->floor_id, vt, v_min, sizeof(v_min));

      char presence[256] = "";

      FloorIter walk = floor_iter(npcs, c->floor_id);
      for (int m; (m = floor_iter_next(&walk)) >= 0;) {
        if (npcs[m].x == nx && npcs[m].y == ny) {
          if (npcs[m].archetype == ARCH_GOLD) {
            strncat(presence, "| [Golden Bag]", sizeof(presence) - strlen(presence) - 1);
          } else if (npcs[m].archetype == ARCH_TREASURE) {
            strncat(presence, "| [Treasury Chest]", sizeof(presence) - strlen(presence) - 1);
          } else if (npcs[m].is_ghost) {
            char gbuf[96]; /*room for a 63-char name + decoration*/
            snprintf(gbuf, sizeof(gbuf), "| [%s remains]", npcs[m].custom_name);
            strncat(presence, gbuf, sizeof(presence) - strlen(presence) - 1);
          } else if (npcs[m].archetype == ARCH_MERCHANT) {
            char mbuf[96];
            snprintf(mbuf, sizeof(mbuf), " | [%s]", npcs[m].merchant.shop_name);
            strncat(presence, mbuf, sizeof(presence) - strlen(presence) - 1);
          } else if (npcs[m].template) {
            char nbuf[128];
            snprintf(nbuf, sizeof(nbuf), " | %s (HP:%d/%d)", npcs[m].template->name, npcs[m].hp, npcs[m].max_hp);
            strncat(presence, nbuf, sizeof(presence) - strlen(presence) - 1);
          }
        }
      }

      char combined[512];
      snprintf(combined, sizeof(combined), "%s%s", v_min, presence);

      send_text_to_client(c->sock, "| %-6s | %-21s | %-53s |", qnames[i], vname, combined);
    }
  }

  send_text_to_client(c->sock, "+--------+-----------------------+-------------------------------------------------------+");

  bool has_mining_tool = false;
  char tool_name[64] = "";
  ItemInstance *hands[] = {&c->slot_hand_r, &c->slot_hand_l};
  for (int i = 0; i < 2; i++) {
    if (hands[i]->template_idx != -1) {
      const char *iname = item_database[hands[i]->template_idx].name;
      if (strcasestr(iname, "Pick") ||
          strcasestr(iname, "Shovel") ||
          strcasestr(iname, "Hammer") ||
          strcasestr(iname, "Mining") ||
          strcasestr(iname, "Tool")) {
        has_mining_tool = true;
        snprintf(tool_name, sizeof(tool_name), "%s", iname);
        break;
      }
    }
  }
  if (!has_mining_tool) {
    for (int i = 0; i < c->backpack_count; i++) {
      if (c->backpack[i].template_idx != -1) {
        const char *iname = item_database[c->backpack[i].template_idx].name;
        if (strcasestr(iname, "Pick") ||
            strcasestr(iname, "Shovel") ||
            strcasestr(iname, "Hammer") ||
            strcasestr(iname, "Mining") ||
            strcasestr(iname, "Tool")) {
          has_mining_tool = true;
          snprintf(tool_name, sizeof(tool_name), "%s", iname);
          break;
        }
      }
    }
  }

  if (has_mining_tool) {
    send_text_to_client(c->sock, "[TOOLS] You have the right tool (%s). Use 'tunnel <dir>' to dig!", tool_name);
  } else {
    send_text_to_client(c->sock, "[TOOLS] No pickaxes or shovels. Get a tool for extracting minerals and treasures.");
  }

  return true;
}

/*"t " (lowercase) is the short alias for tunnel directions: 't n' =
 * tunnel north.*/
bool cmd_tunnel(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  const char *dir_str = strchr(cmd, ' ');
  if (!dir_str) {
    send_text_to_client(c->sock, "[SYSTEM] Use: tunnel <n|s|e|w> or 'tunnel d' (down/under your feet)");
    return true;
  }
  dir_str++;
  while (*dir_str == ' ') dir_str++;

  int tx = c->x, ty = c->y;
  bool digging_down = false;

  if (*dir_str == 'n') ty--;
  else if (*dir_str == 's') ty++;
  else if (*dir_str == 'e') tx++;
  else if (*dir_str == 'w') tx--;
  else if (*dir_str == 'd' || *dir_str == 'g') {
    digging_down = true;
  } else {
    send_text_to_client(c->sock, "[SYSTEM] Invalid direction. Use: n, s, e, w, or d (down)");
    return true;
  }

  Floor *f = &master_world->floors[c->floor_id];
  if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) {
     send_text_to_client(c->sock, "[SYSTEM] Beyond borders.");
     return true;
  }

  VoxelType vt = f->map.data[0][ty][tx];

  bool has_tool = false;
  ItemInstance *hands[] = {&c->slot_hand_r, &c->slot_hand_l};
  for (int i = 0; i < 2; i++) {
    if (hands[i]->template_idx != -1) {
      const char *iname = item_database[hands[i]->template_idx].name;
      if (strcasestr(iname, "Pick") || strcasestr(iname, "Piccone") || strcasestr(iname, "Shovel") ||
          strcasestr(iname, "Pala") || strcasestr(iname, "Hammer") || strcasestr(iname, "Martello") ||
          strcasestr(iname, "Mining") || strcasestr(iname, "Tool")) {
        has_tool = true; break;
      }
    }
  }
  if (!has_tool) {
    for (int i = 0; i < c->backpack_count; i++) {
      if (c->backpack[i].template_idx != -1) {
        const char *iname = item_database[c->backpack[i].template_idx].name;
        if (strcasestr(iname, "Pick") || strcasestr(iname, "Piccone") || strcasestr(iname, "Shovel") ||
            strcasestr(iname, "Pala") || strcasestr(iname, "Hammer") || strcasestr(iname, "Martello") ||
            strcasestr(iname, "Mining") || strcasestr(iname, "Tool")) {
          has_tool = true; break;
        }
      }
    }
  }

  if (vt == VOXEL_GOLD_VEIN) {
    if (!has_tool) {
      send_text_to_client(c->sock, "[MINE] The gold vein is too hard to mine bare-handed. You need a pickaxe!");
      return true;
    }
    int gold_yield = (50 + rand() % 100) * (c->floor_id > 0 ? c->floor_id : 1);
    c->gold += gold_yield;
    c->xp += 15;
    f->map.data[0][ty][tx] = VOXEL_FLOOR;
    broadcast_spell_vfx(tx, ty, tx, ty, 1, 0.8f, 0.7f, 0.1f, c->floor_id); // Gold explosion
    send_text_to_client(c->sock, "[MINE] You have successfully mined pure gold from the vein! (+%d Gold Coins, +15 XP)", gold_yield);
  } else if (vt >= VOXEL_CRYSTAL_BLUE && vt <= VOXEL_CRYSTAL_WHITE) {
    if (!has_tool) {
      send_text_to_client(c->sock, "[MINE] The magic crystal is as hard as diamond. You need a proper tool!");
      return true;
    }
    int gold_yield = (100 + rand() % 200) * (c->floor_id > 0 ? c->floor_id : 1);
    c->gold += gold_yield;
    c->xp += 30;
    f->map.data[0][ty][tx] = VOXEL_FLOOR;

    // Track for respawn
    if (f->crystal_respawn_count < 100) {
        f->crystal_respawns[f->crystal_respawn_count].x = tx;
        f->crystal_respawns[f->crystal_respawn_count].y = ty;
        f->crystal_respawns[f->crystal_respawn_count].type = vt;
        f->crystal_respawns[f->crystal_respawn_count].respawn_timer = 50 + (rand() % 50); // 50-100 ticks
        f->crystal_respawn_count++;
    }

    //Generate VFX based on crystal color
    float cr = 1.0f, cg = 1.0f, cb = 1.0f;
    if (vt == VOXEL_CRYSTAL_BLUE) { cr = 0.3f; cg = 0.7f; cb = 1.0f; }
    else if (vt == VOXEL_CRYSTAL_PURPLE) { cr = 0.8f; cg = 0.2f; cb = 1.0f; }
    else if (vt == VOXEL_CRYSTAL_RED) { cr = 1.0f; cg = 0.1f; cb = 0.1f; }
    else if (vt == VOXEL_CRYSTAL_GREEN) { cr = 0.1f; cg = 1.0f; cb = 0.2f; }
    else if (vt == VOXEL_CRYSTAL_YELLOW) { cr = 1.0f; cg = 0.9f; cb = 0.1f; }
    else if (vt == VOXEL_CRYSTAL_ORANGE) { cr = 1.0f; cg = 0.5f; cb = 0.0f; }
    else if (vt == VOXEL_CRYSTAL_CYAN) { cr = 0.0f; cg = 0.9f; cb = 1.0f; }

    broadcast_spell_vfx(tx, ty, tx, ty, 1, cr, cg, cb, c->floor_id); // Explosion

    send_text_to_client(c->sock, "[MINE] You have shattered the crystal and extracted precious fragments! (+%d Gold, +30 XP)", gold_yield);
  } else if (vt == VOXEL_MUSHROOM_GLOW) {
    c->hp = (c->hp + 15 > c->max_hp) ? c->max_hp : c->hp + 15;
    f->map.data[0][ty][tx] = VOXEL_FLOOR;
    send_detailed_state(c); //Sync HP with the HUD client
    send_text_to_client(c->sock, "[MINE] Extract the glowing mushrooms and absorb their life essence (+15 HP).");
  } else if (vt == VOXEL_WALL || vt == VOXEL_ROCK) {
    if (!has_tool && (rand() % 100 < 80)) {
      send_text_to_client(c->sock, "[MINE] Digging rock bare-handed is exhausting and painful... You can't break through.");
      c->hp -= 1;
      if (c->hp < 1) c->hp = 1;
      send_detailed_state(c); //Sync HP with the HUD client
      return true;
    }
    f->map.data[0][ty][tx] = VOXEL_FLOOR;
    broadcast_spell_vfx(tx, ty, tx, ty, 1, 0.6f, 0.6f, 0.6f, c->floor_id); // Grey rock explosion

    unsigned int hash = (unsigned int)(tx * 73 + ty * 37 + c->floor_id * 19 + 17);
    if (has_tool && (hash % 100 < 15)) {
        int bonus = 20 * (c->floor_id > 0 ? c->floor_id : 1);
        c->gold += bonus;
        send_text_to_client(c->sock, "[MINE] While digging the rock you found a small supply of silver! (+%d Gold)", bonus);
    } else {
        send_text_to_client(c->sock, "[SYSTEM] You have carved out the rock, opening a passage!");
    }
  } else if (digging_down && (vt == VOXEL_MUD || vt == VOXEL_SAND || vt == VOXEL_ASH || vt == VOXEL_GRASS || vt == VOXEL_FLOOR)) {
    if (!has_tool) {
      send_text_to_client(c->sock, "[MINE] Digging the ground without a shovel or pickaxe takes too much time and effort.");
      return true;
    }

    unsigned int hash = (unsigned int)(tx * 31 + ty * 47 + c->floor_id * 13 + 5);
    if (vt != VOXEL_COBBLE) {
        f->map.data[0][ty][tx] = VOXEL_COBBLE;
        if (hash % 100 < 20) {
            int bonus = 10 * (c->floor_id > 0 ? c->floor_id : 1);
            c->gold += bonus;
            send_text_to_client(c->sock, "[MINE] While digging in the loose soil you found some lost coins! (+%d Gold)", bonus);
        } else {
            send_text_to_client(c->sock, "[MINE] You disturbed the ground, but found nothing of value.");
        }
    } else {
        send_text_to_client(c->sock, "[MINE] This ground has already been dug out (loose rubble).");
    }
  } else {
    send_text_to_client(c->sock, "[SYSTEM] There is nothing to dig or mine there.");
  }
  return true;
}

/*--- Door memory ---
 * 'close' may ONLY turn back a tile that this server turned from a door
 * into floor with 'open': closing any floor tile would let a player seal
 * corridors for good (the map is saved). The registry is per floor,
 * bounded, and volatile (after a restart an opened door stays open).*/
static struct { int x, y; }
    g_recently_opened_doors[MAX_FLOORS][32];
static int g_recently_opened_door_n[MAX_FLOORS] = {0};

bool cmd_open(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  const char *dir_str = strchr(cmd, ' ');
  if (!dir_str) {
    send_text_to_client(c->sock, "[SYSTEM] Use: open <n|s|e|w>");
    return true;
  }
  dir_str++;
  int tx = c->x;
  int ty = c->y;
  if (*dir_str == 'n') {
    ty--;
  } else if (*dir_str == 's') {
    ty++;
  } else if (*dir_str == 'e') {
    tx++;
  } else if (*dir_str == 'w') {
    tx--;
  } else {
    send_text_to_client(c->sock, "[SYSTEM] Use: open <n|s|e|w>");
    return true;
  }
  /*Bounds: at the map edge tx/ty can step outside the array.*/
  if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) {
    send_text_to_client(c->sock, "[SYSTEM] Beyond borders.");
    return true;
  }
  Floor *f = &master_world->floors[c->floor_id];
  if (f->map.data[0][ty][tx] == VOXEL_DOOR) {
    f->map.data[0][ty][tx] = VOXEL_FLOOR;
    /*Remember it so 'close' can put the door back.*/
    bool known = false;
    for (int i = 0; i < g_recently_opened_door_n[c->floor_id]; i++) {
      if (g_recently_opened_doors[c->floor_id][i].x == tx &&
          g_recently_opened_doors[c->floor_id][i].y == ty) {
        known = true;
        break;
      }
    }
    if (!known && g_recently_opened_door_n[c->floor_id] < 32) {
      g_recently_opened_doors[c->floor_id]
                               [g_recently_opened_door_n[c->floor_id]].x = tx;
      g_recently_opened_doors[c->floor_id]
                               [g_recently_opened_door_n[c->floor_id]].y = ty;
      g_recently_opened_door_n[c->floor_id]++;
    }
    send_text_to_client(c->sock, "[SYSTEM] You opened the door.");
  } else {
    send_text_to_client(c->sock, "[SYSTEM] There is no door there.");
  }
  return true;
}

bool cmd_close(Client *c, const char *cmd, NPC *npcs) {
  (void)npcs;
  const char *dir_str = strchr(cmd, ' ');
  if (!dir_str) {
    send_text_to_client(c->sock, "[SYSTEM] Use: close <n|s|e|w>");
    return true;
  }
  dir_str++;
  int tx = c->x;
  int ty = c->y;
  if (*dir_str == 'n') {
    ty--;
  } else if (*dir_str == 's') {
    ty++;
  } else if (*dir_str == 'e') {
    tx++;
  } else if (*dir_str == 'w') {
    tx--;
  } else {
    send_text_to_client(c->sock, "[SYSTEM] Use: close <n|s|e|w>");
    return true;
  }
  if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) {
    send_text_to_client(c->sock, "[SYSTEM] Beyond borders.");
    return true;
  }
  Floor *f = &master_world->floors[c->floor_id];
  /*Only a floor tile present in the "recently opened doors" registry
   * can become a door again.*/
  bool was_door = false;
  for (int i = 0; i < g_recently_opened_door_n[c->floor_id]; i++) {
    if (g_recently_opened_doors[c->floor_id][i].x == tx &&
        g_recently_opened_doors[c->floor_id][i].y == ty) {
      /*Remove it from the registry.*/
      g_recently_opened_doors[c->floor_id][i] =
          g_recently_opened_doors[c->floor_id]
                                 [g_recently_opened_door_n[c->floor_id] - 1];
      g_recently_opened_door_n[c->floor_id]--;
      was_door = true;
      break;
    }
  }
  if (f->map.data[0][ty][tx] == VOXEL_FLOOR && was_door) {
    f->map.data[0][ty][tx] = VOXEL_DOOR;
    send_text_to_client(c->sock, "[SYSTEM] You closed the door.");
  } else {
    send_text_to_client(c->sock,
        "[SYSTEM] There is no open door to close there.");
  }
  return true;
}
