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
 * server_commands.c — Management of text commands (Player and DM)
 *
 * Contains the parser and command execution implementation
 * textual messages sent by clients, including inspection commands (look, inventory),
 * interaction (buy, sell, wear), spells (cast, spells) and DM commands.*/

#include "server_commands.h"
#include "server_internal.h"
#include "server_combat.h"
#include "server_spawn.h"
#include "bestiary.h"
#include "classes.h"
#include "data_loader.h"
#include "items.h"
#include "net.h"
#include "protocol.h"
#include "rules.h"
#include "species.h"
#include "spells.h"
#include "ai.h"
#include "aoe.h"
#include "combat_log.h"
#include "spell_router.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

/*Compact structure for temple table*/
typedef struct { int cls; int x0,x1,y0,y1; const char *name; const char *ritual; } TempleInfo;
static const TempleInfo TEMPLES[] = {
  { CLASS_WIZARD,   145,155, 101,111, "Arcane Academy",          "study"      },
  { CLASS_PALADIN,  176,186, 113,123, "Cathedral of Justice","celebrate"  },
  { CLASS_CLERIC,   189,199, 145,155, "Great Temple",             "pray"       },
  { CLASS_SORCERER, 176,186, 176,186, "Spire of Blood",         "pronounce"  },
  { CLASS_WARLOCK,  145,155, 189,199, "Altar of Pacts",          "invoke"     },
  { CLASS_BARD,     113,123, 176,186, "Conservatory",             "intone"     },
  { CLASS_DRUID,    101,111, 145,155, "Sacred Grove",           "awaken"     },
  { CLASS_RANGER,   113,123, 113,123, "Hunter's Outpost",  "practice"   },
  { CLASS_FIGHTER,  144,155,  67, 78, "Gladiator's Arena",      "drill"      },
  { CLASS_BARBARIAN,223,234, 145,156, "Fighting Pit",   "rage"       },
  { CLASS_ROGUE,    145,156, 223,234, "Den of Shadows",          "sneak"      },
  { CLASS_MONK,      67, 78, 145,156, "Lotus Dojo",             "meditate"   },
};
static const int TEMPLE_COUNT = 12;

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

typedef struct {
    char username[32];
    int level;
    int xp;
    uint32_t bosses_defeated;
} LeaderboardEntry;

static int compare_leaderboard(const void *a, const void *b) {
    const LeaderboardEntry *ea = (const LeaderboardEntry *)a;
    const LeaderboardEntry *eb = (const LeaderboardEntry *)b;
    //Primary Sorting: Number of bosses defeated
    int bosses_a = __builtin_popcount(ea->bosses_defeated);
    int bosses_b = __builtin_popcount(eb->bosses_defeated);
    if (bosses_b != bosses_a) return bosses_b - bosses_a;
    //Secondary sorting: level
    if (eb->level != ea->level) return eb->level - ea->level;
    //Tertiary sorting: XP
    return eb->xp - ea->xp;
}

/* ====================================================================
 * SISTEMA MESSAGGISTICA (msg/tell/whisper, say, who, block/unblock)
 *
* Private messages and local chat go through the MSG_TEXT channel
* existing: the client does not require any changes. The list of
* block is persistent (SaveData) and prevents both messages
* both private and the blocked player's local chat.
 * ==================================================================== */

#define MSG_LOCAL_RADIUS 12     /*Radius (Chebyshev distance) for 'say'*/
#define MSG_MIN_INTERVAL_MS 500 /*Minimum interval between two msg/say (anti-spam)*/

/*Find a player online by username (case-insensitive).
*Returns NULL if absent or unauthenticated.*/
static Client *find_online_player(const char *name) {
  if (!name || name[0] == '\0')
    return NULL;
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].active && g_clients[i].authenticated &&
        strcasecmp(g_clients[i].username, name) == 0) {
      return &g_clients[i];
    }
  }
  return NULL;
}

/*True if 'listener' has 'speaker' in its block list.
*DMs cannot be blocked.*/
static bool player_is_blocked(const Client *listener, const char *speaker) {
  if (listener->is_dm)
    return false;
  for (int i = 0; i < listener->blocked_count; i++) {
    if (strcasecmp(listener->blocked_players[i], speaker) == 0)
      return true;
  }
  return false;
}

/*Anti-spam: true if the client can send another message now.*/
static bool msg_rate_ok(Client *c) {
  long long now = get_time_ms();
  if (now - c->last_msg_ms < MSG_MIN_INTERVAL_MS)
    return false;
  c->last_msg_ms = now;
  return true;
}

/*Removes leading and trailing spaces in-place. Returns the beginning of the string.*/
static char *trim_spaces(char *s) {
  while (*s == ' ')
    s++;
  char *end = s + strlen(s);
  while (end > s && end[-1] == ' ')
    *--end = '\0';
  return s;
}

