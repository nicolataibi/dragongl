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

#include "alignment.h"
#include "bestiary.h"
#include "classes.h"
#include "data_loader.h"
#include "game.h"
#include "gen.h"
#include "items.h"
#include "net.h"
#include "protocol.h"
#include "server_net.h"
#include "server_conn.h"
#include "auth.h"
#include "persist.h"
#include "login_worker.h"
#include "writer.h"
#include "rules.h"
#include "species.h"
#include "spells.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

/*Population/respawn constants (RESPAWN_TICKS, DENSITY_CHECK, DENSITY_MIN_PCT,
 * RESPAWN_TRAPS_TICKS) live in server_internal.h — the SINGLE source of
 * truth. They used to be #defined here AND in server_internal.h: two
 * sources that could silently drift and change game behavior in one
 * translation unit only.*/

World *master_world = NULL;
/*See server_internal.h: resolved in main() before any data access.*/
char g_data_dir[DATA_DIR_MAX] = "data";
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

/*epoll data.u32 of the descriptors that are not client slots (a client is
 * identified by its slot index, 0..MAX_CLIENTS-1).*/
#define EPOLL_ID_LISTEN ((uint32_t)MAX_CLIENTS)
#define EPOLL_ID_LOGIN ((uint32_t)MAX_CLIENTS + 1)
static void on_shutdown_signal(int sig) {
    (void)sig;
    g_shutdown_requested = 1;
}

long long get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/*--- Username policy: [A-Za-z0-9_]{1,16}.
 * The username ends up in file names (saves/<name>.save,
 * saves/tombstone_<name>_*.dat): anything else (path separators, "..",
 * empty) is a traversal vector, so reject it at login.*/
static bool valid_username(const char *u) {
    if (!u)
        return false;
    size_t n = strlen(u);
    if (n < 1 || n > 16)
        return false;
    for (size_t i = 0; i < n; i++) {
        char ch = u[i];
        if (!isalpha((unsigned char)ch) && !isdigit((unsigned char)ch) &&
            ch != '_')
            return false;
    }
    return true;
}

/*Clamp a client-provided stat to the playable range 3..18 (previously
 * an edited client could send str=9999 and keep it).*/
static int clamp_stat(int v) {
    if (v < 3)
        return 3;
    if (v > 18)
        return 18;
    return v;
}

/*Seed the RNG ONCE at process start, with kernel entropy when available.
 * The old code seeded only inside world_init() — which is SKIPPED when
 * data/world.dat exists — so after every reboot the server ran on the
 * default seed (1) and every die/loot/aggro sequence was identical
 * until some first event reseeded it.*/
static void seed_game_rng(void) {
    uint32_t seed = 0;
    if (getrandom(&seed, sizeof(seed), 0) != sizeof(seed)) {
        /*Fallback: mix time, pid and a stack address.*/
        seed = (uint32_t)time(NULL) * 2654435761u;
        seed ^= (uint32_t)getpid() * 40503u;
        seed ^= (uint32_t)(uintptr_t)&seed;
    }
    srand(seed);
    /*The seed is NOT logged: with it, anyone reading the log could
     * replay every die roll and loot drop of rand().*/
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
  char path[STATE_DIR_MAX + 32];
  bool flags[MAX_ARTIFACTS] = {false};
  char why[96];
  if (!state_path(path, sizeof(path), "artifacts.dat"))
    return;
  BinFileStatus st = persist_read_flags(path, flags, artifact_count, why, sizeof(why));
  if (st == BINFILE_INVALID) {
    server_log("SYS", "WARNING: artifacts.dat unreadable (%s): every unique artifact is available again.",
               why);
    return;
  }
  if (st == BINFILE_OK) {
    for (int i = 0; i < artifact_count; i++)
      artifact_registry[i].already_dropped = flags[i];
  }
}

static void artifacts_save(void) {
  char path[STATE_DIR_MAX + 32];
  bool flags[MAX_ARTIFACTS];
  if (!state_path(path, sizeof(path), "artifacts.dat"))
    return;
  for (int i = 0; i < artifact_count; i++)
    flags[i] = artifact_registry[i].already_dropped;
  persist_write_flags(path, flags, artifact_count); /*failures: logged by the writer*/
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
      if (!global_clients[i].active || !global_clients[i].authenticated)
        continue;
      /*Bounds: an out-of-range floor or coordinate (corrupted save, bad
       * dm_place) must never index the world out of bounds — this runs
       * on every round tick and would corrupt the heap.*/
      int f = global_clients[i].floor_id;
      if (f < 0 || f >= MAX_FLOORS)
        continue;
      if (global_clients[i].x < 0 || global_clients[i].x >= MAP_WIDTH ||
          global_clients[i].y < 0 || global_clients[i].y >= MAP_HEIGHT)
        continue;
      master_world->floors[f].entity_grid[global_clients[i].y]
                                     [global_clients[i].x] =
          global_clients[i].entity_id;
    }
  }
  if (npcs) {
    for (int i = 0; i < MAX_NPCS; i++) {
      if (!npcs[i].active)
        continue;
      int f = npcs[i].floor_id;
      if (f < 0 || f >= MAX_FLOORS)
        continue;
      if (npcs[i].x < 0 || npcs[i].x >= MAP_WIDTH ||
          npcs[i].y < 0 || npcs[i].y >= MAP_HEIGHT)
        continue;
      master_world->floors[f].entity_grid[npcs[i].y][npcs[i].x] =
          npcs[i].entity_id;
    }
  }
}

/*Called from the game loop and from the writer and login threads:
 * localtime_r instead of the shared static of localtime(), and the stream
 * lock held across the three writes so lines never interleave.*/
void server_log(const char *cat, const char *fmt, ...) {
  time_t now = time(NULL);
  struct tm tm_now;
  char b[20] = "--:--:--";
  if (localtime_r(&now, &tm_now))
    strftime(b, sizeof(b), "%H:%M:%S", &tm_now);
  flockfile(stdout);
  printf("[%s] [%-8s] ", b, cat);
  va_list a;
  va_start(a, fmt);
  vprintf(fmt, a);
  va_end(a);
  printf("\n");
  fflush(stdout);
  funlockfile(stdout);
}

void get_game_time(int *h, int *m) {
  int total_mins = (global_total_turns % 1440);
  *h = (total_mins / 60 + 8) % 24;
  *m = total_mins % 60;
}

/*Is (x,y) a tile a character can safely stand on after a teleport?*/
static bool tile_walkable(const Floor *fl, int x, int y) {
    if (x < 1 || y < 1 || x >= MAP_WIDTH - 1 || y >= MAP_HEIGHT - 1)
        return false;
    switch (fl->map.data[0][y][x]) {
    case VOXEL_FLOOR:
    case VOXEL_COBBLE:
    case VOXEL_MARBLE:
    case VOXEL_WOOD:
    case VOXEL_GRASS:
    case VOXEL_SAND:
    case VOXEL_STAIRS_UP:
    case VOXEL_STAIRS_DOWN:
    case VOXEL_DOOR:
        return true;
    default:
        return false; /*rock, wall, water, lava, traps... */
    }
}

/*Find a walkable tile near (ax,ay) — expanding rings up to radius 4.
 * ALWAYS returns in-bounds coordinates (worst case the clamped anchor),
 * so callers can never push a player out of the map.*/
