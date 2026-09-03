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

#include "bestiary.h"
#include "classes.h"
#include "data_loader.h"
#include "game.h"
#include "gen.h"
#include "items.h"
#include "net.h"
#include "protocol.h"
#include "rules.h"
#include "species.h"
#include "spells.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ai.h"
#include "aoe.h"
#include "combat_log.h"
#include "server_entities.h"
#include "server_internal.h"
#include "server_combat.h"
#include "server_spawn.h"

/*Keep the protocol constant in sync with the server enum*/
_Static_assert((int)BOOKS_MARTIAL == SHOP_SPEC_BOOKS_MARTIAL,
               "SHOP_SPEC_BOOKS_MARTIAL must equal MerchantSpecialization::BOOKS_MARTIAL");
#include "server_world.h"
#include "server_commands.h"
#include "spell_router.h"

//Configuration constants for population and respawn
#define RESPAWN_TICKS 120       //~2 min at 6s/tick
#define DENSITY_CHECK 50        //every N global rounds
#define DENSITY_MIN_PCT 40      //emergency spawn if < 40% active
#define RESPAWN_TRAPS_TICKS 300 // ~30 min

World *master_world = NULL;
int global_total_turns = 0;
int active_event_type = 0;
int event_floor_id = 1;
int event_time_left = 0;
int event_progress = 0;
int event_goal = 0;
int next_id = 1;
static Client *global_clients = NULL;

/*--- Graceful shutdown (Ctrl-C / SIGTERM) ---
* The handler only sets a flag (async-signal-safe); the main loop
* notices it on the next tick and exits through the normal cleanup
* path, so clog_close() can write the closing bracket of
* combat_log.json.*/
static volatile sig_atomic_t g_shutdown_requested = 0;
static void on_shutdown_signal(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}
static char SERVER_ACCESS_PASSWORD[64] = "dragongl_secret";

/*--- FNV-1a 32-bit hash — used to not save passwords in clear text ---*/
static uint32_t fnv1a_hash(const char *str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)(*str++);
        hash *= 16777619u;
    }
    return hash;
}

/*Produces an 8-digit hex string of the FNV-1a hash of the password*/
static void hash_password(const char *pwd, char *out, size_t out_len) {
    uint32_t h = fnv1a_hash(pwd);
    snprintf(out, out_len, "%08x", h);
}

// ===== SISTEMA ARTEFATTI UNICI =====
//Each artifact can only drop ONE time in the entire life of the server.
#define MAX_ARTIFACTS 58
typedef struct {
    const char *name;          //name of the artifact
    int base_template_name_idx; //searched by name in item_database
    int to_hit;                //to-hit bonus
    int to_dam;                //bonus to-dam
    int ac_bonus;              //AC bonuses
    ItemElement element;       // elemento speciale
    ItemBlessing blessing;     // benedizione
    int str_bonus;             //STR bonus
    int dex_bonus;             //DEX bonuses
    int con_bonus;             //bonus WITH
    bool already_dropped;      //true = already dropped, don't drop again
} ArtifactDef;

static ArtifactDef artifact_registry[MAX_ARTIFACTS] = {
    {"The Void Blade",       -1, 5, 8, 0, ELEM_SHOCKING, BLESS_BLESSED, 2, 0, 0, false},
    {"The Shield of Aegis",       -1, 0, 0, 7, ELEM_NONE,     BLESS_BLESSED, 0, 1, 3, false},
    {"The Staff of Dawn",  -1, 3, 5, 0, ELEM_FLAMING,  BLESS_BLESSED, 0, 0, 1, false},
    {"The Shadow Robe",    -1, 0, 0, 5, ELEM_NONE,     BLESS_NORMAL,  0, 3, 0, false},
    {"The Poisoned Dagger",   -1, 4, 3, 0, ELEM_POISONOUS,BLESS_NORMAL,  0, 2, 0, false},
    {"The Frost Gauntlet",      -1, 2, 6, 2, ELEM_FROST,    BLESS_NORMAL,  0, 0, 0, false},
    {"Leviathan's Armor", -1, 0, 0, 10, ELEM_NONE,    BLESS_BLESSED, 1, 0, 4, false},
    {"The Bow of Destiny",      -1, 6, 4, 0, ELEM_SHOCKING, BLESS_BLESSED, 0, 3, 0, false},
    {"Giant's Cleaver",     -1, 2, 9, 0, ELEM_NONE,     BLESS_NORMAL,  4, 0, 0, false},
    {"Sage's Ring",       -1, 0, 0, 0, ELEM_NONE,     BLESS_BLESSED, 0, 0, 1, false},
    {"Boots of the Wind",       -1, 0, 0, 1, ELEM_NONE,     BLESS_NORMAL,  0, 4, 0, false},
    {"Amulet of Life",      -1, 0, 0, 0, ELEM_NONE,     BLESS_BLESSED, 0, 0, 5, false},
    {"Hero's Cloak",      -1, 1, 1, 2, ELEM_NONE,     BLESS_BLESSED, 1, 1, 1, false},
    {"Thunder Hammer",      -1, 3, 7, 0, ELEM_SHOCKING, BLESS_BLESSED, 3, 0, 0, false},
    {"Vigilante's Helm",      -1, 0, 0, 3, ELEM_NONE,     BLESS_NORMAL,  0, 0, 2, false},
    {"Bracers of Strength",   -1, 0, 2, 1, ELEM_NONE,     BLESS_NORMAL,  3, 0, 0, false},
    {"Sword of Fire",          -1, 4, 4, 0, ELEM_FLAMING,  BLESS_NORMAL,  1, 0, 0, false},
    {"Belt of Vigor",      -1, 0, 0, 0, ELEM_NONE,     BLESS_NORMAL,  0, 0, 4, false},
    {"Book of Mysteries",       -1, 1, 0, 0, ELEM_NONE,     BLESS_BLESSED, 0, 0, 0, false},
    {"Shield of the Eclipse",      -1, 0, 0, 6, ELEM_NONE,     BLESS_NORMAL,  0, 0, 2, false},
    {"Assassin's Dagger",     -1, 5, 5, 0, ELEM_POISONOUS,BLESS_NORMAL,  0, 4, 0, false},
    {"Iron Breastplate",      -1, 0, 0, 8, ELEM_NONE,     BLESS_NORMAL,  2, 0, 2, false},
    {"Elven Longbow",       -1, 5, 2, 0, ELEM_NONE,     BLESS_BLESSED, 0, 3, 0, false},
    {"Wizard's Staff",        -1, 2, 2, 0, ELEM_NONE,     BLESS_BLESSED, 0, 0, 1, false},
    {"Ring of Fire",        -1, 0, 2, 0, ELEM_FLAMING,  BLESS_NORMAL,  0, 0, 0, false},
    {"Hunter's Boots",  -1, 1, 1, 1, ELEM_NONE,     BLESS_NORMAL,  0, 2, 0, false},
    {"Royal Cloak",          -1, 0, 0, 2, ELEM_NONE,     BLESS_BLESSED, 0, 1, 2, false},
    {"Fighter's Gloves",  -1, 2, 2, 0, ELEM_NONE,     BLESS_NORMAL,  2, 2, 0, false},
    {"Crown of the Sovereign",      -1, 1, 0, 1, ELEM_NONE,     BLESS_BLESSED, 0, 0, 2, false},
    {"Spear of Destiny",      -1, 4, 4, 0, ELEM_NONE,     BLESS_BLESSED, 2, 2, 0, false},
    {"Battleaxe",           -1, 1, 8, 0, ELEM_NONE,     BLESS_NORMAL,  3, 0, 0, false},
    {"Paladin's Shield",      -1, 0, 0, 5, ELEM_NONE,     BLESS_BLESSED, 0, 0, 3, false},
    {"Talisman of Fortune", -1, 1, 1, 1, ELEM_NONE,     BLESS_BLESSED, 1, 1, 1, false},
    {"Lead Boots",       -1, 0, 0, 3, ELEM_NONE,     BLESS_NORMAL,  2, -2, 2, false},
    {"Cloak of Darkness",       -1, 2, 0, 2, ELEM_NONE,     BLESS_NORMAL,  0, 3, 0, false},
    {"Sword of the Righteous",        -1, 3, 3, 0, ELEM_NONE,     BLESS_BLESSED, 2, 0, 0, false},
    {"Protection Ring",    -1, 0, 0, 2, ELEM_NONE,     BLESS_BLESSED, 0, 0, 0, false},
    {"Leather Bracelets",      -1, 0, 0, 2, ELEM_NONE,     BLESS_NORMAL,  0, 2, 0, false},
    {"Steel helmet",         -1, 0, 0, 4, ELEM_NONE,     BLESS_NORMAL,  0, 0, 1, false},
    {"Arrows of Light",          -1, 3, 3, 0, ELEM_SHOCKING, BLESS_BLESSED, 0, 0, 0, false},
    {"Woodland Staff",       -1, 2, 2, 0, ELEM_NONE,     BLESS_NORMAL,  1, 1, 0, false},
    {"Wanderer's Cloak",  -1, 0, 0, 1, ELEM_NONE,     BLESS_NORMAL,  0, 2, 0, false},
    {"Ring of Power",       -1, 1, 1, 0, ELEM_NONE,     BLESS_BLESSED, 1, 0, 0, false},
    {"Rubber Boots",        -1, 0, 0, 0, ELEM_NONE,     BLESS_NORMAL,  0, 1, 0, false},
    {"Warrior Belt",   -1, 0, 1, 0, ELEM_NONE,     BLESS_NORMAL,  2, 0, 0, false},
    {"Velvet Gloves",       -1, 0, 0, 0, ELEM_NONE,     BLESS_NORMAL,  0, 0, 0, false},
    {"Silver Medallion",    -1, 0, 0, 1, ELEM_NONE,     BLESS_BLESSED, 0, 0, 0, false},
    {"Silver Dagger",       -1, 2, 2, 0, ELEM_NONE,     BLESS_BLESSED, 0, 1, 0, false},
    {"Wooden Bow",           -1, 2, 1, 0, ELEM_NONE,     BLESS_NORMAL,  0, 1, 0, false},
    {"Wooden Shield",          -1, 0, 0, 2, ELEM_NONE,     BLESS_NORMAL,  0, 0, 0, false},
    {"Leather Helm",           -1, 0, 0, 1, ELEM_NONE,     BLESS_NORMAL,  0, 0, 0, false},
    {"Wool cloak",        -1, 0, 0, 0, ELEM_NONE,     BLESS_NORMAL,  0, 0, 1, false},
    {"Copper Ring",          -1, 0, 0, 0, ELEM_NONE,     BLESS_NORMAL,  0, 0, 0, false},
    {"Leather Boots",        -1, 0, 0, 0, ELEM_NONE,     BLESS_NORMAL,  0, 1, 0, false},
    {"Rope Belt",        -1, 0, 0, 0, ELEM_NONE,     BLESS_NORMAL,  0, 0, 0, false},
    {"Linen gloves",          -1, 0, 0, 0, ELEM_NONE,     BLESS_NORMAL,  0, 0, 0, false},
    {"Stone Necklace",       -1, 0, 0, 0, ELEM_NONE,     BLESS_NORMAL,  0, 0, 0, false},
    {"Iron Sword",          -1, 1, 1, 0, ELEM_NONE,     BLESS_NORMAL,  0, 0, 0, false}
};
static int artifact_count = 58;

static void artifacts_load(void) {
    FILE *f = fopen("data/artifacts.dat", "rb");
    if (!f)
        return;
    for (int i = 0; i < artifact_count; i++) {
        bool dropped;
        if (fread(&dropped, sizeof(bool), 1, f) == 1)
            artifact_registry[i].already_dropped = dropped;
    }
    fclose(f);
    printf("[ARTIFACT] Artifact state loaded.\n");
}

static void artifacts_save(void) {
    FILE *f = fopen("data/artifacts.dat", "wb");
    if (!f)
        return;
    for (int i = 0; i < artifact_count; i++) {
        fwrite(&artifact_registry[i].already_dropped, sizeof(bool), 1, f);
    }
    fclose(f);
}

// Forward declarations
void check_level_up(Client *c);
void broadcast_player_state(Client *c);
void notify_player_left_floor(Client *c, int old_floor);
Client *g_clients = NULL;
NPC *g_npcs = NULL;

void broadcast_spell_vfx(int sx, int sy, int tx, int ty, int vfx_type, float r, float g, float b, int floor_id);
/*add_item_to_shop — migrated to server_spawn.c*/
void sync_entity_grid(NPC *npcs);

void sync_entity_grid(NPC *npcs) {
  if (!master_world)
    return;
  for (int f = 0; f < MAX_FLOORS; f++) {
    memset(master_world->floors[f].entity_grid, 0,
           sizeof(master_world->floors[f].entity_grid));
  }
  if (global_clients) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (global_clients[i].active && global_clients[i].authenticated) {
        Floor *fl = &master_world->floors[global_clients[i].floor_id];
        fl->entity_grid[global_clients[i].y][global_clients[i].x] =
            global_clients[i].entity_id;
      }
    }
  }
  if (npcs) {
    for (int i = 0; i < MAX_NPCS; i++) {
      if (npcs[i].active) {
        Floor *fl = &master_world->floors[npcs[i].floor_id];
        fl->entity_grid[npcs[i].y][npcs[i].x] = npcs[i].entity_id;
      }
    }
  }
}

void server_log(const char *cat, const char *fmt, ...) {
  time_t now = time(NULL);
  char b[20];
  strftime(b, 20, "%H:%M:%S", localtime(&now));
  printf("[%s] [%-8s] ", b, cat);
  va_list a;
  va_start(a, fmt);
  vprintf(fmt, a);
  va_end(a);
  printf("\n");
  fflush(stdout);
}

void send_text_to_client(int sock, const char *fmt, ...) {
  MsgHeader h;
  MsgText m;
  va_list a;
  h.type = MSG_TEXT;
  h.length = sizeof(m);
  va_start(a, fmt);
  vsnprintf(m.text, sizeof(m.text), fmt, a);
  va_end(a);
  net_send(sock, &h, sizeof(h));
  net_send(sock, &m, sizeof(m));
}