void handle_text_cmd(Client *c, const char *cmd, NPC *npcs) {
  if (!c || !cmd) {
    return;
  }
  if (cmd[0] == '/') {
    cmd++; //Ignore the leading slash (/) for tolerance
  }
  server_log("CMD", "%s: %s", c->username, cmd);
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    send_text_to_client(c->sock, "[HELP] --- BASIC COMMANDS ---");
    send_text_to_client(c->sock, "  l, look         : Observe the surrounding environment");
    send_text_to_client(c->sock, "  ex, examine     : Examine terrain and minerals 1 step away");
    send_text_to_client(c->sock, "stats : Shows the character's statistics");
    send_text_to_client(c->sock, "s, search : Search for secret passages or traps");
    send_text_to_client(c->sock, "train <stat> : Use level up stat points (e.g. train str / random)");
    send_text_to_client(c->sock, "T, tunnel: Dig the wall in front of you");
    send_text_to_client(c->sock, "or, open : Open a closed door");
    send_text_to_client(c->sock, "c, close : Close an open door");
    send_text_to_client(c->sock, "D, disarm : Disarm at trap");
    send_text_to_client(c->sock, "R, rest : Rest to recover HP/Mana");
    send_text_to_client(c->sock, "fountain : Drink from the sacred fountain to heal wounds (Floor 0)");
    send_text_to_client(c->sock, "statue/crystal : Meditate at the crystal statues (Floor 0)");
    send_text_to_client(c->sock, "save : Save the game");
    
    send_text_to_client(c->sock, "[HELP] --- ITEMS AND INVENTORY ---");
    send_text_to_client(c->sock, "i, inventory : Show inventory");
    send_text_to_client(c->sock, "  w, wield/wear   : Wield/wear an item (e.g. 'w 2')");
    send_text_to_client(c->sock, "t, takeoff/rem : Remove equipped or belted item (e.g. 't 3')");
    send_text_to_client(c->sock, "belt <n> : Move an item from the backpack to the speed belt");
    send_text_to_client(c->sock, "unbelt <n> : Move an item from the belt to the backpack");
    send_text_to_client(c->sock, "d, drop <n> : Drops object <n> to the ground");
    send_text_to_client(c->sock, "v, throw <n> : Throw an object (e.g. 'v 1')");
    send_text_to_client(c->sock, "fill <n> : Refills a lantern with oil (e.g. 'fill lantern')");
    send_text_to_client(c->sock, "use <n> : Use an item (potions, food, wands)");
    send_text_to_client(c->sock, "q, quaff <n> : Drink a potion (aka for 'use')");
    send_text_to_client(c->sock, "  r, read <n>     : Read book/scroll (e.g. 'read 1')");
    send_text_to_client(c->sock, "e, eat <n> : Eat food (aka for 'use')");
    send_text_to_client(c->sock, "z, zap <n> : Use wand (aka for 'use')");

    send_text_to_client(c->sock, "[HELP] --- MAGIC AND CURES ---");
    send_text_to_client(c->sock, "spells : Show your library (spells known)");
    send_text_to_client(c->sock, "cast <spell> : Cast a spell from the library (e.g. 'cast Fire Bolt')");
    send_text_to_client(c->sock, "study <n> : Learn magic (Wizard)");
    send_text_to_client(c->sock, "pray <n> : Learn spells (Cleric)");
    send_text_to_client(c->sock, "awaken <n> : Learn magic (Druid)");
    send_text_to_client(c->sock, "invoke <n> : Learn magic (Warlock)");
    send_text_to_client(c->sock, "intone <n> : Learn magic (Bard)");
    send_text_to_client(c->sock, "celebrate <n> : Learn magic (Paladin)");
    send_text_to_client(c->sock, "pronounce <n> : Learn magic (Sorcerer)");
    send_text_to_client(c->sock, "practice <n> : Learn magic (Ranger)");
    send_text_to_client(c->sock, "  drill <n>       : Execute maneuvers (Fighter)");
    send_text_to_client(c->sock, "rage <n> : Channel fury (Barbarian)");
    send_text_to_client(c->sock, "  sneak <n>       : Hone shadows (Rogue)");
    send_text_to_client(c->sock, "  meditate <n>    : Learn disciplines (Monk)");
    send_text_to_client(c->sock, "lore <n> : Examine a book or inventory item");
    send_text_to_client(c->sock, "cures: Cure diseases/poisons at the temple (cost: gold)");
    send_text_to_client(c->sock, "uncurse : Remove curses from the temple (cost: gold)");
    send_text_to_client(c->sock, "retrieve : Retrieve your items and soul from your ghost");
    
    send_text_to_client(c->sock, "[HELP] --- INTERACTIONS WITH NPCs AND STORES ---");
    send_text_to_client(c->sock, "  talk            : Talk with nearby merchant or NPC");
    send_text_to_client(c->sock, "list : Show goods for sale");
    send_text_to_client(c->sock, "browse <n> : Examine an item for sale");
    send_text_to_client(c->sock, "buy <n> : Buys the item for sale (e.g. 'buy sword')");
    send_text_to_client(c->sock, "sell <n> : Sell an item (e.g. 'sell dagger')");
    send_text_to_client(c->sock, "haggle buy/sell : Haggle to get a better price");
    send_text_to_client(c->sock, "identify <n> : Identifies an unknown object");
    send_text_to_client(c->sock, "repair <n> : Repairs a damaged object");
    
    send_text_to_client(c->sock, "[HELP] --- TRADE BETWEEN PLAYERS ---");
    send_text_to_client(c->sock, "trade <name> : Request to trade with a nearby player");
    send_text_to_client(c->sock, "offer <gold> [id]: Offer gold and/or an item (from your backpack)");
    send_text_to_client(c->sock, "accept : Accept the exchange offer");
    send_text_to_client(c->sock, "cancel : Cancel the exchange");

    send_text_to_client(c->sock, "[HELP] --- MESSAGING AND SOCIAL ---");
    send_text_to_client(
        c->sock,
        "msg <name> <text> : Private message to an online player (aka 'tell'/'whisper')");
    send_text_to_client(
        c->sock,
        "say <text> : Say something to nearby players on this floor (aka 'shout')");
    send_text_to_client(c->sock, "who : List the players currently online");
    send_text_to_client(
        c->sock,
        "block <name> : Block a player (their messages will no longer reach you)");
    send_text_to_client(
        c->sock, "unblock <name> : Remove a player from the block list");
    send_text_to_client(c->sock, "blocks : Show your block list");

    send_text_to_client(c->sock, "[HELP] --- EVENTS AND RANKING ---");
    send_text_to_client(c->sock, "event : Shows the status of the current global event");
    send_text_to_client(c->sock, "top : Shows the global ranking of players");

    if (c->is_dm) {
        send_text_to_client(c->sock, "[DM] --- MASTER COMMANDS ---");
        send_text_to_client(c->sock, "  dm_spawn <id> <x> <y> : Spawn entity/monster");
        send_text_to_client(c->sock, "dm_goto <fl> <x> <y> : Teleport (fl = floor)");
        send_text_to_client(c->sock, "dm_find_monster <id> : Search for the monster");
        send_text_to_client(c->sock, "  dm_find_item <id>     : Search for an item");
        send_text_to_client(c->sock, "dm_pdf <text> : Generate PDF (Lore/Log)");
        send_text_to_client(c->sock, "dm_mapfloor : Generate color PDF of the current floor");
        send_text_to_client(c->sock, "dm_shop : Spawns a custom merchant");
        send_text_to_client(c->sock, "  dm_place <id>         : Place world object (furniture)");
        send_text_to_client(c->sock, "dm_gold <amt> : Add/remove gold coins");
        send_text_to_client(c->sock, "dm_level <lvl> : Increase level up to lvl (max 20)");
        send_text_to_client(c->sock, "  dm_telemetry          : Show server metrics (RAM/CPU)");
        send_text_to_client(c->sock, "dm_boss <floor> : Locate bosses on the indicated floor (stats+loot)");
    }
    return;
  }

  //train <stat> or train random — awards unspent stat points
  if (strncmp(cmd, "train", 5) == 0) {
    const char *stat = cmd + 5;
    while (*stat == ' ') stat++;
    
    if (c->unspent_stat_points <= 0) {
      send_text_to_client(c->sock, "[SYSTEM] You have no stat points to assign.");
      return;
    }
    if (strlen(stat) == 0) {
      send_text_to_client(c->sock, "[SYSTEM] Specify a stat (str, dex, con, int, wis, cha) or 'random'. You have %d points.", c->unspent_stat_points);
      return;
    }
      int chosen = -1; //0=str, 1=dex, 2=con, 3=int, 4=wis, 5=cha
      if (strncasecmp(stat, "str", 3) == 0) chosen = 0;
      else if (strncasecmp(stat, "dex", 3) == 0) chosen = 1;
      else if (strncasecmp(stat, "with", 3) == 0) chosen = 2;
      else if (strncasecmp(stat, "int", 3) == 0) chosen = 3;
      else if (strncasecmp(stat, "wis", 3) == 0) chosen = 4;
      else if (strncasecmp(stat, "cha", 3) == 0) chosen = 5;
      else if (strncasecmp(stat, "random", 6) == 0) chosen = rand() % 6;
      
      if (chosen == -1) {
        send_text_to_client(c->sock, "[SYSTEM] Unknown stat. Use str, dex, con, int, wis, cha or random.");
        return;
      }

      c->unspent_stat_points--;
      char *stat_name = "";
      int old_mod = (c->con - 10) / 2;
      
      if (chosen == 0) { c->str++; stat_name = "Strength"; }
      else if (chosen == 1) { c->dex++; stat_name = "Dexterity"; }
      else if (chosen == 2) { 
          c->con++; 
          stat_name = "Constitution"; 
          int new_mod = (c->con - 10) / 2;
          if (new_mod > old_mod) {
              c->max_hp += c->level;
              c->hp += c->level;
              send_text_to_client(c->sock, "[SYSTEM] Your Constitution makes you more resistant! Max HP +%d.", c->level);
          }
      }
      else if (chosen == 3) { c->intel++; stat_name = "Intelligence"; }
      else if (chosen == 4) { c->wis++; stat_name = "Wisdom"; }
      else if (chosen == 5) { c->cha++; stat_name = "Charisma"; }

      send_text_to_client(c->sock, "[SYSTEM] You have increased %s! (Points remaining: %d)", stat_name, c->unspent_stat_points);
      save_player_data(c);
      send_detailed_state(c);
      return;
  }

  //top, leaderboard — Shows player rankings
  if (strcmp(cmd, "top") == 0 || strcmp(cmd, "leaderboard") == 0) {
      send_text_to_client(c->sock, "[RANKING] Hall of Fame processing in progress...");
      DIR *d;
      struct dirent *dir;
      d = opendir("saves");
      if (d) {
          LeaderboardEntry entries[1000];
          int entry_count = 0;
          while ((dir = readdir(d)) != NULL) {
              if (strstr(dir->d_name, ".save")) {
                  char filepath[128];
                  snprintf(filepath, sizeof(filepath), "saves/%s", dir->d_name);
                  FILE *f = fopen(filepath, "rb");
                  if (f) {
                      SaveData sd;
                      if (fread(&sd, 1, sizeof(SaveData), f) == sizeof(SaveData)) {
                          char username[32];
                          strncpy(username, dir->d_name, sizeof(username) - 1);
                          username[sizeof(username) - 1] = '\0';
                          char *dot = strrchr(username, '.');
                          if (dot) *dot = '\0';
                          if (strcmp(username, "dm") != 0) {
                              strncpy(entries[entry_count].username, username, 32);
                              entries[entry_count].level = sd.level;
                              entries[entry_count].xp = sd.xp;
                              entries[entry_count].bosses_defeated = sd.bosses_defeated;
                              entry_count++;
                          }
                      }
                      fclose(f);
                  }
              }
          }
          closedir(d);
          qsort(entries, entry_count, sizeof(LeaderboardEntry), compare_leaderboard);
          send_text_to_client(c->sock, "=================== LEADERBOARD ===================");
          for (int i = 0; i < entry_count && i < 10; i++) {
              int bosses = __builtin_popcount(entries[i].bosses_defeated);
              send_text_to_client(c->sock, "%d. %s (Level %d) - %d XP - %d Bosses Defeated", 
                                  i + 1, entries[i].username, entries[i].level, entries[i].xp, bosses);
          }
          send_text_to_client(c->sock, "===================================================");
      } else {
          send_text_to_client(c->sock, "[ERROR] Unable to read save files.");
      }
      return;
  }

  //--- GLOBAL EVENT --- displays the current event
  if (strcmp(cmd, "event") == 0) {
      if (active_event_type == 0) {
          send_text_to_client(c->sock, "[EVENT] No global events active at the moment.");
      } else {
          int mins_left = (event_time_left * 200) / 60000;
          int secs_left = ((event_time_left * 200) % 60000) / 1000;
          send_text_to_client(c->sock, "[GLOBAL EVENT] Invasion of Plane %d!", event_floor_id);
          send_text_to_client(c->sock, "  Goal: kill %d Skeletons — Progress: %d/%d", event_goal, event_progress, event_goal);
          send_text_to_client(c->sock, "  Time left: %d min %d sec", mins_left, secs_left);
      }
      return;
  }

  //--- PARTY SYSTEM ---
  if (strncmp(cmd, "invite ", 7) == 0) {
      char target_name[32];
      strncpy(target_name, cmd + 7, 31); target_name[31] = '\0';
      bool found = false;
      for (int i = 0; i < MAX_CLIENTS; i++) {
          if (g_clients[i].active && g_clients[i].authenticated && strcmp(g_clients[i].username, target_name) == 0) {
              strncpy(g_clients[i].pending_invite, c->username, 31);
              send_text_to_client(g_clients[i].sock, "[PARTY] %s has invited you to his party! Type 'join' to accept.", c->username);
              send_text_to_client(c->sock, "[PARTY] Invitation sent to %s.", target_name);
              found = true;
              break;
          }
      }
      if (!found) send_text_to_client(c->sock, "[PARTY] Player %s not found online.", target_name);
      return;
  }
  if (strcmp(cmd, "join") == 0) {
      if (strlen(c->pending_invite) == 0) {
          send_text_to_client(c->sock, "[PARTY] No pending invitations.");
          return;
      }
      //If not already a leader of another party (if so, cannot join unless they leave first)
      if (strlen(c->party_leader) > 0 && strcmp(c->party_leader, c->username) == 0) {
          send_text_to_client(c->sock, "[PARTY] You are the leader of your party. Leave it with 'leave' before joining another.");
          return;
      }
      bool leader_online = false;
      for (int i = 0; i < MAX_CLIENTS; i++) {
          if (g_clients[i].active && g_clients[i].authenticated && strcmp(g_clients[i].username, c->pending_invite) == 0) {
              leader_online = true;
              // Set the leader of the person who invited us as our leader
              if (strlen(g_clients[i].party_leader) > 0) {
                  strncpy(c->party_leader, g_clients[i].party_leader, 31);
              } else {
                  //They didn't have a party, so they become the leader
                  strncpy(g_clients[i].party_leader, g_clients[i].username, 31);
                  strncpy(c->party_leader, g_clients[i].username, 31);
              }
              send_text_to_client(c->sock, "[PARTY] You have joined %s' party!", c->party_leader);
              send_text_to_client(g_clients[i].sock, "[PARTY] %s has joined the party!", c->username);
              c->pending_invite[0] = '\0';
              break;
          }
      }
      if (!leader_online) {
          send_text_to_client(c->sock, "[PARTY] The player who invited you is no longer online.");
          c->pending_invite[0] = '\0';
      }
      return;
  }
  if (strcmp(cmd, "leave") == 0) {
      if (strlen(c->party_leader) == 0) {
          send_text_to_client(c->sock, "[PARTY] You are not in any party.");
          return;
      }
      send_text_to_client(c->sock, "[PARTY] You left the party.");
      // If the leader leaves, the party is dissolved
      if (strcmp(c->party_leader, c->username) == 0) {
          for (int i = 0; i < MAX_CLIENTS; i++) {
              if (g_clients[i].active && strcmp(g_clients[i].party_leader, c->username) == 0 && strcmp(g_clients[i].username, c->username) != 0) {
                  send_text_to_client(g_clients[i].sock, "[PARTY] The leader has disbanded the party.");
                  g_clients[i].party_leader[0] = '\0';
              }
          }
      } else {
          // Notify others that they left
          for (int i = 0; i < MAX_CLIENTS; i++) {
              if (g_clients[i].active && strcmp(g_clients[i].party_leader, c->party_leader) == 0 && strcmp(g_clients[i].username, c->username) != 0) {
                  send_text_to_client(g_clients[i].sock, "[PARTY] %s has left the party.", c->username);
              }
          }
      }
      c->party_leader[0] = '\0';
      return;
  }
  if (strcmp(cmd, "party") == 0) {
      if (strlen(c->party_leader) == 0) {
          send_text_to_client(c->sock, "[PARTY] You are not in any party. Create one by inviting: invite <name>");
          return;
      }
      send_text_to_client(c->sock, "=== PARTY MEMBERS (Leaders: %s) ===", c->party_leader);
      for (int i = 0; i < MAX_CLIENTS; i++) {
          if (g_clients[i].active && strcmp(g_clients[i].party_leader, c->party_leader) == 0) {
              send_text_to_client(c->sock, "- %s (Lvl %d, %d/%d HP)", g_clients[i].username, g_clients[i].level, g_clients[i].hp, g_clients[i].max_hp);
          }
      }
      return;
  }

  // --- TRADE SYSTEM P2P ---
  if (strncmp(cmd, "trade ", 6) == 0) {
      char target_name[32];
      strncpy(target_name, cmd + 6, 31); target_name[31] = '\0';
      
      if (strcmp(c->username, target_name) == 0) {
          send_text_to_client(c->sock, "[TRADE] You cannot trade with yourself.");
          return;
      }
      
      bool found = false;
      for (int i = 0; i < MAX_CLIENTS; i++) {
          Client *t = &g_clients[i];
          if (t->active && t->authenticated && strcmp(t->username, target_name) == 0) {
              if (abs(t->x - c->x) > 2 || abs(t->y - c->y) > 2 || t->floor_id != c->floor_id) {
                  send_text_to_client(c->sock, "[TRADE] %s is too far away.", target_name);
                  return;
              }
              
              strncpy(c->trading_with, target_name, 31);
              c->trade_offer_item_idx = -1;
              c->trade_offer_gold = 0;
              c->trade_accepted = false;
              
              if (strcmp(t->trading_with, c->username) == 0) {
                  send_text_to_client(c->sock, "[TRADE] Trade started with %s. Use 'offer <gold> [object_id]' and 'accept'.", target_name);
                  send_text_to_client(t->sock, "[TRADE] Trade started with %s. Use 'offer <gold> [object_id]' and 'accept'.", c->username);
              } else {
                  send_text_to_client(t->sock, "[TRADE] %s wants to trade with you. Type 'trade %s' to get started.", c->username, c->username);
                  send_text_to_client(c->sock, "[TRADE] Trade request sent to %s.", target_name);
              }
              found = true;
              break;
          }
      }
      if (!found) send_text_to_client(c->sock, "[TRADE] Player %s not found.", target_name);
      return;
  }
  
  if (strncmp(cmd, "offer ", 6) == 0) {
      if (strlen(c->trading_with) == 0) {
          send_text_to_client(c->sock, "[TRADE] You are not in any trade.");
          return;
      }
      Client *t = NULL;
      for (int i = 0; i < MAX_CLIENTS; i++) {
          if (g_clients[i].active && strcmp(g_clients[i].username, c->trading_with) == 0) {
              t = &g_clients[i];
              break;
          }
      }
      if (!t || strcmp(t->trading_with, c->username) != 0) {
          send_text_to_client(c->sock, "[TRADE] The other player canceled or disconnected.");
          c->trading_with[0] = '\0';
          return;
      }
      
      long long gold = 0;
      int item = 0;
      int parsed = sscanf(cmd + 6, "%lld %d", &gold, &item);
      if (parsed >= 1) {
          if (gold < 0 || c->gold < (uint64_t)gold) {
              send_text_to_client(c->sock, "[TRADE] You don't have enough gold.");
              return;
          }
          c->trade_offer_gold = gold;
          c->trade_offer_item_idx = -1;
          if (parsed == 2) {
              int idx = item - 1;
              if (idx >= 0 && idx < c->backpack_count && c->backpack[idx].template_idx >= 0) {
                  c->trade_offer_item_idx = idx;
              } else {
                  send_text_to_client(c->sock, "[TRADE] Invalid item in backpack.");
                  return;
              }
          }
          c->trade_accepted = false;
          t->trade_accepted = false;
          
          char item_name[64] = "None";
          if (c->trade_offer_item_idx != -1) {
              strncpy(item_name, item_database[c->backpack[c->trade_offer_item_idx].template_idx].name, 63);
          }
          send_text_to_client(c->sock, "[TRADE] You bid: %lld gold, %s. Pending...", gold, item_name);
          send_text_to_client(t->sock, "[TRADE] %s offers: %lld gold, %s.", c->username, gold, item_name);
      }
      return;
  }
  
  if (strcmp(cmd, "accept") == 0) {
      if (strlen(c->trading_with) == 0) {
          send_text_to_client(c->sock, "[TRADE] You are not in any trade.");
          return;
      }
      Client *t = NULL;
      for (int i = 0; i < MAX_CLIENTS; i++) {
          if (g_clients[i].active && strcmp(g_clients[i].username, c->trading_with) == 0) {
              t = &g_clients[i];
              break;
          }
      }
      if (!t || strcmp(t->trading_with, c->username) != 0) {
          send_text_to_client(c->sock, "[TRADE] The other player canceled or disconnected.");
          c->trading_with[0] = '\0';
          return;
      }
      
      c->trade_accepted = true;
      send_text_to_client(c->sock, "[TRADE] You accepted the offer.");
      send_text_to_client(t->sock, "[TRADE] %s accepted the offer.", c->username);
      
      if (c->trade_accepted && t->trade_accepted) {
          //Execute the exchange
          // Final validity checks
          if (c->gold < c->trade_offer_gold || t->gold < t->trade_offer_gold) {
              send_text_to_client(c->sock, "[TRADE] ERROR: Insufficient gold, trade cancelled.");
              send_text_to_client(t->sock, "[TRADE] ERROR: Insufficient gold, trade cancelled.");
          } else {
              //Gold swaps
              c->gold -= c->trade_offer_gold;
              c->gold += t->trade_offer_gold;
              t->gold -= t->trade_offer_gold;
              t->gold += c->trade_offer_gold;
              
              // Swap item
              ItemInstance i1, i2;
              memset(&i1, 0, sizeof(ItemInstance));
              memset(&i2, 0, sizeof(ItemInstance));
              i1.template_idx = -1;
              i2.template_idx = -1;
              bool has_i1 = false, has_i2 = false;
              if (c->trade_offer_item_idx != -1) {
                  i1 = c->backpack[c->trade_offer_item_idx];
                  has_i1 = true;
                  //Remove from c backpack
                  for (int k = c->trade_offer_item_idx; k < c->backpack_count - 1; k++) {
                      c->backpack[k] = c->backpack[k+1];
                  }
                  c->backpack_count--;
              }
              if (t->trade_offer_item_idx != -1) {
                  i2 = t->backpack[t->trade_offer_item_idx];
                  has_i2 = true;
                  //Remove from t's backpack
                  for (int k = t->trade_offer_item_idx; k < t->backpack_count - 1; k++) {
                      t->backpack[k] = t->backpack[k+1];
                  }
                  t->backpack_count--;
              }
              
              //Give it to you
              if (has_i1 && t->backpack_count < MAX_BACKPACK) {
                  t->backpack[t->backpack_count++] = i1;
              } else if (has_i1) {
                  // If the backpack is full this shouldn't happen, or the item falls to the ground.
                  // Simplification: there is enough space for 1-to-1, but we don't check here.
                  //If there is no space we give it anyway (minor bug) but better to drop or overwrite.
                  //We overwrite the last slot.
                  t->backpack[MAX_BACKPACK-1] = i1;
              }
              //Give it to c
              if (has_i2 && c->backpack_count < MAX_BACKPACK) {
                  c->backpack[c->backpack_count++] = i2;
              } else if (has_i2) {
                  c->backpack[MAX_BACKPACK-1] = i2;
              }
              
              send_text_to_client(c->sock, "[TRADE] Trade complete!");
              send_text_to_client(t->sock, "[TRADE] Trade complete!");
          }
          c->trading_with[0] = '\0';
          t->trading_with[0] = '\0';
          c->trade_offer_item_idx = -1;
          t->trade_offer_item_idx = -1;
      }
      return;
  }
  
  if (strcmp(cmd, "cancel") == 0) {
      if (strlen(c->trading_with) == 0) {
          send_text_to_client(c->sock, "[TRADE] You are not in any trade.");
          return;
      }
      for (int i = 0; i < MAX_CLIENTS; i++) {
          if (g_clients[i].active && strcmp(g_clients[i].username, c->trading_with) == 0) {
              send_text_to_client(g_clients[i].sock, "[TRADE] %s canceled the trade.", c->username);
              g_clients[i].trading_with[0] = '\0';
              break;
          }
      }
      c->trading_with[0] = '\0';
      send_text_to_client(c->sock, "[TRADE] Trade cancelled.");
      return;
  }

  //--- MESSAGING SYSTEM BETWEEN PLAYERS ---

  /*Messaggio privato: msg/tell/whisper <name> <text>*/
  {
    int pfx = 0;
    if (strncmp(cmd, "msg ", 4) == 0)
      pfx = 3;
    else if (strncmp(cmd, "tell ", 5) == 0)
      pfx = 4;
    else if (strncmp(cmd, "whisper ", 8) == 0)
      pfx = 7;

    if (pfx > 0) {
      const char *rest = cmd + pfx;
      while (*rest == ' ')
        rest++;
      char target_name[32];
      int nlen = 0;
      while (nlen < 31 && rest[nlen] != '\0' && rest[nlen] != ' ') {
        target_name[nlen] = rest[nlen];
        nlen++;
      }
      target_name[nlen] = '\0';
      const char *text = rest + nlen;
      while (*text == ' ')
        text++;

      if (nlen == 0 || text[0] == '\0') {
        send_text_to_client(c->sock,
                            "[MSG] Usage: msg <name> <text> — e.g. 'msg Bob hello'");
        return;
      }
      if (strcasecmp(target_name, c->username) == 0) {
        send_text_to_client(c->sock, "[MSG] You cannot message yourself.");
        return;
      }
      Client *t = find_online_player(target_name);
      if (!t) {
        send_text_to_client(c->sock, "[MSG] Player %s is not online.",
                            target_name);
        return;
      }
      if (!msg_rate_ok(c)) {
        send_text_to_client(c->sock,
                            "[MSG] You are typing too fast. Wait a moment.");
        return;
      }
      /*If the recipient has blocked us (and we are not DM),
*the message is not delivered.*/
      if (!c->is_dm && player_is_blocked(t, c->username)) {
        send_text_to_client(c->sock,
                            "[MSG] Your message was not delivered.");
        return;
      }
      send_text_to_client(t->sock, "[MSG] %s: %s", c->username, text);
      send_text_to_client(c->sock, "[MSG] -> %s: %s", t->username, text);
      return;
    }
  }

  /*Chat locale: say/shout <text> — sentita dai giocatori dello stesso
*floor within MSG_LOCAL_RADIUS, barring blocks.*/
  {
    int pfx = 0;
    if (strncmp(cmd, "say ", 4) == 0)
      pfx = 3;
    else if (strncmp(cmd, "shout ", 6) == 0)
      pfx = 5;

    if (pfx > 0) {
      char text[256];
      strncpy(text, cmd + pfx, sizeof(text) - 1);
      text[sizeof(text) - 1] = '\0';
      char *ttext = trim_spaces(text);
      if (ttext[0] == '\0') {
        send_text_to_client(c->sock,
                            "[CHAT] Usage: say <text> — e.g. 'say anyone here?'");
        return;
      }
      if (!msg_rate_ok(c)) {
        send_text_to_client(c->sock,
                            "[CHAT] You are typing too fast. Wait a moment.");
        return;
      }
      int heard = 0;
      for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *t = &g_clients[i];
        if (!t->active || !t->authenticated || t == c)
          continue;
        if (t->floor_id != c->floor_id)
          continue;
        int dx = abs(t->x - c->x), dy = abs(t->y - c->y);
        if (dx > MSG_LOCAL_RADIUS || dy > MSG_LOCAL_RADIUS)
          continue;
        if (player_is_blocked(t, c->username))
          continue; /*Blocked: Can't hear local chat*/
        send_text_to_client(t->sock, "[CHAT] %s: %s", c->username, ttext);
        heard++;
      }
      send_text_to_client(c->sock, "[CHAT] %s (you): %s  [%d nearby]",
                          c->username, ttext, heard);
      return;
    }
  }

  /*who — "list of players online" */
  if (strcmp(cmd, "who") == 0) {
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
      if (g_clients[i].active && g_clients[i].authenticated)
        count++;
    send_text_to_client(c->sock, "[WHO] %d player(s) online:", count);
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (g_clients[i].active && g_clients[i].authenticated) {
        send_text_to_client(
            c->sock, "  - %s (floor %d)%s", g_clients[i].username,
            g_clients[i].floor_id,
            strcmp(g_clients[i].username, c->username) == 0 ? " (you)" : "");
      }
    }
    return;
  }

  /*block <name> — add a player to the block list.
*Works even if the player is offline: the name comes
*stored like this the block is valid when you return to play.*/
  if (strncmp(cmd, "block ", 6) == 0) {
    char name[32];
    strncpy(name, cmd + 6, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *n = trim_spaces(name);
    if (n[0] == '\0') {
      send_text_to_client(c->sock, "[BLOCK] Usage: block <name>");
      return;
    }
    if (strcasecmp(n, c->username) == 0) {
      send_text_to_client(c->sock, "[BLOCK] You cannot block yourself.");
      return;
    }
    if (player_is_blocked(c, n)) {
      send_text_to_client(c->sock, "[BLOCK] %s is already blocked.", n);
      return;
    }
    if (c->blocked_count >= MAX_BLOCKED_PLAYERS) {
      send_text_to_client(
          c->sock,
          "[BLOCK] Block list is full (%d). Use 'unblock <name>' first.",
          MAX_BLOCKED_PLAYERS);
      return;
    }
    strncpy(c->blocked_players[c->blocked_count], n, 31);
    c->blocked_players[c->blocked_count][31] = '\0';
    c->blocked_count++;
    save_player_data(c);
    send_text_to_client(
        c->sock,
        "[BLOCK] %s has been blocked. Their messages will no longer reach you.",
        n);
    return;
  }

  /*unblock <name> — remove a player from the block list*/
  if (strncmp(cmd, "unblock ", 8) == 0) {
    char name[32];
    strncpy(name, cmd + 8, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *n = trim_spaces(name);
    if (n[0] == '\0') {
      send_text_to_client(c->sock, "[BLOCK] Usage: unblock <name>");
      return;
    }
    for (int i = 0; i < c->blocked_count; i++) {
      if (strcasecmp(c->blocked_players[i], n) == 0) {
        /*Move subsequent items to compact the list*/
        for (int k = i; k < c->blocked_count - 1; k++)
          memcpy(c->blocked_players[k], c->blocked_players[k + 1], 32);
        c->blocked_count--;
        save_player_data(c);
        send_text_to_client(c->sock, "[BLOCK] %s has been unblocked.", n);
        return;
      }
    }
    send_text_to_client(c->sock, "[BLOCK] %s is not in your block list.", n);
    return;
  }

  /*blocks — shows the block list*/
  if (strcmp(cmd, "blocks") == 0 || strcmp(cmd, "blocklist") == 0) {
    if (c->blocked_count == 0) {
      send_text_to_client(c->sock,
                          "[BLOCK] Your block list is empty. Use 'block <name>'.");
      return;
    }
    send_text_to_client(c->sock, "[BLOCK] Blocked players (%d):",
                        c->blocked_count);
    for (int i = 0; i < c->blocked_count; i++)
      send_text_to_client(c->sock, "  - %s", c->blocked_players[i]);
    return;
  }

  //browse <n> — inspect item description in store
  if (strncmp(cmd, "browse ", 7) == 0) {
    int target_item = atoi(cmd + 7) - 1;
    bool found = false;
    for (int i = 0; i < MAX_NPCS; i++) {
      if (npcs[i].active && npcs[i].archetype == ARCH_MERCHANT &&
          npcs[i].floor_id == c->floor_id &&
          abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
        if (target_item >= 0 && target_item < npcs[i].merchant.item_count) {
          ItemTemplate *it =
              &item_database[npcs[i].merchant.item_templates[target_item]];
          send_text_to_client(c->sock, "[INFO] %s: %s", it->name,
                              it->description);
          found = true;
        } else {
          send_text_to_client(c->sock, "[ERROR] Invalid object number.");
          found = true;
        }
        break;
      }
    }
    if (!found)
      send_text_to_client(c->sock,
                          "[SYSTEM] No merchants nearby.");
    return;
  }

  // --- DM COMMANDS ---
  if (c->is_dm) {
    if (strncmp(cmd, "dm_gold ", 8) == 0) {
      long long amount = atoll(cmd + 8);
      if (amount < 0 && c->gold < (uint64_t)(-amount)) {
          c->gold = 0;
      } else {
          c->gold += amount;
      }
      send_text_to_client(c->sock, "[DM] %lld's updated gold. Total: %llu", amount, (unsigned long long)c->gold);
      return;
    }
    if (strncmp(cmd, "dm_level ", 9) == 0) {
      int target = atoi(cmd + 9);
      if (c->level >= 20) {
        send_text_to_client(c->sock, "[DM] You are already at maximum level (20).");
        return;
      }
      if (target <= c->level || target > 20) {
        send_text_to_client(c->sock, "[DM] Use: dm_level <target> (must be between %d and 20)", c->level + 1);
        return;
      }
      c->xp = get_xp_threshold(target);
      check_level_up(c);
      send_text_to_client(c->sock, "[DM] Level increased to required target.");
      return;
    }
    if (strncmp(cmd, "dm_find_monster ", 16) == 0) {
      const char *search = cmd + 16;
      int found_count = 0;
      send_text_to_client(c->sock,
                          "[DM] Monster '%s' search results:", search);
      for (int i = 0; i < bestiary_size; i++) {
        if (strcasestr(bestiary_data[i].name, search)) {
          send_text_to_client(c->sock, "  ID: %d - %s", i,
                              bestiary_data[i].name);
          found_count++;
          if (found_count >= 10) {
            send_text_to_client(c->sock,
                                "...only the first 10 results shown.");
            break;
          }
        }
      }
      if (found_count == 0)
        send_text_to_client(c->sock, "No monsters found.");
      return;
    }
    if (strncmp(cmd, "dm_find_item ", 13) == 0) {
      const char *search = cmd + 13;
      int found_count = 0;
      send_text_to_client(c->sock,
                          "[DM] Object '%s' search results:", search);
      for (int i = 0; i < item_database_size; i++) {
        if (strcasestr(item_database[i].name, search)) {
          send_text_to_client(c->sock, "  ID: %d - %s", i,
                              item_database[i].name);
          found_count++;
          if (found_count >= 10) {
            send_text_to_client(c->sock,
                                "...only the first 10 results shown.");
            break;
          }
        }
      }
      if (found_count == 0)
        send_text_to_client(c->sock, "No items found.");
      return;
    }
    if (strcmp(cmd, "dm_telemetry") == 0) {
      int active_c = 0, active_n = 0, total_t = 0, active_t = 0;
      for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_clients[i].active)
          active_c++;
      for (int i = 0; i < MAX_NPCS; i++)
        if (npcs[i].active)
          active_n++;
      for (int f = 0; f < MAX_FLOORS; f++) {
        total_t += master_world->floors[f].trap_count;
        for (int j = 0; j < master_world->floors[f].trap_count; j++) {
          if (master_world->floors[f].traps[j].active)
            active_t++;
        }
      }
      send_text_to_client(
          c->sock,
          "[DM] Telemetry: Turns:%d, Clients:%d, NPCs:%d/%d, Traps:%d/%d",
          global_total_turns, active_c, active_n, MAX_NPCS, active_t, total_t);
      return;
    }
    if (strncmp(cmd, "dm_spawn_boss", 13) == 0) {
      int target_floor = 0;
      /*%d skips the leading space after the command word*/
      if (sscanf(cmd + 13, "%d", &target_floor) != 1 || target_floor <= 0) {
        send_text_to_client(c->sock, "[DM] Use: dm_spawn_boss <floor> (ex: dm_spawn_boss 10)");
        return;
      }
      
      extern int next_id;
      int empty_idx = -1;
      for (int i = 0; i < MAX_NPCS; i++) {
        if (!npcs[i].active && npcs[i].template == NULL && npcs[i].respawn_timer == -1) {
          empty_idx = i;
          break;
        }
      }
      
      if (empty_idx == -1) {
        send_text_to_client(c->sock, "[DM] ERROR: No free NPC slots (MAX_NPCS reached).");
        return;
      }
      
      /*Look for a random monster suitable for the plan*/
      int pool[1000];
      int psize = 0;
      for (int bi = 0; bi < bestiary_size; bi++) {
        if (target_floor >= bestiary_data[bi].floor_min && target_floor <= bestiary_data[bi].floor_max) {
          if (psize < 1000) pool[psize++] = bi;
        }
      }
      
      if (psize == 0) {
        send_text_to_client(c->sock, "[DM] ERROR: No monsters in the bestiary are suitable for the %d floor.", target_floor);
        return;
      }
      
      int boss_id = pool[rand() % psize];
      NPC *b = &npcs[empty_idx];
      memset(b, 0, sizeof(NPC));
      b->active = true;
      b->archetype = ARCH_BOSS;
      b->entity_id = next_id++;
      b->floor_id = target_floor;
      b->x = MAP_CENTER_X;
      b->y = MAP_CENTER_Y;
      b->spawn_x = MAP_CENTER_X;
      b->spawn_y = MAP_CENTER_Y;
      b->respawn_timer = 0;
      b->template = &bestiary_data[boss_id];
      b->hp = b->template->hp_avg * 5 + (target_floor * 50);
      b->max_hp = b->hp;
      snprintf(b->custom_name, sizeof(b->custom_name), "Boss %s", b->template->name);
      ai_init_npc(b, b->custom_name, b->floor_id);
      
      send_text_to_client(c->sock, "[DM] Boss %s (Entity %d) successfully spawned on floor %d!", b->custom_name, b->entity_id, target_floor);
      return;
    }

    if (strncmp(cmd, "dm_boss", 7) == 0) {
      int target_floor = 0;
      if (sscanf(cmd + 7, "%d", &target_floor) != 1) {
        send_text_to_client(c->sock, "[DM] Use: dm_boss <floor> (ex: dm_boss 10)");
        send_text_to_client(c->sock, "[DM] Bosses appear every 10 floors: 10, 20, 30, ...");
        return;
      }
      /*Bosses live on floors multiples of 10*/
      if (target_floor <= 0 || target_floor % 10 != 0) {
        int nearest_below = (target_floor / 10) * 10;
        int nearest_above = nearest_below + 10;
        if (nearest_below <= 0) {
          send_text_to_client(c->sock,
            "[DM] Floor %d is not a boss floor. The first boss floor is 10.",
            target_floor);
        } else {
          send_text_to_client(c->sock,
            "[DM] Floor %d is not a boss floor (bosses appear every 10 floors).",
            target_floor);
          send_text_to_client(c->sock,
            "[DM] Adjacent boss floors: Floor %d or Floor %d",
            nearest_below, nearest_above);
        }
        return;
      }
      int boss_count = 0;
      for (int i = 0; i < MAX_NPCS; i++) {
        if (!npcs[i].active) continue;
        if (npcs[i].floor_id != target_floor) continue;
        if (npcs[i].archetype != ARCH_BOSS) continue;
        boss_count++;
        const char *bname = (npcs[i].template) ? npcs[i].template->name : "Unknown Boss";
        send_text_to_client(c->sock,
          "[DM] ╔═══════════════════════ ═══════════════════════╗");
        send_text_to_client(c->sock,
          "[DM] BOSS #%d — %s (NPC id %d)", boss_count, bname, npcs[i].entity_id);
        send_text_to_client(c->sock,
          "[DM] ╚═══════════════════════ ═══════════════════════╝");
        /* Posizione */
        send_text_to_client(c->sock,
          "[DM] Position: Floor %d X:%d Y:%d",
          npcs[i].floor_id, npcs[i].x, npcs[i].y);
        send_text_to_client(c->sock,
          "[DM] Spawn : X:%d Y:%d",
          npcs[i].spawn_x, npcs[i].spawn_y);
        /*Combat statistics*/
        send_text_to_client(c->sock,
          "[DM] HP : %d / %d", npcs[i].hp, npcs[i].max_hp);
        send_text_to_client(c->sock,
          "[DM] CA : %d", npcs[i].ac);
        send_text_to_client(c->sock,
          "[DM] Attack: +%d Damage: %dd%d",
          npcs[i].attack_bonus, npcs[i].damage_dice, npcs[i].damage_sides);
        send_text_to_client(c->sock,
          "[DM] XP: %d Gold: %d",
          npcs[i].xp_reward, npcs[i].gold_drop);
        send_text_to_client(c->sock,
          "[DM] Moral: %d", npcs[i].morale);
        /*Data from template bestiary*/
        if (npcs[i].template) {
          const MonsterTemplate *t = npcs[i].template;
          send_text_to_client(c->sock,
            "[DM] View: %d Speed: %d",
            t->sight_range, t->speed);
          send_text_to_client(c->sock,
            "[DM] Floors : %d-%d",
            t->floor_min, t->floor_max);
          if (t->description && t->description[0] != '\0') {
            send_text_to_client(c->sock,
              "[DM] Lore : %.120s", t->description);
          }
        }
        /*Active spell slots*/
        bool has_slots = false;
        for (int sl = 1; sl <= MAX_SPELL_LEVEL; sl++) {
          if (npcs[i].spell_slots_max[sl] > 0) {
            has_slots = true;
            break;
          }
        }
        if (has_slots) {
          char slot_buf[256];
          slot_buf[0] = '\0';
          for (int sl = 1; sl <= MAX_SPELL_LEVEL; sl++) {
            if (npcs[i].spell_slots_max[sl] <= 0) continue;
            char tmp[24];
            snprintf(tmp, sizeof(tmp), "L%d:%d/%d  ",
              sl, npcs[i].spell_slots[sl], npcs[i].spell_slots_max[sl]);
            strncat(slot_buf, tmp, sizeof(slot_buf) - strlen(slot_buf) - 1);
          }
          send_text_to_client(c->sock, "[DM] Ki/Inc Slot: %s", slot_buf);
        }
        /*Loot ghost (items he carries)*/
        bool has_loot = false;
        for (int li = 0; li < 30; li++) {
          if (npcs[i].ghost_loot[li].template_idx >= 0 && npcs[i].ghost_loot[li].stack_count > 0) {
            has_loot = true;
            break;
          }
        }
        if (has_loot) {
          send_text_to_client(c->sock, "[DM] --- Inventory/Loot ---");
          for (int li = 0; li < 30; li++) {
            if (npcs[i].ghost_loot[li].template_idx < 0 || npcs[i].ghost_loot[li].stack_count <= 0) continue;
            char loot_buf[128];
            get_full_item_name(&npcs[i].ghost_loot[li], loot_buf, sizeof(loot_buf));
            send_text_to_client(c->sock, "[DM] [%d] %s", li, loot_buf);
          }
        } else {
          send_text_to_client(c->sock, "[DM] Inventory: empty (loot generated on death)");
        }
        /*Active effects*/
        if (npcs[i].effect_count > 0) {
          send_text_to_client(c->sock, "[DM] --- Active Effects (%d) ---", npcs[i].effect_count);
          for (int ei = 0; ei < npcs[i].effect_count; ei++) {
            const char *ename = npcs[i].effects[ei].name ? npcs[i].effects[ei].name : "?";
            send_text_to_client(c->sock, "[DM] [%d] %s val:%d duration:%d round",
              ei,
              ename,
              npcs[i].effects[ei].value,
              npcs[i].effects[ei].duration_rounds);
          }
        }
        send_text_to_client(c->sock, "[DM] ──────────────────────── ────────────────────────");
      }
      
      int dead_count = 0;
      for (int i = 0; i < MAX_NPCS; i++) {
        if (npcs[i].active) continue;
        if (npcs[i].floor_id != target_floor) continue;
        if (npcs[i].archetype != ARCH_BOSS) continue;
        if (npcs[i].respawn_timer <= 0) continue;
        dead_count++;
        const char *bname = (npcs[i].template) ? npcs[i].template->name : "Unknown Boss";
        send_text_to_client(c->sock,
          "[DM] *** DEAD BOSS — %s (NPC id %d) — respawn in %d round ***",
          bname, npcs[i].entity_id, npcs[i].respawn_timer);
        send_text_to_client(c->sock,
          "[DM] Spawn: X:%d Y:%d |  Max HP: %d",
          npcs[i].spawn_x, npcs[i].spawn_y, npcs[i].max_hp);
      }
      
      if (boss_count == 0 && dead_count == 0) {
        send_text_to_client(c->sock,
          "[DM] No bosses found on Floor %d (neither active nor respawning).", target_floor);
      } else if (boss_count == 0) {
        send_text_to_client(c->sock,
          "[DM] Floor %d: Boss defeated, %d respawning.", target_floor, dead_count);
      } else {
        send_text_to_client(c->sock,
          "[DM] Total active bosses on Floor %d: %d", target_floor, boss_count);
      }
      return;
    }

    if (strncmp(cmd, "dm_merchant ", 12) == 0) {
      int target_floor = 0;
      if (sscanf(cmd + 12, "%d", &target_floor) != 1) {
        send_text_to_client(c->sock, "[DM] Use: dm_merchant <floor> (ex: dm_merchant 5)");
        send_text_to_client(c->sock, "[DM] Wandering merchants appear every 5 floors (e.g. 5, 15, 25), excluding boss floors.");
        return;
      }
      /*Merchants appear every 5 floors, but not on multiples of 10 (boss floors)*/
      if (target_floor <= 0 || target_floor % 5 != 0 || target_floor % 10 == 0) {
        int nearest_below = target_floor - (target_floor % 5);
        if (nearest_below % 10 == 0) nearest_below -= 5;
        
        int nearest_above = target_floor + (5 - (target_floor % 5));
        if (nearest_above % 10 == 0) nearest_above += 5;
        
        if (nearest_below <= 0) {
          send_text_to_client(c->sock,
            "[DM] Floor %d does not have a merchant. The first merchant floor is 5.",
            target_floor);
        } else {
          send_text_to_client(c->sock,
            "[DM] Floor %d does not have a wandering merchant (they appear every 5 non-boss floors).",
            target_floor);
          send_text_to_client(c->sock,
            "[DM] Adjacent merchant floors: Floor %d or Floor %d",
            nearest_below, nearest_above);
        }
        return;
      }
      int merch_count = 0;
      for (int i = 0; i < MAX_NPCS; i++) {
        if (!npcs[i].active) continue;
        if (npcs[i].floor_id != target_floor) continue;
        if (npcs[i].archetype != ARCH_MERCHANT) continue;
        merch_count++;
        send_text_to_client(c->sock,
          "[DM] ╔═══════════════════════ ═══════════════════════╗");
        send_text_to_client(c->sock,
          "[DM] WANDING MERCHANT #%d (NPC id %d)", merch_count, npcs[i].entity_id);
        send_text_to_client(c->sock,
          "[DM] ╚═══════════════════════ ═══════════════════════╝");
        /* Position */
        send_text_to_client(c->sock,
          "[DM] Position: Floor %d X:%d Y:%d",
          npcs[i].floor_id, npcs[i].x, npcs[i].y);
        /*Stats (a merchant usually doesn't fight, but we show it anyway)*/
        send_text_to_client(c->sock,
          "[DM] HP : %d / %d", npcs[i].hp, npcs[i].max_hp);
        send_text_to_client(c->sock,
          "[DM] Restock timer: %d", npcs[i].merchant.restock_timer);
          
        /*Inventory for sale*/
        if (npcs[i].merchant.item_count > 0) {
          send_text_to_client(c->sock, "[DM] --- Goods for sale (%s) ---", npcs[i].merchant.shop_name);
          for (int li = 0; li < npcs[i].merchant.item_count; li++) {
            int t_idx = npcs[i].merchant.item_templates[li];
            int stock = npcs[i].merchant.item_stock[li];
            int max_stock = npcs[i].merchant.item_stock_max[li];
            if (t_idx >= 0 && t_idx < item_database_size) {
              send_text_to_client(c->sock, "[DM] [%d] %s (Quantity: %d/%d)", 
                li, item_database[t_idx].name, stock, max_stock);
            }
          }
        } else {
          send_text_to_client(c->sock, "[DM] Goods for sale: empty");
        }
        send_text_to_client(c->sock, "[DM] ──────────────────────── ────────────────────────");
      }
      if (merch_count == 0) {
        send_text_to_client(c->sock,
          "[DM] No active wandering merchants found on Floor %d.", target_floor);
      } else {
        send_text_to_client(c->sock,
          "[DM] Total merchants found on Floor %d: %d", target_floor, merch_count);
      }
      return;
    }
    if (strncmp(cmd, "dm_spawn ", 9) == 0) {
      int tid = 0, tx = 0, ty = 0;
      if (sscanf(cmd + 9, "%d %d %d", &tid, &tx, &ty) == 3) {
        if (tid >= 0 && tid < bestiary_size) {
          for (int i = 0; i < MAX_NPCS; i++) {
            if (!npcs[i].active) {
              npcs[i].active = true;
              npcs[i].entity_id = next_id++;
              npcs[i].floor_id = c->floor_id;
              npcs[i].x = tx;
              npcs[i].y = ty;
              npcs[i].template_idx = tid;
              npcs[i].template = &bestiary_data[tid];
              npcs[i].hp = npcs[i].max_hp = npcs[i].template->hp_avg;
              ai_attach_behavior(&npcs[i]);
              send_text_to_client(c->sock, "[DM] Spawned %s at %d,%d",
                                  npcs[i].template->name, tx, ty);
              return;
            }
          }
        }
      }
      return;
    }
    if (strncmp(cmd, "dm_goto ", 8) == 0) {
      int f = 0, tx = 0, ty = 0;
      if (sscanf(cmd + 8, "%d %d %d", &f, &tx, &ty) == 3) {
        if (f >= 0 && f < 100) {
          c->floor_id = f;
          client_track_explored_floor(c);
          c->x = tx;
          c->y = ty;
          send_text_to_client(c->sock, "[DM] Teleported to Floor %d (%d,%d)", f,
                              tx, ty);
          // Force map refresh
          send_map_chunk(c->sock, &master_world->floors[f].map, tx, ty, 15);
          return;
        }
      }
    }
    if (strncmp(cmd, "dm_pdf ", 7) == 0) {
      int f = 0;
      if (sscanf(cmd + 7, "%d", &f) == 1 && f >= 0 && f < 100) {
        FILE *fp = fopen("map_dump.txt", "w");
        if (fp) {
          for (int y = 0; y < MAP_HEIGHT; y++) {
            for (int x = 0; x < MAP_WIDTH; x++) {
              int v = master_world->floors[f].map.data[0][y][x];
              char ch = ' ';
              if (v == VOXEL_ROCK) ch = ' ';
              else if (v == VOXEL_WALL || v == VOXEL_OBSIDIAN) ch = '#';
              else if (v == VOXEL_DOOR) ch = '+';
              else if (v == VOXEL_WATER || v == VOXEL_LAVA) ch = '~';
              else if (v == VOXEL_STAIRS_DOWN) ch = '>';
              else if (v == VOXEL_STAIRS_UP) ch = '<';
              else if (v == VOXEL_GOLD_VEIN) ch = '$';
              else if (v == VOXEL_CRYSTAL_BLUE) ch = 'B';
              else if (v == VOXEL_CRYSTAL_PURPLE) ch = 'P';
              else if (v == VOXEL_MUSHROOM_GLOW) ch = 'M';
              else if (v == VOXEL_GRASS) ch = ',';
              else ch = '.';
              fputc(ch, fp);
            }
            fputc('\n', fp);
          }
          fclose(fp);
          char syscmd[256];
          snprintf(syscmd, sizeof(syscmd), "python3 tools/generate_pdf_map.py map_dump.txt map_floor_%d.pdf", f);
          if (system(syscmd) == -1) {}
          send_text_to_client(c->sock, "[DM] Generated PDF: map_floor_%d.pdf", f);
        }
      }
      return;
    }
    if (strcmp(cmd, "dm_mapfloor") == 0) {
      int f = c->floor_id;
      char dump_path[128];
      char pdf_path[128];
      snprintf(dump_path, sizeof(dump_path), "map_dump_floor_%d.txt", f);
      snprintf(pdf_path,  sizeof(pdf_path),  "map_floor_%d.pdf", f);
      FILE *fp = fopen(dump_path, "w");
      if (!fp) {
        send_text_to_client(c->sock, "[DM] Error opening temporary file.");
        return;
      }
      for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
          int v = master_world->floors[f].map.data[0][y][x];
          char ch = ' ';
          if      (v == VOXEL_ROCK)           ch = ' ';
          else if (v == VOXEL_WALL)           ch = '#';
          else if (v == VOXEL_OBSIDIAN)       ch = '#';
          else if (v == VOXEL_DOOR)           ch = '+';
          else if (v == VOXEL_WATER)          ch = '~';
          else if (v == VOXEL_LAVA)           ch = 'L';
          else if (v == VOXEL_STAIRS_DOWN)    ch = '>';
          else if (v == VOXEL_STAIRS_UP)      ch = '<';
          else if (v == VOXEL_GOLD_VEIN)      ch = '$';
          else if (v == VOXEL_CRYSTAL_BLUE)   ch = 'B';
          else if (v == VOXEL_CRYSTAL_PURPLE) ch = 'P';
          else if (v == VOXEL_MUSHROOM_GLOW)  ch = 'M';
          else if (v == VOXEL_GRASS)          ch = ',';
          else                                ch = '.';
          fputc(ch, fp);
        }
        fputc('\n', fp);
      }
      fclose(fp);
      char syscmd[512];
      snprintf(syscmd, sizeof(syscmd),
               "python3 tools/generate_pdf_map.py %s %s",
               dump_path, pdf_path);
      int ret = system(syscmd); (void)ret;
      if (ret == 0) {
        send_text_to_client(c->sock, "[DM] Generated PDF Map: %s (Floor %d)", pdf_path, f);
      } else {
        send_text_to_client(c->sock, "[DM] Error generating PDF (code %d).", ret);
      }
      return;
    }
    if (strncmp(cmd, "dm_shop ", 8) == 0) {
      int iid = 0, qty = 1;
      if (sscanf(cmd + 8, "%d %d", &iid, &qty) >= 1) {
        for (int i = 0; i < MAX_NPCS; i++) {
          if (npcs[i].active && npcs[i].archetype == ARCH_MERCHANT &&
              npcs[i].floor_id == c->floor_id) {
            if (abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 5) {
              add_item_to_shop(&npcs[i], iid, qty);
              send_text_to_client(c->sock, "[DM] Added item %d (qty %d) to %s",
                                  iid, qty, npcs[i].merchant.shop_name);
              return;
            }
          }
        }
      }
      return;
    }
    if (strncmp(cmd, "dm_place ", 9) == 0) {
      int iid = 0, tx = 0, ty = 0;
      if (sscanf(cmd + 9, "%d %d %d", &iid, &tx, &ty) == 3) {
        if (iid >= 0 && iid < item_database_size) {
          for (int i = 0; i < MAX_NPCS; i++) {
            if (!npcs[i].active) {
              npcs[i].active = true;
              npcs[i].entity_id = next_id++;
              npcs[i].floor_id = c->floor_id;
              npcs[i].x = tx;
              npcs[i].y = ty;
              npcs[i].archetype = ARCH_TREASURE;
              npcs[i].template_idx = iid; // Store item ID here for chests
              npcs[i].hp = npcs[i].max_hp = 1;
              send_text_to_client(c->sock,
                                  "[DM] Placed treasure with item %s at %d,%d",
                                  item_database[iid].name, tx, ty);
              return;
            }
          }
        }
      }
      return;
    }
  }

  if (strcmp(cmd, "retrieve") == 0) {
      int ghost_npc_idx = -1;
      char expected_name[64];
      snprintf(expected_name, sizeof(expected_name), "Ghost of %s", c->username);
      
      for (int i = 0; i < MAX_NPCS; i++) {
          if (npcs[i].active && npcs[i].is_ghost && 
              abs(npcs[i].x - c->x) <= 1 && abs(npcs[i].y - c->y) <= 1 &&
              npcs[i].floor_id == c->floor_id) {
              if (strcmp(npcs[i].custom_name, expected_name) == 0) {
                  ghost_npc_idx = i;
                  break;
              }
          }
      }
      
      if (ghost_npc_idx == -1) {
          send_text_to_client(c->sock, "[SYSTEM] No ghosts of you nearby.");
          return;
      }
      
      char fname[128];
      snprintf(fname, sizeof(fname), "data/bones_%d.dat", c->floor_id);
      FILE *bfile = fopen(fname, "rb");
      if (bfile) {
          BonesData b;
          if (fread(&b, sizeof(BonesData), 1, bfile) == 1) {
              c->gold += b.gold;
              for (int i = 0; i < 30; i++) {
                  if (b.items[i] >= 0 && b.amounts[i] > 0 && c->backpack_count < MAX_BACKPACK) {
                      c->backpack[c->backpack_count].template_idx = b.items[i];
                      c->backpack[c->backpack_count].stack_count = b.amounts[i];
                      c->backpack[c->backpack_count].durability = item_database[b.items[i]].max_durability;
                      c->backpack_count++;
                  }
              }
              int equipped[] = { b.w_idx, b.b_idx, b.h_idx, b.s_idx };
              for(int i = 0; i < 4; i++) {
                  if (equipped[i] >= 0 && c->backpack_count < MAX_BACKPACK) {
                      c->backpack[c->backpack_count].template_idx = equipped[i];
                      c->backpack[c->backpack_count].stack_count = 1;
                      c->backpack[c->backpack_count].durability = item_database[equipped[i]].max_durability;
                      c->backpack_count++;
                  }
              }
          }
          fclose(bfile);
          remove(fname);
      } else {
          if (npcs[ghost_npc_idx].gold_drop > 0) {
              c->gold += npcs[ghost_npc_idx].gold_drop;
              npcs[ghost_npc_idx].gold_drop = 0;
          }
          for (int gi = 0; gi < 30; gi++) {
              if (npcs[ghost_npc_idx].ghost_loot[gi].template_idx >= 0 &&
                  npcs[ghost_npc_idx].ghost_loot[gi].stack_count > 0 && c->backpack_count < MAX_BACKPACK) {
                  c->backpack[c->backpack_count++] = npcs[ghost_npc_idx].ghost_loot[gi];
                  npcs[ghost_npc_idx].ghost_loot[gi].template_idx = -1;
                  npcs[ghost_npc_idx].ghost_loot[gi].stack_count  = 0;
              }
          }
      }
      
      send_text_to_client(c->sock, "[SYSTEM] You have recovered your soul and equipment!");
      npcs[ghost_npc_idx].active = false;
      npcs[ghost_npc_idx].respawn_timer = 0;
      master_world->floors[c->floor_id].entity_grid[npcs[ghost_npc_idx].y][npcs[ghost_npc_idx].x] = 0;
      save_player_data(c);
      return;
  }

  if (strcmp(cmd, "list") == 0) {
    /*First check if there is a tombstone nearby*/
    bool tomb_found = false;
    for (int i = 0; i < MAX_TOMBSTONES; i++) {
      if (g_tombstones[i].active &&
          g_tombstones[i].floor_id == c->floor_id &&
          abs(g_tombstones[i].x - c->x) + abs(g_tombstones[i].y - c->y) <= 1) {
        tombstone_list(c);
        tomb_found = true;
        break;
      }
    }
    if (tomb_found) return;
    /*Then look for a merchant*/
    bool found = false;
    for (int i = 0; i < MAX_NPCS; i++) {
      if (npcs[i].active && npcs[i].archetype == ARCH_MERCHANT &&
          npcs[i].floor_id == c->floor_id &&
          abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
        print_merchant_inventory(c, &npcs[i]);
        found = true;
        break;
      }
    }
    if (!found)
      send_text_to_client(c->sock,
                          "[SYSTEM] No merchants or tombstones nearby.");
    return;
  }


  if (strcmp(cmd, "look") == 0 || strcmp(cmd, "l") == 0 || strcmp(cmd, "lock") == 0 ||
      strncmp(cmd, "look ", 5) == 0 || strncmp(cmd, "lock ", 5) == 0) {
    int vis_rad = get_vision_radius(c);
    int px = c->x;
    int py = c->y;
    int f = c->floor_id;

    if (vis_rad <= 0) {
      send_text_to_client(c->sock, "[INSPECTION] You can't see anything: you are surrounded by darkness or blinded!");
      return;
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
    for (int i = 0; i < MAX_NPCS; i++) {
      if (npcs[i].active && npcs[i].floor_id == f && npcs[i].x == px && npcs[i].y == py) {

        if (npcs[i].archetype == ARCH_GOLD) {
          strncat(feet_buf, "| [Golden Bag]", sizeof(feet_buf) - strlen(feet_buf) - 1);
        } else if (npcs[i].archetype == ARCH_TREASURE) {
          strncat(feet_buf, "| [Treasury Chest]", sizeof(feet_buf) - strlen(feet_buf) - 1);
        } else if (npcs[i].is_ghost) {
          char gbuf[64];
          snprintf(gbuf, sizeof(gbuf), "| [%s remains]", npcs[i].custom_name);
          strncat(feet_buf, gbuf, sizeof(feet_buf) - strlen(feet_buf) - 1);
        } else if (npcs[i].archetype == ARCH_MERCHANT) {
          char mbuf[64];
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
        for (int i = 0; i < MAX_NPCS; i++) {
          if (npcs[i].active && npcs[i].floor_id == f && npcs[i].x == nx && npcs[i].y == ny) {
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
    return;
  }

  if (strcmp(cmd, "fountain") == 0) {
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
    return;
  }

  if (strcmp(cmd, "statue") == 0 || strcmp(cmd, "crystal") == 0) {
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
    return;
  }

  if (strcmp(cmd, "status") == 0 || strcmp(cmd, "stats") == 0) {
    int ts, td, tc, ti, tw, th;
    get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
    //Calculate AC
    int ac_base = (c->slot_body.template_idx != -1)
                      ? item_database[c->slot_body.template_idx].ac_base
                      : 0;
    int ac_bonus = 0;
    if (c->slot_head.template_idx != -1)
      ac_bonus += item_database[c->slot_head.template_idx].ac_bonus + c->slot_head.ac_bonus;
    if (c->slot_body.template_idx != -1)
      ac_bonus += c->slot_body.ac_bonus;
    if (c->slot_hand_r.template_idx != -1 &&
        item_database[c->slot_hand_r.template_idx].category == ITEM_SHIELD)
      ac_bonus += item_database[c->slot_hand_r.template_idx].ac_bonus + c->slot_hand_r.ac_bonus;
    if (c->slot_hand_l.template_idx != -1 &&
        item_database[c->slot_hand_l.template_idx].category == ITEM_SHIELD)
      ac_bonus += item_database[c->slot_hand_l.template_idx].ac_bonus + c->slot_hand_l.ac_bonus;
    int ac = (ac_base > 0 ? ac_base : 10 + rules_get_modifier(td)) + ac_bonus;
    //Weapon equipped
    const char *weapon_name = "Bare hands";
    char weapon_buf[128];
    if (c->slot_hand_r.template_idx != -1) {
      get_full_item_name(&c->slot_hand_r, weapon_buf, sizeof(weapon_buf));
      weapon_name = weapon_buf;
    }
    else if (c->slot_hand_l.template_idx != -1) {
      get_full_item_name(&c->slot_hand_l, weapon_buf, sizeof(weapon_buf));
      weapon_name = weapon_buf;
    }
    //Proficiency bonuses
    int prof = 2 + (c->level / 4);
    send_text_to_client(c->sock, "======== %s ========", c->username);
    send_text_to_client(c->sock, "Lvl:%-3d XP:%-6d Floor:%d", c->level,
                        c->xp, c->floor_id);
    send_text_to_client(c->sock, "HP: %d/%d AC:%d Prof:+%d Gold:%lu gp",
                        c->hp, c->max_hp, ac, prof, (unsigned long)c->gold);
    send_text_to_client(c->sock, "--- ATTRIBUTES ---");
    send_text_to_client(c->sock, "  STR %2d(%+d)  DEX %2d(%+d)  CON %2d(%+d)",
                        ts, rules_get_modifier(ts), td, rules_get_modifier(td),
                        tc, rules_get_modifier(tc));
    send_text_to_client(c->sock, "  INT %2d(%+d)  WIS %2d(%+d)  CHA %2d(%+d)",
                        ti, rules_get_modifier(ti), tw, rules_get_modifier(tw),
                        th, rules_get_modifier(th));
    send_text_to_client(c->sock, "--- EQUIPMENT ---");
    send_text_to_client(c->sock, "  Weapon: %s", weapon_name);
    if (c->slot_body.template_idx != -1) {
      char armor_buf[128];
      get_full_item_name(&c->slot_body, armor_buf, sizeof(armor_buf));
      send_text_to_client(c->sock, "Armor: %s (base AC %d)",
                          armor_buf,
                          ac_base);
    }
    //Spell Slots
    bool has_slots = false;
    for (int s = 1; s <= MAX_SPELL_LEVEL; s++) {
      if (c->spell_slots_max[s] > 0) {
        has_slots = true;
        break;
      }
    }
    if (has_slots) {
      send_text_to_client(c->sock, "--- SPELL SLOT ---");
      for (int s = 1; s <= MAX_SPELL_LEVEL; s++) {
        if (c->spell_slots_max[s] > 0)
          send_text_to_client(c->sock, "  Lvl%d: %d/%d", s, c->spell_slots[s],
                              c->spell_slots_max[s]);
      }
    }
    //Active effects
    if (c->effect_count > 0) {
      send_text_to_client(c->sock, "--- ACTIVE EFFECTS ---");
      for (int e = 0; e < c->effect_count; e++) {
        ActiveEffect *ef = &c->effects[e];
        send_text_to_client(c->sock, "  #%d val:%d (%s)", e + 1, ef->value,
                            ef->is_persistent ? "persistent" : "temporary");
      }
    }
    send_text_to_client(c->sock, "==============================");
    return;
  }

  if (strcmp(cmd, "inventory") == 0 || strcmp(cmd, "i") == 0) {
    float w = get_current_weight(c);
    send_text_to_client(
        c->sock,
        "================ INVENTORY (%.1f/%.1f kg) ================", w,
        (float)(c->str * 5));

    ItemInstance *sl[] = {&c->slot_head,  &c->slot_neck,   &c->slot_body,
                          &c->slot_back,  &c->slot_hand_r, &c->slot_hand_l,
                          &c->slot_hands, &c->slot_arm_r, &c->slot_arm_l,
                          &c->slot_feet};
    const char *ns[] = {"HEAD", "NECK", "BODY", "BACK",
                        "R.HAND",  "L.HAND",  "HANDS",  "R.ARM", "L.ARM", "FEET"};

    send_text_to_client(c->sock, "--- EQUIPPED ---");
    for (int i = 0; i < 10; i++) {
      if (sl[i]->template_idx != -1) {
        char buf[128];
        get_full_item_name(sl[i], buf, sizeof(buf));
        send_text_to_client(c->sock, " [%-7s] %s (Dur:%d/%d)", ns[i],
                            buf,
                            sl[i]->durability,
                            item_database[sl[i]->template_idx].max_durability);
      }
    }

    // Ring display
    bool has_rings = false;
    for (int i = 0; i < 10; i++) {
      if (c->slot_rings[i].template_idx != -1) {
        if (!has_rings) {
          send_text_to_client(c->sock, "--- RINGS ---");
          has_rings = true;
        }
        char buf[128];
        get_full_item_name(&c->slot_rings[i], buf, sizeof(buf));
        send_text_to_client(
            c->sock, " [RING %d] %s", i + 1, buf);
      }
    }

    send_text_to_client(c->sock, "--- ON BELT ---");
    for (int i = 0; i < MAX_BELT; i++) {
      if (c->belt[i].template_idx != -1) {
        char buf[128];
        get_full_item_name(&c->belt[i], buf, sizeof(buf));
        send_text_to_client(c->sock, " [SLOT %d] %s (x%d)", i + 1,
                            buf,
                            c->belt[i].stack_count);
      }
    }

    send_text_to_client(c->sock, "--- IN BACKPACK ---");
    if (c->backpack_count == 0) {
      send_text_to_client(c->sock, "  (empty backpack)");
    } else {
      for (int i = 0; i < c->backpack_count; i++) {
        char buf[128];
        get_full_item_name(&c->backpack[i], buf, sizeof(buf));
        send_text_to_client(
            c->sock, " [%d] %s (x%d)", i + 1,
            buf,
            c->backpack[i].stack_count);
      }
    }
    send_text_to_client(
        c->sock, "==========================================================");
    return;
  }

  if (strncmp(cmd, "wear ", 5) == 0 || strncmp(cmd, "equip ", 6) == 0 || strncmp(cmd, "wield ", 6) == 0 || strncmp(cmd, "w ", 2) == 0) {
    const char *input = strchr(cmd, ' '); if (input) input++; else return;
    ItemInstance *t = NULL;
    int loc = -1, si = -1;

    //1. Search for the item in your backpack or belt (by name or index)
    if (isdigit(input[0])) {
      int requested_idx = atoi(input) - 1;
      if (requested_idx >= 0 && requested_idx < c->backpack_count) {
        t = &c->backpack[requested_idx];
        loc = 0;
        si = requested_idx;
      }
    } else {
      //Search your backpack by name
      for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx != -1 &&
            strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
          t = &c->backpack[i];
          loc = 0;
          si = i;
          break;
        }
      }
      //Search by name in belt
      if (!t) {
        for (int i = 0; i < MAX_BELT; i++) {
          if (c->belt[i].template_idx != -1 &&
              strcasestr(item_database[c->belt[i].template_idx].name, input) != NULL) {
            t = &c->belt[i];
            loc = 1;
            si = i;
            break;
          }
        }
      }
    }

    if (!t) {
      send_text_to_client(c->sock, "[ERROR] Object not found (use name"
                                   "or backpack number).");
      return;
    }

    const ItemTemplate *it = &item_database[t->template_idx];
    ItemInstance *slot = NULL;
    switch (it->category) {
    case ITEM_WEAPON:
    case ITEM_SHIELD:
      if (c->slot_hand_r.template_idx == -1)
        slot = &c->slot_hand_r;
      else
        slot = &c->slot_hand_l;
      break;
    case ITEM_HEAD:
      slot = &c->slot_head;
      break;
    case ITEM_NECK:
      slot = &c->slot_neck;
      break;
    case ITEM_ARMOR:
      slot = &c->slot_body;
      break;
    case ITEM_BACK:
      slot = &c->slot_back;
      break;
    case ITEM_HANDS:
      slot = &c->slot_hands;
      break;
    case ITEM_FEET:
      slot = &c->slot_feet;
      break;
    case ITEM_BRACELET:
      if (c->slot_arm_r.template_idx == -1)
        slot = &c->slot_arm_r;
      else
        slot = &c->slot_arm_l;
      break;
    case ITEM_RING:
      for (int r = 0; r < 10; r++) {
        if (c->slot_rings[r].template_idx == -1) {
          slot = &c->slot_rings[r];
          break;
        }
      }
      if (!slot)
        slot = &c->slot_rings[0]; //Overwrites the first one if full
      break;
    default:
      send_text_to_client(c->sock,
                          "[ERROR] You cannot wear this item.");
      return;
    }

    if (slot) {
      ItemInstance prev = *slot;
      *slot = *t;
      // Remove from source
      if (loc == 0) {
        c->backpack[si] = c->backpack[c->backpack_count - 1];
        c->backpack_count--;
      } else {
        c->belt[si].template_idx = -1;
      }
      //Place the removed item in your backpack
      if (prev.template_idx != -1) {
        if (c->backpack_count < MAX_BACKPACK) {
          c->backpack[c->backpack_count++] = prev;
        } else {
          send_text_to_client(c->sock, "[WARNING] Full backpack, the object"
                                       "removed it was lost in the void!");
        }
      }
      //===== BLIND IDENTIFICATION (Mechanics 2) =====
      //Wearing an unidentified object reveals it... but if it's Cursed
      //it's a trap!
      if (!slot->is_identified) {
        slot->is_identified = true;
        char id_buf[128];
        get_full_item_name(slot, id_buf, sizeof(id_buf));
        if (slot->blessing == BLESS_CURSED) {
          send_text_to_client(c->sock,
              "[MAGIC] By wearing the object you identify it: it's %s!",
              id_buf);
          send_text_to_client(c->sock,
              "[CURSE] A dark aura envelops your hand!"
              "The item is CURSED and you can't take it away!");
        } else {
          send_text_to_client(c->sock,
              "[MAGIC] By wearing the object you identify it: it's %s!",
              id_buf);
        }
      } else {
        send_text_to_client(c->sock, "[SYSTEM] You have equipped %s.", it->name);
      }
      send_detailed_state(c);
    }
    return;
  }

  //===== MECHANICAL 1: REMOVE (remove equipped item) =====
  if (strncmp(cmd, "remove ", 7) == 0 || strncmp(cmd, "unequip ", 8) == 0 || strncmp(cmd, "takeoff ", 8) == 0 || strncmp(cmd, "t ", 2) == 0) {
    const char *input = strchr(cmd, ' '); if (input) input++; else return;
    //Slots map by name
    ItemInstance *sls[] = {
        &c->slot_head, &c->slot_neck, &c->slot_body, &c->slot_back,
        &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
      &c->slot_arm_l, &c->slot_feet
    };
    const char *snames[] = {
        "head", "neck", "body", "back",
        "hand_r", "hand_l", "hands", "arm_r", "arm_l", "feet"
    };
    ItemInstance *target_slot = NULL;
    int scount = 10;
    //Search by slot name or item name
    for (int si2 = 0; si2 < scount; si2++) {
      if (sls[si2]->template_idx == -1)
        continue;
      const char *iname = item_database[sls[si2]->template_idx].name;
      bool slot_match = (strcasecmp(input, snames[si2]) == 0);
      if (!slot_match) {
        if (si2 == 7 && (strcasecmp(input, "right_arm") == 0 || strcasecmp(input, "arm_r") == 0 || strcasecmp(input, "r.a") == 0)) slot_match = true;
        else if (si2 == 8 && (strcasecmp(input, "left_arm") == 0 || strcasecmp(input, "arm_l") == 0 || strcasecmp(input, "l.a") == 0)) slot_match = true;
        else if ((si2 == 7 || si2 == 8) && (strcasecmp(input, "arm") == 0 || strcasecmp(input, "arms") == 0)) slot_match = true;
      }
      if (strcasestr(iname, input) != NULL || slot_match ||
          (sls[si2]->is_artifact &&
           strcasestr(sls[si2]->artifact_name, input) != NULL)) {
        target_slot = sls[si2];
        break;
      }
    }
    //Also check the rings
    if (!target_slot) {
      for (int ri = 0; ri < 10; ri++) {
        if (c->slot_rings[ri].template_idx == -1)
          continue;
        const char *iname = item_database[c->slot_rings[ri].template_idx].name;
        if (strcasestr(iname, input) != NULL) {
          target_slot = &c->slot_rings[ri];
          break;
        }
      }
    }
    //Also check your seat belt
    if (!target_slot) {
      for (int bi = 0; bi < MAX_BELT; bi++) {
        if (c->belt[bi].template_idx == -1)
          continue;
        const char *iname = item_database[c->belt[bi].template_idx].name;
        if (strcasestr(iname, input) != NULL) {
          target_slot = &c->belt[bi];
          break;
        }
      }
    }
    if (!target_slot) {
      send_text_to_client(c->sock,
          "[ERROR] Item not found among those equipped."
          "Use 'stats' to see what you're wearing.");
      return;
    }
    //===== CURSE BLOCK (Heart of Mechanics 1) =====
    if (target_slot->blessing == BLESS_CURSED && target_slot->is_identified) {
      char curse_buf[128];
      get_full_item_name(target_slot, curse_buf, sizeof(curse_buf));
      send_text_to_client(c->sock,
          "[CURSE] The object '%s' is surrounded by an evil aura!"
          "You can't take it off!",
          curse_buf);
      send_text_to_client(c->sock,
          "[TIP] Find a cleric and use 'uncurse' (200gp)"
          "or use a Scroll of Curse Removal.");
      return;
    }
    //Normal removal: put in backpack
    if (c->backpack_count >= MAX_BACKPACK) {
      send_text_to_client(c->sock,
          "[ERROR] Backpack full! Empty your backpack first.");
      return;
    }
    char rem_buf[128];
    get_full_item_name(target_slot, rem_buf, sizeof(rem_buf));
    c->backpack[c->backpack_count++] = *target_slot;
    target_slot->template_idx = -1;
    send_text_to_client(c->sock,
        "[SYSTEM] You have removed %s and moved it to the backpack.", rem_buf);
    send_detailed_state(c);
    return;
  }

  if (strncmp(cmd, "buy ", 4) == 0) {
    const char *input = cmd + 4;
    //1. Look for a nearby merchant
    int m_idx = -1;
    for (int i = 0; i < MAX_NPCS; i++) {
      if (npcs[i].active && npcs[i].archetype == ARCH_MERCHANT &&
          npcs[i].floor_id == c->floor_id) {
        if (abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
          m_idx = i;
          break;
        }
      }
    }

    if (m_idx == -1) {
      send_text_to_client(c->sock, "[ERROR] No merchant nearby.");
      return;
    }

    //2. Search for the object by name or index
    int shop_item_idx = -1;
    if (isdigit(input[0])) {
      int requested_idx = atoi(input) - 1;
      if (requested_idx >= 0 &&
          requested_idx < npcs[m_idx].merchant.item_count) {
        shop_item_idx = requested_idx;
      }
    } else {
      for (int j = 0; j < npcs[m_idx].merchant.item_count; j++) {
        int t_idx = npcs[m_idx].merchant.item_templates[j];
        if (strcasestr(item_database[t_idx].name, input) != NULL) {
          shop_item_idx = j;
          break;
        }
      }
    }

    if (shop_item_idx == -1) {
      send_text_to_client(c->sock, "[STORE] Item not found.");
      return;
    }

    if (npcs[m_idx].merchant.item_stock[shop_item_idx] <= 0) {
      send_text_to_client(c->sock, "[STORE] Item sold out!");
      return;
    }

    int d_idx = npcs[m_idx].merchant.item_templates[shop_item_idx];
    const ItemTemplate *it = &item_database[d_idx];
    
    /* --- Phase 3: Reactive Economy (Dynamic Prices) --- */
    float supply = 1.0f;
    if (npcs[m_idx].merchant.item_stock_max[shop_item_idx] > 0) {
      supply = (float)npcs[m_idx].merchant.item_stock[shop_item_idx] / (float)npcs[m_idx].merchant.item_stock_max[shop_item_idx];
    }
    float economy_modifier = 1.0f + (1.0f - supply) * 0.5f;
    if (economy_modifier < 0.5f) economy_modifier = 0.5f;
    if (economy_modifier > 2.0f) economy_modifier = 2.0f;
    uint64_t final_cost = (uint64_t)(it->cost * economy_modifier);

    //Check if there is a negotiated offer
    if (c->pending_trade_merchant_id == npcs[m_idx].entity_id &&
        c->pending_trade_is_buy && c->pending_trade_item_idx == shop_item_idx) {
      final_cost = c->pending_trade_price;
      send_text_to_client(c->sock,
                          "[SYSTEM] Apply the negotiated price: %lu gp.",
                          (unsigned long)final_cost);
    }

    if (c->gold < final_cost) {
      send_text_to_client(c->sock,
                          "[ERROR] Insufficient gold (%lu gp required).",
                          (unsigned long)final_cost);
      return;
    }

    //3. Backpack management (stacking or new slot)
    int backpack_idx = -1;
    if (it->max_stack > 1) {
      for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx == d_idx &&
            c->backpack[i].stack_count < it->max_stack) {
          backpack_idx = i;
          break;
        }
      }
    }

    if (backpack_idx != -1) {
      //Existing stack
      c->gold -= final_cost;
      c->backpack[backpack_idx].stack_count++;
      npcs[m_idx].merchant.item_stock[shop_item_idx]--;
      send_text_to_client(
          c->sock, "[STORE] Bought %s (added to stack).", it->name);
    } else if (c->backpack_count < MAX_BACKPACK) {
      //New slots
      c->gold -= final_cost;
      memset(&c->backpack[c->backpack_count], 0, sizeof(ItemInstance));
      c->backpack[c->backpack_count].template_idx  = d_idx;
      c->backpack[c->backpack_count].durability    = (it->category == ITEM_LIGHT_SOURCE && strcasestr(it->name, "lantern")) ? 5000 : it->max_durability;
      c->backpack[c->backpack_count].stack_count   = 1;
      c->backpack[c->backpack_count].is_identified = true; //Merchant guarantees the item
      c->backpack[c->backpack_count].blessing      = BLESS_NORMAL;
      c->backpack[c->backpack_count].quality       = QUALITY_NORMAL;
      c->backpack[c->backpack_count].element       = ELEM_NONE;
      c->backpack_count++;
      npcs[m_idx].merchant.item_stock[shop_item_idx]--;
      send_text_to_client(c->sock, "[STORE] Bought %s.", it->name);
    } else {
      send_text_to_client(c->sock, "[ERROR] Backpack full!");
    }
    save_player_data(c);

    //Reset offer after use
    c->pending_trade_merchant_id = -1;
    return;
  }

  if (strncmp(cmd, "sell ", 5) == 0) {
    const char *input = cmd + 5;
    int si = -1;
    if (isdigit(input[0])) {
      int requested_idx = atoi(input) - 1;
      if (requested_idx >= 0 && requested_idx < c->backpack_count)
        si = requested_idx;
    } else {
      for (int i = 0; i < c->backpack_count; i++)
        if (strcasecmp(input,
                       item_database[c->backpack[i].template_idx].name) == 0) {
          si = i;
          break;
        }
    }

    if (si != -1) {
      uint64_t final_val = item_database[c->backpack[si].template_idx].cost / 2;
      int m_idx = -1;
      for (int i = 0; i < MAX_NPCS; i++) {
        if (npcs[i].active && npcs[i].archetype == ARCH_MERCHANT &&
            npcs[i].floor_id == c->floor_id) {
          if (abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
            m_idx = i;
            break;
          }
        }
      }
      
      if (m_idx != -1) {
        /* -- Phase 3: Reactive Economy (Selling) --- */
        int shop_idx = -1;
        for (int j = 0; j < npcs[m_idx].merchant.item_count; j++) {
          if (npcs[m_idx].merchant.item_templates[j] == c->backpack[si].template_idx) {
            shop_idx = j;
            break;
          }
        }
        if (shop_idx != -1) {
          float supply = 1.0f;
          if (npcs[m_idx].merchant.item_stock_max[shop_idx] > 0) {
            supply = (float)npcs[m_idx].merchant.item_stock[shop_idx] / (float)npcs[m_idx].merchant.item_stock_max[shop_idx];
          }
          float economy_modifier = 1.0f + (1.0f - supply) * 0.5f;
          if (economy_modifier < 0.5f) economy_modifier = 0.5f;
          if (economy_modifier > 2.0f) economy_modifier = 2.0f;
          final_val = (uint64_t)(final_val * economy_modifier);
          
          /*The merchant acquires the item, increasing local supply*/
          npcs[m_idx].merchant.item_stock[shop_idx]++;
        }
      }

      if (m_idx != -1 &&
          c->pending_trade_merchant_id == npcs[m_idx].entity_id &&
          !c->pending_trade_is_buy && c->pending_trade_item_idx == si) {
        final_val = c->pending_trade_price;
        send_text_to_client(c->sock,
                            "[SYSTEM] Apply the negotiated price: %lu gp.",
                            (unsigned long)final_val);
      }
      c->gold += final_val;
      send_text_to_client(c->sock, "[STORE] Sold %s for %lu gp.",
                          item_database[c->backpack[si].template_idx].name,
                          (unsigned long)final_val);
      if (c->backpack[si].stack_count > 1) {
        c->backpack[si].stack_count--;
      } else {
        c->backpack[si] = c->backpack[c->backpack_count - 1];
        c->backpack_count--;
      }
      save_player_data(c);
      c->pending_trade_merchant_id = -1;
    }
    return;
  }

  if (strncmp(cmd, "haggle ", 7) == 0) {
    const char *sub = cmd + 7;
    bool is_buying = (strncmp(sub, "buy ", 4) == 0);
    bool is_selling = (strncmp(sub, "sell ", 5) == 0);

    if (!is_buying && !is_selling) {
      send_text_to_client(c->sock,
                          "[ERROR] Usage: haggle buy <n> or haggle sell <n>");
      return;
    }

    const char *input = is_buying ? sub + 4 : sub + 5;
    int m_idx = -1;
    for (int i = 0; i < MAX_NPCS; i++) {
      if (npcs[i].active && npcs[i].archetype == ARCH_MERCHANT &&
          npcs[i].floor_id == c->floor_id) {
        if (abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
          m_idx = i;
          break;
        }
      }
    }
    if (m_idx == -1) {
      send_text_to_client(c->sock, "[ERROR] No merchant nearby.");
      return;
    }

    int item_idx = -1;
    if (is_buying) {
      if (isdigit(input[0])) {
        int r = atoi(input) - 1;
        if (r >= 0 && r < npcs[m_idx].merchant.item_count)
          item_idx = r;
      }
      if (item_idx == -1 || npcs[m_idx].merchant.item_stock[item_idx] <= 0) {
        send_text_to_client(
            c->sock, "[SHOP] Item not available for trading.");
        return;
      }
    } else {
      if (isdigit(input[0])) {
        int r = atoi(input) - 1;
        if (r >= 0 && r < c->backpack_count)
          item_idx = r;
      }
      if (item_idx == -1) {
        send_text_to_client(c->sock,
                            "[ERROR] Item not found in backpack.");
        return;
      }
    }

    //THE GAME: Duel of Charisma
    if (c->pending_trade_merchant_id == npcs[m_idx].entity_id &&
        c->pending_trade_item_idx == item_idx &&
        c->pending_trade_is_buy == is_buying) {
      c->pending_trade_attempts++;
    } else {
      c->pending_trade_attempts = 1;
    }

    send_text_to_client(
        c->sock, "[GAME] You try to bargain with %s... (Attempt %d)",
        npcs[m_idx].merchant.shop_name, c->pending_trade_attempts);
    int p_roll = rules_roll_dice(1, 20);
    int m_impazienza = (c->pending_trade_attempts - 1) * 3;
    int m_roll = rules_roll_dice(1, 20) + 2 +
                 m_impazienza; //The merchants are cunning and lose patience
    int p_total = p_roll + rules_get_modifier(c->cha);

    send_text_to_client(c->sock, "> Your Roll: %d + %d = %d", p_roll,
                        rules_get_modifier(c->cha), p_total);
    send_text_to_client(c->sock, "> Merchant Roll: %d (Impatience: +%d)",
                        m_roll - m_impazienza, m_impazienza);

    float multiplier = 1.0f;
    if (p_roll == 20) {
      send_text_to_client(
          c->sock,
          "[CRITICAL SUCCESS] The merchant is enraptured by your words!");
      multiplier = is_buying ? 0.5f : 1.75f;
    } else if (p_roll == 1) {
      send_text_to_client(
          c->sock, "[CRITICAL FAIL] The merchant is mortally offended!");
      multiplier = is_buying ? 2.5f : 0.10f; // Terrible prices!
      c->pending_trade_attempts += 5;        // Much harder to recover from
    } else if (p_total >= m_roll) {
      send_text_to_client(
          c->sock, "[SUCCESS] You managed to get a better price.");
      multiplier = is_buying ? 0.8f : 1.25f;
    } else {
      send_text_to_client(
          c->sock,
          "[FAILURE] The merchant does not give up. Maybe you should stop...");
      multiplier = is_buying ? 1.2f : 0.8f; //The price gets worse!
    }

    if (is_buying) {
      int d_idx = npcs[m_idx].merchant.item_templates[item_idx];
      /* --- Phase 3: Reactive Economy --- */
      float supply = 1.0f;
      if (npcs[m_idx].merchant.item_stock_max[item_idx] > 0) {
        supply = (float)npcs[m_idx].merchant.item_stock[item_idx] / (float)npcs[m_idx].merchant.item_stock_max[item_idx];
      }
      float economy_modifier = 1.0f + (1.0f - supply) * 0.5f;
      if (economy_modifier < 0.5f) economy_modifier = 0.5f;
      if (economy_modifier > 2.0f) economy_modifier = 2.0f;
      
      uint64_t final_cost =
          (uint64_t)((float)item_database[d_idx].cost * economy_modifier * multiplier);
      c->pending_trade_item_idx = item_idx;
      c->pending_trade_price = final_cost;
      c->pending_trade_merchant_id = npcs[m_idx].entity_id;
      c->pending_trade_is_buy = true;
      send_text_to_client(c->sock,
                          "[STORE] Current offer for %s: %lu gp. Use 'buy"
                          "%d' to confirm.",
                          item_database[d_idx].name, (unsigned long)final_cost,
                          item_idx + 1);
    } else {
      int d_idx = c->backpack[item_idx].template_idx;
      /* --- Phase 3: Reactive Economy --- */
      float economy_modifier = 1.0f;
      int shop_idx = -1;
      for (int j = 0; j < npcs[m_idx].merchant.item_count; j++) {
        if (npcs[m_idx].merchant.item_templates[j] == d_idx) {
          shop_idx = j;
          break;
        }
      }
      if (shop_idx != -1) {
        float supply = 1.0f;
        if (npcs[m_idx].merchant.item_stock_max[shop_idx] > 0) {
          supply = (float)npcs[m_idx].merchant.item_stock[shop_idx] / (float)npcs[m_idx].merchant.item_stock_max[shop_idx];
        }
        economy_modifier = 1.0f + (1.0f - supply) * 0.5f;
        if (economy_modifier < 0.5f) economy_modifier = 0.5f;
        if (economy_modifier > 2.0f) economy_modifier = 2.0f;
      }
      
      uint64_t final_val =
          (uint64_t)((float)(item_database[d_idx].cost / 2) * economy_modifier * multiplier);
      c->pending_trade_item_idx = item_idx;
      c->pending_trade_price = final_val;
      c->pending_trade_merchant_id = npcs[m_idx].entity_id;
      c->pending_trade_is_buy = false;
      send_text_to_client(c->sock,
                          "[STORE] Current offer for your %s: %lu gp."
                          "Use 'sell %d' to confirm.",
                          item_database[d_idx].name, (unsigned long)final_val,
                          item_idx + 1);
    }
    return;
  }

  //identify [n] — identifies the first unidentified object
  //or the nth if specified. Cost: 100 gp per item.
  if (strncmp(cmd, "identify", 8) == 0) {
    //Check if we are close to a merchant
    bool near_merchant = false;
    for (int i = 0; i < MAX_NPCS; i++) {
      if (npcs[i].active && npcs[i].archetype == ARCH_MERCHANT &&
          npcs[i].floor_id == c->floor_id &&
          abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
        near_merchant = true;
        break;
      }
    }
    if (!near_merchant) {
      send_text_to_client(c->sock,
          "[ERROR] You must be near a merchant to identify.");
      return;
    }
    //Optional argument parsing: "identify 3" or "identify sword"
    int target_idx = -1;
    if (cmd[8] == ' ') {
      const char *arg = cmd + 9;
      if (isdigit(arg[0])) {
        int n = atoi(arg) - 1;
        if (n >= 0 && n < c->backpack_count && !c->backpack[n].is_identified)
          target_idx = n;
      } else {
        for (int i = 0; i < c->backpack_count; i++) {
          if (!c->backpack[i].is_identified)
            target_idx = i;
          const char *iname = item_database[c->backpack[i].template_idx].name;
          if (strcasecmp(iname, arg) == 0) {
            target_idx = i;
            break;
          }
        }
      }
    } else {
      //Without argument: first unidentified
      for (int i = 0; i < c->backpack_count; i++) {
        if (!c->backpack[i].is_identified) {
          target_idx = i;
          break;
        }
      }
    }
    if (target_idx == -1) {
      send_text_to_client(c->sock,
          "[ALCHEMIST] You have no unidentified items.");
      return;
    }
    if (c->gold < 100) {
      send_text_to_client(c->sock,
          "[ALCHEMIST] Identification service costs 100gp."
          "You don't have enough gold.");
      return;
    }
    c->gold -= 100;
    c->backpack[target_idx].is_identified = true;
    char id_buf[128];
    get_full_item_name(&c->backpack[target_idx], id_buf, sizeof(id_buf));
    if (c->backpack[target_idx].blessing == BLESS_CURSED) {
      send_text_to_client(c->sock,
          "[ALCHEMIST] *whistles softly* The item is: %s"
          "-- WARNING: it's CURSED! Don't wear it!",
          id_buf);
    } else {
      send_text_to_client(c->sock,
          "[ALCHEMIST] The item is: %s", id_buf);
    }
    return;
  }

  if (strcmp(cmd, "cure") == 0) {
    if (c->gold >= 50) {
      c->gold -= 50;
      c->effect_count = 0;
      send_text_to_client(
          c->sock,
          "[TEMPLE] Your afflictions have been cleansed by the Arcanum!");
    } else
      send_text_to_client(c->sock, "[ERROR] Not enough gold (50gp).");
    return;
  }

  if (strncmp(cmd, "belt ", 5) == 0) {
    const char *input = cmd + 5;
    int bi = -1;
    if (isdigit(input[0])) {
      int requested_idx = atoi(input) - 1;
      if (requested_idx >= 0 && requested_idx < c->backpack_count)
        bi = requested_idx;
    } else {
      for (int i = 0; i < c->backpack_count; i++)
        if (strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
          bi = i;
          break;
        }
    }

    if (bi != -1) {
      for (int i = 0; i < MAX_BELT; i++) {
        if (c->belt[i].template_idx == -1) {
          c->belt[i] = c->backpack[bi];
          c->backpack[bi] = c->backpack[c->backpack_count - 1];
          c->backpack_count--;
          send_text_to_client(c->sock, "[SYSTEM] %s moved to belt.",
                              item_database[c->belt[i].template_idx].name);
          send_detailed_state(c); //Update client view radius
          return;
        }
      }
      send_text_to_client(c->sock, "[ERROR] Belt full!");
    } else {
      send_text_to_client(c->sock, "[ERROR] Item not found in backpack.");
    }
    return;
  }

  if (strncmp(cmd, "unbelt ", 7) == 0 || strncmp(cmd, "unbelt", 6) == 0) {
    const char *input = strchr(cmd, ' ');
    if (input) input++;
    int slot_idx = -1;
    if (input && isdigit(input[0])) {
      int requested_idx = atoi(input) - 1;
      if (requested_idx >= 0 && requested_idx < MAX_BELT && c->belt[requested_idx].template_idx != -1) {
        slot_idx = requested_idx;
      }
    } else if (input) {
      for (int i = 0; i < MAX_BELT; i++) {
        if (c->belt[i].template_idx != -1 &&
            strcasestr(item_database[c->belt[i].template_idx].name, input) != NULL) {
          slot_idx = i;
          break;
        }
      }
    } else {
      /*If unbelt without arguments, move the first belted item into the backpack*/
      for (int i = 0; i < MAX_BELT; i++) {
        if (c->belt[i].template_idx != -1) {
          slot_idx = i;
          break;
        }
      }
    }

    if (slot_idx != -1) {
      if (c->backpack_count >= MAX_BACKPACK) {
        send_text_to_client(c->sock, "[ERROR] Backpack full!");
        return;
      }
      ItemInstance item = c->belt[slot_idx];
      c->backpack[c->backpack_count++] = item;
      c->belt[slot_idx].template_idx = -1;
      c->belt[slot_idx].stack_count = 0;
      send_text_to_client(c->sock, "[SYSTEM] %s moved from belt to backpack.",
                          item_database[item.template_idx].name);
      send_detailed_state(c);
    } else {
      send_text_to_client(c->sock, "[ERROR] No items found in belt (use 'unbelt <n>' or 'unbelt <name>').");
    }
    return;
  }
  //repair [slot|name] — repairs ONE equipped item or the slot.
  //Must be done on floor 0 near the Blacksmith (NPC with name "Blacksmith"
  //or ARCH_BLACKSMITH or any merchant on floor 0 as a fallback).
  if (strncmp(cmd, "repair", 6) == 0) {
    if (c->floor_id != 0) {
      send_text_to_client(c->sock,
          "[ERROR] The Blacksmith is only found on floor 0 (city).");
      return;
    }
    //Check for a merchant/blacksmith near floor 0
    bool near_smith = false;
    for (int i = 0; i < MAX_NPCS; i++) {
      if (npcs[i].active && npcs[i].floor_id == 0 &&
          npcs[i].archetype == ARCH_MERCHANT &&
          abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 5) {
        near_smith = true;
        break;
      }
    }
    if (!near_smith) {
      send_text_to_client(c->sock,
          "[ERROR] You must be near the Blacksmith to repair.");
      return;
    }
    //Collects all repairable slots
    ItemInstance *sls[] = {
        &c->slot_head, &c->slot_neck, &c->slot_body, &c->slot_back,
        &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
      &c->slot_arm_l, &c->slot_feet
    };
    //Optional argument: specific slot
    const char *repair_arg = (cmd[6] == ' ') ? cmd + 7 : NULL;
    int total_cost = 0;
    int items_repaired = 0;
    for (int i = 0; i < 10; i++) {
      if (sls[i]->template_idx == -1)
        continue;
      //If an argument is specified, repair only that object
      if (repair_arg != NULL) {
        const char *iname = item_database[sls[i]->template_idx].name;
        if (strcasecmp(iname, repair_arg) != 0 &&
            !(sls[i]->is_artifact &&
              strcasecmp(sls[i]->artifact_name, repair_arg) == 0))
          continue;
      }
      int max_dur = item_database[sls[i]->template_idx].max_durability;
      if (max_dur <= 0)
        continue;
      int diff = max_dur - sls[i]->durability;
      if (diff <= 0)
        continue;
      //Cost: 5gp per point of durability lost; artifacts cost 10x
      int item_cost = diff * (sls[i]->is_artifact ? 50 : 5);
      if ((uint64_t)item_cost > c->gold) {
        char rep_buf[128];
        get_full_item_name(sls[i], rep_buf, sizeof(rep_buf));
        send_text_to_client(c->sock,
            "[BLACKSMITH] You don't have enough gold to repair %s (%dgp).",
            rep_buf, item_cost);
        continue;
      }
      c->gold -= (uint64_t)item_cost;
      sls[i]->durability = max_dur;
      //Restore the quality if it was Rusty due to wear
      if (sls[i]->quality == QUALITY_RUSTY)
        sls[i]->quality = QUALITY_NORMAL;
      total_cost += item_cost;
      items_repaired++;
      char rep_buf[128];
      get_full_item_name(sls[i], rep_buf, sizeof(rep_buf));
      send_text_to_client(c->sock,
          "[BLACKSMITH] Fixed: %s for %d gp. Durab. now %d/%d.",
          rep_buf, item_cost, sls[i]->durability, max_dur);
    }
    if (items_repaired == 0 && total_cost == 0) {
      send_text_to_client(c->sock,
          "[BLACKSMITH] Your items are in excellent shape, there is nothing"
          "to repair (or you don't have enough gold).");
    } else {
      send_text_to_client(c->sock,
          "[BLACKSMITH] Work completed. Total spent: %d gp.",
          total_cost);
    }
    return;
  }

  //uncurse — ask a cleric to remove a curse.
  //Cost: 200gp. Requires merchant near floor 0.
  if (strcmp(cmd, "uncurse") == 0) {
    if (c->floor_id != 0) {
      send_text_to_client(c->sock,
          "[ERROR] The Cleric is only on floor 0 (city).");
      return;
    }
    bool near_cleric = false;
    for (int i = 0; i < MAX_NPCS; i++) {
      if (npcs[i].active && npcs[i].floor_id == 0 &&
          npcs[i].archetype == ARCH_MERCHANT &&
          abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 4) {
        near_cleric = true;
        break;
      }
    }
    if (!near_cleric) {
      send_text_to_client(c->sock,
          "[ERROR] You must be near the Cleric to remove"
          "to curse.");
      return;
    }
    if (c->gold < 200) {
      send_text_to_client(c->sock,
          "[CLERIC] The purification rite costs 200gp."
          "You don't have enough gold.");
      return;
    }
    //Remove the curse from ALL equipped slots
    ItemInstance *sls2[] = {
        &c->slot_head, &c->slot_neck, &c->slot_body, &c->slot_back,
        &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
      &c->slot_arm_l, &c->slot_feet
    };
    int uncursed_count = 0;
    for (int i = 0; i < 10; i++) {
      if (sls2[i]->template_idx != -1 &&
          sls2[i]->blessing == BLESS_CURSED) {
        sls2[i]->blessing = BLESS_NORMAL;
        uncursed_count++;
      }
    }
    for (int ri = 0; ri < 10; ri++) {
      if (c->slot_rings[ri].template_idx != -1 &&
          c->slot_rings[ri].blessing == BLESS_CURSED) {
        c->slot_rings[ri].blessing = BLESS_NORMAL;
        uncursed_count++;
      }
    }
    if (uncursed_count == 0) {
      send_text_to_client(c->sock,
          "[CLERIC] Do not carry cursed objects. Save your 200gp.");
      return;
    }
    c->gold -= 200;
    send_text_to_client(c->sock,
        "[CLERIC] The sacred fire burns the curses! %d item(s)."
        "purified(s) (200gp).", uncursed_count);
    return;
  }
  if (strcmp(cmd, "rest") == 0 || strcmp(cmd, "R") == 0) {
    if (c->floor_id == 0) {
      c->hp = c->max_hp;
      for (int s = 1; s <= MAX_SPELL_LEVEL; s++)
        c->spell_slots[s] = c->spell_slots_max[s];
      send_detailed_state(c); //Sync HP and spell slots with the HUD client
      send_text_to_client(c->sock, "[SYSTEM] Rested.");
    }
    return;
  }
  if (strcmp(cmd, "save") == 0) {
    save_player_data(c);
    send_text_to_client(c->sock, "[SYSTEM] Saved.");
    return;
  }
  if (strcmp(cmd, "talk") == 0) {
    for (int i = 0; i < MAX_NPCS; i++)
      if (npcs[i].active && npcs[i].archetype == ARCH_MERCHANT &&
          npcs[i].floor_id == c->floor_id &&
          abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y) <= 2) {
        send_text_to_client(c->sock,
            "[%s] Welcome! Commands: 'list', 'browse <n>', 'buy <n>', 'sell <n>',"
            " 'haggle buy/sell', 'identify [n]', 'repair [name]', 'uncurse', 'cure'.",
            npcs[i].merchant.shop_name);
        print_merchant_inventory(c, &npcs[i]);
        return;
      }
  }

  // lore <name> — inspects an item in the backpack or reads its content if it is a book
  if (strncmp(cmd, "lore", 4) == 0) {
    const char *input = strchr(cmd, ' ');
    if (input) {
      input++;
    } else {
      send_text_to_client(c->sock, "[ERROR] Specify the object to inspect (e.g. 'lore 1' or 'lore book').");
      return;
    }
    int found_idx = -1;
    if (isdigit(input[0])) {
      int requested_idx = atoi(input) - 1;
      if (requested_idx >= 0 && requested_idx < c->backpack_count) {
        found_idx = requested_idx;
      }
    } else {
      for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx != -1 &&
            strcasecmp(item_database[c->backpack[i].template_idx].name, input) == 0) {
          found_idx = i;
          break;
        }
      }
    }

    if (found_idx == -1) {
      send_text_to_client(c->sock, "[ERROR] Item not found in backpack.");
      return;
    }

    if (!c->backpack[found_idx].is_identified) {
      send_text_to_client(c->sock, "[ERROR] You must first identify this object.");
      return;
    }

    ItemTemplate *tmpl = &item_database[c->backpack[found_idx].template_idx];
    send_text_to_client(c->sock, "[LORE] %s: %s", tmpl->name, tmpl->description);
    
    if (tmpl->category == ITEM_BOOK) {
      send_text_to_client(c->sock, "--- Spell Index ---");
      if (tmpl->book_spell_count > 0) {
        for (int local_idx = 0; local_idx < tmpl->book_spell_count; local_idx++) {
          int global_idx = tmpl->book_seq * MAX_BOOK_SPELLS + local_idx;
          int required_pg_level = global_idx / 3;
          send_text_to_client(c->sock, "  [#%2d] %-25s (Req. Lv PG: %d)", 
                              global_idx, tmpl->book_spell_names[local_idx], required_pg_level);
        }
      } else {
        send_text_to_client(c->sock, "(No readable spells listed here)");
      }
      send_text_to_client(c->sock, "--------------------------------");
    }
    return;
  }
  //fill <n> - fills a lantern using an oil (fuel) from the backpack
  if (strncmp(cmd, "fill ", 5) == 0) {
    const char *input = cmd + 5;
    int lantern_idx = -1;
    bool in_belt = false;
    if (isdigit(input[0])) {
      int requested_idx = atoi(input) - 1;
      if (requested_idx >= 0 && requested_idx < c->backpack_count) {
        lantern_idx = requested_idx;
      }
    } else {
      for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx != -1 &&
            strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
          lantern_idx = i;
          break;
        }
      }
      if (lantern_idx == -1) {
        for (int i = 0; i < MAX_BELT; i++) {
          if (c->belt[i].template_idx != -1 && 
              strcasestr(item_database[c->belt[i].template_idx].name, input) != NULL) {
            lantern_idx = i;
            in_belt = true;
            break;
          }
        }
      }
    }

    if (lantern_idx == -1) {
      send_text_to_client(c->sock, "[ERROR] Item to fill not found (search by name or number in backpack).");
      return;
    }

    ItemInstance *target = in_belt ? &c->belt[lantern_idx] : &c->backpack[lantern_idx];
    ItemTemplate *ltmpl = &item_database[target->template_idx];
    
    if (ltmpl->category != ITEM_LIGHT_SOURCE) {
      send_text_to_client(c->sock, "[ERROR] '%s' is not a rechargeable light source.", ltmpl->name);
      return;
    }
    
    if (strcasestr(ltmpl->name, "torch") || strcasestr(ltmpl->name, "Torcia")) {
      send_text_to_client(c->sock, "[ERROR] The torches just wear out, they cannot be refilled with oil.");
      return;
    }

    int fuel_idx = -1;
    for (int i = 0; i < c->backpack_count; i++) {
      if (c->backpack[i].template_idx != -1 && 
          item_database[c->backpack[i].template_idx].category == ITEM_FUEL) {
        fuel_idx = i;
        break;
      }
    }

    if (fuel_idx == -1) {
      send_text_to_client(c->sock, "[ERROR] You don't have any oil or fuel in your backpack to refill.");
      return;
    }

    ItemInstance *fuel = &c->backpack[fuel_idx];
    ItemTemplate *ftmpl = &item_database[fuel->template_idx];

    if (target->durability >= ltmpl->max_durability) {
      send_text_to_client(c->sock, "[OBJECT] %s is already full of fuel.", ltmpl->name);
      return;
    }

    target->durability = ltmpl->max_durability;
    send_text_to_client(c->sock, "[OBJECT] You filled %s using a dose of %s.", ltmpl->name, ftmpl->name);

    fuel->stack_count--;
    if (fuel->stack_count <= 0) {
      c->backpack[fuel_idx] = c->backpack[c->backpack_count - 1];
      c->backpack_count--;
    }
    return;
  }

  //use <name> — uses a consumable from your backpack or belt
  //The ritual commands (study/pray/awaken/invoke/intone/celebrate/pronounce/practice)
  //they are aliases for 'read' but can only be activated within the temple of your class.
  bool is_eat_cmd = (strcmp(cmd, "eat") == 0 || strncmp(cmd, "eat ", 4) == 0 || strcmp(cmd, "e") == 0 || strncmp(cmd, "e ", 2) == 0);
  bool is_use_group = (strncmp(cmd, "use", 3) == 0 || strncmp(cmd, "quaff", 5) == 0
      || strcmp(cmd, "q") == 0 || strncmp(cmd, "q ", 2) == 0
      || strncmp(cmd, "read", 4) == 0 || strcmp(cmd, "r") == 0 || strncmp(cmd, "r ", 2) == 0
      || is_eat_cmd || strncmp(cmd, "zap", 3) == 0 || strcmp(cmd, "z") == 0 || strncmp(cmd, "z ", 2) == 0
      /*--- Learning Ritual Commands ---*/
      || strncmp(cmd, "study",      5) == 0
      || strncmp(cmd, "pray",       4) == 0
      || strncmp(cmd, "awaken",     6) == 0
      || strncmp(cmd, "invoke",     6) == 0
      || strncmp(cmd, "intone",     6) == 0
      || strncmp(cmd, "celebrate",  9) == 0
      || strncmp(cmd, "pronounce",  9) == 0
      || strncmp(cmd, "practice",   8) == 0
      || strncmp(cmd, "drill",      5) == 0
      || strncmp(cmd, "rage",       4) == 0
      || strncmp(cmd, "sneak",      5) == 0
      || strncmp(cmd, "meditate",   8) == 0);

  if (is_use_group) {
    const char *input = strchr(cmd, ' ');
    if (input) {
      input++;
    } else if (is_eat_cmd) {
      /*Auto-search for food/rations in backpack if 'eat' is typed without arguments*/
      input = "ration";
    } else {
      send_text_to_client(c->sock, "[ERROR] Specify the subject (e.g. 'use 1' or 'study book').");
      return;
    }
    int found_idx = -1;
    bool in_belt = false;

    bool is_ritual_cmd = (strncmp(cmd, "study", 5) == 0 ||
                          strncmp(cmd, "pray", 4) == 0 ||
                          strncmp(cmd, "awaken", 6) == 0 ||
                          strncmp(cmd, "invoke", 6) == 0 ||
                          strncmp(cmd, "intone", 6) == 0 ||
                          strncmp(cmd, "celebrate", 9) == 0 ||
                          strncmp(cmd, "pronounce", 9) == 0 ||
                          strncmp(cmd, "practice", 8) == 0 ||
                          strncmp(cmd, "drill", 5) == 0 ||
                          strncmp(cmd, "rage", 4) == 0 ||
                          strncmp(cmd, "sneak", 5) == 0 ||
                          strncmp(cmd, "meditate", 8) == 0);

    //1. Search the backpack by index
    if (isdigit(input[0])) {
      int requested_idx = atoi(input) - 1;
      if (requested_idx >= 0 && requested_idx < c->backpack_count) {
        found_idx = requested_idx;
      }
    } else {
      //2. Search your backpack by name
      for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx != -1 &&
            strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
          found_idx = i;
          break;
        }
      }
      //3. Search by name in belt
      if (found_idx == -1) {
        for (int i = 0; i < MAX_BELT; i++) {
          if (c->belt[i].template_idx != -1 && c->belt[i].stack_count > 0 &&
              strcasestr(item_database[c->belt[i].template_idx].name, input) != NULL) {
            found_idx = i;
            in_belt = true;
            break;
          }
        }
      }
    }

    //If found on the belt (by name), let's momentarily move it to the backpack
    //for the consumption process
    if (found_idx != -1 && in_belt) {
      if (c->backpack_count < MAX_BACKPACK) {
        c->backpack[c->backpack_count] = c->belt[found_idx];
        c->belt[found_idx].template_idx = -1;
        found_idx = c->backpack_count;
        c->backpack_count++;
      } else {
        send_text_to_client(c->sock, "[ERROR] Backpack too full for"
                                     "handle the belt item!");
        return;
      }
    }

    if (found_idx == -1) {
      send_text_to_client(c->sock,
                          "[ERROR] Item not found in inventory (use"
                          "name or number).");
      return;
    }

    ItemTemplate *tmpl = &item_database[c->backpack[found_idx].template_idx];
    if (!c->backpack[found_idx].is_identified) {
      send_text_to_client(c->sock,
                          "[ERROR] You must first identify this object.");
      return;
    }
    
    if (is_ritual_cmd && tmpl->category != ITEM_BOOK) {
      send_text_to_client(c->sock, "[ERROR] Ritual commands (like 'study') are only for learning from magical books.");
      return;
    }

    if (tmpl->category == ITEM_BOOK) {
      if (!is_ritual_cmd) {
        send_text_to_client(c->sock, "[ERROR] Use 'study' (or your class ritual) to learn from this book, or use 'lore' to browse it.");
        return;
      }
      /* =======================================================
       * RITUAL LEARNING SYSTEM
       *
       * To learn spells from a book the player MUST:
       *   1. Be physically inside the temple of their
       *      class (on floor 0).
       *   2. Have a sufficient player level for each
       *      individual spell in the book.
       *   3. Use the ritual command of their class
       *      (study / pray / awaken / invoke / intone /
       *       celebrate / pronounce / practice).
       *
       * The book is NEVER consumed: it must be kept
       * in the inventory. If lost or sold, the spells
       * remain known, but the book will need to be repurchased.
       * ======================================================= */

      /* ---- Temple definitions by class (floor 0) ----
       * Each temple is a rectangle [x0,x1] x [y0,y1].
       * Coordinates centered at cx/cy=500; see map.c.
       *
       *  Wizard   : X[494..506] Y[439..451]  (North, BLUE crystal)
       *  Paladin  : X[532..544] Y[455..467]  (North-East, YELLOW crystal)
       *  Cleric   : X[549..561] Y[494..506]  (East, YELLOW crystal)
       *  Sorcerer : X[532..544] Y[532..544]  (South-East, RED crystal)
       *  Warlock  : X[494..506] Y[549..561]  (South, GREEN crystal)
       *  Bard     : X[455..467] Y[532..544]  (South-West, RED crystal)
       *  Druid    : X[439..451] Y[494..506]  (West, GREEN crystal)
       *  Ranger   : X[455..467] Y[455..467]  (North-West, GREEN crystal)
       */

      /* Temple table is now global */

      /*---- 1. Check class compatible with the book ----*/
      uint32_t cls_bit = (1u << (int)c->class_id);
      if (!(tmpl->book_class_mask & cls_bit)) {
        send_text_to_client(c->sock,
            "[BOOK] '%s' is written in a language that your class"
            "(%s) does not include.",
            tmpl->name, CLASSES[c->class_id].name);
        return;
      }

      /*---- 2. Locate the PC's class temple ----*/
      const TempleInfo *my_temple = NULL;
      for (int ti = 0; ti < TEMPLE_COUNT; ti++) {
        if (TEMPLES[ti].cls == (int)c->class_id) {
          my_temple = &TEMPLES[ti];
          break;
        }
      }

      if (!my_temple) {
        /*Non-magical class (e.g. Fighter, Rogue, Barbarian, Monk)*/
        send_text_to_client(c->sock,
            "[BOOK] Class %s does not have a learning temple:"
            "you cannot activate this book.", CLASSES[c->class_id].name);
        return;
      }

      /*---- 3. Check location: the PC must be in the temple ----*/
      bool in_temple = (
        c->floor_id == 0 &&
        c->x >= my_temple->x0 && c->x <= my_temple->x1 &&
        c->y >= my_temple->y0 && c->y <= my_temple->y1
      );

      if (!in_temple) {
        send_text_to_client(c->sock,
            "[RITUAL] To activate '%s' you must go to floor 0, in %s,"
            "and use the command '%s %d' while you are physically inside.",
            tmpl->name, my_temple->name,
            my_temple->ritual, found_idx + 1);
        return;
      }

      /*---- 4. Determine the PC's level (1..20) ----*/
      /*We use the character level saved in the client state*/
      int pg_level = c->level;
      if (pg_level < 1) {
        pg_level = 1;
      }

      /*---- 5. Learns spells from the ordered list in the book ----
       * Level gate:
       * global_idx = book_seq * 10 + local_location
       * required_level = global_idx / 3
       * (60 total spells per class / 20 PC levels = 3 spells unlocked per level)*/
      int learned = 0;
      int locked  = 0;
      int already = 0;

      send_text_to_client(c->sock,
          "[RITUAL] %s... '%s' (you are in %s, Lv PG %d):",
          my_temple->ritual, tmpl->name, my_temple->name, pg_level);

      int use_ordered = (tmpl->book_spell_count > 0);

      if (use_ordered) {
        /*--- New path: explicit spell list in book ---*/
        for (int local_idx = 0; local_idx < tmpl->book_spell_count; local_idx++) {
          const char *spell_name = tmpl->book_spell_names[local_idx];

          /*Find the spell in the database*/
          int si = -1;
          for (int k = 0; k < spell_database_size; k++) {
            if (strcasecmp(spell_database[k].name, spell_name) == 0) {
              si = k;
              break;
            }
          }
          if (si < 0) {
            continue;
          }

          SpellTemplate *sp = &spell_database[si];

          /*Check that the spell is accessible to the PC's class*/
          if (!(sp->class_mask & cls_bit)) {
            continue;
          }

          /*New adjusted DragonGL curve for cap at level 20: Required level = (Magic Level * 2)*/
          int required_pg_level = (sp->level > 0) ? (sp->level * 2) : 1;

          if (pg_level < required_pg_level) {
            send_text_to_client(c->sock,
                "  [-] %-30s  (requires PC Lv %d — LOCKED)",
                sp->name, required_pg_level);
            locked++;
            continue;
          }

          /*Check if already known*/
          int w = si / 64;
          int b = si % 64;
          bool already_known = (c->known_spells[w] >> b) & 1ULL;

          if (already_known) {
            send_text_to_client(c->sock,
                "  [Lv%d] %-30s  (already in grimoire)",
                sp->level, sp->name);
            already++;
          } else {
            /*Learn magic*/
            c->known_spells[w] |= (1ULL << b);
            send_text_to_client(c->sock,
                "  [Lv%d] %-30s  *** LEARNED ***",
                sp->level, sp->name);
            learned++;
          }
        }
      } else {
        /*--- Legacy path: filter by book_min_level / book_max_level ---*/
        for (int si = 0; si < spell_database_size; si++) {
          SpellTemplate *sp = &spell_database[si];

          /*The magic must be in the range of the book*/
          if (sp->level < tmpl->book_min_level || sp->level > tmpl->book_max_level) {
            continue;
          }

          /*Magic must be accessible to the PC's class*/
          if (!(sp->class_mask & cls_bit)) {
            continue;
          }

          /*New DragonGL curves (no copyright 5e)*/
          int required_pg_level = (sp->level > 0) ? (sp->level * 2 + (sp->level / 2)) : 1;
          if (pg_level < required_pg_level) {
            send_text_to_client(c->sock,
                "  [Lv%d] %-30s  (requires PC Lv %d - LOCKED)",
                sp->level, sp->name, required_pg_level);
            locked++;
            continue;
          }

          /*Check if already known*/
          int w = si / 64;
          int b = si % 64;
          bool already_known = (c->known_spells[w] >> b) & 1ULL;

          if (already_known) {
            send_text_to_client(c->sock,
                "  [Lv%d] %-30s  (already in grimoire)",
                sp->level, sp->name);
            already++;
          } else {
            /*Learn magic*/
            c->known_spells[w] |= (1ULL << b);
            send_text_to_client(c->sock,
                "  [Lv%d] %-30s  *** LEARNED ***",
                sp->level, sp->name);
            learned++;
          }
        }
      }

      /*---- 6. Summary message ----*/
      if (learned == 0 && already == 0 && locked == 0) {
        send_text_to_client(c->sock,
            "[RITUAL] No compatible spells found for your class.");
      } else {
        if (learned > 0) {
          send_text_to_client(c->sock,
              "[RITUAL] You have learned %d new magic(s)! Use 'spells' for"
              "see your updated library.", learned);
        }
        if (locked > 0) {
          send_text_to_client(c->sock,
              "[RITUAL] %d spell(s) are still blocked by your level."
              "Keep the book and return when you are more powerful.", locked);
        }
        if (learned == 0 && locked == 0) {
          send_text_to_client(c->sock,
              "[RITUAL] You already know all the accessible magic in this book.");
        }
      }

      /*The book is NOT removed from inventory*/
      c->needs_study = false;
      return;
    }

    if (tmpl->category != ITEM_CONSUMABLE && tmpl->category != ITEM_FUEL) {
      send_text_to_client(
          c->sock,
          "[ERROR] %s is not consumable or usable as fuel.",
          tmpl->name);
      return;
    }

    //Case: Fuel (Oil) for Lanterns
    if (tmpl->category == ITEM_FUEL) {
      bool refilled = false;
      ItemInstance *ls[] = {&c->slot_hand_r, &c->slot_hand_l, &c->belt[0],
                            &c->belt[1],     &c->belt[2],     &c->belt[3]};
      for (int j = 0; j < 6; j++) {
        if (ls[j]->template_idx != -1) {
          const ItemTemplate *lit = &item_database[ls[j]->template_idx];
          //If the object is a lantern (light radius > 6)
          if (lit->category == ITEM_LIGHT_SOURCE && lit->light_radius > 6) {
            ls[j]->durability = lit->max_durability;
            refilled = true;
            send_text_to_client(c->sock,
                                "[SYSTEM] You have replenished your %s with %s.",
                                lit->name, tmpl->name);
            send_detailed_state(c); //Update client view radius
            break;
          }
        }
      }
      if (!refilled) {
        send_text_to_client(c->sock, "[ERROR] You don't have a lantern ready from"
                                     "Replenish in hand or on belt.");
        return;
      }
    } else if (tmpl->heal_amount > 0) {
      //Care effect
      int healed = tmpl->heal_amount;
      c->hp += healed;
      if (c->hp > c->max_hp)
        c->hp = c->max_hp;
      if (strcasestr(tmpl->name, "Ration") != NULL) {
        c->hunger_level = 0;
        send_text_to_client(c->sock,
                            "[ITEM] You ate %s. Hunger fully satisfied! (+%d HP).",
                            tmpl->name, healed);
      } else {
        unsigned int hash = 5381;
        for (int i = 0; tmpl->name[i] != '\0'; i++) hash = ((hash << 5) + hash) + tmpl->name[i];
        float col_r = 0.2f + (float)(hash % 80) / 100.0f;
        float col_g = 0.2f + (float)((hash >> 8) % 80) / 100.0f;
        float col_b = 0.2f + (float)((hash >> 16) % 80) / 100.0f;
        float m_col = (col_r > col_g) ? ((col_r > col_b) ? col_r : col_b) : ((col_g > col_b) ? col_g : col_b);
        if (m_col < 1.0f) { col_r /= m_col; col_g /= m_col; col_b /= m_col; }
        broadcast_spell_vfx(c->x, c->y, c->x, c->y, 2, col_r, col_g, col_b, c->floor_id);
        
        send_text_to_client(c->sock,
                            "[ITEM] You used %s and recovered %d HP (HP: %d/%d).",
                            tmpl->name, healed, c->hp, c->max_hp);
      }
    } else {
      //Unhealed Item: Apply a generic effect
      unsigned int hash = 5381;
      for (int i = 0; tmpl->name[i] != '\0'; i++) hash = ((hash << 5) + hash) + tmpl->name[i];
      float col_r = 0.2f + (float)(hash % 80) / 100.0f;
      float col_g = 0.2f + (float)((hash >> 8) % 80) / 100.0f;
      float col_b = 0.2f + (float)((hash >> 16) % 80) / 100.0f;
      float m_col = (col_r > col_g) ? ((col_r > col_b) ? col_r : col_b) : ((col_g > col_b) ? col_g : col_b);
      if (m_col < 1.0f) { col_r /= m_col; col_g /= m_col; col_b /= m_col; }
      broadcast_spell_vfx(c->x, c->y, c->x, c->y, 2, col_r, col_g, col_b, c->floor_id);

      if (c->effect_count < MAX_EFFECTS_PER_ENTITY) {
        ActiveEffect ef;
        ef.name = tmpl->name;
        ef.trigger = EVENT_ON_TURN_START;
        ef.mod_type = MOD_ADDITIVE;
        ef.value = 2;
        ef.duration_rounds =
            tmpl->duration_turns > 0 ? tmpl->duration_turns : 10;
        ef.is_persistent = false;
        c->effects[c->effect_count++] = ef;
        send_text_to_client(c->sock,
                            "[ITEM] You used %s. Effect active for %d rounds.",
                            tmpl->name, ef.duration_rounds);
      } else {
        send_text_to_client(c->sock, "[ITEM] You used %s, but your magic-saturated body dispels the effect.", tmpl->name);
      }
    }
    //Remove a unit from your backpack
    c->backpack[found_idx].stack_count--;
    if (c->backpack[found_idx].stack_count <= 0) {
      c->backpack[found_idx] = c->backpack[c->backpack_count - 1];
      c->backpack_count--;
    }
    send_detailed_state(c); //Instantly sync HP, inventory and Vitality (hunger) with the HUD client
    return;
  }

  /*-------------------------------------------------------
   * spells — lists the spells in your personal grimoire
   * Show only spells in the known_spells bitfield, grouped together
   * per level with the number of slots remaining.
   * -------------------------------------------------------*/
  if (strcmp(cmd, "spells") == 0) {
    int listed = 0;
    /*Group by level: 0 (cantrip) .. MAX_SPELL_LEVEL*/
    for (int lv = 0; lv <= MAX_SPELL_LEVEL; lv++) {
      int lv_count = 0;
      /*First pass: Count how many spells this level has*/
      for (int i = 0; i < spell_database_size; i++) {
        if (spell_database[i].level != lv)
          continue;
        int w = i / 64;
        int b = i % 64;
        if (!((c->known_spells[w] >> b) & 1ULL))
          continue;
        lv_count++;
      }
      if (lv_count == 0)
        continue;
      /*Level header*/
      if (lv == 0)
        send_text_to_client(c->sock, "--- Cantrips (Slot: unlimited) ---");
      else
        send_text_to_client(c->sock, "--- Level %d (Slot: %d/%d) ---",
                            lv,
                            c->spell_slots[lv],
                            c->spell_slots_max[lv]);
      /*Second pass: List the spells*/
      for (int i = 0; i < spell_database_size; i++) {
        if (spell_database[i].level != lv)
          continue;
        int w = i / 64;
        int b = i % 64;
        if (!((c->known_spells[w] >> b) & 1ULL))
          continue;
        send_text_to_client(c->sock, "  cast %-30s  [Range: %d]",
                            spell_database[i].name,
                            spell_database[i].range);
        listed++;
      }
    }
    if (listed == 0)
      send_text_to_client(c->sock,
          "[GRIMORY] You don't know any magic yet."
          "Find a magical book and study it with 'study <n>' (in your classroom temple).");
    return;
  }

  //cast <name> or cast <book_idx> <spell_idx>
  if (strncmp(cmd, "cast ", 5) == 0 || strncmp(cmd, "c ", 2) == 0) {
    //--- ACTION BLOCK FOR SILENCE ---
    if (rules_has_condition(c->effects, c->effect_count, "Silenced")) {
      send_text_to_client(
          c->sock, "[SYSTEM] You are silenced and cannot cast spells!");
      return;
    }
    const char *spell_name = strchr(cmd, ' ');
    if (spell_name) spell_name++; else return;
    
    //Support for "cast <book_idx> <spell_idx>" (shortcut)
    int b_idx, l_idx;
    if (sscanf(spell_name, "%d %d", &b_idx, &l_idx) == 2) {
      b_idx--; //From 1-based to 0-based index in the backpack
      if (b_idx >= 0 && b_idx < c->backpack_count) {
        ItemTemplate *tmpl = &item_database[c->backpack[b_idx].template_idx];
        if (tmpl->category == ITEM_BOOK) {
           int local_idx = l_idx - (tmpl->book_seq * MAX_BOOK_SPELLS);
           if (local_idx >= 0 && local_idx < tmpl->book_spell_count) {
               spell_name = tmpl->book_spell_names[local_idx];
           }
        }
      }
    }

    int sp_idx = -1;
    for (int i = 0; i < spell_database_size; i++) {
      if (strcasecmp(spell_database[i].name, spell_name) == 0) {
        sp_idx = i;
        break;
      }
    }
    if (sp_idx == -1) {
      send_text_to_client(c->sock, "[ERROR] Spell '%s' not found.",
                          spell_name);
      return;
    }
    SpellTemplate *sp = &spell_database[sp_idx];

    /*-------------------------------------------------------
     * GATEKEEPER: Check that the spell is in the library
     * of the character (known_spells bitfield).
     * -------------------------------------------------------*/
    {
      int w = sp_idx / 64;
      int b = sp_idx % 64;
      if (!((c->known_spells[w] >> b) & 1ULL)) {
        send_text_to_client(c->sock,
            "[MAGIC] You don't know '%s'. Find the corresponding book"
            "and study it at the temple with 'study <n>'.",
            sp->name);
        return;
      }
    }

    /*-------------------------------------------------------
     *BOOK REQUIREMENT: Player must own the
     * book that contains this spell in the inventory to cast it.
     * Innate class magic (e.g. the 12 transit cantrips) is part of
     * the character and needs no book.
     * -------------------------------------------------------*/
    if (!sp->innate) {
      bool has_book = false;
      
      //Check your backpack
      for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx == -1) continue;
        ItemTemplate *tmpl = &item_database[c->backpack[i].template_idx];
        if (tmpl->category == ITEM_BOOK) {
          for (int k = 0; k < tmpl->book_spell_count; k++) {
            if (strcasecmp(tmpl->book_spell_names[k], sp->name) == 0) {
              has_book = true; break;
            }
          }
        }
        if (has_book) break;
      }
      
      //Check your belt
      if (!has_book) {
        for (int i = 0; i < 4; i++) {
          if (c->belt[i].template_idx == -1) continue;
          ItemTemplate *tmpl = &item_database[c->belt[i].template_idx];
          if (tmpl->category == ITEM_BOOK) {
            for (int k = 0; k < tmpl->book_spell_count; k++) {
              if (strcasecmp(tmpl->book_spell_names[k], sp->name) == 0) {
                has_book = true; break;
              }
            }
          }
          if (has_book) break;
        }
      }

      //Check in the hands (equipped)
      if (!has_book) {
          ItemInstance* hands[2] = { &c->slot_hand_l, &c->slot_hand_r };
          for (int i = 0; i < 2; i++) {
              if (hands[i]->template_idx == -1) continue;
              ItemTemplate *tmpl = &item_database[hands[i]->template_idx];
              if (tmpl->category == ITEM_BOOK) {
                for (int k = 0; k < tmpl->book_spell_count; k++) {
                  if (strcasecmp(tmpl->book_spell_names[k], sp->name) == 0) {
                    has_book = true; break;
                  }
                }
              }
              if (has_book) break;
          }
      }

      if (!has_book) {
        send_text_to_client(c->sock,
            "[MAGIC] You have memorized '%s', but the book containing it is not in your inventory! You have to carry it with you to throw it.",
            sp->name);
        return;
      }
    }


    /*Check spell slot*/
    int slv = sp->level;
    if (slv > 0 && c->spell_slots[slv] <= 0) {
      send_text_to_client(c->sock,
                          "[MAGIC] No level %d slots left.", slv);
      return;
    }
    /*Consume the slot*/
    if (slv > 0)
      c->spell_slots[slv]--;

    //Find target: Closest active NPC in range
    NPC *target = NULL;
    int min_d = sp->range + 1;
    for (int i = 0; i < MAX_NPCS; i++) {
      if (!npcs[i].active || npcs[i].archetype == ARCH_MERCHANT ||
          npcs[i].floor_id != c->floor_id)
        continue;
      int d = abs(npcs[i].x - c->x) + abs(npcs[i].y - c->y);
      if (d < min_d) {
        min_d = d;
        target = &npcs[i];
      }
    }


    int tx = c->x, ty = c->y;
    if (target && sp->target_type != SPELL_TARGET_SELF) { tx = target->x; ty = target->y; }

    broadcast_spell_vfx(c->x, c->y, tx, ty, sp->vfx_type, sp->vfx_r, sp->vfx_g, sp->vfx_b, c->floor_id);

    // -------------------------------------------------------
    //MAGIC ROUTER: spell dispatch with custom logic
    //O(log N) binary search in the hook table.
    //If the handler returns true, the handling is complete.
    // -------------------------------------------------------
    {
      SpellContext router_ctx;
      router_ctx.caster = c;
      router_ctx.sp     = sp;
      router_ctx.npcs   = npcs;
      router_ctx.target = target;
      if (spell_router_dispatch(&router_ctx)) {
        check_level_up(c);
        return;
      }
    }

    if (sp->target_type == SPELL_TARGET_SELF) {
      //Effect on oneself
      if (sp->effect_type == SPELL_EFFECT_HEAL) {
        int dice_hp = rules_roll_dice(sp->dice_count, sp->dice_sides);
        c->hp = (c->hp + dice_hp > c->max_hp) ? c->max_hp : c->hp + dice_hp;
        send_text_to_client(c->sock, "[MAGIC] %s: Recover %d HP (HP: %d/%d).",
                            sp->name, dice_hp, c->hp, c->max_hp);
      } else if (sp->has_status_effect &&
                 c->effect_count < MAX_EFFECTS_PER_ENTITY) {
        c->effects[c->effect_count++] = sp->status_effect;
        send_text_to_client(c->sock, "[MAGIC] %s active for %d rounds.",
                            sp->name, sp->status_effect.duration_rounds);
      } else {
        send_text_to_client(
            c->sock,
            "> You whisper arcane words and the energy of %s envelops you...",
            sp->name);
      }
      return;
    }

    // -------------------------------------------------------
    //AoE: Circle, Cone, Line, Cloud -> aoe.c engine
    // -------------------------------------------------------
    if (sp->target_type == SPELL_TARGET_AOE_CIRCLE ||
        sp->target_type == SPELL_TARGET_AOE_CONE ||
        sp->target_type == SPELL_TARGET_AOE_LINE ||
        sp->target_type == SPELL_TARGET_AOE_CLOUD) {
      int hits = aoe_resolve_spell(sp, c, npcs, MAX_NPCS, c->x, c->y);
      // Proportional XP: level-up is already handled inside
      //aoe_apply_damage_to_npc Check level-up after each explosion
      check_level_up(c);
      (void)hits;
      return;
    }

    // -------------------------------------------------------
    //SINGLE TARGET (SPELL_TARGET_SINGLE)
    // -------------------------------------------------------
    if (!target) {
      send_text_to_client(c->sock,
                          "[MAGIC] No targets in range (%d) of %s.",
                          sp->range, sp->name);
      return;
    }

    if (sp->effect_type == SPELL_EFFECT_DAMAGE) {
      int spell_save_dc = 8 + 2 + rules_get_modifier(c->intel);
      bool saved = rules_roll_save(0, spell_save_dc, false, false, NULL);
      int raw_dmg = rules_roll_dice(sp->dice_count > 0 ? sp->dice_count : 1,
                                    sp->dice_sides > 0 ? sp->dice_sides : 6);
      int dmg = rules_calculate_damage(raw_dmg, DMG_MOD_NORMAL);
      if (saved)
        dmg /= 2;
      target->hp -= dmg;
      send_text_to_client(
          c->sock, "[ACTION] Call upon the arcane forces and unleash %s against %s!",
          sp->name, target->template ? target->template->name : "???");
      send_text_to_client(
          c->sock,
          " > Mystic energy impacts for %d damage %s (HP: %d/%d).",
          dmg, saved ? "[Halved on Saving Throw!]" : "", target->hp,
          target->max_hp);
      clog_spell(c->username, sp->name,
                 target->template ? target->template->name : "???", dmg, saved);
      if (target->hp <= 0) {
        target->active = false;
        target->respawn_timer = RESPAWN_TICKS;
        int gained_xp = target->template ? target->template->xp : 10;
        c->xp += gained_xp;
        check_level_up(c);
        send_text_to_client(c->sock,
                            "[VICTORY] The power of the spell disintegrates"
                            " %s, reducing it to ashes! (+%d XP)",
                            target->template ? target->template->name : "???",
                            gained_xp);
        clog_death(target->template ? target->template->name : "???",
                   c->username, target->floor_id);
        if (target->archetype == ARCH_BOSS) {
          handle_boss_death(c, target);
        }
      }
    } else if (sp->effect_type == SPELL_EFFECT_HEAL) {
      int dice_hp = rules_roll_dice(sp->dice_count, sp->dice_sides);
      c->hp = (c->hp + dice_hp > c->max_hp) ? c->max_hp : c->hp + dice_hp;
      send_text_to_client(c->sock, "[MAGIC] %s: Recover %d HP.", sp->name,
                          dice_hp);
    } else if (sp->has_status_effect) {
      if (target->effect_count < MAX_EFFECTS_PER_ENTITY) {
        target->effects[target->effect_count++] = sp->status_effect;
        send_text_to_client(c->sock, "[MAGIC] %s applied to %s for %d rounds.",
                            sp->name,
                            target->template ? target->template->name : "???",
                            sp->status_effect.duration_rounds);
      }
    }
    return;
  }
  // --- RECOVERY FROM TOMBSTONE ---
  if (strcmp(cmd, "pickup") == 0) {
    tombstone_pickup(c);
    save_player_data(c);
    return;
  }

  // --- MISSING ROGUELIKE COMMANDS ---
  if (strncmp(cmd, "get", 3) == 0 || strncmp(cmd, "g ", 2) == 0 || strcmp(cmd, "g") == 0) {

    int target_x = c->x;
    int target_y = c->y;
    int target_idx = -1;
    
    char dir[16] = {0};
    char idx_str[16] = {0};
    const char *args = cmd + (cmd[0] == 'g' && cmd[1] == 'e' ? 3 : 1);
    while (*args == ' ') args++;
    
    if (sscanf(args, "%15s %15s", dir, idx_str) >= 1) {
       if (isdigit(dir[0])) {
           target_idx = atoi(dir);
       } else {
           if (strcasecmp(dir, "N") == 0) target_y--;
           else if (strcasecmp(dir, "S") == 0) target_y++;
           else if (strcasecmp(dir, "W") == 0) target_x--;
           else if (strcasecmp(dir, "E") == 0) target_x++;
           else if (strcasecmp(dir, "NW") == 0) { target_y--; target_x--; }
           else if (strcasecmp(dir, "NE") == 0) { target_y--; target_x++; }
           else if (strcasecmp(dir, "SW") == 0) { target_y++; target_x--; }
           else if (strcasecmp(dir, "SE") == 0) { target_y++; target_x++; }
           
           if (idx_str[0] && isdigit(idx_str[0])) {
               target_idx = atoi(idx_str);
           }
       }
    }

    int items[64];
    int count = 0;
    for (int i = 0; i < MAX_NPCS; i++) {
        if (npcs[i].active && npcs[i].floor_id == c->floor_id && npcs[i].x == target_x && npcs[i].y == target_y) {
            if (npcs[i].archetype == ARCH_TREASURE || npcs[i].archetype == ARCH_GOLD) {
                if (count < 64) items[count++] = i;
            }
        }
    }

    if (count == 0) {
        send_text_to_client(c->sock, "[SYSTEM] There is nothing to collect here.");
        return;
    }

    if (count == 1 || target_idx > 0) {
        int pick = (target_idx > 0 && target_idx <= count) ? items[target_idx - 1] : items[0];
        NPC *n = &npcs[pick];
        if (n->archetype == ARCH_GOLD) {
            c->gold += n->gold_drop;
            send_text_to_client(c->sock, "[SYSTEM] You have collected %d gold coins!", n->gold_drop);
            n->active = false;
        } else if (n->archetype == ARCH_TREASURE) {
            if (n->ghost_loot[0].stack_count > 0) {
                if (c->backpack_count >= MAX_BACKPACK) {
                    send_text_to_client(c->sock, "[SYSTEM] Backpack full! Unable to collect.");
                } else {
                    c->backpack[c->backpack_count++] = n->ghost_loot[0];
                    send_text_to_client(c->sock, "[SYSTEM] You have collected: %s", item_database[n->ghost_loot[0].template_idx].name);
                    n->active = false;
                    n->respawn_timer = 0;
                }
            } else {
                send_text_to_client(c->sock, "[SYSTEM] Open the chest and find something...");
                drop_loot_from_monster(c, n);
                n->active = false;
                n->respawn_timer = 100;
            }
        }
        extern void floor_stats_npc_died(int floor_id);
        floor_stats_npc_died(c->floor_id);
        save_player_data(c);
    } else {
        send_text_to_client(c->sock, "[SYSTEM] There are more objects:");
        for (int i = 0; i < count; i++) {
            NPC *n = &npcs[items[i]];
            if (n->archetype == ARCH_GOLD) {
                char buf[128];
                snprintf(buf, sizeof(buf), "%d. %d gold coins", i + 1, n->gold_drop);
                send_text_to_client(c->sock, buf);
            } else if (n->ghost_loot[0].stack_count > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf), "  %d. %s", i + 1, item_database[n->ghost_loot[0].template_idx].name);
                send_text_to_client(c->sock, buf);
            } else {
                char buf[128];
                snprintf(buf, sizeof(buf), "%d. Treasure chest", i + 1);
                send_text_to_client(c->sock, buf);
            }
        }
        char prompt[128];
        if (target_x == c->x && target_y == c->y) {
            snprintf(prompt, sizeof(prompt), "What do you want to collect? Use 'get <n>'.");
        } else {
            snprintf(prompt, sizeof(prompt), "What do you want to collect? Use 'get %s <n>'.", dir);
        }
        send_text_to_client(c->sock, prompt);
    }
    return;
  }
  if (strncmp(cmd, "drop ", 5) == 0 || strncmp(cmd, "d ", 2) == 0) {
    const char *input = strchr(cmd, ' ');
    if (input) {
      input++;
    } else {
      return;
    }
    int found_idx = -1;
    bool in_belt = false;

    //1. Search the backpack by index
    if (isdigit(input[0])) {
      int requested_idx = atoi(input) - 1;
      if (requested_idx >= 0 && requested_idx < c->backpack_count) {
        found_idx = requested_idx;
      }
    } else {
      //2. Search your backpack by name
      for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx != -1 &&
            strcasestr(item_database[c->backpack[i].template_idx].name, input) != NULL) {
          found_idx = i;
          break;
        }
      }
      //3. Search by name in belt
      if (found_idx == -1) {
        for (int i = 0; i < MAX_BELT; i++) {
          if (c->belt[i].template_idx != -1 && c->belt[i].stack_count > 0 &&
              strcasestr(item_database[c->belt[i].template_idx].name, input) != NULL) {
            found_idx = i;
            in_belt = true;
            break;
          }
        }
      }
    }

    if (found_idx == -1) {
      send_text_to_client(c->sock, "[SYSTEM] Object '%s' not found.", input);
      return;
    }

    ItemInstance *item_to_drop = in_belt ? &c->belt[found_idx] : &c->backpack[found_idx];

    //Find empty NPC slots
    int npc_slot = -1;
    for (int i = 0; i < MAX_NPCS; i++) {
      if (!npcs[i].active) {
        npc_slot = i;
        break;
      }
    }

    if (npc_slot == -1) {
      send_text_to_client(c->sock, "[SYSTEM] Unable to drop item: too many items on the ground.");
      return;
    }

    // Assign NPC as dropped item
    npcs[npc_slot].active = true;
    npcs[npc_slot].entity_id = next_id++;
    npcs[npc_slot].floor_id = c->floor_id;
    npcs[npc_slot].x = c->x;
    npcs[npc_slot].y = c->y;
    npcs[npc_slot].archetype = ARCH_TREASURE;
    npcs[npc_slot].template_idx = item_to_drop->template_idx; // Used for rendering client-side
    npcs[npc_slot].hp = npcs[npc_slot].max_hp = 1;
    memset(npcs[npc_slot].ghost_loot, 0, sizeof(npcs[npc_slot].ghost_loot));
    npcs[npc_slot].ghost_loot[0] = *item_to_drop; // Copy full item state
    npcs[npc_slot].template = NULL;

    // Register on the grid so players can pick it up
    master_world->floors[c->floor_id].entity_grid[c->y][c->x] = npcs[npc_slot].entity_id;

    send_text_to_client(c->sock, "[ACTION] You dropped: %s on the ground", item_database[item_to_drop->template_idx].name);

    // Remove from inventory
    if (in_belt) {
      c->belt[found_idx].template_idx = -1;
      c->belt[found_idx].stack_count = 0;
    } else {
      for (int i = found_idx; i < c->backpack_count - 1; i++) {
        c->backpack[i] = c->backpack[i + 1];
      }
      c->backpack_count--;
    }
    return;
  }
  if (strncmp(cmd, "throw ", 6) == 0 || strncmp(cmd, "v ", 2) == 0) {
    const char *input = strchr(cmd, ' ');
    if (input) {
      input++;
    } else {
      return;
    }
    char item_query[64] = {0};
    char dir_char = '\0';
    
    const char *last_space = strrchr(input, ' ');
    if (last_space) {
        dir_char = last_space[1];
        int len = last_space - input;
        if (len > 63) len = 63;
        strncpy(item_query, input, len);
        item_query[len] = '\0';
    } else {
        send_text_to_client(c->sock, "[SYSTEM] Specifies the direction. Example: throw dart n");
        return;
    }
    
    if (dir_char != 'n' && dir_char != 's' && dir_char != 'e' && dir_char != 'w') {
        send_text_to_client(c->sock, "[SYSTEM] Invalid direction. Use: n, s, e, w (e.g. throw dart e)");
        return;
    }
    
    int found_idx = -1;
    bool in_belt = false;

    if (item_query[0] >= '0' && item_query[0] <= '9') {
      int requested_idx = atoi(item_query) - 1;
      if (requested_idx >= 0 && requested_idx < c->backpack_count) {
        found_idx = requested_idx;
      }
    } else {
      for (int i = 0; i < c->backpack_count; i++) {
        if (c->backpack[i].template_idx != -1 &&
            strcasestr(item_database[c->backpack[i].template_idx].name, item_query) != NULL) {
          found_idx = i;
          break;
        }
      }
      if (found_idx == -1) {
        for (int i = 0; i < MAX_BELT; i++) {
          if (c->belt[i].template_idx != -1 && c->belt[i].stack_count > 0 &&
              strcasestr(item_database[c->belt[i].template_idx].name, item_query) != NULL) {
            found_idx = i;
            in_belt = true;
            break;
          }
        }
      }
    }

    if (found_idx == -1) {
      send_text_to_client(c->sock, "[SYSTEM] Item '%s' not found in your inventory.", item_query);
      return;
    }

    ItemInstance *item_to_throw = in_belt ? &c->belt[found_idx] : &c->backpack[found_idx];
    ItemTemplate *t_item = &item_database[item_to_throw->template_idx];

    int dx = 0, dy = 0;
    if (dir_char == 'n') dy = -1;
    else if (dir_char == 's') dy = 1;
    else if (dir_char == 'e') dx = 1;
    else if (dir_char == 'w') dx = -1;

    int max_range = 5;
    if (t_item->category == ITEM_AMMO) max_range = 8;

    int curr_x = c->x;
    int curr_y = c->y;
    int last_valid_x = c->x;
    int last_valid_y = c->y;
    int hit_npc_idx = -1;

    for (int step = 1; step <= max_range; step++) {
      int next_x = curr_x + dx;
      int next_y = curr_y + dy;

      if (next_x < 0 || next_x >= MAP_WIDTH || next_y < 0 || next_y >= MAP_HEIGHT) {
        break;
      }

      uint8_t voxel = master_world->floors[c->floor_id].map.data[0][next_y][next_x];
      if (voxel == 0 || voxel == 2 || voxel == 3) { 
        break; 
      }

      curr_x = next_x;
      curr_y = next_y;
      last_valid_x = curr_x;
      last_valid_y = curr_y;

      for (int i = 0; i < MAX_NPCS; i++) {
        if (npcs[i].active && npcs[i].floor_id == c->floor_id && npcs[i].x == curr_x && npcs[i].y == curr_y && npcs[i].archetype != ARCH_TREASURE && npcs[i].archetype != ARCH_GOLD) {
          hit_npc_idx = i;
          break;
        }
      }

      if (hit_npc_idx != -1) {
        break; 
      }
    }

    send_text_to_client(c->sock, "[ACTION] You throw %s towards %c...", t_item->name, dir_char);

    broadcast_spell_vfx(c->x, c->y, last_valid_x, last_valid_y, 0, 1.0f, 0.8f, 0.3f, c->floor_id);

    if (hit_npc_idx != -1) {
      NPC *target = &npcs[hit_npc_idx];
      int roll = (rand() % 20) + 1;
      int to_hit = roll + rules_get_modifier(c->dex) + t_item->attack_bonus;
      if (to_hit >= target->ac) {
        int dice = t_item->damage_dice_count > 0 ? t_item->damage_dice_count : 1;
        int sides = t_item->damage_dice_sides > 0 ? t_item->damage_dice_sides : 4;
        int dmg = rules_roll_dice(dice, sides);
        target->hp -= dmg;
        
        send_text_to_client(c->sock, "[COMBAT] Hit %s for %d damage! (HP: %d/%d)", 
                            target->template ? target->template->name : "the target", dmg, target->hp, target->max_hp);
        
        if (target->hp <= 0) {
          target->active = false;
          target->respawn_timer = RESPAWN_TICKS;
          int gained_xp = target->template ? target->template->xp : 10;
          c->xp += gained_xp;
          check_level_up(c);
          send_text_to_client(c->sock, "[VICTORY] You defeated %s by throwing %s at them! (+%d XP)", target->template ? target->template->name : "the target", t_item->name, gained_xp);
          if (target->archetype == ARCH_BOSS) {
            handle_boss_death(c, target);
          }
        }
      } else {
        send_text_to_client(c->sock, "[COMBAT] Missed! %s dodges the throw.", target->template ? target->template->name : "the target");
      }
    }

    int npc_slot = -1;
    for (int i = 0; i < MAX_NPCS; i++) {
      if (!npcs[i].active) {
        npc_slot = i;
        break;
      }
    }

    if (npc_slot != -1) {
      npcs[npc_slot].active = true;
      npcs[npc_slot].entity_id = next_id++;
      npcs[npc_slot].floor_id = c->floor_id;
      npcs[npc_slot].x = last_valid_x;
      npcs[npc_slot].y = last_valid_y;
      npcs[npc_slot].archetype = ARCH_TREASURE;
      npcs[npc_slot].template_idx = item_to_throw->template_idx;
      npcs[npc_slot].hp = npcs[npc_slot].max_hp = 1;
      memset(npcs[npc_slot].ghost_loot, 0, sizeof(npcs[npc_slot].ghost_loot));
      npcs[npc_slot].ghost_loot[0] = *item_to_throw;
      npcs[npc_slot].template = NULL;

      master_world->floors[c->floor_id].entity_grid[last_valid_y][last_valid_x] = npcs[npc_slot].entity_id;
    }

    if (in_belt) {
      c->belt[found_idx].template_idx = -1;
      c->belt[found_idx].stack_count = 0;
    } else {
      for (int i = found_idx; i < c->backpack_count - 1; i++) {
        c->backpack[i] = c->backpack[i + 1];
      }
      c->backpack_count--;
    }

    return;
  }
  if (strcmp(cmd, "search") == 0 || strcmp(cmd, "s") == 0) {
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
    return;
  }
  if (strcmp(cmd, "disarm") == 0 || strcmp(cmd, "D") == 0 || strncmp(cmd, "disarm ", 7) == 0 ) {
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
      return;
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
    return;
  }
  if (strcmp(cmd, "pray") == 0 || strcmp(cmd, "p") == 0) {
    if (c->xp >= 100) {
      c->xp -= 100;
      int heal = c->max_hp / 4;
      c->hp += heal;
      if (c->hp > c->max_hp) c->hp = c->max_hp;
      send_detailed_state(c); //Sync HP and XP with the HUD client
      send_text_to_client(c->sock, "[SYSTEM] You pray hard, sacrificing 100 XP. Restores %d HP (HP: %d/%d).", heal, c->hp, c->max_hp);
    } else {
      send_text_to_client(c->sock, "[SYSTEM] The Gods do not respond. You don't have enough experience (XP) to sacrifice.");
    }
    return;
  }
  if (strcmp(cmd, "examine") == 0 || strcmp(cmd, "ex") == 0) {
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
        
        for (int m = 0; m < MAX_NPCS; m++) {
          if (npcs[m].active && npcs[m].x == nx && npcs[m].y == ny && npcs[m].floor_id == c->floor_id) {
            if (npcs[m].archetype == ARCH_GOLD) {
              strncat(presence, "| [Golden Bag]", sizeof(presence) - strlen(presence) - 1);
            } else if (npcs[m].archetype == ARCH_TREASURE) {
              strncat(presence, "| [Treasury Chest]", sizeof(presence) - strlen(presence) - 1);
            } else if (npcs[m].is_ghost) {
              char gbuf[64];
              snprintf(gbuf, sizeof(gbuf), "| [%s remains]", npcs[m].custom_name);
              strncat(presence, gbuf, sizeof(presence) - strlen(presence) - 1);
            } else if (npcs[m].archetype == ARCH_MERCHANT) {
              char mbuf[64];
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
    
    return;
  }
  if (strncmp(cmd, "tunnel ", 7) == 0 || strncmp(cmd, "T ", 2) == 0 || strcmp(cmd, "tunnel") == 0 || strcmp(cmd, "T") == 0) {
    const char *dir_str = strchr(cmd, ' ');
    if (!dir_str) {
      send_text_to_client(c->sock, "[SYSTEM] Use: tunnel <n|s|e|w> or 'tunnel d' (down/under your feet)");
      return;
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
      return;
    }
    
    Floor *f = &master_world->floors[c->floor_id];
    if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) {
       send_text_to_client(c->sock, "[SYSTEM] Beyond borders.");
       return;
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
        return;
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
        return;
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
        return;
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
        return;
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
    return;
  }
  if (strcmp(cmd, "open") == 0 || strcmp(cmd, "o") == 0) {
    const char *dir_str = strchr(cmd, ' ');
    if (!dir_str) {
      send_text_to_client(c->sock, "[SYSTEM] Use: open <n|s|e|w>");
      return;
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
    }
    if (master_world->floors[c->floor_id].map.data[0][ty][tx] == 3 /* VOXEL_DOOR */) {
      master_world->floors[c->floor_id].map.data[0][ty][tx] = 1; //Temporarily it becomes the floor
      send_text_to_client(c->sock, "[SYSTEM] You opened the door.");
    } else {
      send_text_to_client(c->sock, "[SYSTEM] There is no door there.");
    }
    return;
  }
  if (strcmp(cmd, "close") == 0 || strcmp(cmd, "c") == 0) {
    const char *dir_str = strchr(cmd, ' ');
    if (!dir_str) {
      send_text_to_client(c->sock, "[SYSTEM] Use: close <n|s|e|w>");
      return;
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
    }
    // We only check if it is a floor tile to allow closing (assumes it was previously a door).
    if (master_world->floors[c->floor_id].map.data[0][ty][tx] == 1 /* VOXEL_FLOOR */) {
      master_world->floors[c->floor_id].map.data[0][ty][tx] = 3; /* VOXEL_DOOR */
      send_text_to_client(c->sock, "[SYSTEM] You closed the door.");
    } else {
      send_text_to_client(c->sock, "[SYSTEM] There is no open passage to close there.");
    }
    return;
  }
  if (strcmp(cmd, "disarm") == 0 || strcmp(cmd, "D") == 0) {
    int roll = (rand() % 20) + 1;
    int dex_mod = rules_get_modifier(c->dex);
    int check = roll + dex_mod;
    bool found = false;
    Floor *f = &master_world->floors[c->floor_id];
    for (int i = 0; i < f->trap_count; i++) {
      Trap *t = &f->traps[i];
      if (!t->active || !t->detected) continue;
      
      int tx = t->x, ty = t->y;
      if (t->type == 1) { // DART_WALL
          tx += (t->wall_dir == 1 ? 1 : (t->wall_dir == 3 ? -1 : 0));
          ty += (t->wall_dir == 2 ? 1 : (t->wall_dir == 0 ? -1 : 0));
      }
      
      if (abs(c->x - tx) <= 1 && abs(c->y - ty) <= 1) {
        found = true;
        int disarm_dc = t->detection_dc + 2;
        if (check >= disarm_dc) {
          t->active = false;
          send_text_to_client(c->sock, "[SYSTEM] Disarmed! The trap has been neutralized.");
        } else if (check <= disarm_dc - 5) {
          // Critical failure
          t->detected = false; // "Reset" but triggers if walked on
          send_text_to_client(c->sock, "[SYSTEM] Critical failure! (Roll: %d). You have accidentally triggered or worsened the trap!", check);
        } else {
          send_text_to_client(c->sock, "[SYSTEM] You failed to disarm the trap (Roll: %d vs DC: %d). You can try again.", check, disarm_dc);
        }
        break; //Attempt to disarm only one trap at a time
      }
    }
    if (!found) {
      send_text_to_client(c->sock, "[SYSTEM] No traps (detected) nearby to disarm.");
    }
    return;
  }

  //Select everything else as an unrecognized command, instead of silently discarding it
  send_text_to_client(c->sock, "[SYSTEM] Unrecognized command '%s' or incorrect syntax. Type 'help' or '?' for the list.", cmd);
}