static void find_walkable_near(const Floor *fl, int ax, int ay,
                               int *ox, int *oy) {
    if (ax < 1) ax = 1;
    if (ax > MAP_WIDTH - 2) ax = MAP_WIDTH - 2;
    if (ay < 1) ay = 1;
    if (ay > MAP_HEIGHT - 2) ay = MAP_HEIGHT - 2;
    if (tile_walkable(fl, ax, ay)) {
        *ox = ax;
        *oy = ay;
        return;
    }
    for (int r = 1; r <= 4; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (abs(dx) + abs(dy) != r)
                    continue;
                int x = ax + dx, y = ay + dy;
                if (tile_walkable(fl, x, y)) {
                    *ox = x;
                    *oy = y;
                    return;
                }
            }
        }
    }
    /*No walkable ring found (fully walled pocket): clamped anchor is still
     * in-bounds — safe even if it means standing in rock.*/
    *ox = ax;
    *oy = ay;
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
      static const char *dir_names[4] = {"north", "east", "south", "west"};
      if (c->x == fire_x && c->y == fire_y) {
        send_text_to_client(
            c->sock,
            "[DANGER] CLACK! A poison dart shoots out of the wall at %s!",
            dir_names[t->wall_dir]);
        bool saved = rules_roll_save(rules_get_modifier(c->dex), t->save_dc,
                                     false, false, NULL);
        if (saved) {
          send_text_to_client(c->sock,
                              "[SYSTEM] You throw yourself to the side! The dart grazes"
                              " your ear and embeds itself in the wall.");
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
            /*A3: this death site used to set floor_id/x/y/hp by hand, so
             *the player was never reported as left-floor (a "ghost" on
             *other clients' screens) and the client kept showing the
             *dungeon. player_respawn_town does it all (L2).*/
            player_respawn_town(c);
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
      static const char *dn[4] = {"north", "east", "south", "west"};
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
            /*A3: same as the dart wall — the manual floor_id/x/y/hp set
             *skipped notify_player_left_floor() and the client re-sync.
             *player_respawn_town() does it all (L2).*/
            player_respawn_town(c);
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
            /*Bounded scatter + guaranteed in-bounds walkable landing.
             * The old `c->x += rand()%5 - 2` had NO bounds check: near the
             * map edge the player could end up at x<0 and the very next
             * tick read/wrote the map out of bounds.*/
            Floor *nf = &master_world->floors[c->floor_id];
            int lx = c->x + (rand() % 5) - 2;
            int ly = c->y + (rand() % 5) - 2;
            find_walkable_near(nf, lx, ly, &c->x, &c->y);
            nf->entity_grid[c->y][c->x] = c->entity_id;
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
              /*Keep entity_grid consistent with the swap (old code left
               * the player ID on the NPC's old cell and vice versa, so
               * collisions were corrupted until the next grid sync).*/
              Floor *sfl = &master_world->floors[c->floor_id];
              if (sfl->entity_grid[ty][tx] == c->entity_id)
                sfl->entity_grid[ty][tx] = 0;
              if (sfl->entity_grid[target_npc->y][target_npc->x] == target_npc->entity_id)
                sfl->entity_grid[target_npc->y][target_npc->x] = 0;
              c->x = target_npc->x;
              c->y = target_npc->y;
              target_npc->x = tx;
              target_npc->y = ty;
              sfl->entity_grid[c->y][c->x] = c->entity_id;
              sfl->entity_grid[target_npc->y][target_npc->x] = target_npc->entity_id;
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
          /*A3: this was the third death site setting floor_id/x/y by hand
           *instead of using player_respawn_town() (the starvation/poison/
           *burn/bleed paths and MSG_MOVE all use it). Without it the other
           *players kept seeing the corpse on the floor until the next sync
           *and the dead client kept rendering the dungeon.*/
          player_respawn_town(c);
          send_text_to_client(c->sock,
                              "[DEATH] You died from a trap... The Arcane has returned you to town without your equipment!");
        }
      }
      if (t->type == TRAP_TELEPORT || t->type == TRAP_FAKE_DOOR) {
        /*Random teleport — but ONLY onto a walkable tile: the old
         * `rand() % MAP_WIDTH` could drop the player inside a rock, on a
         * staircase (involuntary descent) or onto an occupied cell.*/
        Floor *tfl = &master_world->floors[c->floor_id];
        int anchor_x = 1 + rand() % (MAP_WIDTH - 2);
        int anchor_y = 1 + rand() % (MAP_HEIGHT - 2);
        int dest_x = c->x, dest_y = c->y;
        find_walkable_near(tfl, anchor_x, anchor_y, &dest_x, &dest_y);
        if (tfl->entity_grid[c->y][c->x] == c->entity_id)
          tfl->entity_grid[c->y][c->x] = 0;
        c->x = dest_x;
        c->y = dest_y;
        tfl->entity_grid[c->y][c->x] = c->entity_id;
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

/*Opens/closes the city shop doors by day/night.
 *
 * IMPORTANT: the door positions MUST be computed with the same formula as
 * the shop ring built in map.c (generate_procedural_dungeon, floor 0):
 * 11 shops, radius 26, slot i at angle i*(360/11) degrees, door on the
 * dominant axis 4 tiles out from the building center, facing the city
 * center. The old version used 10 hard-coded (±20/±12) coordinates from
 * the obsolete 1000x1000 layout, so the day/night doors were flipping
 * tiles that were NOT doors — while the real shop doors never changed.*/
static bool is_inside_shop(int x, int y) {
    int cx = MAP_CENTER_X;
    int cy = MAP_CENTER_Y;
    for (int i = 0; i < 11; i++) {
        float angle = (i * (360.0f / 11.0f)) * (M_PI / 180.0f);
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
        float angle = (i * (360.0f / 11.0f)) * (M_PI / 180.0f);
        int sx = cx + (int)(cosf(angle) * 26.0f);
        int sy = cy + (int)(sinf(angle) * 26.0f);
        int dx = cx - sx, dy = cy - sy;
        int tx = sx, ty = sy;
        if (abs(dx) > abs(dy)) {
            if (dx > 0) tx = sx + 4;
            else        tx = sx - 4;
        } else {
            if (dy > 0) ty = sy + 4;
            else        ty = sy - 4;
        }
        if (x == tx && y == ty) return true;
    }
    return false;
}

void update_city_doors(void) {
  int h, m;
  get_game_time(&h, &m);
  VoxelType state = (h >= 6 && h < 20) ? VOXEL_DOOR : VOXEL_WALL;
  Map *city = &master_world->floors[0].map;
  int cx = MAP_CENTER_X;
  int cy = MAP_CENTER_Y;

  for (int i = 0; i < 11; i++) {
    float angle = (i * (360.0f / 11.0f)) * (M_PI / 180.0f);
    int sx = cx + (int)(cosf(angle) * 26.0f);
    int sy = cy + (int)(sinf(angle) * 26.0f);
    /*Door faces the center: dominant axis, 4 tiles from the shop center
     * (same rule as map.c).*/
    int dx = cx - sx, dy = cy - sy;
    int tx = sx, ty = sy;
    if (abs(dx) > abs(dy)) {
      if (dx > 0) tx = sx + 4;
      else        tx = sx - 4;
    } else {
      if (dy > 0) ty = sy + 4;
      else        ty = sy - 4;
    }
    if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT)
      continue;
    /*Only flip tiles that are actually a door or a closed wall: never
     * turn floor/rock into a door (that would seal corridors).*/
    if (city->data[0][ty][tx] == VOXEL_DOOR ||
        city->data[0][ty][tx] == VOXEL_WALL) {
      city->data[0][ty][tx] = state;
    }
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
        strcat(prefix, "Fine ");
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
                            npc_name(killer),
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
        copy_str(art.artifact_name, artifact_registry[ai].name,
                sizeof(art.artifact_name));
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
            npc_name(killer), abuf);
        return; //artifact drop: does not continue with normal drop
      }
    }
  }

  c->backpack[c->backpack_count++] = drop;

  char buf[128];
  get_full_item_name(&drop, buf, sizeof(buf));
  send_text_to_client(c->sock, "[LOOT] %s dropped: %s [%s]",
                      npc_name(killer),
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

float get_movement_cooldown(Client *c) {
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

int get_equipped_mask(Client *c) {
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
  if (rules_has_condition_t(c->effects, c->effect_count, COND_BLINDED)) {
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
  if (rules_has_condition_t(c->effects, c->effect_count, COND_PETRIFIED)) {
    final_ac += 10;
  }
  return final_ac;
}

void save_player_data(Client *c) {
  if (!c || !c->authenticated)
    return;
  char p[STATE_DIR_MAX + 64];
  if (!state_path(p, sizeof(p), "saves/%s.save", c->username))
    return;
  SaveData sd;
  memset(&sd, 0, sizeof(sd));
  /*The Argon2id string computed when the character was created (random
   * salt and parameters are encoded in it): the plaintext is never kept.*/
  copy_str(sd.password_hash, c->password_hash, sizeof(sd.password_hash));
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
  /*--- Phase 3: Active Effects, Resources and Statistics ---
   * Effects are flattened to SavedEffect: ActiveEffect.name is a
   * pointer, so a raw memcpy would serialize a heap address into the
   * file (it "worked" only because the loader ignored it). The name
   * becomes its canonical ConditionType; a name that is not a known
   * condition round-trips as MAX_CONDITIONS (see SavedEffect).*/
  int n_saved_effects = 0;
  for (int i = 0; i < c->effect_count && i < MAX_EFFECTS_PER_ENTITY; i++) {
    const ActiveEffect *e = &c->effects[i];
    SavedEffect *se = &sd.effects[i];
    se->cond            = (int)condition_from_name(e->name);
    se->trigger         = (int)e->trigger;
    se->mod_type        = (int)e->mod_type;
    se->value           = e->value;
    se->duration_rounds = e->duration_rounds;
    se->is_persistent   = e->is_persistent ? 1 : 0;
    n_saved_effects++;
  }
  sd.effect_count = n_saved_effects;
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
  /*Encoded here, field by field (persist.c), into a versioned,
   * CRC-checked container; the writer thread stores it atomically (a
   * failure keeps the previous save and is logged there).*/
  bool ok = persist_write_player(p, &sd);
  auth_wipe(&sd, sizeof(sd));
  if (ok)
    server_log("SAVE", "Saved: %s", c->username);
}

/*Sanitize an ItemInstance loaded from a save file: template_idx must be
 * -1 (empty slot) or a valid index into item_database. A hostile or
 * corrupted value (e.g. 9999) would later be used as an item_database[]
 * index (OOB read), so invalid slots are dropped (treated as empty).*/
static void sanitize_item_instance(ItemInstance *it) {
  if (it->template_idx != -1 &&
      (it->template_idx < 0 || it->template_idx >= item_database_size)) {
    memset(it, 0, sizeof(*it));
    it->template_idx = -1;
  }
}

/*The log line of a refused login (the worker never logs: the client may
 * already be gone, and the slot decides what the user is told).*/
static void log_login_outcome(const LoginJob *job) {
  switch (job->outcome) {
  case LOGIN_BAD_PASSWORD:
    server_log("AUTH", "Incorrect password for: %s", job->ml.username);
    break;
  case LOGIN_UNREADABLE:
    /*Never half-loaded, never treated as a new character (the next save
     * would overwrite it): refused, file kept.*/
    server_log("SAVE", "%s.save is unreadable (%s) — login refused (file kept).",
               job->ml.username, job->why);
    break;
  case LOGIN_BAD_DM:
    server_log("AUTH", "Invalid DM password from slot %d.", job->slot);
    break;
  case LOGIN_SERVER_ERROR:
    server_log("AUTH", "Password hashing failed for: %s", job->ml.username);
    break;
  default:
    break;
  }
}

/*Apply a verified save image to the client (login_finish, or
 * load_player_data below). Everything in it is untrusted input and is
 * range-checked here.*/
static void apply_player_save(Client *c, const SaveData *sd) {
  copy_str(c->password_hash, sd->password_hash, sizeof(c->password_hash));
  //Restore state
  /*--- SANITIZE THE LOADED STATE --------------------------------
   * A save file is untrusted input: saves are written atomically
   * (temp+rename), but anyone with write access to saves/ can still
   * edit them while the server is down, and a disk failure can
   * corrupt them. The fields below are used as array indices
   * (floors[f], entity_grid[y][x],
   * backpack[n], item_database[i]) or in HP arithmetic: e.g. a
   * floor_id=9999 would read/write ~90 KB past the world allocation at
   * login, and a backpack_count>=32 would write out of bounds on the
   * next pickup. Everything is clamped to its valid range, with a safe
   * fallback on error. --------------------------------------------*/
  int save_corrupted = 0;

  /*Floor: must be in [0, MAX_FLOORS), otherwise force to 0 (surface).*/
  if (sd->floor_id < 0 || sd->floor_id >= MAX_FLOORS) {
    c->floor_id = 0;
    save_corrupted = 1;
  } else {
    c->floor_id = sd->floor_id;
  }

  /*Position: must be strictly inside the map (the old check only
   * rejected x/y >= WIDTH/HEIGHT, so a NEGATIVE x or y survived and
   * produced an OOB write in entity_grid on the first action).
   * Fallback: town center.*/
  c->x = (sd->x >= 0 && sd->x < MAP_WIDTH) ? sd->x : MAP_CENTER_X;
  c->y = (sd->y >= 0 && sd->y < MAP_HEIGHT) ? sd->y : MAP_CENTER_Y;
  if (c->x != sd->x || c->y != sd->y)
    save_corrupted = 1;

  /*Identity: out-of-range enums would index the RACES/SUBRACES/CLASSES/
   * ALIGNMENTS tables out of bounds. Same fallbacks as the new-character
   * path.*/
  c->race_id =
      (sd->race_id >= 0 && sd->race_id < RACE_COUNT) ? sd->race_id : 0;
  c->subrace_id =
      (sd->subrace_id >= -1 && sd->subrace_id < SUBRACE_COUNT)
          ? sd->subrace_id
          : -1;
  c->class_id = (sd->class_id >= 0 && sd->class_id < CLASS_COUNT)
                    ? sd->class_id
                    : CLASS_BARBARIAN;
  c->alignment =
      (sd->alignment >= 0 && sd->alignment < ALIGN_COUNT) ? sd->alignment : 0;

  /*Progression: level is capped at 20 (check_level_up / XP table);
   * xp cannot be negative; stats go through the same clamp used on the
   * new-character path.*/
  c->level = sd->level;
  if (c->level < 1 || c->level > 20) {
    c->level = 1;
    save_corrupted = 1;
  }
  c->xp = sd->xp;
  if (c->xp < 0) {
    c->xp = 0;
    save_corrupted = 1;
  }
  c->str = clamp_stat(sd->str);
  c->dex = clamp_stat(sd->dex);
  c->con = clamp_stat(sd->con);
  c->intel = clamp_stat(sd->intel);
  c->wis = clamp_stat(sd->wis);
  c->cha = clamp_stat(sd->cha);
  c->gold = sd->gold;

  /*HP: max_hp must be positive; hp must be in [0, max_hp]. A negative
   * hp is a "dead" character that would come back full on the next
   * damage — a free heal via a modified save.*/
  c->max_hp = sd->max_hp;
  if (c->max_hp <= 0) {
    c->max_hp = 20 + rules_get_modifier(c->con); /*new-character default*/
    if (c->max_hp < 1)
      c->max_hp = 1;
    save_corrupted = 1;
  }
  c->hp = sd->hp;
  if (c->hp < 0) {
    c->hp = 0;
    save_corrupted = 1;
  }
  if (c->hp > c->max_hp) {
    c->hp = c->max_hp;
    save_corrupted = 1;
  }

  /*Inventory: the item arrays are copied wholesale, but the COUNT must
   * be in [0, MAX_BACKPACK]. MAX_BACKPACK itself is legal: it is a full
   * backpack (every pickup checks for space before writing), and treating
   * it as corrupt used to wipe the whole inventory on the next login.*/
  memcpy(c->backpack, sd->backpack, sizeof(sd->backpack));
  c->backpack_count = sd->backpack_count;
  if (c->backpack_count < 0 || c->backpack_count > MAX_BACKPACK) {
    c->backpack_count = 0;
    save_corrupted = 1;
  }
  memcpy(c->belt, sd->belt, sizeof(sd->belt));
  c->slot_head = sd->s_head;
  c->slot_neck = sd->s_neck;
  c->slot_body = sd->s_body;
  c->slot_back = sd->s_back;
  c->slot_hand_r = sd->s_hand_r;
  c->slot_hand_l = sd->s_hand_l;
  c->slot_hands = sd->s_hands;
  c->slot_arm_r = sd->s_arm_r;
  c->slot_arm_l = sd->s_arm_l;
  c->slot_feet = sd->s_feet;
  memcpy(c->slot_rings, sd->s_rings, sizeof(sd->s_rings));
  memcpy(c->spell_slots, sd->spell_slots, sizeof(sd->spell_slots));
  memcpy(c->spell_slots_max, sd->spell_slots_max, sizeof(sd->spell_slots_max));
  /*Every ItemInstance loaded from disk may carry a hostile template_idx
   * (used later as an item_database[] index): any slot that is neither
   * empty (-1) nor a valid database index is dropped.*/
  for (int i = 0; i < MAX_BACKPACK; i++)
    sanitize_item_instance(&c->backpack[i]);
  for (int i = 0; i < MAX_BELT; i++)
    sanitize_item_instance(&c->belt[i]);
  sanitize_item_instance(&c->slot_head);
  sanitize_item_instance(&c->slot_neck);
  sanitize_item_instance(&c->slot_body);
  sanitize_item_instance(&c->slot_back);
  sanitize_item_instance(&c->slot_hand_r);
  sanitize_item_instance(&c->slot_hand_l);
  sanitize_item_instance(&c->slot_hands);
  sanitize_item_instance(&c->slot_arm_r);
  sanitize_item_instance(&c->slot_arm_l);
  sanitize_item_instance(&c->slot_feet);
  for (int i = 0; i < 10; i++)
    sanitize_item_instance(&c->slot_rings[i]);
  /*Spell slots: negative values would defeat the `<= 0' / `> 0' spend
   * checks; clamp into [0, max].*/
  for (int i = 0; i < MAX_SPELL_LEVEL + 1; i++) {
    if (c->spell_slots_max[i] < 0)
      c->spell_slots_max[i] = 0;
    if (c->spell_slots[i] < 0)
      c->spell_slots[i] = 0;
    if (c->spell_slots[i] > c->spell_slots_max[i])
      c->spell_slots[i] = c->spell_slots_max[i];
  }
  /*--- Step 3: Restore active effects, resources and statistics ---*/
  /*v1 saves effects as SavedEffect (POD): the name round-trips as its
   * canonical ConditionType and is re-bound to the table string, so
   * there is no dangling pointer. Non-condition effects (name saved as
   * MAX_CONDITIONS) are transient spell/trap bookkeeping and are
   * dropped by design.*/
  c->effect_count = 0;
  if (sd->effect_count < 0 || sd->effect_count > MAX_EFFECTS_PER_ENTITY) {
    c->effect_count = 0;
    save_corrupted = 1;
  } else {
    for (int i = 0; i < sd->effect_count; i++) {
      const SavedEffect *se = &sd->effects[i];
      if (se->cond < 0 || se->cond >= MAX_CONDITIONS)
        continue; /*not a canonical condition: dropped by design*/
      ActiveEffect *e = &c->effects[c->effect_count];
      e->name = condition_to_name((ConditionType)se->cond);
      e->trigger = (RuleEventType)se->trigger;
      e->mod_type = (ModifierType)se->mod_type;
      e->value = se->value;
      e->duration_rounds = se->duration_rounds;
      if (e->duration_rounds < 0)
        e->duration_rounds = 0;
      e->is_persistent = (se->is_persistent != 0);
      c->effect_count++;
    }
  }
  c->light_turns_left = sd->light_turns_left;
  if (c->light_turns_left < 0)
    c->light_turns_left = 0;
  c->hunger_level     = sd->hunger_level;
  if (c->hunger_level < 0)
    c->hunger_level = 0;
  else if (c->hunger_level > HUNGER_MAX)
    c->hunger_level = HUNGER_MAX;
  c->exhaustion_level = sd->exhaustion_level;
  if (c->exhaustion_level < 0 || c->exhaustion_level > 6)
    c->exhaustion_level = 0;
  c->total_kills      = sd->total_kills;
  if (c->total_kills < 0)
    c->total_kills = 0;
  c->total_steps      = sd->total_steps;
  if (c->total_steps < 0)
    c->total_steps = 0;
  /* --- Innate transit magic: restore deepest floor ever reached.
   * Old saves (smaller files) leave the field at zero, which is correct:
   * the character has no recorded descent yet. */
  c->max_floor_explored = sd->max_floor_explored;
  if (c->max_floor_explored < 0 || c->max_floor_explored >= MAX_FLOORS)
    c->max_floor_explored = 0;
  if (c->floor_id > c->max_floor_explored)
    c->max_floor_explored = c->floor_id;
  /* --- Fase 5: Ripristina grimorio --- */
  memcpy(c->known_spells, sd->known_spells, sizeof(sd->known_spells));
  /* Innate class magic is always known, even on saves created before the
   * spell existed: force-set the grimoire bits for this character's class. */
  {
    uint32_t cls_bit = (1u << (int)c->class_id);
    for (int si = 0; si < spell_database_size; si++) {
      if (!spell_database[si].innate)
        continue;
      if (!(spell_database[si].class_mask & cls_bit))
        continue;
      /*Guarded: si may exceed MAX_SPELL_DB_SIZE on huge datasets*/
      spell_known_set(c, si);
    }
  }
  /*--- Step 6: Reset boss flags ---*/
  c->bosses_defeated = sd->bosses_defeated;
  c->unspent_stat_points = sd->unspent_stat_points;
  if (c->unspent_stat_points < 0)
    c->unspent_stat_points = 0;
  else if (c->unspent_stat_points > 60) /*max: 19 level-ups * 3 points*/
    c->unspent_stat_points = 60;
  /*--- Messaging: Reset block list ---
* Older (smaller) saves leave the field at zero,
* therefore the list is correctly empty.*/
  if (sd->blocked_count > 0 && sd->blocked_count <= MAX_BLOCKED_PLAYERS) {
    memcpy(c->blocked_players, sd->blocked_players,
           sizeof(sd->blocked_players));
    c->blocked_count = sd->blocked_count;
  } else {
    c->blocked_count = 0;
  }
  if (save_corrupted)
    server_log("SAVE",
               "Corrupted/tampered fields in %s.save — state sanitized on load.",
               c->username);
  server_log("SAVE", "Loaded: %s (floor %d, HP %d/%d, gold %lu, effects %d)",
             c->username, c->floor_id, c->hp, c->max_hp,
             (unsigned long)c->gold, c->effect_count);
}

/*Synchronous load (tests and tools; a network login goes through the
 * login worker). Returns 1 if loaded, 0 if the character does not exist,
 * -1 if the password is incorrect, -2 if the file exists but is unreadable
 * (other format version, corrupt): the login is refused and the file is
 * kept intact.*/
int load_player_data(Client *c, const char *password) {
  if (!c || !password)
    return 0;
  LoginJob *job = calloc(1, sizeof(*job));
  if (!job)
    return -2;
  int rc = -2;
  copy_str(job->ml.username, c->username, sizeof(job->ml.username));
  copy_str(job->ml.password, password, sizeof(job->ml.password));
  if (state_path(job->save_path, sizeof(job->save_path), "saves/%s.save", c->username)) {
    login_job_run(job);
    log_login_outcome(job);
    switch (job->outcome) {
    case LOGIN_LOADED:
      apply_player_save(c, &job->sd);
      rc = 1;
      break;
    case LOGIN_NEW:
      rc = 0;
      break;
    case LOGIN_BAD_PASSWORD:
      rc = -1;
      break;
    default:
      rc = -2;
      break;
    }
  }
  login_job_free(job);
  return rc;
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
        /*active is set at the END of the setup, after the template is
         *chosen, via npc_set_active: the helper must see the FINAL fields
         *to update the floor cache correctly (A2).*/
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
        /*Activate through the single entry point now that the template is
         *final (A2): the old code set active=true with no floor-cache
         *update at all, so every death leaked one "missing" NPC from the
         *density monitor's view of the floor.*/
        npc_set_active(g, true);
        /*Register in the entity_grid*/
        master_world->floors[c->floor_id].entity_grid[c->y][c->x] = g->entity_id;
        /*The corpse must be visible in the very next broadcast (the
         * per-floor index is rebuilt on the next tick at the latest,
         * but the player is standing right here: do it now).*/
        floor_index_rebuild(g_npcs);
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
  if (vt == VOXEL_ICE) {
    /*Slippery ground: Prone for 1 round.
     * NOTE: this used to trigger on VOXEL_WATER, so EVERY step in a lake
     * forced a guaranteed Prone. Only ice is slippery.*/
    if (c->effect_count < MAX_EFFECTS_PER_ENTITY) {
      c->effects[c->effect_count] = (ActiveEffect){
          "Prone", EVENT_ON_TURN_START, MOD_ADDITIVE, 0, 1, false};
      c->effect_count++;
      send_text_to_client(
          c->sock, "[DANGER] The ground is slippery! You fell to the ground!");
    }
  } else if (vt == VOXEL_STAIRS_DOWN) {
    /*Floor boundary: there is no world below the last floor. The old
     * unguarded c->floor_id++ on MAX_FLOORS-1 read/wrote floors[101]
     * (heap OOB: crash or silent corruption). TRAP_FALLING_FLOOR already
     * had this guard — the stairs were the only floor transition missing
     * it.*/
    if (c->floor_id >= MAX_FLOORS - 1) {
      send_text_to_client(c->sock,
          "[SYSTEM] The dungeon ends here: there is no floor below this one.");
      return;
    }
    if (c->floor_id > 0 && c->floor_id % 10 == 0) {
      int boss_index = (c->floor_id / 10) - 1; /*>= 0: floor_id is a positive multiple of 10*/
      // cppcheck-suppress [shiftTooManyBits,signConversionCond]
      if (!(c->bosses_defeated & (1u << boss_index))) {
        bool boss_alive = false;
        if (g_npcs) {
          /*Per-floor index (falls back to a full scan if the floor
           * overflowed the index cap).*/
          int bn = 0;
          const int *bidx = floor_index_for(c->floor_id, &bn);
          if (bidx) {
            for (int k = 0; k < bn; k++) {
              const NPC *n = &g_npcs[bidx[k]];
              if (n->active && n->floor_id == c->floor_id && n->archetype == ARCH_BOSS) {
                boss_alive = true;
                break;
              }
            }
          } else {
            for (int i = 0; i < MAX_NPCS; i++) {
              if (g_npcs[i].active && g_npcs[i].floor_id == c->floor_id && g_npcs[i].archetype == ARCH_BOSS) {
                boss_alive = true;
                break;
              }
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
        /*Guarded: si may exceed MAX_SPELL_DB_SIZE on huge datasets*/
        spell_known_set(c, si);
      }
    }
  }
}


/*DRAGONGL_NO_MAIN: the in-process test harness (tests/integration)
 * links every server translation unit and provides its own main().*/
#ifndef DRAGONGL_NO_MAIN

/*--- Connection management (main loop only) ---------------------------*/

/*Refuse a login: queue the reason, forget any authentication state set
 * so far — the reaper must NEVER save a character whose password was
 * wrong — and mark the connection for removal.*/
static void reject_login(Client *c, int slot, const char *reason,
                         bool count_failure) {
  send_auth_fail(c->sock, reason);
  c->authenticated = false;
  c->is_dm = false;
  if (count_failure)
    ipguard_login_failed(conn_peer_ip(slot), get_time_ms());
  conn_mark_drop(slot, reason);
}

static void peer_ip_string(const struct sockaddr_storage *ss, char *out,
                           size_t out_len) {
  const void *addr = NULL;
  if (ss->ss_family == AF_INET)
    addr = &((const struct sockaddr_in *)ss)->sin_addr;
  else if (ss->ss_family == AF_INET6)
    addr = &((const struct sockaddr_in6 *)ss)->sin6_addr;
  if (!addr || !inet_ntop(ss->ss_family, addr, out, (socklen_t)out_len))
    copy_str(out, "?", out_len);
}

/*In play, or with a login of that name in the login worker.*/
static bool character_online(const Client *clients, const char *username) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].active)
      continue;
    if (clients[i].authenticated && strcmp(clients[i].username, username) == 0)
      return true;
    if (clients[i].login_pending && strcmp(clients[i].login_user, username) == 0)
      return true;
  }
  return false;
}

/*TCP keepalive: a client that vanished without closing (power loss,
 * dropped Wi-Fi) is detected after ~90 s, its slot freed and its
 * character saved and released for the next login.*/
static void enable_keepalive(int fd) {
  int on = 1, idle = 60, interval = 10, count = 3;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}

static int pending_logins_from(const Client *clients, const char *ip) {
  int n = 0;
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (clients[i].active && !clients[i].authenticated &&
        strcmp(conn_peer_ip(i), ip) == 0)
      n++;
  return n;
}

/*Accept every queued connection (the listening socket is non-blocking).
 * A connection that cannot be served — address locked out, too many
 * pending logins from it, server full — gets a MSG_AUTH_FAIL and is
 * closed at once: it never occupies a slot or leaks its descriptor.*/
static void accept_pending_connections(int listen_fd, int epoll_fd,
                                       Client *clients) {
  for (;;) {
    struct sockaddr_storage ss;
    socklen_t ss_len = sizeof(ss);
    int cs = accept(listen_fd, (struct sockaddr *)&ss, &ss_len);
    if (cs >= 0) {
      net_set_nonblocking(cs);
      fcntl(cs, F_SETFD, FD_CLOEXEC); /*not inherited by the PDF generator*/
      enable_keepalive(cs);
    }
    if (cs < 0) {
      if (errno == EINTR)
        continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK)
        server_log("NET", "accept failed: %s", strerror(errno));
      return;
    }
    char ip[CONN_IP_MAX];
    peer_ip_string(&ss, ip, sizeof(ip));
    long long now = get_time_ms();

    const char *refusal = NULL;
    int slot = -1;
    if (ipguard_locked(ip, now)) {
      refusal = "Too many failed logins from your address: try again later.";
    } else if (pending_logins_from(clients, ip) >= MAX_PENDING_PER_IP) {
      refusal = "Too many pending connections from your address.";
    } else {
      for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
          slot = i;
          break;
        }
      }
      if (slot < 0)
        refusal = "Server full: try again later.";
    }
    if (refusal) {
      server_log("NET", "Connection from %s refused: %s", ip, refusal);
      conn_send_auth_fail_now(cs, refusal);
      close(cs);
      continue;
    }

    /*A slot starts from a clean Client: nothing of its previous
     * occupant (username, trade state, DM flag...) may leak in.*/
    memset(&clients[slot], 0, sizeof(clients[slot]));
    clients[slot].sock = cs;
    clients[slot].active = true;
    clients[slot].trade_offer_item_idx = -1;
    clients[slot].pending_trade_merchant_id = -1;
    conn_open(slot, cs, ip, now);
    client_seen_reset(slot, -1);

    struct epoll_event client_ev;
    memset(&client_ev, 0, sizeof(client_ev));
    client_ev.events = EPOLLIN | EPOLLRDHUP;
    client_ev.data.u32 = (uint32_t)slot;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, cs, &client_ev) != 0) {
      server_log("NET", "epoll_ctl(ADD) failed for %s: %s", ip, strerror(errno));
      conn_release(slot);
      clients[slot].active = false;
      close(cs);
      continue;
    }
    server_log("NET", "Client %d connected from %s", slot, ip);
  }
}