void send_map_chunk(int sock, Map *map, int cx, int cy, int size) {
  MsgHeader h;
  MsgMapChunk mc;
  int sx = cx - size / 2, sy = cy - size / 2;
  if (sx < 0) {
    sx = 0;
  }
  if (sy < 0) {
    sy = 0;
  }
  if (sx + size > MAP_WIDTH) {
    sx = MAP_WIDTH - size;
  }
  if (sy + size > MAP_HEIGHT) {
    sy = MAP_HEIGHT - size;
  }
  mc.start_x = sx;
  mc.start_y = sy;
  mc.width = size;
  mc.height = size;
  h.type = MSG_MAP_CHUNK;
  h.length = sizeof(mc) + (size * size * sizeof(VoxelType));
  VoxelType *buf = malloc(size * size * sizeof(VoxelType));
  if (!buf)
    return;
  int idx = 0;
  for (int iy = sy; iy < sy + size; iy++)
    for (int ix = sx; ix < sx + size; ix++)
      buf[idx++] = map->data[0][iy][ix];
  net_send(sock, &h, sizeof(h));
  net_send(sock, &mc, sizeof(mc));
  net_send(sock, buf, size * size * sizeof(VoxelType));
  free(buf);
}

static void get_game_time(int *h, int *m) {
  int total_mins = (global_total_turns % 1440);
  *h = (total_mins / 60 + 8) % 24;
  *m = total_mins % 60;
}

void check_traps(Client *c, NPC *npcs) {
  Floor *f = &master_world->floors[c->floor_id];
  for (int i = 0; i < f->trap_count; i++) {
    Trap *t = &f->traps[i];
    if (!t->active)
      continue;

    // Passive Perception to detect (WIS modifier + 10)
    int passive_perc = 10 + rules_get_modifier(c->wis);

    // DART_WALL detection: player senses the tiny hole in the wall
    if (t->type == TRAP_DART_WALL && !t->detected) {
      //Dart fires FROM wall tile (t->x, t->y) into adjacent floor tile
      int wall_dx[4] = {0, 1, 0, -1};
      int wall_dy[4] = {-1, 0, 1, 0};
      int fire_x = t->x + wall_dx[t->wall_dir];
      int fire_y = t->y + wall_dy[t->wall_dir];
      // Detect if player is within 2 tiles of the floor-side trigger tile
      if (abs(c->x - fire_x) <= 2 && abs(c->y - fire_y) <= 2) {
        if (passive_perc >= t->detection_dc) {
          t->detected = true;
          send_text_to_client(
              c->sock, "[SYSTEM] You notice a tiny hole in the wall..."
                       "It looks like the launch tube of a dart trap!");
        }
      }
    } else if (!t->detected &&
               (abs(c->x - t->x) <= 2 && abs(c->y - t->y) <= 2)) {
      if (passive_perc >= t->detection_dc) {
        t->detected = true;
        f->map.data[0][t->y][t->x] = VOXEL_TRAP;
        send_text_to_client(
            c->sock, "[SYSTEM] You notice something suspicious on the floor...");
      }
    }

    //--- Trigger: DART_WALL fires when player steps onto the floor tile in
    // front
    if (t->type == TRAP_DART_WALL && t->active) {
      int wall_dx[4] = {0, 1, 0, -1};
      int wall_dy[4] = {-1, 0, 1, 0};
      int fire_x = t->x + wall_dx[t->wall_dir];
      int fire_y = t->y + wall_dy[t->wall_dir];
      static const char *dir_names[4] = {"nord", "est", "sud", "ovest"};
      if (c->x == fire_x && c->y == fire_y) {
        send_text_to_client(
            c->sock,
            "[DANGER] CLACK! A poison dart shoots out of the wall at %s!",
            dir_names[t->wall_dir]);
        bool saved = rules_roll_save(rules_get_modifier(c->dex), t->save_dc,
                                     false, false, NULL);
        if (saved) {
          send_text_to_client(c->sock,
                              "[SYSTEM] You throw yourself to the side! The dart grazes you"
                              "l'orecchio e si conficca nel muro.");
        } else {
          int dmg_pierce = rules_roll_dice(t->damage_dice, t->damage_sides);
          c->hp -= dmg_pierce;
          send_text_to_client(
              c->sock, "[COMBAT] The dart hits you! %d piercing damage.",
              dmg_pierce);
          //Secondary WITH save vs poison
          int poison_dc = t->save_dc - 2;
          bool saved_poison = rules_roll_save(rules_get_modifier(c->con),
                                              poison_dc, false, false, NULL);
          if (!saved_poison && c->effect_count < MAX_EFFECTS_PER_ENTITY) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Poisoned", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 4, false};
            send_text_to_client(
                c->sock,
                "[STATE] The poison on the dart enters your blood!"
                "You feel your strength abandon you... (Poisoned for 4 rounds)");
          } else if (!saved_poison) {
            send_text_to_client(
                c->sock,
                "[STATE] The poison burns, but your constitution holds!");
          }
          if (c->hp <= 0) {
            c->hp = 0;
            send_text_to_client(
                c->sock,
                "[DEATH] The poison reaches your heart. You fell...");
            clog_death(c->username, "Dardo Avvelenato", c->floor_id);
            save_bones(c);
            c->floor_id = 0;
            c->x = MAP_CENTER_X;
            c->y = MAP_CENTER_Y;
            c->hp = c->max_hp;
          }
        }
        t->active = false;
        t->respawn_timer = RESPAWN_TRAPS_TICKS;
        t->detected = false;
      }
      continue; //Skip floor-tile trigger below
    }

    //--- Trigger: SPRING_SPEAR fires when player steps onto floor tile in
    // front
    if (t->type == TRAP_SPRING_SPEAR && t->active) {
      int wall_dx[4] = {0, 1, 0, -1};
      int wall_dy[4] = {-1, 0, 1, 0};
      static const char *dn[4] = {"nord", "est", "sud", "ovest"};
      int fire_x = t->x + wall_dx[t->wall_dir];
      int fire_y = t->y + wall_dy[t->wall_dir];
      if (c->x == fire_x && c->y == fire_y) {
        send_text_to_client(
            c->sock,
            "[DANGER] SHHKK! A spring-loaded spear pops out of the wall at %s!",
            dn[t->wall_dir]);
        bool sv = rules_roll_save(rules_get_modifier(c->dex), t->save_dc, false,
                                  false, NULL);
        if (sv) {
          send_text_to_client(
              c->sock, "[SYSTEM] Throw yourself to the ground! The spear grazes you!");
        } else {
          int dmg = rules_roll_dice(t->damage_dice, t->damage_sides);
          c->hp -= dmg;
          send_text_to_client(
              c->sock, "[COMBAT] The spear pierces you! %d piercing damage.",
              dmg);
          if (c->effect_count < MAX_EFFECTS_PER_ENTITY) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Restrained", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 2, false};
            send_text_to_client(
                c->sock, "[STATE] You're stuck on the wall! (Restrained 2r)");
          }
          if (c->hp <= 0) {
            c->hp = 0;
            send_text_to_client(c->sock,
                                "[DEATH] Pierced by the spear, you have fallen.");
            clog_death(c->username, "Spring spear", c->floor_id);
            save_bones(c);
            c->floor_id = 0;
            c->x = MAP_CENTER_X;
            c->y = MAP_CENTER_Y;
            c->hp = c->max_hp;
          }
        }
        t->active = false;
        t->respawn_timer = RESPAWN_TRAPS_TICKS;
        t->detected = false;
      }
      continue;
    }

    //--- Trigger: floor tile traps (all other types)
    if (c->x == t->x && c->y == t->y) {
      send_text_to_client(c->sock, "[DANGER] You have activated a trap!");
      bool saved = false;

      //Type-specific save logic
      if (t->type == TRAP_PIT || t->type == TRAP_SPIKES ||
          t->type == TRAP_BLADE || t->type == TRAP_WEB ||
          t->type == TRAP_FALLING_FLOOR || t->type == TRAP_BOULDER ||
          t->type == TRAP_FLOOD || t->type == TRAP_CEILING_COLLAPSE ||
          t->type == TRAP_CRYSTAL_BURST) {
        saved = rules_roll_save(rules_get_modifier(c->dex), t->save_dc, false,
                                false, NULL);
      } else if (t->type == TRAP_GAS || t->type == TRAP_POISON_NEEDLE ||
                 t->type == TRAP_LIGHTNING || t->type == TRAP_SILENCE ||
                 t->type == TRAP_DARKNESS ||
                 t->type == TRAP_PARALYZING_SPORES ||
                 t->type == TRAP_SLEEP_GAS || t->type == TRAP_GAS_VEIN ||
                 t->type == TRAP_HUNGER_CURSE) {
        saved = rules_roll_save(rules_get_modifier(c->con), t->save_dc, false,
                                false, NULL);
      } else if (t->type == TRAP_QUICKSAND || t->type == TRAP_CRUSHER_WALL ||
                 t->type == TRAP_STONE_TOMB) {
        saved = rules_roll_save(rules_get_modifier(c->str), t->save_dc, false,
                                false, NULL);
      } else if (t->type == TRAP_CURSE_RUNE ||
                 t->type == TRAP_ILLUSION_MIRROR ||
                 t->type == TRAP_INVERSION_RUNE || t->type == TRAP_FAKE_DOOR) {
        saved = rules_roll_save(rules_get_modifier(c->wis), t->save_dc, false,
                                false, NULL);
      }

      if (saved) {
        send_text_to_client(c->sock, "[SYSTEM] Can you avoid the worst!");
      } else {
        int dmg = rules_roll_dice(t->damage_dice, t->damage_sides);

        //Special effects traps
        if (t->type == TRAP_LIGHTNING) {
          //Extra damage if in water
          Floor *fl = &master_world->floors[c->floor_id];
          if (fl->map.data[0][c->y][c->x] == VOXEL_WATER)
            dmg *= 2;
        }

        c->hp -= dmg;
        send_text_to_client(c->sock, "[COMBAT] Take %d trap damage!",
                            dmg);

        //Application of environmental conditions and special effects
        if (c->effect_count < MAX_EFFECTS_PER_ENTITY) {
          if (t->type == TRAP_POISON_NEEDLE || t->type == TRAP_GAS ||
              t->type == TRAP_GAS_VEIN) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Poisoned", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 5, false};
            send_text_to_client(c->sock, "[STATE] You are poisoned!");
          } else if (t->type == TRAP_WEB) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Restrained", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 3, false};
            send_text_to_client(c->sock,
                                "[STATE] You are held by the spider web!");
          } else if (t->type == TRAP_BLADE) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Bleeding", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 3, false};
            send_text_to_client(c->sock, "[STATE] You're bleeding!");
          } else if (t->type == TRAP_SILENCE) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Silenced", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 2, false};
            send_text_to_client(c->sock,
                                "[STATE] You are surrounded by magical silence!");
          } else if (t->type == TRAP_QUICKSAND) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Prone", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 1, false};
            send_text_to_client(c->sock,
                                "[STATE] You've landed in quicksand!");
          } else if (t->type == TRAP_DARKNESS) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Blinded", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 2, false};
            send_text_to_client(c->sock, "[STATUS] You are blinded by darkness!");
          } else if (t->type == TRAP_PARALYZING_SPORES) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Paralyzed", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 2, false};
            send_text_to_client(
                c->sock, "[STATUS] The spores paralyze you completely!");
          } else if (t->type == TRAP_SLEEP_GAS) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Unconscious", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 3, false};
            send_text_to_client(c->sock,
                                "[STATE] The gas makes you lose consciousness...");
          } else if (t->type == TRAP_ANTIMAGIC_ZONE) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Silenced", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 5, false};
            send_text_to_client(
                c->sock, "[STATUS] The magic leaves you (Silenced for 5 rounds)!");
          } else if (t->type == TRAP_INVERSION_RUNE) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Confused", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 3, false};
            send_text_to_client(
                c->sock, "[STATE] Your mind becomes dizzyingly confused!");
          } else if (t->type == TRAP_HUNGER_CURSE) {
            c->effects[c->effect_count++] = (ActiveEffect){
                "Bleeding", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 5, false};
            send_text_to_client(
                c->sock,
                "[STATE] A dark hunger slowly drains your life...");
          }
        }

        // Voxel effects
        if (t->type == TRAP_FALLING_FLOOR) {
          send_text_to_client(
              c->sock, "[DANGER] THE FLOOR COLLAPSES UNDER YOUR FEET!");
          if (c->floor_id < MAX_FLOORS - 1) {
            int old_floor = c->floor_id;
            if (master_world->floors[old_floor].entity_grid[c->y][c->x] == c->entity_id) {
              master_world->floors[old_floor].entity_grid[c->y][c->x] = 0;
            }
            c->floor_id++;
            notify_player_left_floor(c, old_floor);
            c->x += (rand() % 5) - 2;
            c->y += (rand() % 5) - 2;
            client_track_explored_floor(c);
            /*Arrival broadcast AFTER the position is set*/
            broadcast_player_state(c);
          }
        } else if (t->type == TRAP_FLOOD) {
          send_text_to_client(c->sock, "[DANGER] Water starts gushing out"
                                       "rivers from hidden valves!");
          Floor *fl = &master_world->floors[c->floor_id];
          for (int dy = -3; dy <= 3; dy++) {
            for (int dx = -3; dx <= 3; dx++) {
              int nx = c->x + dx;
              int ny = c->y + dy;
              if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
                if (fl->map.data[0][ny][nx] == VOXEL_FLOOR)
                  fl->map.data[0][ny][nx] = VOXEL_WATER;
              }
            }
          }
        } else if (t->type == TRAP_CEILING_COLLAPSE) {
          send_text_to_client(c->sock, "[DANGER] THE CEILING IS COLLAPSING!");
          Floor *fl = &master_world->floors[c->floor_id];
          for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
              int nx = c->x + dx;
              int ny = c->y + dy;
              if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
                if (fl->map.data[0][ny][nx] == VOXEL_FLOOR &&
                    (rand() % 2 == 0)) {
                  fl->map.data[0][ny][nx] = VOXEL_ROCK;
                }
              }
            }
          }
        } else if (t->type == TRAP_STONE_TOMB) {
          send_text_to_client(c->sock, "[DANGER] Thick stone walls yes"
                                       "they rise from the ground, trapping you!");
          Floor *fl = &master_world->floors[c->floor_id];
          for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
              if (dx == 0 && dy == 0)
                continue;
              int nx = c->x + dx;
              int ny = c->y + dy;
              if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
                fl->map.data[0][ny][nx] = VOXEL_WALL;
              }
            }
          }
        } else if (t->type == TRAP_SWAP_TELEPORT || t->type == TRAP_BODY_SWAP) {
          //Find a nearby enemy (within 10 tiles) to trade
          NPC *target_npc = NULL;
          for (int n = 0; n < MAX_NPCS; n++) {
            if (npcs[n].active && npcs[n].floor_id == c->floor_id) {
              if (abs(npcs[n].x - c->x) <= 10 && abs(npcs[n].y - c->y) <= 10) {
                target_npc = &npcs[n];
                break;
              }
            }
          }
          if (target_npc) {
            if (t->type == TRAP_SWAP_TELEPORT) {
              send_text_to_client(
                  c->sock,
                  "[MAGIC] A flash of light! You trade places with a %s!",
                  target_npc->template->name);
              int tx = c->x;
              int ty = c->y;
              c->x = target_npc->x;
              c->y = target_npc->y;
              target_npc->x = tx;
              target_npc->y = ty;
            } else {
              send_text_to_client(c->sock,
                                  "[MAGIC] A dark force envelops you! Exchanges"
                                  "your vitality with a %s!",
                                  target_npc->template->name);
              int thp = c->hp;
              c->hp = target_npc->hp;
              target_npc->hp = thp;
            }
          } else {
            send_text_to_client(c->sock,
                                "[MAGIC] The trap activates but does not find"
                                "valid targets nearby.");
          }
        }

        if (c->hp <= 0) {
          clog_death(c->username, "Trap", c->floor_id);
          save_bones(c);
          c->hp = c->max_hp;
          send_text_to_client(c->sock,
                              "[DEATH] You died from a trap... The Arcane has returned you to town without your equipment!");
          c->floor_id = 0;
          c->x = MAP_CENTER_X + 1;
          c->y = MAP_CENTER_Y + 1;
        }
      }
      if (t->type == TRAP_TELEPORT || t->type == TRAP_FAKE_DOOR) {
        // Random teleport
        c->x = rand() % MAP_WIDTH;
        c->y = rand() % MAP_HEIGHT;
        send_text_to_client(
            c->sock,
            "[MAGIC] Space distorts, you have been teleported!");
      } else if (t->type != TRAP_ILLUSION_MIRROR &&
                 t->type != TRAP_SWAP_TELEPORT) {
        t->active = false;
      }
      t->respawn_timer = RESPAWN_TRAPS_TICKS;
      t->detected = false; // Reset detection for next time
      if (f->map.data[0][t->y][t->x] == VOXEL_TRAP) {
        f->map.data[0][t->y][t->x] = VOXEL_FLOOR;
      }
    }
  }
}

