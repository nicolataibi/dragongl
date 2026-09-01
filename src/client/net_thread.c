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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include "client_state.h"
#include "client_fct.h"
#include "client_minimap.h"
#include "client_particles.h"
#include "protocol.h"
#include "net.h"

void* net_thread_loop(void* arg) {
    MsgHeader hdr;
    MsgWelcome msg_wel;
    MsgState msg_state;
    MsgMapChunk msg_chunk;
    MsgText msg_txt;
    MsgAuthFail msg_fail;
    TileType *chunk_buf;
    int bytes;
    int chunk_size;
    int i, cx, cy;
    static int last_floor = -1;
    
    (void)arg;
    
    while (g_running) {
        bytes = net_receive(g_server_sock, &hdr, sizeof(MsgHeader));
        if (bytes > 0) {
            if (hdr.type == MSG_WELCOME) {
                if (net_receive_all(g_server_sock, &msg_wel, sizeof(MsgWelcome)) > 0) {
                    pthread_mutex_lock(&g_state_mutex);
                    g_my_entity_id = msg_wel.entity_id;
                    g_my_x = msg_wel.x; g_my_y = msg_wel.y;
                    g_my_hp = msg_wel.hp; g_my_max_hp = msg_wel.max_hp;
                    g_my_gold = msg_wel.gold;
                    g_my_level = msg_wel.level;
                    g_race_id = msg_wel.race_id;
                    g_subrace_id = msg_wel.subrace_id;
                    g_class_id = msg_wel.class_id;
                    g_alignment = msg_wel.alignment;
                    pthread_mutex_unlock(&g_state_mutex);
                    printf("\n[NET] Authentication completed!\n[STAT] HP: %d/%d | Gold: %lu\n>", msg_wel.hp, msg_wel.max_hp, (unsigned long)msg_wel.gold);
                    fflush(stdout);
                }
            } else if (hdr.type == MSG_AUTH_FAIL) {
                if (net_receive_all(g_server_sock, &msg_fail, sizeof(MsgAuthFail)) > 0) {
                    printf("\n[NET] Authentication failed: %s\n", msg_fail.reason);
                    g_running = false;
                }
            } else if (hdr.type == MSG_STATE) {
                if (net_receive_all(g_server_sock, &msg_state, sizeof(MsgState)) > 0) {
                    pthread_mutex_lock(&g_state_mutex);
                    if (last_floor != -1 && last_floor != msg_state.floor_id) {
                        for(int ly=0; ly<MAP_HEIGHT; ly++) {
                            for(int lx=0; lx<MAP_WIDTH; lx++) {
                                g_local_map[ly][lx] = VOXEL_ROCK;
                            }
                        }
                        //Cleans ALL entities on plan change
                        for(int li=0; li<MAX_NPCS; li++) {
                            g_entities[li].active = false;
                            g_entities[li].id = 0;
                            g_entities[li].floor_id = -1;
                        }
                        minimap_reset();
                    }
                    last_floor = msg_state.floor_id;
                    if (msg_state.entity_id == g_my_entity_id) {
                        g_my_x = msg_state.x; g_my_y = msg_state.y;
                        g_my_hp = msg_state.hp; g_my_max_hp = msg_state.max_hp;
                        g_game_h = msg_state.game_hour; g_game_m = msg_state.game_min;
                        g_total_turns = msg_state.total_turns;
                        g_str = msg_state.str; g_dex = msg_state.dex; g_con = msg_state.con;
                        g_intel = msg_state.intel; g_wis = msg_state.wis; g_cha = msg_state.cha;
                        g_movement_cooldown = msg_state.movement_cooldown;
                        g_equipped_mask = msg_state.equipped_mask;
                        g_my_level = msg_state.level;
                        g_my_xp = msg_state.xp;
                        g_my_gold = msg_state.gold;
                        g_my_ac = msg_state.ac;
                        g_my_floor = msg_state.floor_id;
                        g_vision_radius = msg_state.vision_radius;
                        strncpy(g_weapon_name, msg_state.weapon_name, 31);
                        strncpy(g_armor_name,  msg_state.armor_name,  31);
                        g_to_hit = msg_state.to_hit;
                        g_to_dmg = msg_state.to_dmg;
                        g_bosses_defeated = msg_state.bosses_defeated;
                        g_status_icons = msg_state.status_icons;
                        g_hunger_level = msg_state.hunger_level;
                        for (int i = 0; i < 10; i++) {
                            g_my_spell_slots[i] = msg_state.spell_slots[i];
                            g_my_spell_slots_max[i] = msg_state.spell_slots_max[i];
                        }
                        strncpy(g_eq_head,   msg_state.eq_head,   31);
                        strncpy(g_eq_neck,   msg_state.eq_neck,   31);
                        strncpy(g_eq_body,   msg_state.eq_body,   31);
                        strncpy(g_eq_back,   msg_state.eq_back,   31);
                        strncpy(g_eq_hand_r, msg_state.eq_hand_r, 31);
                        strncpy(g_eq_hand_l, msg_state.eq_hand_l, 31);
                        strncpy(g_eq_hands,  msg_state.eq_hands,  31);
                        strncpy(g_eq_arm_r, msg_state.eq_arm_r, 31);
                        strncpy(g_eq_arm_l, msg_state.eq_arm_l, 31);
                        strncpy(g_eq_feet,   msg_state.eq_feet,   31);
                        for (int i = 0; i < 10; i++) {
                            strncpy(g_eq_ring[i], msg_state.eq_ring[i], 31);
                            g_eq_ring[i][31] = '\0';
                        }
                        for (int i = 0; i < 4; i++) {
                            strncpy(g_eq_belt[i], msg_state.eq_belt[i], 31);
                            g_eq_belt[i][31] = '\0';
                        }
                    } else {
                        //It's an NPC, tombstone, or other player
                        if (msg_state.floor_id != g_my_floor) {
                            pthread_mutex_unlock(&g_state_mutex);
                            continue;
                        }
                        if (msg_state.hp <= 0) {
                            //The entity died or left this floor (stairs,
                            //teleport, disconnect): remove it, so it no
                            //longer renders (shared by GL and Vulkan)
                            for(int i=0; i<MAX_NPCS; i++) {
                                if (g_entities[i].id == msg_state.entity_id) {
                                    g_entities[i].active   = false;
                                    g_entities[i].id       = 0;
                                    g_entities[i].floor_id = -1;
                                    break;
                                }
                            }
                            pthread_mutex_unlock(&g_state_mutex);
                            continue;
                        }
                        bool found = false;
                        for(int i=0; i<MAX_NPCS; i++) {
                            if (g_entities[i].id == msg_state.entity_id) {
                                g_entities[i].x           = msg_state.x;
                                g_entities[i].y           = msg_state.y;
                                g_entities[i].hp          = msg_state.hp;
                                g_entities[i].floor_id    = msg_state.floor_id;
                                g_entities[i].active      = true;
                                g_entities[i].is_merchant  = (msg_state.is_merchant  != 0);
                                g_entities[i].shop_spec    = msg_state.shop_spec;
                                g_entities[i].is_tombstone = (msg_state.is_tombstone != 0);
                                g_entities[i].is_player = (msg_state.is_player != 0);
                                strncpy(g_entities[i].username, msg_state.username, 31);
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            for(int i=0; i<MAX_NPCS; i++) {
                                if (!g_entities[i].active) {
                                    g_entities[i].id          = msg_state.entity_id;
                                    g_entities[i].x           = msg_state.x;
                                    g_entities[i].y           = msg_state.y;
                                    g_entities[i].hp          = msg_state.hp;
                                    g_entities[i].floor_id    = msg_state.floor_id;
                                    g_entities[i].active      = true;
                                    g_entities[i].is_merchant  = (msg_state.is_merchant  != 0);
                                    g_entities[i].shop_spec    = msg_state.shop_spec;
                                    g_entities[i].is_tombstone = (msg_state.is_tombstone != 0);
                                g_entities[i].is_player = (msg_state.is_player != 0);
                                    strncpy(g_entities[i].username, msg_state.username, 31);
                                    break;
                                }
                            }
                        }
                    }
                    pthread_mutex_unlock(&g_state_mutex);

                }
            } else if (hdr.type == MSG_MAP_CHUNK) {
                if (net_receive_all(g_server_sock, &msg_chunk, sizeof(MsgMapChunk)) > 0) {
                    chunk_size = msg_chunk.width * msg_chunk.height * sizeof(TileType);
                    chunk_buf = malloc(chunk_size);
                    if (net_receive_all(g_server_sock, chunk_buf, chunk_size) > 0) {
                        pthread_mutex_lock(&g_state_mutex);
                        i = 0;
                        for (cy = msg_chunk.start_y; cy < msg_chunk.start_y + msg_chunk.height; cy++) {
                            for (cx = msg_chunk.start_x; cx < msg_chunk.start_x + msg_chunk.width; cx++) {
                                if (cx >= 0 && cx < MAP_WIDTH && cy >= 0 && cy < MAP_HEIGHT) g_local_map[cy][cx] = chunk_buf[i];
                                i++;
                            }
                        }
                        pthread_mutex_unlock(&g_state_mutex);
                    }
                    free(chunk_buf);
                }
            } else if (hdr.type == MSG_SPELL_VFX) {
                MsgSpellVFX msg_vfx;
                if (net_receive_all(g_server_sock, &msg_vfx, sizeof(MsgSpellVFX)) > 0) {
                    spawn_vfx(msg_vfx.vfx_type, (float)msg_vfx.start_x, (float)msg_vfx.start_y, (float)msg_vfx.target_x, (float)msg_vfx.target_y, msg_vfx.color_r, msg_vfx.color_g, msg_vfx.color_b);
                }
            } else if (hdr.type == MSG_TEXT) {
                if (net_receive_all(g_server_sock, &msg_txt, sizeof(MsgText)) > 0) {
                    client_log_add(msg_txt.text);
                    fct_parse_log(msg_txt.text);
                    printf("\r\033[K%s\n> ", msg_txt.text); fflush(stdout);
                }
            } else if (hdr.type == MSG_TIME_SYNC) {
                MsgTimeSync ts;
                if (net_receive_all(g_server_sock, &ts, sizeof(MsgTimeSync)) > 0) {
                    pthread_mutex_lock(&g_state_mutex);
                    g_game_h = ts.game_hour;
                    g_game_m = ts.game_min;
                    g_total_turns = ts.total_turns;
                    pthread_mutex_unlock(&g_state_mutex);
                }
            } else if (hdr.type == MSG_TOMBSTONE_REMOVE) {
                MsgTombstoneRemove rm_msg;
                if (net_receive_all(g_server_sock, &rm_msg, sizeof(MsgTombstoneRemove)) > 0) {
                    pthread_mutex_lock(&g_state_mutex);
                    for (int i = 0; i < MAX_NPCS; i++) {
                        if (g_entities[i].active &&
                            g_entities[i].id == rm_msg.entity_id &&
                            g_entities[i].is_tombstone) {
                            g_entities[i].active      = false;
                            g_entities[i].is_tombstone = false;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&g_state_mutex);
                }
            }
        } else if (bytes == 0) {
            printf("\n[NET] Server disconnected.\n");
            g_running = false;
        } else {
            usleep(10000);
        }
    }
    return NULL;
}