/*A connection must log in within LOGIN_TIMEOUT_MS of being accepted.*/
static void expire_pending_logins(Client *clients) {
  long long now = get_time_ms();
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active && !clients[i].authenticated && !conn_dropping(i) &&
        now - conn_opened_ms(i) > LOGIN_TIMEOUT_MS) {
      send_auth_fail(clients[i].sock, "Login timeout.");
      conn_mark_drop(i, "login timeout");
    }
  }
}

static void flush_all_clients(Client *clients) {
  for (int i = 0; i < MAX_CLIENTS; i++)
    if (clients[i].active && conn_tx_pending(i))
      conn_flush(i);
}

/*Remove every connection marked for removal during this iteration. This
 * is the ONLY place a client slot is closed: saving, the "left floor"
 * notice and the grid cleanup happen once, in one order, and no send path
 * ever disconnects a client synchronously.*/
static void reap_dropped_clients(Client *clients) {
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].active || !conn_dropping(i))
      continue;
    Client *c = &clients[i];
    server_log("NET", "Client %d ('%s', %s) disconnected: %s", i,
               c->authenticated ? c->username : "-", conn_peer_ip(i),
               conn_drop_reason(i));
    if (c->authenticated) {
      save_player_data(c);
      if (c->floor_id >= 0 && c->floor_id < MAX_FLOORS && c->x >= 0 &&
          c->x < MAP_WIDTH && c->y >= 0 && c->y < MAP_HEIGHT) {
        int *cell = &master_world->floors[c->floor_id].entity_grid[c->y][c->x];
        if (*cell == c->entity_id)
          *cell = 0;
        notify_player_left_floor(c, c->floor_id);
      }
    }
    conn_flush(i); /*best effort: deliver a queued MSG_AUTH_FAIL*/
    close(c->sock); /*also removes the fd from the epoll set*/
    conn_release(i);
    c->sock = -1;
    c->active = false;
    c->authenticated = false;
    c->is_dm = false;
  }
}