void update_city_doors(void) {
  int h, m;
  get_game_time(&h, &m);
  VoxelType state = (h >= 6 && h < 20) ? VOXEL_DOOR : VOXEL_WALL;
  Map *city = &master_world->floors[0].map;
  int cx = MAP_CENTER_X;
  int cy = MAP_CENTER_Y;
  int shop_coords[10][2] = {
      {cx - 12, cy - 20}, {cx, cy - 20}, {cx + 12, cy - 20},
      {cx - 20, cy - 6},  {cx - 20, cy + 6},
      {cx + 20, cy - 6},  {cx + 20, cy + 6},
      {cx - 12, cy + 20}, {cx, cy + 20}, {cx + 12, cy + 20}
  };

  for (int i = 0; i < 10; i++) {
      int sx = shop_coords[i][0];
      int sy = shop_coords[i][1];
      if (sy < cy - 10) city->data[0][sy + 4][sx] = state;
      else if (sy > cy + 10) city->data[0][sy - 4][sx] = state;
      else if (sx < cx - 10) city->data[0][sy][sx + 4] = state;
      else if (sx > cx + 10) city->data[0][sy][sx - 4] = state;
  }
}

void get_total_stats(Client *c, int *ts, int *td, int *tc, int *ti,
                            int *tw, int *th) {
  *ts = c->str;
  *td = c->dex;
  *tc = c->con;
  *ti = c->intel;
  *tw = c->wis;
  *th = c->cha;
  ItemInstance *s[] = {&c->slot_head,  &c->slot_neck,   &c->slot_body,
                       &c->slot_back,  &c->slot_hand_r, &c->slot_hand_l,
                       &c->slot_hands, &c->slot_feet};
  for (int i = 0; i < 8; i++)
    if (s[i]->template_idx != -1) {
      const ItemTemplate *it = &item_database[s[i]->template_idx];
      *ts += it->str_bonus;
      *td += it->dex_bonus;
      *tc += it->con_bonus;
      *ti += it->int_bonus;
      *tw += it->wis_bonus;
      *th += it->cha_bonus;
    }
  for (int i = 0; i < 10; i++)
    if (c->slot_rings[i].template_idx != -1) {
      const ItemTemplate *it = &item_database[c->slot_rings[i].template_idx];
      *ts += it->str_bonus;
      *td += it->dex_bonus;
      *tc += it->con_bonus;
      *ti += it->int_bonus;
      *tw += it->wis_bonus;
      *th += it->cha_bonus;
    }
}

// -----------------------------------------------------------------------
void get_full_item_name(const ItemInstance *inst, char *buf, size_t max_len) {
    if (!inst->is_identified) {
        snprintf(buf, max_len, "??? (unknown)");
        return;
    }
    //Unique Artifact: Uses its proper name directly
    if (inst->is_artifact && inst->artifact_name[0] != '\0') {
        const ItemTemplate *tmpl = &item_database[inst->template_idx];
        char suffix[64] = "";
        if (inst->to_hit_bonus != 0 || inst->to_dam_bonus != 0)
            snprintf(suffix, sizeof(suffix), " (+%d, +%d)", inst->to_hit_bonus, inst->to_dam_bonus);
        else if (inst->ac_bonus != 0)
            snprintf(suffix, sizeof(suffix), " [+%d]", inst->ac_bonus);
        snprintf(buf, max_len, "** %s [%s]%s **",
                 inst->artifact_name, tmpl->name, suffix);
        return;
    }
    const ItemTemplate *tmpl = &item_database[inst->template_idx];
     char prefix[64] = "";
    if (inst->blessing == BLESS_BLESSED) {
        strcat(prefix, "Blessed ");
    } else if (inst->blessing == BLESS_CURSED) {
        strcat(prefix, "Cursed ");
    }

    if (inst->quality == QUALITY_RUSTY) {
        strcat(prefix, "Rusty ");
    } else if (inst->quality == QUALITY_FINE) {
        strcat(prefix, "End");
    } else if (inst->quality == QUALITY_MASTERWORK) {
        strcat(prefix, "Masterwork ");
    }
    
    if (inst->element == ELEM_FLAMING) {
        strcat(prefix, "Flaming ");
    } else if (inst->element == ELEM_FROST) {
        strcat(prefix, "Frost ");
    } else if (inst->element == ELEM_SHOCKING) {
        strcat(prefix, "Shocking ");
    } else if (inst->element == ELEM_POISONOUS) {
        strcat(prefix, "Poisonous ");
    }
    
    char suffix[64] = "";
    if (tmpl->category == ITEM_WEAPON && (inst->to_hit_bonus != 0 || inst->to_dam_bonus != 0)) {
        snprintf(suffix, sizeof(suffix), " (+%d, +%d)", inst->to_hit_bonus, inst->to_dam_bonus);
    } else if ((tmpl->category == ITEM_ARMOR || tmpl->category == ITEM_SHIELD) && inst->ac_bonus != 0) {
        snprintf(suffix, sizeof(suffix), " [+%d]", inst->ac_bonus);
    } else if (tmpl->category == ITEM_LIGHT_SOURCE) {
        snprintf(suffix, sizeof(suffix), " (%d/%d light turns)", inst->durability, tmpl->max_durability > 0 ? tmpl->max_durability : 100);
    } else if (tmpl->category == ITEM_FUEL) {
        snprintf(suffix, sizeof(suffix), " (%d light turns)", tmpl->duration_turns > 0 ? tmpl->duration_turns : 300);
    }
    
    snprintf(buf, max_len, "%s%s%s", prefix, tmpl->name, suffix);
}

// -----------------------------------------------------------------------
// LOOT SYSTEM: drops an item from the NPC's archetype-biased loot table
// based on floor depth (rarity chance) and the monster's archetype.
// -----------------------------------------------------------------------

void drop_loot_from_monster(Client *c, NPC *killer) {
  if (!c || !killer)
    return;
  if ((rand() % 100) >= LOOT_DROP_CHANCE)
    return;
  if (c->backpack_count >= MAX_BACKPACK) {
    send_text_to_client(c->sock, "[LOOT] Backpack full! Lose %s loot.",
                        killer->template ? killer->template->name : "treasure");
    return;
  }

  int floor_id = killer->floor_id;

  // Rarity tier driven by depth
  int roll = rand() % 100;
  int rarity_tier;
  if (floor_id <= 10) {
    if (roll < 60) {
      rarity_tier = 0;
    } else if (roll < 90) {
      rarity_tier = 1;
    } else {
      rarity_tier = 2;
    }
  } else if (floor_id <= 30) {
    if (roll < 30) {
      rarity_tier = 0;
    } else if (roll < 60) {
      rarity_tier = 1;
    } else if (roll < 85) {
      rarity_tier = 2;
    } else {
      rarity_tier = 3;
    }
  } else if (floor_id <= 50) {
    if (roll < 15) {
      rarity_tier = 1;
    } else if (roll < 45) {
      rarity_tier = 2;
    } else if (roll < 80) {
      rarity_tier = 3;
    } else {
      rarity_tier = 4;
    }
  } else if (floor_id <= 70) {
    if (roll < 15) {
      rarity_tier = 2;
    } else if (roll < 45) {
      rarity_tier = 3;
    } else if (roll < 80) {
      rarity_tier = 4;
    } else {
      rarity_tier = 5;
    }
  } else if (floor_id <= 85) {
    if (roll < 15) {
      rarity_tier = 3;
    } else if (roll < 45) {
      rarity_tier = 4;
    } else if (roll < 80) {
      rarity_tier = 5;
    } else {
      rarity_tier = 6;
    }
  } else if (floor_id <= 95) {
    if (roll < 15) {
      rarity_tier = 4;
    } else if (roll < 45) {
      rarity_tier = 5;
    } else if (roll < 80) {
      rarity_tier = 6;
    } else {
      rarity_tier = 7;
    }
  } else {
    if (roll < 15) {
      rarity_tier = 5;
    } else if (roll < 45) {
      rarity_tier = 6;
    } else if (roll < 80) {
      rarity_tier = 7;
    } else {
      rarity_tier = 8;
    }
  }

  // Archetype -> preferred item category
  int preferred_cat = -1; //-1 = no preference
  if (killer->archetype == ARCH_MELEE || killer->archetype == ARCH_BRUTE)
    preferred_cat = (rand() % 2 == 0) ? ITEM_WEAPON : ITEM_ARMOR;
  else if (killer->archetype == ARCH_ASSASSIN)
    preferred_cat = ITEM_WEAPON;
  else if (killer->archetype == ARCH_CASTER)
    preferred_cat = ITEM_CONSUMABLE;
  // Cost bands approximate rarity
  static const uint64_t rarity_min[] = {0, 100, 1000, 5000, 20000, 100000, 250000, 500000, 1000000};
  static const uint64_t rarity_max[] = {99, 999, 4999, 19999, 99999, 249999, 499999, 999999, UINT64_MAX};
  static const char *rarity_name[] = {"MUNDANE", "COMMON", "UNCOMMON",
                                     "RARE", "VERY_RARE", "LEGENDARY",
                                     "EPIC", "MYTHIC_RELIC", "ARTIFACT"};
  if (rarity_tier > 8)
    rarity_tier = 8;

  /*Size with margin compared to item_database (max 1024 candidates)*/
  #define MAX_LOOT_CANDIDATES 1024
  int candidates[MAX_LOOT_CANDIDATES];
  int n_candidates = 0;
  for (int pass = 0; pass < 2 && n_candidates == 0; pass++) {
    for (int i = 0; i < item_database_size; i++) {
      uint64_t cost = item_database[i].cost;
      if (cost < rarity_min[rarity_tier] || cost > rarity_max[rarity_tier])
        continue;
      /*On the first pass it respects the category preference, on the second it ignores it*/
      if (pass == 0 && preferred_cat != -1 &&
          (int)item_database[i].category != preferred_cat)
        continue;
      candidates[n_candidates++] = i;
      if (n_candidates >= MAX_LOOT_CANDIDATES)
        break;
    }
  }
  #undef MAX_LOOT_CANDIDATES
  // Ultimate fallback
  if (n_candidates == 0) {
    candidates[0] = rand() % item_database_size;
    n_candidates = 1;
  }

  int chosen_idx = candidates[rand() % n_candidates];
  ItemTemplate *it = &item_database[chosen_idx];
  bool identified = (rarity_tier <= 2); // RARE+ drop unidentified for mystery

  // Stack consumables
  if (it->max_stack > 1) {
    for (int i = 0; i < c->backpack_count; i++) {
      if (c->backpack[i].template_idx == chosen_idx &&
          c->backpack[i].stack_count < it->max_stack) {
        c->backpack[i].stack_count++;
        send_text_to_client(c->sock, "[LOOT] %s dropped: %s (x%d)",
                            killer->template->name,
                            identified ? it->name : "??? (unknown)",
                            c->backpack[i].stack_count);
        return;
      }
    }
  }

  ItemInstance drop;
  memset(&drop, 0, sizeof(ItemInstance));
  drop.template_idx = chosen_idx;
  drop.durability = (it->category == ITEM_LIGHT_SOURCE && strcasestr(it->name, "lantern")) ? 5000 : ((it->max_durability > 0) ? it->max_durability : 100);
  drop.stack_count = 1;
  drop.is_identified = identified;
  drop.quality = QUALITY_NORMAL;
  drop.blessing = BLESS_NORMAL;
  drop.element = ELEM_NONE;
  
  if (it->category == ITEM_WEAPON) {
      if (rarity_tier >= 1) drop.quality = (rand() % 100 < 20) ? QUALITY_MASTERWORK : (rand() % 100 < 20) ? QUALITY_FINE : QUALITY_NORMAL;
      else drop.quality = (rand() % 100 < 30) ? QUALITY_RUSTY : QUALITY_NORMAL;

      drop.to_hit_bonus = (rand() % (rarity_tier + 1));
      drop.to_dam_bonus = rarity_tier + (rand() % 2);
      
      if (rarity_tier >= 2 && rand() % 100 < 30) {
          drop.element = (rand() % 4) + 1;
      }
      if (rand() % 100 < 10) drop.blessing = BLESS_BLESSED;
      else if (rand() % 100 < 10) drop.blessing = BLESS_CURSED;
  }
  else if (it->category == ITEM_ARMOR || it->category == ITEM_SHIELD) {
      if (rarity_tier >= 1) drop.quality = (rand() % 100 < 20) ? QUALITY_MASTERWORK : (rand() % 100 < 20) ? QUALITY_FINE : QUALITY_NORMAL;
      else drop.quality = (rand() % 100 < 30) ? QUALITY_RUSTY : QUALITY_NORMAL;
      drop.ac_bonus = rarity_tier;
      if (rand() % 100 < 10) drop.blessing = BLESS_BLESSED;
      else if (rand() % 100 < 10) drop.blessing = BLESS_CURSED;
  }

  //===== UNIQUE ARTIFACT: drop attempt at rarity >= 5 (LEGENDARY) =====
  if (rarity_tier >= 5 && rand() % 100 < 25) {
    for (int ai = 0; ai < artifact_count; ai++) {
      if (!artifact_registry[ai].already_dropped) {
        //Search the database for the base template by name
        int base_idx = -1;
        //Artifact map -> preferred category to find valid template
        for (int ti = 0; ti < item_database_size; ti++) {
          if (item_database[ti].cost >= 100000 &&
              (item_database[ti].category == ITEM_WEAPON ||
               item_database[ti].category == ITEM_ARMOR ||
               item_database[ti].category == ITEM_SHIELD)) {
            base_idx = ti;
            break;
          }
        }
        if (base_idx == -1) {
          base_idx = chosen_idx; // fallback
        }
        //Create the artifact instance
        ItemInstance art;
        memset(&art, 0, sizeof(ItemInstance));
        art.template_idx = base_idx;
        art.durability = (item_database[base_idx].max_durability > 0)
                         ? item_database[base_idx].max_durability : 100;
        art.stack_count = 1;
        art.is_identified = true; //the artefacts are immediately recognisable
        art.is_artifact = true;
        strncpy(art.artifact_name, artifact_registry[ai].name,
                sizeof(art.artifact_name) - 1);
        art.to_hit_bonus = artifact_registry[ai].to_hit;
        art.to_dam_bonus = artifact_registry[ai].to_dam;
        art.ac_bonus = artifact_registry[ai].ac_bonus;
        art.element = artifact_registry[ai].element;
        art.blessing = artifact_registry[ai].blessing;
        art.artifact_str_bonus = artifact_registry[ai].str_bonus;
        art.artifact_dex_bonus = artifact_registry[ai].dex_bonus;
        art.artifact_con_bonus = artifact_registry[ai].con_bonus;
        art.quality = QUALITY_MASTERWORK;
        //Mark as dropped and save immediately (it's a global event!)
        artifact_registry[ai].already_dropped = true;
        artifacts_save();
        c->backpack[c->backpack_count++] = art;
        char abuf[160];
        get_full_item_name(&art, abuf, sizeof(abuf));
        send_text_to_client(
            c->sock,
            "[*** ARTIFACT ***] %s has surrendered the legendary: %s"
            "-- FIRST AND ONLY IN THE WORLD!",
            killer->template->name, abuf);
        return; //artifact drop: does not continue with normal drop
      }
    }
  }

  c->backpack[c->backpack_count++] = drop;

  char buf[128];
  get_full_item_name(&drop, buf, sizeof(buf));
  send_text_to_client(c->sock, "[LOOT] %s dropped: %s [%s]",
                      killer->template->name,
                      buf,
                      rarity_name[rarity_tier]);
}

