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

#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "map.h"

/*Client-side entity cache size.
 * NOTE: the server has its OWN MAX_NPCS (50000) in server_entities.h —
 * same name, different meaning, in a different binary. Renamed here to
 * avoid confusion when reading both codebases.
 * FEATURE LIMIT (documented, L5): the cache holds AT MOST 512 entities.
 * On a crowded plane, NEW entities that arrive while every slot is in
 * use are silently dropped (they simply never render on this client —
 * no error, no log). The whole cache is flushed on floor change, so
 * the effect is transient; if a full plane ever becomes a feature,
 * this constant is the knob to raise.*/
#define CLIENT_MAX_ENTITIES 512

extern int g_server_sock;
extern int g_my_entity_id;
extern int g_my_x;
extern int g_my_y;
typedef struct {
    int id;
    int x, y, hp;
    int floor_id;
    bool active;
    bool is_merchant;
    int shop_spec;   /*Merchant specialization (SHOP_SPEC_* from protocol.h)*/
    bool is_tombstone;
    bool is_player;
    char username[32];
} Entity;

extern Entity g_entities[CLIENT_MAX_ENTITIES];
extern int g_my_hp, g_my_max_hp;
extern int g_game_h, g_game_m, g_total_turns;
extern int g_str, g_dex, g_con, g_intel, g_wis, g_cha;
extern int g_equipped_mask;
extern int g_my_level, g_my_xp, g_my_floor;
extern uint64_t g_my_gold;
extern int g_my_ac;
extern int g_vision_radius;
extern float g_movement_cooldown;
extern char g_weapon_name[32];
extern char g_armor_name[32];
extern int g_to_hit;
extern int g_to_dmg;
extern int g_race_id;
extern int g_subrace_id;
extern int g_class_id;
extern int g_alignment;
extern uint32_t g_bosses_defeated;  //bitmask of defeated bosses
extern uint32_t g_status_icons;     // bitmask of active status conditions
extern int g_hunger_level;          // current hunger (vitality) level
extern int g_my_spell_slots[10];
extern int g_my_spell_slots_max[10];
extern char g_eq_head[32];
extern char g_eq_neck[32];
extern char g_eq_body[32];
extern char g_eq_back[32];
extern char g_eq_hand_r[32];
extern char g_eq_hand_l[32];
extern char g_eq_arm_r[32];
extern char g_eq_arm_l[32];
extern char g_eq_hands[32];
extern char g_eq_feet[32];
extern char g_eq_ring[10][32];
extern char g_eq_belt[4][32];
extern TileType g_local_map[MAP_HEIGHT][MAP_WIDTH];

/*Per-frame snapshot of the game state (map + entities + player) used by
 * BOTH renderers: copy it under a SHORT g_state_mutex lock, then render
 * from the snapshot WITHOUT the lock. Before this, each renderer held
 * g_state_mutex for the entire 3D scene, so the net thread (map chunks,
 * entity moves) was blocked for the whole duration of the frame.*/
typedef struct {
    int my_x, my_y;
    int my_entity_id;
    int my_floor;
    int vision_radius;
    TileType map[MAP_HEIGHT][MAP_WIDTH];
    Entity entities[CLIENT_MAX_ENTITIES];
} FrameSnapshot;
void frame_snapshot_acquire(FrameSnapshot *snap); /*lock, copy, unlock*/

#define MAX_LOG_LINES 8
extern char g_log_lines[MAX_LOG_LINES][256];
extern int g_log_count;

extern pthread_mutex_t g_state_mutex;
extern pthread_mutex_t g_net_mutex;
/*Shared by THREE threads (render loop, net thread, CLI thread): a plain
 * bool written/read across threads is a data race (it "works" in
 * practice, but -fsanitize=thread flags it). It is therefore an atomic
 * (L1). C11 atomics need no special syntax at the use sites:
 * `while (g_running)` is an atomic load, `g_running = false` an atomic
 * store — the only change is the declaration.*/
extern atomic_bool g_running;

void client_send_move(int dx, int dy);
void client_send_text_cmd(const char *cmd);
void client_log_add(const char *text);

#endif // CLIENT_STATE_H
extern bool g_map_dirty;