/*Interest set of every slot: EPOLLOUT only while bytes are waiting
 * (level-triggered EPOLLOUT on an empty queue would wake the loop
 * continuously), and no EPOLLIN while the slot's login is in the worker
 * (its unread bytes would wake the loop continuously too).*/
static void update_interest(int epoll_fd, Client *clients) {
  static uint32_t current[MAX_CLIENTS];
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!clients[i].active) {
      current[i] = 0;
      continue;
    }
    uint32_t want = EPOLLRDHUP | (clients[i].login_pending ? 0u : EPOLLIN) |
                    (conn_tx_pending(i) ? EPOLLOUT : 0u);
    if (current[i] == 0)
      current[i] = EPOLLIN | EPOLLRDHUP; /*as registered by accept*/
    if (want == current[i])
      continue;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = want;
    ev.data.u32 = (uint32_t)i;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, clients[i].sock, &ev) == 0)
      current[i] = want;
  }
}

static void print_server_usage(const char *argv0) {
  printf("Usage: %s [options]\n"
         "Options:\n"
         "  -P, --port <port>          TCP port to listen on (default: 8080)\n"
         "  -D, --data-dir <dir>       Read-only game data (JSON datasets).\n"
         "                             Default: ./data if present, else\n"
         "                             /usr/share/dragongl/data (RPM).\n"
         "  -S, --state-dir <dir>      Everything the server writes: world,\n"
         "                             NPCs, saves, logs, DM maps. Default:\n"
         "                             $DRAGONGL_STATE_DIR, else\n"
         "                             $XDG_STATE_HOME/dragongl, else\n"
         "                             ~/.local/state/dragongl\n"
         "      --password-file <f>    Read the server access password from f\n"
         "      --dm-password-file <f> Read the Dungeon Master password from f\n"
         "  -h, --help                 Show this help\n"
         "\n"
         "Passwords are never accepted on the command line. Without a file the\n"
         "server reads DRAGONGL_SERVER_PASSWORD (required) and DRAGONGL_DM_PASSWORD\n"
         "(optional: without it the DM account is disabled). Each password has\n"
         "8-31 printable ASCII characters, no spaces.\n",
         argv0);
}