float get_current_weight(Client *c) {
  float w = (float)c->gold * 0.01f;
  ItemInstance *s[] = {&c->slot_head,  &c->slot_neck,   &c->slot_body,
                       &c->slot_back,  &c->slot_hand_r, &c->slot_hand_l,
                       &c->slot_hands, &c->slot_arm_r,  &c->slot_arm_l,
                       &c->slot_feet};
  for (int i = 0; i < 10; i++)
    if (s[i]->template_idx != -1)
      w += item_database[s[i]->template_idx].weight;
  for (int i = 0; i < 10; i++)
    if (c->slot_rings[i].template_idx != -1)
      w += item_database[c->slot_rings[i].template_idx].weight;
  for (int i = 0; i < MAX_BELT; i++)
    if (c->belt[i].template_idx != -1)
      w += item_database[c->belt[i].template_idx].weight *
           (float)c->belt[i].stack_count;
  for (int i = 0; i < c->backpack_count; i++)
    if (c->backpack[i].template_idx >= 0)
      w += item_database[c->backpack[i].template_idx].weight *
           (float)c->backpack[i].stack_count;
  return w;
}

static float get_movement_cooldown(Client *c) {
  float w = get_current_weight(c);
  float limit = (float)c->str * 5.0f;
  if (w > limit * 2.0f) {
    return 1.0f;
  }
  if (w > limit) {
    return 0.4f;
  }
  return 0.2f;
}

static int get_equipped_mask(Client *c) {
  int mask = 0;
  /*bits 0-9: single slots (head, neck, body, back, hand_r, hand_l,
   * hands, arm_r, arm_l, feet)
   * bits 10-19: slot_rings[0..9] (not overlapping with the previous ones)*/
  if (c->slot_head.template_idx != -1)
    mask |= (1 << 0);
  if (c->slot_neck.template_idx != -1)
    mask |= (1 << 1);
  if (c->slot_body.template_idx != -1)
    mask |= (1 << 2);
  if (c->slot_back.template_idx != -1)
    mask |= (1 << 3);
  if (c->slot_hand_r.template_idx != -1)
    mask |= (1 << 4);
  if (c->slot_hand_l.template_idx != -1)
    mask |= (1 << 5);
  if (c->slot_hands.template_idx != -1)
    mask |= (1 << 6);
  if (c->slot_arm_r.template_idx != -1)
    mask |= (1 << 7);
  if (c->slot_arm_l.template_idx != -1)
    mask |= (1 << 8);
  if (c->slot_feet.template_idx != -1)
    mask |= (1 << 9);
  /*Rings: bits 10..19 (no overlap with base slot)*/
  for (int i = 0; i < 10; i++)
    if (c->slot_rings[i].template_idx != -1)
      mask |= (1 << (10 + i));
  return mask;
}

int get_vision_radius(Client *c) {
  if (c->floor_id == 0)
    return 50; //In the open city you can see very far

  // --- BLINDNESS CONDITION ---
  if (rules_has_condition(c->effects, c->effect_count, "Blinded")) {
    return 0; //If blinded, you see nothing (radius 0)
  }

  int best_radius = 4; //Minimal vision in the dark (very limited)

  //Check belt and hands for a light source (Torches, Lanterns)
  ItemInstance *slots[] = {&c->slot_hand_r, &c->slot_hand_l, &c->belt[0],
                           &c->belt[1],     &c->belt[2],     &c->belt[3]};
  for (int i = 0; i < 6; i++) {
    if (slots[i]->template_idx != -1) {
      const ItemTemplate *it = &item_database[slots[i]->template_idx];
      if (it->category == ITEM_LIGHT_SOURCE && slots[i]->durability > 0) {
        if (it->light_radius > best_radius) {
          best_radius = it->light_radius;
        }
      }
    }
  }
  return best_radius;
}

int get_player_ac(Client *c) {
  int ts, td, tc, ti, tw, th;
  get_total_stats(c, &ts, &td, &tc, &ti, &tw, &th);
  int ac_b = (c->slot_body.template_idx != -1)
                 ? item_database[c->slot_body.template_idx].ac_base
                 : 0;
  int bonus = 0;
  if (c->slot_head.template_idx != -1)
    bonus += item_database[c->slot_head.template_idx].ac_bonus;
  if (c->slot_hand_r.template_idx != -1 &&
      item_database[c->slot_hand_r.template_idx].category == ITEM_SHIELD)
    bonus += item_database[c->slot_hand_r.template_idx].ac_bonus;
  if (c->slot_hand_l.template_idx != -1 &&
      item_database[c->slot_hand_l.template_idx].category == ITEM_SHIELD)
    bonus += item_database[c->slot_hand_l.template_idx].ac_bonus;
  if (c->slot_arm_r.template_idx != -1)
    bonus += item_database[c->slot_arm_r.template_idx].ac_bonus;
  if (c->slot_arm_l.template_idx != -1)
    bonus += item_database[c->slot_arm_l.template_idx].ac_bonus;
  int final_ac = (ac_b > 0 ? ac_b : 10 + rules_get_modifier(td)) + bonus;
  if (rules_has_condition(c->effects, c->effect_count, "Petrified")) {
    final_ac += 10;
  }
  return final_ac;
}

void send_detailed_state(Client *c) {
  MsgHeader h = {MSG_STATE, sizeof(MsgState)};
  MsgState s;
  memset(&s, 0, sizeof(s));
  s.entity_id = c->entity_id;
  s.x = c->x;
  s.y = c->y;
  strncpy(s.username, c->username, 31);
  s.hp = c->hp;
  s.max_hp = c->max_hp;
  s.floor_id = c->floor_id;
  get_game_time(&s.game_hour, &s.game_min);
  s.total_turns = global_total_turns;
  s.str = c->str;
  s.dex = c->dex;
  s.con = c->con;
  s.intel = c->intel;
  s.wis = c->wis;
  s.cha = c->cha;
  s.movement_cooldown = get_movement_cooldown(c);
  s.equipped_mask = get_equipped_mask(c);
  s.level = c->level;
  s.xp = c->xp;
  s.gold = c->gold;
  s.vision_radius = get_vision_radius(c);
  s.ac = get_player_ac(c);
  // Weapon name
  if (c->slot_hand_r.template_idx != -1) {
    strncpy(s.weapon_name, item_database[c->slot_hand_r.template_idx].name, 31);
  } else if (c->slot_hand_l.template_idx != -1) {
    strncpy(s.weapon_name, item_database[c->slot_hand_l.template_idx].name, 31);
  } else {
    strncpy(s.weapon_name, "Bare Hands", 31);
  }
  // Armor name
  if (c->slot_body.template_idx != -1) {
    strncpy(s.armor_name, item_database[c->slot_body.template_idx].name, 31);
  } else {
    strncpy(s.armor_name, "None", 31);
  }
  // Combat bonuses
  int ts2, td2, tc2, ti2, tw2, th2;
  get_total_stats(c, &ts2, &td2, &tc2, &ti2, &tw2, &th2);
  s.to_dmg = rules_get_modifier(ts2);
  {
    const ItemTemplate *w2 = NULL;
    if (c->slot_hand_r.template_idx != -1)
      w2 = &item_database[c->slot_hand_r.template_idx];
    else if (c->slot_hand_l.template_idx != -1)
      w2 = &item_database[c->slot_hand_l.template_idx];
    int a_mod = (w2 && w2->damage_dice_sides <= 6) ? rules_get_modifier(td2)
                                                   : rules_get_modifier(ts2);
    s.to_hit = (w2 ? w2->attack_bonus : 0) + a_mod + (c->level / 2);
  }

  //--- POPULATION OF STATUS ICONS (BITMASK) ---
  s.status_icons = 0;
  if (rules_has_condition(c->effects, c->effect_count, "Poisoned"))
    s.status_icons |= (1 << 0);
  if (rules_has_condition(c->effects, c->effect_count, "Blinded"))
    s.status_icons |= (1 << 1);
  if (rules_has_condition(c->effects, c->effect_count, "Paralyzed"))
    s.status_icons |= (1 << 2);
  if (rules_has_condition(c->effects, c->effect_count, "Stunned"))
    s.status_icons |= (1 << 3);
  if (rules_has_condition(c->effects, c->effect_count, "Unconscious"))
    s.status_icons |= (1 << 4);
  if (rules_has_condition(c->effects, c->effect_count, "Burning"))
    s.status_icons |= (1 << 5);
  if (rules_has_condition(c->effects, c->effect_count, "Bleeding"))
    s.status_icons |= (1 << 6);
  if (rules_has_condition(c->effects, c->effect_count, "Petrified"))
    s.status_icons |= (1 << 7);
  if (rules_has_condition(c->effects, c->effect_count, "Cursed"))
    s.status_icons |= (1 << 8);
  if (rules_has_condition(c->effects, c->effect_count, "Frozen"))
    s.status_icons |= (1 << 9);
  if (c->exhaustion_level > 0)
    s.status_icons |= (1 << 10);
  if (c->needs_study)
    s.status_icons |= (1 << 11);
  
  s.bosses_defeated = c->bosses_defeated;
  s.hunger_level = c->hunger_level;
  for (int i = 0; i < 10; i++) {
      s.spell_slots[i] = c->spell_slots[i];
      s.spell_slots_max[i] = c->spell_slots_max[i];
  }

  /* Slot equipment names */
  #define SLOT_NAME(slot_field, dest) \
    do { \
      if ((slot_field).template_idx >= 0 && (slot_field).template_idx < item_database_size) { \
          if ((slot_field).is_artifact) \
              strncpy(dest, (slot_field).artifact_name, 31); \
          else \
              strncpy(dest, item_database[(slot_field).template_idx].name, 31); \
          dest[31] = '\0'; \
      } else { \
          dest[0] = '\0'; \
      } \
    } while(0)

  SLOT_NAME(c->slot_head,   s.eq_head);
  SLOT_NAME(c->slot_neck,   s.eq_neck);
  SLOT_NAME(c->slot_body,   s.eq_body);
  SLOT_NAME(c->slot_back,   s.eq_back);
  SLOT_NAME(c->slot_hand_r, s.eq_hand_r);
  SLOT_NAME(c->slot_hand_l, s.eq_hand_l);
  SLOT_NAME(c->slot_hands,  s.eq_hands);
  SLOT_NAME(c->slot_arm_r,  s.eq_arm_r);
  SLOT_NAME(c->slot_arm_l,  s.eq_arm_l);
  SLOT_NAME(c->slot_feet,   s.eq_feet);
  for (int i = 0; i < 10; i++) {
      SLOT_NAME(c->slot_rings[i], s.eq_ring[i]);
  }
  for (int i = 0; i < 4; i++) {
      SLOT_NAME(c->belt[i], s.eq_belt[i]);
  }
  #undef SLOT_NAME

  net_send(c->sock, &h, sizeof(h));
  net_send(c->sock, &s, sizeof(s));
}