/*Server access password (required) and DM password (optional), from a
 * file or the environment. Only digests are kept; the plaintext is wiped.*/
static bool load_server_secrets(const char *gate_file, const char *dm_file) {
  char secret[64], err[256];
  int rc = auth_load_secret(gate_file, "DRAGONGL_SERVER_PASSWORD", secret,
                            sizeof(secret), err, sizeof(err));
  if (rc < 0) {
    fprintf(stderr, "[SYS] Server password: %s\n", err);
    return false;
  }
  if (rc == 0) {
    fprintf(stderr, "[SYS] No server password: set DRAGONGL_SERVER_PASSWORD or use "
                    "--password-file <file>.\n");
    return false;
  }
  if (err[0])
    fprintf(stderr, "[SYS] WARNING: %s\n", err);
  const char *why = auth_secret_problem(secret);
  if (why) {
    auth_wipe(secret, sizeof(secret));
    fprintf(stderr, "[SYS] Server password %s.\n", why);
    return false;
  }
  auth_set_gate_secret(secret);
  auth_wipe(secret, sizeof(secret));

  rc = auth_load_secret(dm_file, "DRAGONGL_DM_PASSWORD", secret, sizeof(secret),
                        err, sizeof(err));
  if (rc < 0) {
    fprintf(stderr, "[SYS] DM password: %s\n", err);
    return false;
  }
  if (rc == 0) {
    server_log("SYS", "DM account disabled (no DM password configured).");
    return true;
  }
  if (err[0])
    fprintf(stderr, "[SYS] WARNING: %s\n", err);
  why = auth_secret_problem(secret);
  bool ok = (why == NULL) && auth_set_dm_secret(secret);
  auth_wipe(secret, sizeof(secret));
  if (!ok) {
    fprintf(stderr, "[SYS] DM password %s.\n", why ? why : "could not be hashed");
    return false;
  }
  server_log("SYS", "DM account enabled.");
  return true;
}

static uint64_t g_login_gen;

/*MSG_LOGIN, first half (game loop): the cheap checks, then the Argon2id
 * work and the save read go to the login worker; login_finish() completes
 * the login. Every refusal goes through reject_login().*/
static void login_begin(Client *clients, int i, MsgLogin *ml) {
  /*Force NUL terminators (M5): username/password/server_pass
   * are fixed-size arrays and a malicious client can send all
   * 32 bytes of each without a terminator. The first uses are
   * strlen (valid_username) and strcmp (server_pass/DM check) —
   * un-terminated, those read past the struct (UB). Same
   * treatment MSG_TEXT_CMD already gets for tc.cmd.*/
  ml->username[sizeof(ml->username) - 1] = '\0';
  ml->password[sizeof(ml->password) - 1] = '\0';
  ml->server_pass[sizeof(ml->server_pass) - 1] = '\0';

  //Server access ("gate") password: keyed digest, constant-time compare.
  bool gate_ok = auth_check_gate(ml->server_pass);
  auth_wipe(ml->server_pass, sizeof(ml->server_pass));
  if (!gate_ok) {
      server_log("AUTH", "Server access denied (wrong server password) from %s",
                 conn_peer_ip(i));
      reject_login(&clients[i], i, "Wrong server password.", true);
      return;
  }

  //--- Username policy (§1.3): [A-Za-z0-9_]{1,16} ---
  //The username goes into file names (saves/<u>.save,
  //saves/tombstone_<u>_*.dat): slashes, ".." etc. were path-
  //traversal vectors, so reject anything that doesn't match.
  if (!valid_username(ml->username)) {
      reject_login(&clients[i], i, "Invalid username: use 1-16 chars (a-z, A-Z, 0-9, _).", false);
      return;
  }

  //One session per character: two sessions would load and save
  //the same character independently (the last save wins), which
  //duplicates items. The live session keeps the character.
  if (character_online(clients, ml->username)) {
      reject_login(&clients[i], i, "Character already online.", false);
      return;
  }

  if (!auth_password_acceptable(ml->password)) {
      reject_login(&clients[i], i,
                   "Invalid personal password: 1-31 printable characters.", false);
      return;
  }

  //--- Dungeon Master: a separate credential. The account exists only if
  //the server was started with a DM password (checked by the worker). ---
  bool is_dm = (strcmp(ml->username, "dm") == 0);
  if (is_dm && !auth_dm_enabled()) {
      reject_login(&clients[i], i, "The DM account is disabled on this server.", false);
      return;
  }

  LoginJob *job = calloc(1, sizeof(*job));
  if (!job || !state_path(job->save_path, sizeof(job->save_path), "saves/%s.save",
                          ml->username)) {
      free(job);
      reject_login(&clients[i], i, "Server error while logging in.", false);
      return;
  }
  job->slot = i;
  job->gen = ++g_login_gen;
  job->is_dm = is_dm;
  job->ml = *ml; /*the gate password is already wiped*/
  clients[i].login_pending = true;
  clients[i].login_gen = job->gen;
  copy_str(clients[i].login_user, ml->username, sizeof(clients[i].login_user));
  login_worker_submit(job);
}

/*MSG_LOGIN, second half (game loop): the worker's verdict. The slot may
 * have been dropped (timeout, disconnect) and even reused meanwhile: the
 * result only applies to the very connection that asked for it.*/
static void login_finish(Client *clients, LoginJob *job, NPC *npcs) {
  int i = job->slot;
  if (i < 0 || i >= MAX_CLIENTS || !clients[i].active || !clients[i].login_pending ||
      clients[i].login_gen != job->gen || conn_dropping(i))
    return;
  clients[i].login_pending = false;
  clients[i].login_user[0] = '\0';
  MsgLogin *ml = &job->ml;
  log_login_outcome(job);
  switch (job->outcome) {
  case LOGIN_BAD_DM:
      reject_login(&clients[i], i, "Invalid DM password.", true);
      return;
  case LOGIN_BAD_PASSWORD:
      reject_login(&clients[i], i, "Incorrect character password.", true);
      return;
  case LOGIN_UNREADABLE:
      //Other format version or CRC mismatch: the file is kept.
      reject_login(&clients[i], i,
                   "Save file unreadable (other format version or corrupt).", false);
      return;
  case LOGIN_SERVER_ERROR:
      reject_login(&clients[i], i, "Server error while creating the character.", false);
      return;
  case LOGIN_LOADED:
  case LOGIN_NEW:
      break;
  }
  bool is_dm = job->is_dm;
  int load_status = (job->outcome == LOGIN_LOADED) ? 1 : 0;


  //Initialize base fields
  clients[i].entity_id = next_id++;
  copy_str(clients[i].username, ml->username, sizeof(clients[i].username));
  clients[i].effect_count = 0;

  //The saved character (its Argon2id hash verified by the worker), or a
  //new one with the hash the worker computed.
  if (load_status == 1)
    apply_player_save(&clients[i], &job->sd);
  else
    copy_str(clients[i].password_hash, job->new_hash, sizeof(clients[i].password_hash));
  clients[i].authenticated = true;
  clients[i].is_dm = is_dm;
  if (is_dm)
      server_log("AUTH", "DM account authenticated from %s.", conn_peer_ip(i));
  if (load_status == 0) {
    //New character. Stats are rolled on the SERVER below,
    //once race/subrace are sanitized: the client-provided
    //str..cha were the client's own 3d6 roll, i.e. untrusted
    //(a modified client used to send 18/18/18/18/18/18 and
    //the server only clamped them). M9.
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
    /*Client-provided enums: out-of-range values would index the
     * RACES/SUBRACES/CLASSES/ALIGNMENTS tables out of bounds.*/
    clients[i].race_id = (ml->race_id >= 0 && ml->race_id < RACE_COUNT)
                             ? ml->race_id : 0;
    clients[i].subrace_id =
        (ml->subrace_id >= -1 && ml->subrace_id < SUBRACE_COUNT)
            ? ml->subrace_id : -1;
    clients[i].class_id = (ml->class_id >= 0 && ml->class_id < CLASS_COUNT)
                              ? ml->class_id : CLASS_BARBARIAN;
    clients[i].alignment =
        (ml->alignment >= 0 && ml->alignment < ALIGN_COUNT)
            ? ml->alignment : 0;

    /*--- Ability scores: rolled HERE, on the server (M9) ---
     * The client used to roll 3d6 locally (with a reroll
     * loop) and send the totals; an edited client simply
     * sent 18 six times. Same formula the client used to
     * compute: 3d6 per stat + race bonus + subrace bonus,
     * with the result clamped to the playable range 3..18.
     * The rolled values are sent back as a [CHARACTER] line
     * (below) so the player sees them in-game.*/
    {
      const RaceType rt = (RaceType)clients[i].race_id;
      int bonus[6] = {
          RACES[rt].str_bonus, RACES[rt].dex_bonus,
          RACES[rt].con_bonus, RACES[rt].int_bonus,
          RACES[rt].wis_bonus, RACES[rt].cha_bonus
      };
      if (clients[i].subrace_id >= 0) {
        const SubraceType st = (SubraceType)clients[i].subrace_id;
        bonus[0] += SUBRACES[st].str_bonus;
        bonus[1] += SUBRACES[st].dex_bonus;
        bonus[2] += SUBRACES[st].con_bonus;
        bonus[3] += SUBRACES[st].int_bonus;
        bonus[4] += SUBRACES[st].wis_bonus;
        bonus[5] += SUBRACES[st].cha_bonus;
      }
      clients[i].str   = clamp_stat(rules_roll_dice(3, 6) + bonus[0]);
      clients[i].dex   = clamp_stat(rules_roll_dice(3, 6) + bonus[1]);
      clients[i].con   = clamp_stat(rules_roll_dice(3, 6) + bonus[2]);
      clients[i].intel = clamp_stat(rules_roll_dice(3, 6) + bonus[3]);
      clients[i].wis   = clamp_stat(rules_roll_dice(3, 6) + bonus[4]);
      clients[i].cha   = clamp_stat(rules_roll_dice(3, 6) + bonus[5]);
    }
    clients[i].hp = 20 + rules_get_modifier(clients[i].con);
    clients[i].max_hp = clients[i].hp;

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
  MsgHeader wh = msg_hdr(MSG_WELCOME, (int)sizeof(MsgWelcome));
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
  /*A failed welcome means the client is gone (net_send_client
   * already dropped it): skip the rest of the login sequence.*/
  if (!net_send_client(clients[i].sock, &wh, sizeof(MsgHeader)) ||
      !net_send_client(clients[i].sock, &mw, sizeof(MsgWelcome))) {
      return;
  }
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
    /*The rolled ability summary (M9): the dice were thrown on
     * the server, this is the player's view of the result.*/
    send_text_to_client(
        clients[i].sock,
        "[CHARACTER] Rolled by the server: STR %d, DEX %d, CON %d, "
        "INT %d, WIS %d, CHA %d (Max HP %d)",
        clients[i].str, clients[i].dex, clients[i].con,
        clients[i].intel, clients[i].wis, clients[i].cha,
        clients[i].max_hp);
  }
  ipguard_login_succeeded(conn_peer_ip(i));
  server_log("AUTH", "'%s' %s", ml->username,
             (load_status == 1) ? "logged in" : "created");
}