void save_player_data(Client *c) {
  if (!c || !c->authenticated)
    return;
  //Create the saves/ directory if it does not exist
  if (system("mkdir -p saves") == -1) {}
  char p[64];
  snprintf(p, 64, "saves/%s.save", c->username);
  FILE *f = fopen(p, "wb");
  if (!f)
    return;
  SaveData sd;
  memset(&sd, 0, sizeof(sd));
  /*Save the password hash instead of plain text*/
  hash_password(c->password, sd.password, sizeof(sd.password));
  sd.x = c->x;
  sd.y = c->y;
  sd.floor_id = c->floor_id;
  sd.race_id = c->race_id;
  sd.subrace_id = c->subrace_id;
  sd.class_id = c->class_id;
  sd.level = c->level;
  sd.xp = c->xp;
  sd.str = c->str;
  sd.dex = c->dex;
  sd.con = c->con;
  sd.intel = c->intel;
  sd.wis = c->wis;
  sd.cha = c->cha;
  sd.gold = c->gold;
  sd.hp = c->hp;
  sd.max_hp = c->max_hp;
  sd.alignment = c->alignment;
  memcpy(sd.backpack, c->backpack, sizeof(c->backpack));
  sd.backpack_count = c->backpack_count;
  memcpy(sd.belt, c->belt, sizeof(c->belt));
  sd.s_head = c->slot_head;
  sd.s_neck = c->slot_neck;
  sd.s_body = c->slot_body;
  sd.s_back = c->slot_back;
  sd.s_hand_r = c->slot_hand_r;
  sd.s_hand_l = c->slot_hand_l;
  sd.s_hands = c->slot_hands;
  sd.s_arm_r = c->slot_arm_r;
  sd.s_arm_l = c->slot_arm_l;
  sd.s_feet = c->slot_feet;
  memcpy(sd.s_rings, c->slot_rings, sizeof(c->slot_rings));
  memcpy(sd.spell_slots, c->spell_slots, sizeof(c->spell_slots));
  memcpy(sd.spell_slots_max, c->spell_slots_max, sizeof(c->spell_slots_max));
  /*--- Phase 3: Active Effects, Resources and Statistics ---*/
  memcpy(sd.effects, c->effects, sizeof(ActiveEffect) * c->effect_count);
  sd.effect_count     = c->effect_count;
  sd.light_turns_left = c->light_turns_left;
  sd.hunger_level     = c->hunger_level;
  sd.exhaustion_level = c->exhaustion_level;
  sd.total_kills      = c->total_kills;
  sd.total_steps      = c->total_steps;
  sd.max_floor_explored = c->max_floor_explored;
  /* --- Fase 5: Grimorio persistente --- */
  memcpy(sd.known_spells, c->known_spells, sizeof(c->known_spells));
  /*--- Phase 6: Boss flags ---*/
  sd.bosses_defeated = c->bosses_defeated;
  sd.unspent_stat_points = c->unspent_stat_points;
  /*--- Messaging: block list ---*/
  memcpy(sd.blocked_players, c->blocked_players,
         sizeof(c->blocked_players));
  sd.blocked_count = c->blocked_count;
  fwrite(&sd, sizeof(sd), 1, f);
  fclose(f);
  server_log("SAVE", "Saved: %s", c->username);
}

//Returns 1 if loaded, 0 if not exists, -1 if password incorrect
int load_player_data(Client *c) {
  if (!c)
    return 0;
  char p[64];
  snprintf(p, 64, "saves/%s.save", c->username);
  FILE *f = fopen(p, "rb");
  if (!f)
    return 0; //New character
  SaveData sd;
  memset(&sd, 0, sizeof(SaveData));
  if (fread(&sd, 1, sizeof(sd), f) == 0) {
    fclose(f);
    return 0;
  }
  fclose(f);
  /*Verify Password: Compare saved hash with hash of entered password.
   * Fallback for old saves: if hash comparison fails, try direct comparison
   * (clear password) and if so, update the save with the hash.*/
  char expected_hash[12];
  hash_password(c->password, expected_hash, sizeof(expected_hash));
  bool pwd_ok = (strncmp(sd.password, expected_hash, 8) == 0);
  if (!pwd_ok) {
    /*Legacy attempt: File may contain plaintext password*/
    pwd_ok = (strncmp(sd.password, c->password, 31) == 0);
    if (pwd_ok) {
      server_log("AUTH", "Password hash migration for: %s", c->username);
    }
  }
  if (!pwd_ok) {
    server_log("AUTH", "Incorrect password for: %s", c->username);
    return -1; //Incorrect password
  }
  //Restore state
  c->x = sd.x;
  c->y = sd.y;
  if (c->x >= MAP_WIDTH)
    c->x = MAP_CENTER_X;
  if (c->y >= MAP_HEIGHT)
    c->y = MAP_CENTER_Y;
  c->floor_id = sd.floor_id;
  c->race_id = sd.race_id;
  c->subrace_id = sd.subrace_id;
  c->class_id = sd.class_id;
  c->level = sd.level;
  c->xp = sd.xp;
  c->str = sd.str;
  c->dex = sd.dex;
  c->con = sd.con;
  c->intel = sd.intel;
  c->wis = sd.wis;
  c->cha = sd.cha;
  c->gold = sd.gold;
  c->hp = sd.hp;
  c->max_hp = sd.max_hp;
  c->alignment = sd.alignment;
  memcpy(c->backpack, sd.backpack, sizeof(sd.backpack));
  c->backpack_count = sd.backpack_count;
  memcpy(c->belt, sd.belt, sizeof(sd.belt));
  c->slot_head = sd.s_head;
  c->slot_neck = sd.s_neck;
  c->slot_body = sd.s_body;
  c->slot_back = sd.s_back;
  c->slot_hand_r = sd.s_hand_r;
  c->slot_hand_l = sd.s_hand_l;
  c->slot_hands = sd.s_hands;
  c->slot_arm_r = sd.s_arm_r;
  c->slot_arm_l = sd.s_arm_l;
  c->slot_feet = sd.s_feet;
  memcpy(c->slot_rings, sd.s_rings, sizeof(sd.s_rings));
  memcpy(c->spell_slots, sd.spell_slots, sizeof(sd.spell_slots));
  memcpy(c->spell_slots_max, sd.spell_slots_max, sizeof(sd.spell_slots_max));
  /*+20 Slot Bonus: Applied to all non-zero slots of the loaded character*/
  for (int _s = 1; _s <= MAX_SPELL_LEVEL; _s++) {
    if (c->spell_slots_max[_s] > 0) {
      c->spell_slots_max[_s] += 20;
      c->spell_slots[_s]     += 20;
    }
  }
  /*--- Step 3: Restore active effects, resources and statistics ---*/
  if (sd.effect_count > 0 && sd.effect_count <= MAX_EFFECTS_PER_ENTITY) {
    memcpy(c->effects, sd.effects, sizeof(ActiveEffect) * sd.effect_count);
    c->effect_count = sd.effect_count;
  } else {
    c->effect_count = 0;
  }
  c->light_turns_left = sd.light_turns_left;
  c->hunger_level     = sd.hunger_level;
  c->exhaustion_level = sd.exhaustion_level;
  c->total_kills      = sd.total_kills;
  c->total_steps      = sd.total_steps;
  /* --- Innate transit magic: restore deepest floor ever reached.
   * Old saves (smaller files) leave the field at zero, which is correct:
   * the character has no recorded descent yet. */
  c->max_floor_explored = sd.max_floor_explored;
  if (c->floor_id > c->max_floor_explored)
    c->max_floor_explored = c->floor_id;
  /* --- Fase 5: Ripristina grimorio --- */
  memcpy(c->known_spells, sd.known_spells, sizeof(sd.known_spells));
  /* Innate class magic is always known, even on saves created before the
   * spell existed: force-set the grimoire bits for this character's class. */
  {
    uint32_t cls_bit = (1u << (int)c->class_id);
    for (int si = 0; si < spell_database_size; si++) {
      if (!spell_database[si].innate)
        continue;
      if (!(spell_database[si].class_mask & cls_bit))
        continue;
      int w = si / 64;
      int b = si % 64;
      c->known_spells[w] |= (1ULL << b);
    }
  }
  /*--- Step 6: Reset boss flags ---*/
  c->bosses_defeated = sd.bosses_defeated;
  c->unspent_stat_points = sd.unspent_stat_points;
  /*--- Messaging: Reset block list ---
* Older (smaller) saves leave the field at zero,
* therefore the list is correctly empty.*/
  if (sd.blocked_count > 0 && sd.blocked_count <= MAX_BLOCKED_PLAYERS) {
    memcpy(c->blocked_players, sd.blocked_players,
           sizeof(sd.blocked_players));
    c->blocked_count = sd.blocked_count;
  } else {
    c->blocked_count = 0;
  }
  server_log("SAVE", "Loaded: %s (floor %d, HP %d/%d, gold %lu, effects %d)",
             c->username, c->floor_id, c->hp, c->max_hp,
             (unsigned long)c->gold, c->effect_count);
  return 1;
}

void damage_item(ItemInstance *it, int amt) {
  if (it->template_idx == -1 ||
      item_database[it->template_idx].max_durability == 0)
    return;
  it->durability -= amt;
  if (it->durability < 0)
    it->durability = 0;
}


void save_bones(Client* c) {
    if (c->floor_id <= 0) return;

    /*--- 1. Create the Tombstone BEFORE emptying your inventory ---*/
    tombstone_create(c);

    /*--- 2. Create in-world NPC ghost (live ghost) ---*/
    int ghost_slot = -1;
    if (g_npcs) {
        for (int i = 0; i < MAX_NPCS; i++) {
            if (!g_npcs[i].active) {
                ghost_slot = i;
                break;
            }
        }
    }

    if (ghost_slot >= 0) {
        NPC *g = &g_npcs[ghost_slot];
        memset(g, 0, sizeof(NPC));
        g->active       = true;
        g->is_ghost     = true;
        g->archetype    = ARCH_BOSS;
        g->entity_id    = next_id++;
        g->floor_id     = c->floor_id;
        g->x            = c->x;
        g->y            = c->y;
        g->spawn_x      = c->x;
        g->spawn_y      = c->y;
        g->respawn_timer = 0;
        g->hp           = 250;
        g->max_hp       = 250;
        g->template_idx = -1;
        g->template     = NULL;
        /*Look for a Ghost/Wraith/Spirit template in the bestiary*/
        extern int bestiary_size;
        extern MonsterTemplate *bestiary_data;
        for (int i = 0; i < bestiary_size; i++) {
            if (strcasestr(bestiary_data[i].name, "Ghost") ||
                strcasestr(bestiary_data[i].name, "Wraith") ||
                strcasestr(bestiary_data[i].name, "Spirit")) {
                g->template     = &bestiary_data[i];
                g->template_idx = i;
                break;
            }
        }
        if (!g->template && bestiary_size > 0) {
            g->template     = &bestiary_data[0];
            g->template_idx = 0;
        }
        snprintf(g->custom_name, sizeof(g->custom_name), "Ghost of %s", c->username);
        g->gold_drop = 0; /*The gold is in the tombstone, not in the ghost*/
        /*Register in the entity_grid*/
        master_world->floors[c->floor_id].entity_grid[c->y][c->x] = g->entity_id;
        extern void ai_init_npc(NPC *n, const char *name, int floor_id);
        ai_init_npc(g, g->custom_name, g->floor_id);
        server_log("BONES", "Ghost of %s spawned on floor %d (%d,%d)",
                   c->username, c->floor_id, c->x, c->y);
    }

    /*--- 3. Empty the player's inventory ---*/
    c->gold = 0;
    for (int i = 0; i < MAX_BACKPACK; i++) {
        c->backpack[i].template_idx = -1;
        c->backpack[i].stack_count  = 0;
    }
    c->backpack_count         = 0;
    for (int i = 0; i < MAX_BELT; i++) {
        c->belt[i].template_idx = -1;
        c->belt[i].stack_count  = 0;
    }
    c->slot_hand_r.template_idx = -1;
    c->slot_body.template_idx   = -1;
    c->slot_head.template_idx   = -1;
    c->slot_hand_l.template_idx = -1;
    c->slot_neck.template_idx   = -1;
    c->slot_back.template_idx   = -1;
    c->slot_hands.template_idx  = -1;
    c->slot_arm_r.template_idx  = -1;
    c->slot_arm_l.template_idx  = -1;
    c->slot_feet.template_idx   = -1;
    for (int i = 0; i < 10; i++) {
        c->slot_rings[i].template_idx = -1;
    }
}






/* Innate transit magic: remember the deepest floor this character has ever
* stood on. Called every time a client's floor_id changes.*/
void client_track_explored_floor(Client *c) {
  if (c && c->floor_id > c->max_floor_explored)
    c->max_floor_explored = c->floor_id;
}

void check_tile_events(Client *c, NPC *npcs) {
  VoxelType vt = master_world->floors[c->floor_id].map.data[0][c->y][c->x];
  // --- EVENTI AMBIENTALI ---
  if (vt == VOXEL_WATER) { // Ad esempio ghiaccio o fango
    if (c->effect_count < MAX_EFFECTS_PER_ENTITY) {
      c->effects[c->effect_count] = (ActiveEffect){
          "Prone", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 1, false};
      c->effect_count++;
      send_text_to_client(
          c->sock, "[DANGER] The ground is slippery! You fell to the ground!");
    }
  } else if (vt == VOXEL_STAIRS_DOWN) {
    if (c->floor_id > 0 && c->floor_id % 10 == 0) {
      int boss_index = (c->floor_id / 10) - 1;
      if (!(c->bosses_defeated & (1u << boss_index))) {
        bool boss_alive = false;
        if (g_npcs) {
          for (int i = 0; i < MAX_NPCS; i++) {
            if (g_npcs[i].active && g_npcs[i].floor_id == c->floor_id && g_npcs[i].archetype == ARCH_BOSS) {
              boss_alive = true;
              break;
            }
          }
        }

        if (boss_alive) {
          send_text_to_client(c->sock, "[SYSTEM] The Boss' energy seals the stairs. Defeat him before proceeding!");
        } else {
          send_text_to_client(c->sock, "[SYSTEM] Only those who have proven their worth by defeating the Boss can go down. Wait for his return and face him!");
        }
        return;
      }
    }
    master_world->floors[c->floor_id].entity_grid[c->y][c->x] = 0;
    int old_floor = c->floor_id; c->floor_id++; notify_player_left_floor(c, old_floor);
    // Global search for the nearest up-staircase (or the first one found)
    bool found = false;
    int best_x = MAP_CENTER_X, best_y = MAP_CENTER_Y;
    float min_dist = 999999.0f;

    for (int iy = 0; iy < MAP_HEIGHT; iy++) {
      for (int ix = 0; ix < MAP_WIDTH; ix++) {
        if (master_world->floors[c->floor_id].map.data[0][iy][ix] ==
            VOXEL_STAIRS_UP) {
          float d = sqrtf(powf(ix - c->x, 2) + powf(iy - c->y, 2));
          if (d < min_dist) {
            min_dist = d;
            best_x = ix;
            best_y = iy;
            found = true;
          }
        }
      }
    }
    if (found) {
      c->x = best_x;
      c->y = best_y;
      master_world->floors[c->floor_id].entity_grid[best_y][best_x] = c->entity_id;
    }
    client_track_explored_floor(c);
    /*Arrival broadcast AFTER the landing position is set, so clients on
    * the new floor see the player where he actually is*/
    broadcast_player_state(c);
    send_detailed_state(c);
    send_map_chunk(c->sock, &master_world->floors[c->floor_id].map, c->x, c->y,
                   INITIAL_VIEW_RADIUS);
    broadcast_nearby_entities(c, npcs);
    save_player_data(c);
  } else if (vt == VOXEL_STAIRS_UP && c->floor_id > 0) {
    master_world->floors[c->floor_id].entity_grid[c->y][c->x] = 0;
    int old_floor = c->floor_id; c->floor_id--; notify_player_left_floor(c, old_floor);
    bool found = false;
    int best_x = MAP_CENTER_X, best_y = MAP_CENTER_Y;
    float min_dist = 999999.0f;

    for (int iy = 0; iy < MAP_HEIGHT; iy++) {
      for (int ix = 0; ix < MAP_WIDTH; ix++) {
        if (master_world->floors[c->floor_id].map.data[0][iy][ix] ==
            VOXEL_STAIRS_DOWN) {
          float d = sqrtf(powf(ix - c->x, 2) + powf(iy - c->y, 2));
          if (d < min_dist) {
            min_dist = d;
            best_x = ix;
            best_y = iy;
            found = true;
          }
        }
      }
    }
    if (found) {
      c->x = best_x;
      c->y = best_y;
      master_world->floors[c->floor_id].entity_grid[best_y][best_x] = c->entity_id;
    }
    /*Arrival broadcast AFTER the landing position is set, so clients on
    * the new floor see the player where he actually is*/
    broadcast_player_state(c);
    send_detailed_state(c);
    send_map_chunk(c->sock, &master_world->floors[c->floor_id].map, c->x, c->y,
                   INITIAL_VIEW_RADIUS);
    broadcast_nearby_entities(c, npcs);
    save_player_data(c);
  }
}

int get_xp_threshold(int level) {
  const int thresholds[] = {0,      300,    900,    2700,   6500,
                            14000,  23000,  34000,  48000,  64000,
                            85000,  100000, 120000, 140000, 165000,
                            195000, 225000, 265000, 305000, 355000};
  if (level < 1)
    return 0;
  if (level > 20)
    return 355000;
  return thresholds[level - 1];
}

void check_level_up(Client *c) {
  if (c->level >= 20)
    return;
  int next_threshold = get_xp_threshold(c->level + 1);
  bool leveled = false;

  while (c->xp >= next_threshold && c->level < 20) {
    c->level++;
    leveled = true;

    // d8 class hit dice approximation
    int hp_gain = rules_roll_dice(1, 8) + rules_get_modifier(c->con);
    if (hp_gain < 1)
      hp_gain = 1;
    c->max_hp += hp_gain;
    c->hp = c->max_hp; // Heal to full on level up

    //Magic/Ki: Unlock new spell slots based on class
    int highest_spell_lvl = 1;
    switch (c->class_id) {
    case CLASS_PALADIN:
    case CLASS_RANGER:
      highest_spell_lvl = c->level / 2;
      if (highest_spell_lvl > 5) highest_spell_lvl = 5;
      break;
    case CLASS_FIGHTER:
    case CLASS_MONK:
    case CLASS_BARBARIAN:
    case CLASS_ROGUE:
      /* Martial arts: all four classes unlock a new Ki tier every
       * 3 character levels, up to L6, so the six tomes of every
       * martial codex (sold at The Archive of a Thousand Battles)
       * are learnable AND castable. */
      highest_spell_lvl = (c->level + 2) / 3;
      if (highest_spell_lvl > 6) highest_spell_lvl = 6;
      break;
    default:
      highest_spell_lvl = (c->level + 1) / 2;
      if (highest_spell_lvl > MAX_SPELL_LEVEL)
        highest_spell_lvl = MAX_SPELL_LEVEL;
      break;
    }
    if (highest_spell_lvl < 1) highest_spell_lvl = 1;
    c->spell_slots_max[highest_spell_lvl]++;

    //Reset all slots
    for (int i = 1; i <= MAX_SPELL_LEVEL; i++) {
      c->spell_slots[i] = c->spell_slots_max[i];
    }

    c->unspent_stat_points += 3;

    send_text_to_client(c->sock, "[LEVEL] *** YOU HAVE RISE TO LEVEL %d! ***",
                        c->level);
    send_text_to_client(c->sock,
                        "[LEVEL] Maximum HP increased by %d (Total: %d)."
                        "Health and mana restored.",
                        hp_gain, c->max_hp);
    send_text_to_client(c->sock, "[LEVEL] You got 3 stat points! Use 'train <stat>' to assign them (e.g. 'train str') or 'train random'.");

    if (c->class_id == CLASS_BARD || c->class_id == CLASS_CLERIC ||
        c->class_id == CLASS_DRUID || c->class_id == CLASS_PALADIN ||
        c->class_id == CLASS_RANGER || c->class_id == CLASS_SORCERER ||
        c->class_id == CLASS_WARLOCK || c->class_id == CLASS_WIZARD) {
        c->needs_study = true;
        send_text_to_client(c->sock, "[MAGIC] Your horizons expand. Head to the Temple to study your books and learn new spells!");
    } else if (c->class_id == CLASS_BARBARIAN || c->class_id == CLASS_FIGHTER ||
               c->class_id == CLASS_MONK || c->class_id == CLASS_ROGUE) {
        c->needs_study = true;
        send_text_to_client(c->sock, "[KI] You have learned new techniques. Go to your Guild/Temple to study the manuals and master them!");
    }

    next_threshold = get_xp_threshold(c->level + 1);
  }

  if (leveled) {
    server_log("LEVELUP", "Player %s has reached level %d", c->username,
               c->level);
    save_player_data(c);
  }
}

void print_merchant_inventory(Client *c, NPC *merchant) {
  send_text_to_client(c->sock, ".----------------------------------------------"
                               "----------------------------.");
  char title[128];
  snprintf(title, sizeof(title), "=== %s ===", merchant->merchant.shop_name);
  int pad = (74 - strlen(title)) / 2;
  send_text_to_client(c->sock, "|%*s%s%*s|", pad, "", title,
                      74 - pad - (int)strlen(title), "");
  send_text_to_client(c->sock, "|----------------------------------------------"
                               "----------------------------|");
  send_text_to_client(c->sock, "| ID | %-24s | %-10s | %-15s | %-9s |", "ITEM",
                      "TYPE", "STATISTICS", "COST");
  send_text_to_client(c->sock, "|----|--------------------------|------------|-"
                               "----------------|-----------|");
  for (int j = 0; j < merchant->merchant.item_count; j++) {
    ItemTemplate *it = &item_database[merchant->merchant.item_templates[j]];
    char stat_info[32] = "-";
    char type_info[16] = "Misc";

    if (it->category == ITEM_WEAPON) {
      strcpy(type_info, "Weapon");
      snprintf(stat_info, sizeof(stat_info), "%dd%d dmg",
               it->damage_dice_count, it->damage_dice_sides);
    } else if (it->category == ITEM_ARMOR) {
      strcpy(type_info, "Armor");
      snprintf(stat_info, sizeof(stat_info), "AC %d", it->ac_base);
    } else if (it->category == ITEM_SHIELD) {
      strcpy(type_info, "Shield");
      snprintf(stat_info, sizeof(stat_info), "+%d AC", it->ac_bonus);
    } else if (it->category == ITEM_CONSUMABLE) {
      strcpy(type_info, "Consum.");
      if (it->heal_amount > 0)
        snprintf(stat_info, sizeof(stat_info), "Heal %d HP", it->heal_amount);
    } else if (it->category == ITEM_RING) {
      strcpy(type_info, "Ring");
    } else if (it->category == ITEM_NECK) {
      strcpy(type_info, "Amulet");
    } else if (it->category == ITEM_LIGHT_SOURCE) {
      strcpy(type_info, "Light");
      snprintf(stat_info, sizeof(stat_info), "%d turns",
               it->max_durability > 0 ? it->max_durability : 100);
    } else if (it->category == ITEM_FUEL) {
      strcpy(type_info, "Fuel");
      snprintf(stat_info, sizeof(stat_info), "%d turns",
               it->duration_turns > 0 ? it->duration_turns : 300);
    }

    if (it->str_bonus > 0 || it->dex_bonus > 0 || it->con_bonus > 0 ||
        it->int_bonus > 0 || it->wis_bonus > 0 || it->cha_bonus > 0) {
      if (strcmp(stat_info, "-") == 0)
        strcpy(stat_info, "Magic");
      else
        strcat(stat_info, " (*)");
    }

    /* --- Fase 3: Economia Reattiva (Prezzi Dinamici) --- */
    float supply = 1.0f;
    if (merchant->merchant.item_stock_max[j] > 0) {
      supply = (float)merchant->merchant.item_stock[j] / (float)merchant->merchant.item_stock_max[j];
    }
    //+50% price if empty stock, up to -50% if double stock
    float economy_modifier = 1.0f + (1.0f - supply) * 0.5f;
    if (economy_modifier < 0.5f) economy_modifier = 0.5f;
    if (economy_modifier > 2.0f) economy_modifier = 2.0f;
    uint64_t dynamic_cost = (uint64_t)(it->cost * economy_modifier);

    char cost_str[16];
    snprintf(cost_str, sizeof(cost_str), "%lu gp", (unsigned long)dynamic_cost);

    send_text_to_client(c->sock, "| %2d | %-24.24s | %-10s | %-15.15s | %9s |",
                        j + 1, it->name, type_info, stat_info, cost_str);
  }
  send_text_to_client(c->sock, "'----------------------------------------------"
                               "----------------------------'");
  send_text_to_client(
      c->sock, "[TIP] Use 'browse <n>' for info, 'buy <n>' to purchase.");
}

/* handle_text_cmd has been migrated to server_commands.c */

/*========================================================================================
 * The following functions have been migrated to src/server/server_spawn.c:
 * is_item_matching_spec(), fill_shop_by_specialization(), add_item_to_shop(),
 * get_item_idx_by_name(), fill_provisioner_floor0(),
 * spawn_city_merchants(), spawn_magic_shops(), spawn_martial_archive().
 * ========================================================================================*/

/*The functions give_item_by_name and give_starting_gear remain here because
 * are used by the character creation logic in main_server.c.*/

static int get_item_idx_by_name(const char *name) {
  for (int i = 0; i < item_database_size; i++) {
    if (strcasecmp(item_database[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static void give_item_by_name(Client *c, const char *name) {
  int idx = get_item_idx_by_name(name);
  if (idx == -1)
    return;
  if (c->backpack_count >= MAX_BACKPACK)
    return;

  ItemInstance it;
  memset(&it, 0, sizeof(ItemInstance));
  it.template_idx = idx;
  it.durability = (item_database[idx].category == ITEM_LIGHT_SOURCE && strcasestr(item_database[idx].name, "lantern")) ? 5000 : item_database[idx].max_durability;
  it.stack_count = 1;
  it.is_identified = true;
  it.quality = QUALITY_NORMAL;
  it.blessing = BLESS_NORMAL;
  it.element = ELEM_NONE;

  c->backpack[c->backpack_count++] = it;
}

void give_starting_gear(Client *c) {
  switch (c->class_id) {
  case CLASS_BARBARIAN:
    give_item_by_name(c, "Greataxe");
    give_item_by_name(c, "Way of Fury");
    break;
  case CLASS_WIZARD:
    give_item_by_name(c, "Arcane Focus, Staff");
    give_item_by_name(c, "Book of the Arcane Novice");
    break;
  case CLASS_SORCERER:
    give_item_by_name(c, "Arcane Focus, Staff");
    give_item_by_name(c, "Awakening of the Arcane Blood");
    break;
  case CLASS_WARLOCK:
    give_item_by_name(c, "Arcane Focus, Staff");
    give_item_by_name(c, "Initiate's Pact");
    break;
  case CLASS_CLERIC:
    give_item_by_name(c, "Mace");
    give_item_by_name(c, "Shield");
    give_item_by_name(c, "Cleric's Missal");
    break;
  case CLASS_DRUID:
    give_item_by_name(c, "Quarterstaff");
    give_item_by_name(c, "Whispers of the Forest");
    break;
  case CLASS_BARD:
    give_item_by_name(c, "Rapier");
    give_item_by_name(c, "Ballads of the Caster");
    break;
  case CLASS_PALADIN:
    give_item_by_name(c, "Longsword");
    give_item_by_name(c, "Shield");
    give_item_by_name(c, "Chain Mail");
    give_item_by_name(c, "Vow of the Neophyte");
    break;
  case CLASS_RANGER:
    give_item_by_name(c, "Longbow");
    give_item_by_name(c, "Tracks of Nature");
    break;
  case CLASS_ROGUE:
    give_item_by_name(c, "Shortsword");
    give_item_by_name(c, "Dagger");
    give_item_by_name(c, "Cutpurse's Notes");
    break;
  case CLASS_FIGHTER:
    give_item_by_name(c, "Greatsword");
    give_item_by_name(c, "Chain Mail");
    give_item_by_name(c, "Recruit's Manual");
    break;
  case CLASS_MONK:
    give_item_by_name(c, "Quarterstaff");
    give_item_by_name(c, "Scroll of the Lotus");
    break;
  default:
    give_item_by_name(c, "Dagger");
    break;
  }

  /*-------------------------------------------------------
   * Grimoire initialization: assign cantrips (level 0)
   * and the 1st level spells of your caster class.
   * Non-spellcasting characters gain an empty grimoire
   * (known_spells = {0}) and will not be able to use 'cast'.
   * -------------------------------------------------------*/
  {
    /*Non-enchanter classes: empty grimoire (already cleared by calloc)*/
    static const bool IS_CASTER[CLASS_COUNT] = {
      [CLASS_BARBARIAN] = true,
      [CLASS_BARD]      = true,
      [CLASS_CLERIC]    = true,
      [CLASS_DRUID]     = true,
      [CLASS_FIGHTER]   = true,
      [CLASS_MONK]      = true,
      [CLASS_PALADIN]   = true,
      [CLASS_RANGER]    = true,
      [CLASS_ROGUE]     = true,
      [CLASS_SORCERER]  = true,
      [CLASS_WARLOCK]   = true,
      [CLASS_WIZARD]    = true,
    };
    if (IS_CASTER[c->class_id]) {
      uint32_t cls_bit = (1u << (int)c->class_id);
      for (int si = 0; si < spell_database_size; si++) {
        /*Learns cantrip (level 0) and 1st level spells of your class*/
        if (spell_database[si].level > 1)
          continue;
        if (!(spell_database[si].class_mask & cls_bit))
          continue;
        int w = si / 64;
        int b = si % 64;
        c->known_spells[w] |= (1ULL << b);
      }
    }
  }
}


void broadcast_spell_vfx(int sx, int sy, int tx, int ty, int vfx_type, float r, float g, float b, int floor_id) {
    MsgHeader hdr;
    hdr.type = MSG_SPELL_VFX;
    hdr.length = sizeof(MsgSpellVFX);
    
    MsgSpellVFX msg;
    msg.start_x = sx; msg.start_y = sy;
    msg.target_x = tx; msg.target_y = ty;
    msg.vfx_type = vfx_type;
    msg.color_r = r; msg.color_g = g; msg.color_b = b;
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].authenticated && g_clients[i].floor_id == floor_id) {
            if (write(g_clients[i].sock, &hdr, sizeof(MsgHeader)) < 0) {}
            if (write(g_clients[i].sock, &msg, sizeof(MsgSpellVFX)) < 0) {}
        }
    }
}

void broadcast_nearby_entities(Client *c, NPC *npcs) {
  /*--- Active NPCs ---*/
  for (int i = 0; i < MAX_NPCS; i++) {
    if (npcs[i].active && npcs[i].floor_id == c->floor_id) {
      MsgHeader h = {MSG_STATE, sizeof(MsgState)};
      MsgState s;
      memset(&s, 0, sizeof(s));
      s.entity_id   = npcs[i].entity_id;
      s.x           = npcs[i].x;
      s.y           = npcs[i].y;
      s.hp          = npcs[i].hp;
      s.max_hp      = npcs[i].max_hp;
      s.floor_id    = npcs[i].floor_id;
      s.is_merchant = (npcs[i].archetype == ARCH_MERCHANT) ? 1 : 0;
      s.shop_spec = (npcs[i].archetype == ARCH_MERCHANT)
                        ? (int)npcs[i].merchant.spec
                        : SHOP_SPEC_NONE;
      s.is_tombstone = 0;
      s.is_player = 0;
      net_send(c->sock, &h, sizeof(h));
      net_send(c->sock, &s, sizeof(s));
    }
  }
  
  /*--- Active Tombstones on the same plane ---*/
  for (int i = 0; i < MAX_TOMBSTONES; i++) {
    if (!g_tombstones[i].active) continue;
    if (g_tombstones[i].floor_id != c->floor_id) continue;
    
    int tomb_id = -(i + 1);
    MsgHeader h = {MSG_STATE, sizeof(MsgState)};
    MsgState s;
    memset(&s, 0, sizeof(s));
    s.entity_id    = tomb_id;
    s.x            = g_tombstones[i].x;
    s.y            = g_tombstones[i].y;
    s.hp           = 1;
    s.max_hp       = 1;
    s.floor_id     = g_tombstones[i].floor_id;
    s.is_merchant  = 0;
    s.shop_spec    = SHOP_SPEC_NONE;
    s.is_tombstone = 1;
    s.is_player    = 0;
    net_send(c->sock, &h, sizeof(h));
    net_send(c->sock, &s, sizeof(s));
  }
  
  /*--- Active Players on the same plane ---*/
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].active && g_clients[i].authenticated && g_clients[i].floor_id == c->floor_id) {
      if (g_clients[i].entity_id == c->entity_id) continue;
      
      MsgHeader h = {MSG_STATE, sizeof(MsgState)};
      MsgState s;
      memset(&s, 0, sizeof(s));
      s.entity_id   = g_clients[i].entity_id;
      s.x           = g_clients[i].x;
      s.y           = g_clients[i].y;
      s.hp          = g_clients[i].hp;
      s.max_hp      = g_clients[i].max_hp;
      s.floor_id    = g_clients[i].floor_id;
      s.is_merchant = 0;
      s.shop_spec   = SHOP_SPEC_NONE;
      s.is_tombstone= 0;
      s.is_player   = 1;
      strncpy(s.username, g_clients[i].username, 31);
      net_send(c->sock, &h, sizeof(h));
      net_send(c->sock, &s, sizeof(s));
    }
  }
}