static void handle_login(Client *clients, int i, const void *payload) {
  if (clients[i].authenticated) {
    conn_mark_drop(i, "MSG_LOGIN on an authenticated session");
    return;
  }
  if (clients[i].login_pending) {
    conn_mark_drop(i, "second MSG_LOGIN while the first is being checked");
    return;
  }
  MsgLogin ml;
  memcpy(&ml, payload, sizeof(ml));
  login_begin(clients, i, &ml);
  auth_wipe(&ml, sizeof(ml)); /*the job keeps the only copy, wiped by the worker*/
  conn_wipe_message(i);
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF,
          0); // Disable stdout buffering for live log files
  int server_port = 8080;
  const char *password_file = NULL;
  const char *dm_password_file = NULL;
  const char *state_dir_arg = NULL;
  for (int i = 1; i < argc; i++) {
    const char *opt = argv[i];
    const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (strcmp(opt, "-h") == 0 || strcmp(opt, "--help") == 0) {
      print_server_usage(argv[0]);
      return 0;
    }
    if (strcmp(opt, "-p") == 0 || strcmp(opt, "--password") == 0 ||
        strcmp(opt, "-d") == 0 || strcmp(opt, "--dm-password") == 0) {
      fprintf(stderr,
              "%s: passwords are not accepted on the command line (every local\n"
              "user can read them with ps). Use --password-file / --dm-password-file\n"
              "or the DRAGONGL_SERVER_PASSWORD / DRAGONGL_DM_PASSWORD variables.\n",
              argv[0]);
      return 2;
    }
    bool takes_value = strcmp(opt, "-P") == 0 || strcmp(opt, "--port") == 0 ||
                       strcmp(opt, "-D") == 0 || strcmp(opt, "--data-dir") == 0 ||
                       strcmp(opt, "-S") == 0 || strcmp(opt, "--state-dir") == 0 ||
                       strcmp(opt, "--password-file") == 0 ||
                       strcmp(opt, "--dm-password-file") == 0;
    if (!takes_value) {
      fprintf(stderr, "%s: unknown option '%s' (see --help)\n", argv[0], opt);
      return 2;
    }
    if (!val) {
      fprintf(stderr, "%s: %s needs a value\n", argv[0], opt);
      return 2;
    }
    i++;
    if (strcmp(opt, "-P") == 0 || strcmp(opt, "--port") == 0) {
      char *end = NULL;
      long port = strtol(val, &end, 10);
      if (!end || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "%s: invalid port '%s'\n", argv[0], val);
        return 2;
      }
      server_port = (int)port;
    } else if (strcmp(opt, "-D") == 0 || strcmp(opt, "--data-dir") == 0) {
      copy_str(g_data_dir, val, sizeof(g_data_dir));
    } else if (strcmp(opt, "-S") == 0 || strcmp(opt, "--state-dir") == 0) {
      state_dir_arg = val;
    } else if (strcmp(opt, "--password-file") == 0) {
      password_file = val;
    } else {
      dm_password_file = val;
    }
  }
  if (!auth_init()) {
    fprintf(stderr, "%s: libsodium initialization failed\n", argv[0]);
    return 1;
  }
  if (!load_server_secrets(password_file, dm_password_file))
    return 2;
  {
    char err[STATE_DIR_MAX + 128];
    if (!persist_init_state_dir(state_dir_arg, err, sizeof(err))) {
      fprintf(stderr, "[SYS] State directory: %s\n", err);
      return 2;
    }
  }
  /*Resolve the data directory (see server_internal.h).
   * 1. explicit --data-dir (trusted as-is)
   * 2. local ./data (development layout)
   * 3. /usr/share/dragongl/data (RPM: the server is installed in
   *    /usr/bin but the JSON data live in the data subpackage)*/
  {
    bool explicit_dir = (strlen(g_data_dir) > 0 &&
                         strcmp(g_data_dir, "data") != 0);
    if (!explicit_dir) {
      char probe[DATA_DIR_MAX + 32];
      struct stat st;
      snprintf(probe, sizeof(probe), "%s/bestiary.json", g_data_dir);
      if (stat(probe, &st) != 0) {
        const char *sys_dir = "/usr/share/dragongl/data";
        snprintf(probe, sizeof(probe), "%s/bestiary.json", sys_dir);
        if (stat(probe, &st) == 0) {
          copy_str(g_data_dir, sys_dir, sizeof(g_data_dir));
        }
      }
    }
  }
  /*Seed the RNG BEFORE anything else: even when a world is loaded from
   * disk (world_init skipped), dice must not run on the default seed.*/
  seed_game_rng();
  server_log("SYS", "Starting Dragon GL Server...");
  server_log("SYS", "Data directory (read-only): %s", g_data_dir);
  server_log("SYS", "State directory: %s", persist_state_dir());
  {
    char clog_path[STATE_DIR_MAX + 32];
    if (state_path(clog_path, sizeof(clog_path), "combat_log.json")) {
      clog_init(clog_path);
      server_log("SYS", "Tactical log opened: %s", clog_path);
    }
  }
  /* Ctrl-C / kill: request a graceful shutdown instead of the default
   * immediate termination (which would leave combat_log.json truncated).*/
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_shutdown_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  /*A write to a socket the peer already closed must fail with EPIPE, not
   * kill the process (every send also passes MSG_NOSIGNAL).*/
  signal(SIGPIPE, SIG_IGN);
  int s_sock = net_create_server(server_port);
  if (s_sock < 0) {
    server_log("SYS", "Cannot bind to port %d", server_port);
    clog_close();
    return 1;
  }
  server_log("SYS", "Listening on port %d", server_port);
  net_set_nonblocking(s_sock);
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd == -1) {
    server_log("SYS", "Cannot create epoll");
    return 1;
  }
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.u32 = EPOLL_ID_LISTEN;
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, s_sock, &ev);
  
  struct epoll_event ep_events[MAX_CLIENTS + 2];
  Client *clients = calloc(MAX_CLIENTS, sizeof(Client));
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

  {
    char probe[DATA_DIR_MAX + 32];
    snprintf(probe, sizeof(probe), "%s/bestiary.json", g_data_dir);
    if (access(probe, R_OK) != 0) {
      server_log("SYS", "ERROR: data directory not found or missing bestiary.json (%s).\n"
                 "Use --data-dir <dir>.", probe);
      clog_close();
      return 1;
    }
  }
  if (!init_data_loaders(g_data_dir)) {
    server_log("SYS", "JSON Error (in %s)", g_data_dir);
    clog_close();
    return 1;
  }
  artifacts_load();
  server_log("SYS", "Unique Artifact System initialized.");

  /*Leftovers of a write interrupted by a crash (<file>.tmp).*/
  persist_cleanup_tmp();

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

  /*world.dat and npcs.dat are a pair: if either is missing or invalid a
   * fresh world is generated, never a partial one.*/
  char world_path[STATE_DIR_MAX + 32], npcs_path[STATE_DIR_MAX + 32], why[96];
  bool world_ok = false, npcs_ok = false;
  if (state_path(world_path, sizeof(world_path), "world.dat") &&
      state_path(npcs_path, sizeof(npcs_path), "npcs.dat")) {
    BinFileStatus st = persist_read_world(world_path, master_world, why, sizeof(why));
    world_ok = (st == BINFILE_OK);
    if (st == BINFILE_INVALID)
      server_log("SYS", "WARNING: %s unreadable (%s).", world_path, why);
    if (world_ok) {
      int loaded = 0;
      st = persist_read_npcs(npcs_path, npcs, MAX_NPCS, &next_id, &global_total_turns,
                             &loaded, why, sizeof(why));
      npcs_ok = (st == BINFILE_OK);
      if (npcs_ok)
        server_log("SYS", "World and %d NPCs loaded from %s.", loaded, persist_state_dir());
      else if (st == BINFILE_INVALID)
        server_log("SYS", "WARNING: %s unreadable (%s).", npcs_path, why);
    }
  }

  if (world_ok && npcs_ok) {
    /*Re-bind what is not stored: the template pointer (from template_idx)
     * and the behavior derived from the archetype.*/
    for (int i = 0; i < MAX_NPCS; i++) {
      if (npcs[i].active || npcs[i].respawn_timer >= 0) {
        if (npcs[i].archetype == ARCH_TREASURE || npcs[i].archetype == ARCH_GOLD) {
          npcs[i].template = NULL;
        } else {
          int tidx = npcs[i].template_idx;
          if (tidx < 0 || tidx >= bestiary_size)
            tidx = 0;
          npcs[i].template = &bestiary_data[tidx];
          ai_attach_behavior(&npcs[i]);
        }
      }
    }
  } else {
    /*Start from a clean slate: a half-read npcs.dat may have filled slots.*/
    memset(npcs, 0, sizeof(NPC) * MAX_NPCS);
    for (int i = 0; i < MAX_NPCS; i++)
      npcs[i].respawn_timer = -1;
    next_id = 1;
    global_total_turns = 0;
    server_log("SYS", "Generating a new world in %s ...", persist_state_dir());
    world_init(master_world);
    spawn_city_merchants(npcs, &next_id);
    spawn_magic_shops(npcs, &next_id);
    spawn_martial_archive(npcs, &next_id);
    populate_dungeons(npcs, &next_id);
    if (!save_world_state(npcs)) {
      server_log("SYS", "ERROR: the new world could not be saved in %s.", persist_state_dir());
      clog_close();
      return 1;
    }
    server_log("SYS", "New world generated and saved.");
  }

  sync_entity_grid(npcs);
  floor_stats_rebuild(npcs);
  floor_index_rebuild(npcs);
  aoe_init_clouds();
  spell_router_init();
  /*From here on saves are written by the writer thread and logins are
   * checked by the login worker: neither touches the game state.*/
  writer_start();
  if (!login_worker_start()) {
    server_log("SYS", "Cannot start the login worker: %s", strerror(errno));
    clog_close();
    return 1;
  }
  ev.events = EPOLLIN;
  ev.data.u32 = EPOLL_ID_LOGIN;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, login_worker_eventfd(), &ev) != 0) {
    server_log("SYS", "epoll_ctl(login worker) failed: %s", strerror(errno));
    clog_close();
    return 1;
  }
  server_log("SYS", "Server ready!");
  /*Fixed-step game clock.
   * update_world() must run at exactly 1/TICK_MS Hz (5 rounds/s) of REAL
   * time, whatever the poll loop does: before the fix the simulation ran
   * once per loop iteration (~200/s when idle, slower under load), so
   * NPC energy — and therefore the whole game speed — followed the CPU
   * load instead of the clock. The accumulator runs at most
   * MAX_TICK_CATCHUP_MS of simulation per loop pass; a stalled system
   * drops the excess game time instead of fast-forwarding.*/
  long long last_iter_ms = 0;
  long long tick_acc_ms = 0;
  while (!g_shutdown_requested) {
    /*Timeout = TICK_MS/2 (100 ms). The old 5 ms woke the loop 200 times
     * per second even with ZERO connected clients, for no gain: the
     * simulation below runs on a fixed-step accumulator (TICK_MS) that
     * is driven by real time, so halving the wake rate (100 ms = half a
     * tick) keeps update_world() exactly as punctual while new
     * connections and player input are still serviced within half a
     * game tick.*/
    int n_events = epoll_wait(epoll_fd, ep_events, MAX_CLIENTS + 2, (int)(TICK_MS / 2));
    /*Slots whose login just completed with messages already buffered:
     * served after the epoll events, as if their socket had fired.*/
    int resume[MAX_CLIENTS];
    int n_resume = 0;
    if (n_events < 0)
      n_events = 0;
    {
      for (int ev_idx = 0; ev_idx < n_events + n_resume; ev_idx++) {
        uint32_t id = (ev_idx < n_events) ? ep_events[ev_idx].data.u32
                                          : (uint32_t)resume[ev_idx - n_events];
        uint32_t ev_mask = (ev_idx < n_events) ? ep_events[ev_idx].events : EPOLLIN;
        if (id == EPOLL_ID_LISTEN) {
          accept_pending_connections(s_sock, epoll_fd, clients);
        } else if (id == EPOLL_ID_LOGIN) {
          LoginJob *job;
          while ((job = login_worker_take()) != NULL) {
            int slot = job->slot;
            login_finish(clients, job, npcs);
            login_job_free(job);
            if (slot >= 0 && slot < MAX_CLIENTS && clients[slot].active &&
                clients[slot].authenticated && !conn_dropping(slot) &&
                conn_rx_pending(slot) && n_resume < MAX_CLIENTS)
              resume[n_resume++] = slot;
          }
        } else {
          int i = (int)id;
          if (!clients[i].active || conn_dropping(i))
            continue;
          /*While its login is being checked a slot reads nothing (its
           * EPOLLIN interest is off, see update_interest): only a hang-up
           * is acted upon.*/
          if (clients[i].login_pending) {
            if (ev_mask & (EPOLLHUP | EPOLLERR | EPOLLRDHUP))
              conn_mark_drop(i, "connection closed during login");
            continue;
          }
          /*Drain the socket, then handle every COMPLETE message already
           * buffered. Nothing here ever waits for missing bytes: a
           * partial message simply stays in the buffer until the next
           * EPOLLIN (see server_conn.h).*/
          ConnRxStatus rx = conn_read(i);
          MsgHeader hdr;
          const void *payload = NULL;
          int got = 0;
          /*A MSG_LOGIN hands the slot to the login worker: the messages
           * after it stay buffered until login_finish() (resume list).*/
          while (clients[i].active && !conn_dropping(i) && !clients[i].login_pending &&
                 (got = conn_next_message(i, &hdr, &payload)) == 1) {
          if (hdr.type == MSG_LOGIN) {
            handle_login(clients, i, payload);
          } else if (hdr.type == MSG_MOVE) {
            if (!clients[i].authenticated) {
              conn_mark_drop(i, "MSG_MOVE before login");
              continue;
            }
            MsgMove m;
            memcpy(&m, payload, sizeof(m));

            /*Position/floor BEFORE the move: the 10 KB map chunk is
             * re-sent only if the view actually moved (M7).*/
            int old_x = clients[i].x, old_y = clients[i].y;
            int old_floor = clients[i].floor_id;

            long long now_ms = get_time_ms();
            if (now_ms - clients[i].last_action_ms < 200) {
                continue; //Rate limit: max 5 actions per second
            }
            clients[i].last_action_ms = now_ms;

            //--- ACTION BLOCK DUE TO CONDITIONS ---
            if (rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_PARALYZED) ||
                rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_STUNNED) ||
                rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_PETRIFIED) ||
                rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_FROZEN) ||
                rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_UNCONSCIOUS)) {
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
                          "[%s] Welcome! Use 'buy <n>', 'sell <n>', 'haggle "
                          "buy <n>', 'identify' or 'cure'.",
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
                           npc_set_active(&npcs[n], false);
                           npcs[n].respawn_timer = 0;
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
                             npc_set_active(&npcs[n], false);
                             npcs[n].respawn_timer = 0;
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
                                 "[SYSTEM] You picked up: %s",
                                 item_database[npcs[n].ghost_loot[0].template_idx].name);
                             npc_set_active(&npcs[n], false);
                             npcs[n].respawn_timer = 0;
                             fl->entity_grid[ny][nx] = 0;
                             coll = false;
                             save_player_data(&clients[i]);
                           }
                         } else {
                           /*Chest without ghost_loot: generate random loot*/
                           send_text_to_client(clients[i].sock,
                               "[SYSTEM] You open the treasure chest and find"
                               " something inside...");
                           drop_loot_from_monster(&clients[i], &npcs[n]);
                           npc_set_active(&npcs[n], false);
                           npcs[n].respawn_timer = 100;
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
                bool can_pass = (vt != VOXEL_WALL && vt != VOXEL_ROCK);
                
                if (!can_pass && vt == VOXEL_WALL && clients[i].floor_id == 0) {
                    if (is_shop_door(nx, ny) && is_inside_shop(old_x, old_y)) {
                        can_pass = true;
                    }
                }
                
                if (can_pass) {
                  //Update grid: leave old tile, enter new
                  Floor *fl = &master_world->floors[clients[i].floor_id];
                  if (fl->entity_grid[clients[i].y][clients[i].x] == clients[i].entity_id) {
                    fl->entity_grid[clients[i].y][clients[i].x] = 0;
                  }

                  clients[i].x = nx;
                  clients[i].y = ny;
                  fl->entity_grid[ny][nx] = clients[i].entity_id;

                  /*A5: the old `global_total_turns++` here (once per player
                   *move) is GONE. It is a leftover of the pre-epoll design
                   *where a round advanced per action: with the fixed-step
                   *clock (2026.09.09) the counter is already incremented
                   *exactly 5/s in update_world() (server_world.c), so this
                   *second, per-move increment made day/night, hunger and
                   *trap respawns (all expressed in rounds) run faster the
                   *more players walked — up to 64 clients x 5 moves/s.
                   *The simulation clock must be independent of player input.*/

                  //--- BLOODY MOVEMENT EFFECT ---
                  if (rules_has_condition_t(clients[i].effects, clients[i].effect_count, COND_BLEEDING)) {
                    clients[i].hp -= 2;
                    send_text_to_client(clients[i].sock,
                                        "[DANGER] Moving reopens your"
                                        " wounds! You bleed heavily...");
                    if (clients[i].hp <= 0) {
                      server_log(
                          "DEATH",
                          "%s bled to death during the movement.",
                          clients[i].username);
                      save_bones(&clients[i]);
                      player_respawn_town(&clients[i]);
                      send_text_to_client(clients[i].sock, "[SYSTEM] You died! The Arcane has returned you to town without your equipment!");
                    }
                  }

                  /*A5: `global_total_turns++` USED to be here (see the
                   *comment at the top of the can_pass block): removed.
                   *The counter advances exactly 5/s in update_world().*/
                  update_city_doors();
                  check_tile_events(&clients[i], npcs);
                  check_traps(&clients[i], npcs);
                }
              }
            }
            broadcast_nearby_entities(&clients[i], npcs);
            send_detailed_state(&clients[i]);
            broadcast_player_state(&clients[i]);
            //Send the map chunk around the new location — but ONLY if
            //x, y or the floor actually changed (M7). Holding a key
            //against a wall used to re-ship the same 100x100 chunk 5x/s
            //(~50 KB/s wasted per player, client redrawing the same
            //map). check_tile_events/check_traps can change the floor
            //(stairs, teleports), so floor_id is compared too. The state
            //broadcasts above stay unconditional: a blocked move can
            //still end a fight (attack sets coll) or leave NPC positions
            //one tick stale. (The old two server_log("NET", ...) lines
            //per step spammed the log: up to 64 players * 5 steps/s =
            //~500 lines/second.)
            if (clients[i].x != old_x || clients[i].y != old_y ||
                clients[i].floor_id != old_floor) {
              send_map_chunk(clients[i].sock,
                             &master_world->floors[clients[i].floor_id].map,
                             clients[i].x, clients[i].y, INITIAL_VIEW_RADIUS);
            }
          } else if (hdr.type == MSG_TEXT_CMD) {
            if (!clients[i].authenticated) {
              conn_mark_drop(i, "MSG_TEXT_CMD before login");
              continue;
            }
            MsgTextCmd tc;
            memcpy(&tc, payload, sizeof(tc));
            /*Rate limit text commands exactly like MSG_MOVE: the flag was
             * written but never tested, so a client spamming 'top', 'cast'
             * or 'look' at 10 kHz saturated the server CPU (every 'top'
             * opens, reads and qsorts each save file).*/
            long long now_ms = get_time_ms();
            if (now_ms - clients[i].last_action_ms < 200) {
              continue; //Rate limit: max 5 text commands per second
            }
            clients[i].last_action_ms = now_ms;
            /*cmd is a fixed-size array, but make sure it is terminated
             * (a malicious client could omit the NUL).*/
            tc.cmd[sizeof(tc.cmd) - 1] = '\0';
            /*No control characters reach the game: they would travel
             * verbatim to other players' terminals (ANSI escapes, \r
             * overwriting lines) through chat and private messages.*/
            for (char *p = tc.cmd; *p; p++) {
              unsigned char ch = (unsigned char)*p;
              if (ch < 0x20 || ch == 0x7F)
                *p = ' ';
            }
            handle_text_cmd(&clients[i], tc.cmd, npcs);
          }
          }
          if (got < 0) {
            server_log("NET", "Protocol violation from client %d (%s): disconnect",
                       i, conn_peer_ip(i));
            conn_mark_drop(i, "protocol violation");
          } else if (rx == CONN_RX_CLOSED) {
            conn_mark_drop(i, "connection closed by peer");
          } else if (rx == CONN_RX_ERROR) {
            conn_mark_drop(i, "socket error");
          }
        }
      }
    }
    expire_pending_logins(clients);
    /*--- Fixed-step game clock (see the accumulator declaration) ---*/
    long long now_ms = get_time_ms();
    if (last_iter_ms == 0) {
      last_iter_ms = now_ms;
    }
    tick_acc_ms += now_ms - last_iter_ms;
    last_iter_ms = now_ms;
    if (tick_acc_ms > MAX_TICK_CATCHUP_MS) {
      tick_acc_ms = MAX_TICK_CATCHUP_MS; /*drop the excess, never fast-forward*/
    }
    while (tick_acc_ms >= TICK_MS) {
      update_world(clients, npcs);
      tick_acc_ms -= TICK_MS;
    }
    /*Output of this iteration: write what the kernel accepts, remove the
     * clients marked for removal (their "left floor" notices are queued
     * for the others), write again, then ask epoll for EPOLLOUT only where
     * bytes are still waiting.*/
    flush_all_clients(clients);
    reap_dropped_clients(clients);
    flush_all_clients(clients);
    update_interest(epoll_fd, clients);
    pdf_jobs_poll();
  }
  /*Graceful shutdown: logins still being checked are abandoned, queued
   * writes reach the disk, then every character, the world and the
   * artifacts are written synchronously (the periodic autosave runs once
   * a minute, so up to 60 s of progress would be lost).*/
  login_worker_stop();
  if (!writer_stop())
    server_log("SYS", "WARNING: some queued saves failed (see above).");
  for (int i = 0; i < MAX_CLIENTS; i++) {
    if (clients[i].active && clients[i].authenticated)
      save_player_data(&clients[i]);
  }
  if (save_world_state(npcs))
    server_log("SYS", "World and NPCs saved.");
  artifacts_save();
  clog_close();
  server_log("SYS", "Server shut down. Log saved.");
  return 0;
}
#endif /* DRAGONGL_NO_MAIN */