void notify_player_left_floor(Client *c, int old_floor) {
  MsgHeader h = {MSG_STATE, sizeof(MsgState)};
  MsgState s;
  memset(&s, 0, sizeof(s));
  s.entity_id   = c->entity_id;
  s.x           = c->x;
  s.y           = c->y;
  s.hp          = 0; // Client removes entities with hp <= 0
  s.max_hp      = c->max_hp;
  s.floor_id    = old_floor;
  s.is_merchant = 0;
  s.shop_spec   = SHOP_SPEC_NONE;
  s.is_tombstone= 0;
  s.is_player   = 1;
  strncpy(s.username, c->username, 31);
  
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].active && g_clients[i].authenticated && g_clients[i].floor_id == old_floor) {
      if (g_clients[i].entity_id == c->entity_id) continue;
      net_send(g_clients[i].sock, &h, sizeof(h));
      net_send(g_clients[i].sock, &s, sizeof(s));
    }
  }
}

void broadcast_player_state(Client *c) {
  MsgHeader h = {MSG_STATE, sizeof(MsgState)};
  MsgState s;
  memset(&s, 0, sizeof(s));
  s.entity_id   = c->entity_id;
  s.x           = c->x;
  s.y           = c->y;
  s.hp          = c->hp;
  s.max_hp      = c->max_hp;
  s.floor_id    = c->floor_id;
  s.is_merchant = 0;
  s.shop_spec   = SHOP_SPEC_NONE;
  s.is_tombstone= 0;
  s.is_player   = 1;
  strncpy(s.username, c->username, 31);
  
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (g_clients[i].active && g_clients[i].authenticated && g_clients[i].floor_id == c->floor_id) {
      if (g_clients[i].entity_id == c->entity_id) continue;
      net_send(g_clients[i].sock, &h, sizeof(h));
      net_send(g_clients[i].sock, &s, sizeof(s));
    }
  }
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF,
          0); // Disable stdout buffering for live log files
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      printf("Usage: %s [options]\n", argv[0]);
      printf("Options:\n");
      printf("  -p, --password <pwd>    Sets the server password "
             "(default: dragongl_secret)\n");
      printf("  -h, --help              Shows this help\n");
      return 0;
    }
    if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--password") == 0) &&
        i + 1 < argc) {
      strncpy(SERVER_ACCESS_PASSWORD, argv[i + 1], 63);
      i++;
    }
  }
  server_log("SYS", "Starting Dragon GL Server...");
  clog_init("combat_log.json");
  server_log("SYS", "Tactical log opened: combat_log.json");
  /* Ctrl-C / kill: request a graceful shutdown instead of the default
   * immediate termination (which would leave combat_log.json truncated).*/
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_shutdown_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  int s_sock = net_create_server(8080);
  if (s_sock < 0) {
    clog_close();
    return 1;
  }
  net_set_nonblocking(s_sock);
  struct pollfd fds[MAX_CLIENTS + 1];
  Client *clients = malloc(sizeof(Client) * MAX_CLIENTS);
  g_clients = clients;

  global_clients = clients;
  NPC *npcs = calloc(MAX_NPCS, sizeof(NPC));
  g_npcs = npcs;
  master_world = malloc(sizeof(World));
  if (!master_world) {
    server_log("SYS", "World memory allocation error");
    clog_close();
    return 1;
  }

  game_init();

  if (!init_data_loaders("data")) {
    server_log("SYS", "JSON Error");
    clog_close();
    return 1;
  }
  artifacts_load();
  server_log("SYS", "Unique Artifact System initialized.");

  tombstone_load_all();
  server_log("SYS", "Tombstone System initialized.");

  for (int i = 0; i < MAX_CLIENTS; i++)
    clients[i].active = false;
  for (int i = 0; i < MAX_NPCS; i++) {
    npcs[i].active = false;
    npcs[i].archetype = ARCH_MELEE;
    npcs[i].effect_count = 0;
    npcs[i].respawn_timer = -1;
  }

  if (world_load(master_world, "data/world.dat")) {
    server_log("SYS", "World loaded from data/world.dat");
    FILE *fn = fopen("data/npcs.dat", "rb");
    if (fn) {
      /*Try the new compact format (magic + count + used slots).
       * If the magic doesn't match, use the old legacy format.*/
      const uint32_t MAGIC = 0xDEAD7ECC;
      uint32_t file_magic = 0;
      if (fread(&file_magic, sizeof(uint32_t), 1, fn) != 1) {}
      if (file_magic == MAGIC) {
        /*New format: read only used slots*/
        int used = 0;
        if (fread(&used, sizeof(int), 1, fn) != 1) {}
        if (used < 0 || used > MAX_NPCS) {
          used = 0;
        }
        for (int ni = 0; ni < used; ni++) {
          NPC tmp;
          if (fread(&tmp, sizeof(NPC), 1, fn) == 1) {
            /*Find a free slot and insert*/
            for (int si = 0; si < MAX_NPCS; si++) {
              if (!npcs[si].active && npcs[si].template == NULL) {
                npcs[si] = tmp;
                break;
              }
            }
          }
        }
      } else {
        /*Legacy format: entire array, reread from beginning*/
        rewind(fn);
        size_t _r1 = fread(npcs, sizeof(NPC), MAX_NPCS, fn); (void)_r1;
      }
      if (fread(&next_id, sizeof(int), 1, fn) != 1) {}
      if (fread(&global_total_turns, sizeof(int), 1, fn) != 1) {}
      fclose(fn);
      server_log("SYS", "NPCs loaded from data/npcs.dat");
      for (int i = 0; i < MAX_NPCS; i++) {
        // Reset the only dangling pointer that cannot be serialized
        npcs[i].ai_ctx.behavior_tree_root = NULL;
        /*One-time repair: merchants created by older builds were spawned
         * without hp (calloc => hp=0). The client treats any entity with
         * hp<=0 as dead and never renders it, so shop keepers on floor 0
         * existed on the server but were invisible to players.*/
        if (npcs[i].active && npcs[i].archetype == ARCH_MERCHANT && npcs[i].hp <= 0) {
          npcs[i].hp = 1;
          if (npcs[i].max_hp <= 0) npcs[i].max_hp = 1;
          server_log("SYS", "Repaired merchant hp (entity_id %d, slot %d)",
                     npcs[i].entity_id, i);
        }
        if (npcs[i].active || npcs[i].respawn_timer >= 0) {
          if (npcs[i].archetype == ARCH_TREASURE || npcs[i].archetype == ARCH_GOLD) {
            npcs[i].template = NULL;
          } else {
            int tidx = npcs[i].template_idx;
            if (tidx < 0 || tidx >= bestiary_size)
              tidx = 0;
            npcs[i].template = &bestiary_data[tidx];
            // Reattach behavior tree WITHOUT resetting HP/stats
            ai_attach_behavior(&npcs[i]);
          }
        }
      }
    }
  } else {
    server_log("SYS", "Generating new world...");
    world_init(master_world);
    spawn_city_merchants(npcs, &next_id);
    spawn_magic_shops(npcs, &next_id);
    spawn_martial_archive(npcs, &next_id);
    populate_dungeons(npcs, &next_id);
    world_save(master_world, "data/world.dat");
    FILE *fn = fopen("data/npcs.dat", "wb");
    if (fn) {
      fwrite(npcs, sizeof(NPC), MAX_NPCS, fn);
      fwrite(&next_id, sizeof(int), 1, fn);
      fwrite(&global_total_turns, sizeof(int), 1, fn);
      fclose(fn);
    }
    server_log("SYS", "World generated and saved in data/.");
  }

  sync_entity_grid(npcs);
  floor_stats_rebuild(npcs);
  aoe_init_clouds();
  spell_router_init();
  server_log("SYS", "Server ready!");
  while (!g_shutdown_requested) {
    fds[0].fd = s_sock;
    fds[0].events = POLLIN;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      fds[i + 1].fd = clients[i].active ? clients[i].sock : -1;
      fds[i + 1].events = POLLIN;
    }
    if (poll(fds, MAX_CLIENTS + 1, 5) > 0) {
      if (fds[0].revents & POLLIN) {
        int cs = accept(s_sock, NULL, NULL);
        if (cs >= 0) {
          net_set_nonblocking(cs);
          for (int i = 0; i < MAX_CLIENTS; i++)
            if (!clients[i].active) {
              clients[i].sock = cs;
              clients[i].active = true;
              clients[i].authenticated = false;
              server_log("NET", "Client %d", i);
              break;
            }
        }
      }
      for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].active && (fds[i + 1].revents & POLLIN)) {
          MsgHeader hdr;
          if (net_receive(clients[i].sock, &hdr, sizeof(MsgHeader)) <= 0) {
            save_player_data(&clients[i]);
            notify_player_left_floor(&clients[i], clients[i].floor_id);
            net_close(clients[i].sock);
            clients[i].active = false;
          } else if (hdr.type == MSG_LOGIN) {
            MsgLogin ml;
            net_receive_all(clients[i].sock, &ml, sizeof(MsgLogin));
            
            //Check Server Password to allow connection
            if (strlen(SERVER_ACCESS_PASSWORD) > 0 && strcmp(ml.server_pass, SERVER_ACCESS_PASSWORD) != 0) {
                server_log("AUTH", "Server access denied (Wrong Server Password) for IP/Socket %d", clients[i].sock);
                MsgHeader fail_hdr;
                fail_hdr.type = MSG_AUTH_FAIL;
                fail_hdr.length = sizeof(MsgAuthFail);
                MsgAuthFail fail_msg;
                strcpy(fail_msg.reason, "Server Password errata.");
                net_send(clients[i].sock, &fail_hdr, sizeof(MsgHeader));
                net_send(clients[i].sock, &fail_msg, sizeof(MsgAuthFail));
                net_close(clients[i].sock);
                clients[i].active = false;
                continue;
            }

            //Initialize base fields
            clients[i].authenticated = true;
            clients[i].entity_id = next_id++;
            strncpy(clients[i].username, ml.username, 31);
            strncpy(clients[i].password, ml.password, 31);
            clients[i].is_dm = (strcmp(ml.username, "dm") == 0);
            clients[i].effect_count = 0;
            
            //Try loading an existing save
            int load_status = load_player_data(&clients[i]);
            if (load_status == -1) {
                //Personal Password incorrect for this character
                MsgHeader fail_hdr;
                fail_hdr.type = MSG_AUTH_FAIL;
                fail_hdr.length = sizeof(MsgAuthFail);
                MsgAuthFail fail_msg;
                strcpy(fail_msg.reason, "Incorrect character password.");
                net_send(clients[i].sock, &fail_hdr, sizeof(MsgHeader));
                net_send(clients[i].sock, &fail_msg, sizeof(MsgAuthFail));
                net_close(clients[i].sock);
                clients[i].active = false;
                continue;
            }
            if (load_status == 0) {
              //New character: uses the values ​​sent by the client (or default
              //if disabled)
              clients[i].str = ml.str > 0 ? ml.str : 10;
              clients[i].dex = ml.dex > 0 ? ml.dex : 10;
              clients[i].con = ml.con > 0 ? ml.con : 10;
              clients[i].intel = ml.intel > 0 ? ml.intel : 10;
              clients[i].wis = ml.wis > 0 ? ml.wis : 10;
              clients[i].cha = ml.cha > 0 ? ml.cha : 10;
              clients[i].hp = 20 + rules_get_modifier(clients[i].con);
              clients[i].max_hp = clients[i].hp;
              clients[i].floor_id = 0;
              clients[i].max_floor_explored = 0;
              clients[i].x = MAP_CENTER_X;
              clients[i].y = MAP_CENTER_Y;
              clients[i].gold = 1000;
              clients[i].level = 1;
              clients[i].xp = 0;
              clients[i].backpack_count = 0;
              clients[i].slot_head.template_idx = -1;
              clients[i].slot_neck.template_idx = -1;
              clients[i].slot_body.template_idx = -1;
              clients[i].slot_back.template_idx = -1;
              clients[i].slot_hand_r.template_idx = -1;
              clients[i].slot_hand_l.template_idx = -1;
              clients[i].slot_hands.template_idx = -1;
              clients[i].slot_arm_r.template_idx = -1;
              clients[i].slot_arm_l.template_idx = -1;
              clients[i].slot_feet.template_idx = -1;
              for (int r = 0; r < 10; r++)
                clients[i].slot_rings[r].template_idx = -1;
              for (int b = 0; b < MAX_BELT; b++)
                clients[i].belt[b].template_idx = -1;
              clients[i].pending_trade_merchant_id = -1;
              clients[i].race_id = ml.race_id;
              clients[i].subrace_id = ml.subrace_id;
              clients[i].class_id = ml.class_id;
              clients[i].alignment = ml.alignment;

              memset(clients[i].spell_slots_max, 0,
                     sizeof(clients[i].spell_slots_max));
              switch (clients[i].class_id) {
              case CLASS_PALADIN:
              case CLASS_RANGER:
                clients[i].spell_slots_max[1] = 21;
                break;
              case CLASS_BARBARIAN:
              case CLASS_ROGUE:
                clients[i].spell_slots_max[1] = 22;
                break;
              case CLASS_FIGHTER:
              case CLASS_MONK:
                clients[i].spell_slots_max[1] = 23;
                break;
              default:
                clients[i].spell_slots_max[1] = 24;
                clients[i].spell_slots_max[2] = 23;
                clients[i].spell_slots_max[3] = 22;
                clients[i].spell_slots_max[4] = 21;
                clients[i].spell_slots_max[5] = 20;
                clients[i].spell_slots_max[6] = 19;
                clients[i].spell_slots_max[7] = 18;
                clients[i].spell_slots_max[8] = 17;
                clients[i].spell_slots_max[9] = 16;
                break;
              }
              for (int s = 1; s <= MAX_SPELL_LEVEL; s++) {
                clients[i].spell_slots[s] = clients[i].spell_slots_max[s];
              }

              give_starting_gear(&clients[i]);
            }

            //Send welcome with current location (saved or default)
            MsgHeader wh = {MSG_WELCOME, sizeof(MsgWelcome)};
            MsgWelcome mw = {clients[i].entity_id,
                             clients[i].x,
                             clients[i].y,
                             1,
                             (load_status == 1) ? 0 : 1,
                             clients[i].hp,
                             clients[i].max_hp,
                             clients[i].gold,
                             (int)clients[i].race_id,
                             (int)clients[i].subrace_id,
                             (int)clients[i].class_id,
                             clients[i].level,
                             clients[i].alignment};
            net_send(clients[i].sock, &wh, sizeof(MsgHeader));
            net_send(clients[i].sock, &mw, sizeof(MsgWelcome));
            send_detailed_state(&clients[i]);
            send_map_chunk(clients[i].sock,
                           &master_world->floors[clients[i].floor_id].map,
                           clients[i].x, clients[i].y, INITIAL_VIEW_RADIUS);
            broadcast_nearby_entities(&clients[i], npcs);
            broadcast_player_state(&clients[i]);
            master_world->floors[clients[i].floor_id].entity_grid[clients[i].y][clients[i].x] = clients[i].entity_id;
            if (load_status == 1) {
              send_text_to_client(clients[i].sock,
                                  "[SYSTEM] Welcome back, %s! (Floor %d, HP "
                                  "%d/%d, Gold %lu gp)",
                                  clients[i].username, clients[i].floor_id,
                                  clients[i].hp, clients[i].max_hp,
                                  (unsigned long)clients[i].gold);
            } else {
              send_text_to_client(
                  clients[i].sock,
                  "[SYSTEM] Welcome, %s! Your adventure begins...",
                  clients[i].username);
            }
            server_log("AUTH", "'%s' %s", ml.username,
                       (load_status == 1) ? "rientrato" : "new");
          } else if (hdr.type == MSG_MOVE && clients[i].authenticated) {
            MsgMove m;
            net_receive_all(clients[i].sock, &m, sizeof(MsgMove));

            long long now_ms = get_time_ms();
            if (now_ms - clients[i].last_action_ms < 200) {
                continue; //Rate limit: max 5 actions per second
            }
            clients[i].last_action_ms = now_ms;

            //--- ACTION BLOCK DUE TO CONDITIONS ---
            if (rules_has_condition(clients[i].effects, clients[i].effect_count,
                                    "Paralyzed") ||
                rules_has_condition(clients[i].effects, clients[i].effect_count,
                                    "Stunned") ||
                rules_has_condition(clients[i].effects, clients[i].effect_count,
                                    "Petrified") ||
                rules_has_condition(clients[i].effects, clients[i].effect_count,
                                    "Frozen") ||
                rules_has_condition(clients[i].effects, clients[i].effect_count,
                                    "Unconscious")) {
              send_text_to_client(
                  clients[i].sock,
                  "[SYSTEM] You can't move in this state!");
              continue;
            }

            int dx = (m.dx > 0) ? 1 : (m.dx < 0 ? -1 : 0),
                dy = (m.dy > 0) ? 1 : (m.dy < 0 ? -1 : 0);
            int nx = clients[i].x + dx, ny = clients[i].y + dy;
            bool coll = false;

            if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
              Floor *fl = &master_world->floors[clients[i].floor_id];
              int ent_id = fl->entity_grid[ny][nx];

              if (ent_id > 0) {
                // Find entity by ID (still O(N) but only if occupied, and we
                // can optimize this further) For now, check NPCs then Players
                bool target_found = false;
                for (int n = 0; n < MAX_NPCS; n++) {
                  if (npcs[n].active && npcs[n].entity_id == ent_id) {
                    target_found = true;
                    if (npcs[n].archetype == ARCH_MERCHANT) {
                      send_text_to_client(
                          clients[i].sock,
                          "[%s] Benvenuto! Uso 'buy <n>', 'sell <n>', 'haggle "
                          "buy <n>', 'identify' o 'cure'.",
                          npcs[n].merchant.shop_name);
                      print_merchant_inventory(&clients[i], &npcs[n]);
                      coll = true;
                    } else {
                       /*--- Management of ghost/spoils (is_ghost) or dropped treasure ---*/
                       if ((npcs[n].archetype == ARCH_BOSS && npcs[n].is_ghost) ||
                           npcs[n].archetype == ARCH_TREASURE ||
                           npcs[n].archetype == ARCH_GOLD) {
                         if (npcs[n].archetype == ARCH_GOLD && !npcs[n].is_ghost) {
                           /*Normal pile of gold*/
                           clients[i].gold += npcs[n].gold_drop;
                           send_text_to_client(clients[i].sock,
                               "[SYSTEM] Collect a stack of %d gold coins!",
                               npcs[n].gold_drop);
                           npcs[n].active = false;
                           npcs[n].respawn_timer = 0;
                           floor_stats_npc_died(npcs[n].floor_id);
                           fl->entity_grid[ny][nx] = 0;
                           coll = false;
                           save_player_data(&clients[i]);
                         } else if (npcs[n].is_ghost) {
                           /*Player Ghost: Transfer gold + all items*/
                           bool any = false;
                           bool full = false;
                           if (npcs[n].gold_drop > 0) {
                             clients[i].gold += npcs[n].gold_drop;
                             npcs[n].gold_drop = 0;
                             any = true;
                           }
                           for (int gi = 0; gi < 30; gi++) {
                             if (npcs[n].ghost_loot[gi].template_idx >= 0 &&
                                 npcs[n].ghost_loot[gi].stack_count > 0) {
                               if (clients[i].backpack_count >= MAX_BACKPACK) {
                                 full = true;
                                 break;
                               }
                               clients[i].backpack[clients[i].backpack_count++] =
                                   npcs[n].ghost_loot[gi];
                               npcs[n].ghost_loot[gi].template_idx = -1;
                               npcs[n].ghost_loot[gi].stack_count  = 0;
                               any = true;
                             }
                           }
                           if (full) {
                             send_text_to_client(clients[i].sock,
                                 "[SYSTEM] Backpack full! You have only recovered part of the loot.");
                             coll = true; /*blocks movement*/
                           } else {
                             if (any) {
                               send_text_to_client(clients[i].sock,
                                   "[SYSTEM] Recover everything you were carrying!");
                             }
                             npcs[n].active = false;
                             npcs[n].respawn_timer = 0;
                             floor_stats_npc_died(npcs[n].floor_id);
                             fl->entity_grid[ny][nx] = 0;
                             coll = false;
                           }
                           save_player_data(&clients[i]);
                         } else if (npcs[n].ghost_loot[0].stack_count > 0) {
                           /* Tesoro dropped (singolo item) */
                           if (clients[i].backpack_count >= MAX_BACKPACK) {
                             send_text_to_client(clients[i].sock,
                                 "[SYSTEM] Backpack full! You cannot pick up the item.");
                             coll = true;
                           } else {
                             clients[i].backpack[clients[i].backpack_count++] =
                                 npcs[n].ghost_loot[0];
                             send_text_to_client(clients[i].sock,
                                 "[SISTEMA] Hai raccolto: %s",
                                 item_database[npcs[n].ghost_loot[0].template_idx].name);
                             npcs[n].active = false;
                             npcs[n].respawn_timer = 0;
                             floor_stats_npc_died(npcs[n].floor_id);
                             fl->entity_grid[ny][nx] = 0;
                             coll = false;
                             save_player_data(&clients[i]);
                           }
                         } else {
                           /*Chest without ghost_loot: generate random loot*/
                           send_text_to_client(clients[i].sock,
                               "[SYSTEM] Open the treasure chest and find us"
                               "dentro qualcosa...");
                           drop_loot_from_monster(&clients[i], &npcs[n]);
                           npcs[n].active = false;
                           npcs[n].respawn_timer = 100;
                           floor_stats_npc_died(npcs[n].floor_id);
                           fl->entity_grid[ny][nx] = 0;
                           coll = false;
                           save_player_data(&clients[i]);
                         }
                       } else {
                         perform_attack(&clients[i], &npcs[n], npcs);
                         coll = true;
                       }
                    }
                    break;
                  }
                }
                // If not an NPC, check players
                if (!target_found) {
                  for (int p = 0; p < MAX_CLIENTS; p++) {
                    if (clients[p].active && clients[p].authenticated &&
                        clients[p].entity_id == ent_id) {
                      send_text_to_client(clients[i].sock,
                                          "[SOCIAL] You see %s in front of you.",
                                          clients[p].username);
                      VoxelType target_vt = fl->map.data[0][ny][nx];
                      if (target_vt == VOXEL_STAIRS_UP || target_vt == VOXEL_STAIRS_DOWN) {
                        coll = false;
                      } else {
                        coll = true;
                      }
                      break;
                    }
                  }
                }
              }
            }

            if (!coll) {
              if (nx >= 0 && nx < MAP_WIDTH && ny >= 0 && ny < MAP_HEIGHT) {
                VoxelType vt = master_world->floors[clients[i].floor_id]
                                   .map.data[0][ny][nx];
                if (vt != VOXEL_WALL && vt != VOXEL_ROCK) {
                  //Update grid: leave old tile, enter new
                  Floor *fl = &master_world->floors[clients[i].floor_id];
                  if (fl->entity_grid[clients[i].y][clients[i].x] == clients[i].entity_id) {
                    fl->entity_grid[clients[i].y][clients[i].x] = 0;
                  }

                  clients[i].x = nx;
                  clients[i].y = ny;
                  fl->entity_grid[ny][nx] = clients[i].entity_id;

                  //--- BLOODY MOVEMENT EFFECT ---
                  if (rules_has_condition(clients[i].effects,
                                          clients[i].effect_count,
                                          "Bleeding")) {
                    clients[i].hp -= 2;
                    send_text_to_client(clients[i].sock,
                                        "[DANGER] Moving reopens yours"
                                        "ferite! Sanguini copiosamente...");
                    if (clients[i].hp <= 0) {
                      server_log(
                          "DEATH",
                          "%s bled to death during the movement.",
                          clients[i].username);
                      save_bones(&clients[i]);
                      clients[i].hp = clients[i].max_hp;
                      clients[i].floor_id = 0;
                      clients[i].x = MAP_CENTER_X + 1;
                      clients[i].y = MAP_CENTER_Y + 1;
                      send_text_to_client(clients[i].sock, "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
                    }
                  }

                  global_total_turns++;
                  update_city_doors();
                  check_tile_events(&clients[i], npcs);
                  check_traps(&clients[i], npcs);
                }
              }
            }
            broadcast_nearby_entities(&clients[i], npcs);
            send_detailed_state(&clients[i]);
            broadcast_player_state(&clients[i]);
            //Send the map chunk around the new location
            server_log("NET", "Sending map chunk for player at %d, %d",
                       clients[i].x, clients[i].y);
            send_map_chunk(clients[i].sock,
                           &master_world->floors[clients[i].floor_id].map,
                           clients[i].x, clients[i].y, INITIAL_VIEW_RADIUS);
            server_log("NET", "Map chunk sent");
          } else if (hdr.type == MSG_TEXT_CMD && clients[i].authenticated) {
            MsgTextCmd tc;
            net_receive_all(clients[i].sock, &tc, sizeof(MsgTextCmd));
            long long now_ms = get_time_ms();
            clients[i].last_action_ms = now_ms;
            handle_text_cmd(&clients[i], tc.cmd, npcs);
          }
        }
    }
    update_world(clients, npcs);
  }
  clog_close();
  server_log("SYS", "Server shut down. Log saved.");
  return 0;
}
