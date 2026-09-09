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
bool g_map_dirty = false;
#include "client_minimap.h"
#include "client_particles.h"
#include "protocol.h"
#include "net.h"

/*True if the message type/payload size pair is coherent.
 * A mismatched length (buggy or malicious server) would desync the whole
 * protocol stream — every following message would be parsed as garbage.
 * On mismatch the client disconnects instead of rendering garbage.*/
static bool valid_msg(MsgType type, int length) {
    switch (type) {
    case MSG_WELCOME:          return length == sizeof(MsgWelcome);
    case MSG_STATE:            return length == sizeof(MsgState);
    case MSG_MAP_CHUNK:        return (size_t)length >= sizeof(MsgMapChunk); /*+ w*h*VoxelType, checked later*/
    case MSG_TEXT_CMD:         return length == sizeof(MsgTextCmd);
    case MSG_TEXT:             return length == sizeof(MsgText);
    case MSG_AUTH_FAIL:        return length == sizeof(MsgAuthFail);
    case MSG_SPELL_VFX:        return length == sizeof(MsgSpellVFX);
    case MSG_TOMBSTONE_REMOVE: return length == sizeof(MsgTombstoneRemove);
    case MSG_TIME_SYNC:        return length == sizeof(MsgTimeSync);
    /*[int32 count][EntityUpdateRec count]: the exact size is validated
     * against count in the handler (count comes from the payload).*/
    case MSG_ENTITY_UPDATE:    return length >= (int)sizeof(int32_t);
    default:                    return false;
    }
}

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
        /*net_receive_exact: a raw recv() could return a PARTIAL header
         * (fewer than sizeof(MsgHeader) bytes), which used to be treated
         * as a complete (corrupted) header.*/
        bytes = net_receive_exact(g_server_sock, &hdr, sizeof(MsgHeader));
        if (bytes > 0) {
            /*Version first: a mismatched build (old client + new server)
             * must fail fast, not parse garbage for a while.*/
            if (hdr.version != PROTOCOL_VERSION) {
                fprintf(stderr, "[NET] Protocol error: server speaks protocol version %u, this client expects %u — disconnecting.\n",
                        hdr.version, PROTOCOL_VERSION);
                g_running = false;
                break;
            }
            if (!valid_msg(hdr.type, hdr.length)) {
                fprintf(stderr, "[NET] Protocol error: bad message (type %u, length %d) — disconnecting.\n",
                        hdr.type, hdr.length);
                g_running = false;
                break;
            }
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
                        for(int li=0; li<CLIENT_MAX_ENTITIES; li++) {
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
                        copy_str(g_weapon_name, msg_state.weapon_name, sizeof(g_weapon_name));
                        copy_str(g_armor_name, msg_state.armor_name, sizeof(g_armor_name));
                        g_to_hit = msg_state.to_hit;
                        g_to_dmg = msg_state.to_dmg;
                        g_bosses_defeated = msg_state.bosses_defeated;
                        g_status_icons = msg_state.status_icons;
                        g_hunger_level = msg_state.hunger_level;
                        for (int i = 0; i < 10; i++) {
                            g_my_spell_slots[i] = msg_state.spell_slots[i];
                            g_my_spell_slots_max[i] = msg_state.spell_slots_max[i];
                        }
                        copy_str(g_eq_head, msg_state.eq_head, sizeof(g_eq_head));
                        copy_str(g_eq_neck, msg_state.eq_neck, sizeof(g_eq_neck));
                        copy_str(g_eq_body, msg_state.eq_body, sizeof(g_eq_body));
                        copy_str(g_eq_back, msg_state.eq_back, sizeof(g_eq_back));
                        copy_str(g_eq_hand_r, msg_state.eq_hand_r, sizeof(g_eq_hand_r));
                        copy_str(g_eq_hand_l, msg_state.eq_hand_l, sizeof(g_eq_hand_l));
                        copy_str(g_eq_hands, msg_state.eq_hands, sizeof(g_eq_hands));
                        copy_str(g_eq_arm_r, msg_state.eq_arm_r, sizeof(g_eq_arm_r));
                        copy_str(g_eq_arm_l, msg_state.eq_arm_l, sizeof(g_eq_arm_l));
                        copy_str(g_eq_feet, msg_state.eq_feet, sizeof(g_eq_feet));
                        for (int i = 0; i < 10; i++) {
                            copy_str(g_eq_ring[i], msg_state.eq_ring[i], sizeof(g_eq_ring[0]));
                        }
                        for (int i = 0; i < 4; i++) {
                            copy_str(g_eq_belt[i], msg_state.eq_belt[i], sizeof(g_eq_belt[0]));
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
                            for(int i=0; i<CLIENT_MAX_ENTITIES; i++) {
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
                        for(int i=0; i<CLIENT_MAX_ENTITIES; i++) {
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
                                copy_str(g_entities[i].username, msg_state.username, sizeof(g_entities[i].username));
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            /*FEATURE LIMIT (L5, see CLIENT_MAX_ENTITIES in
                             * client_state.h): if all 512 cache slots are
                             * active, there is NO free slot and this new
                             * entity is SILENTLY DROPPED — it simply never
                             * renders on this client. Acceptable for a LAN
                             * game (planes rarely exceed a few dozen visible
                             * entities, and the cache is flushed whole on
                             * floor change), but documented on purpose: a
                             * "missing NPC/player on a packed plane" report
                             * means the cache overflowed, not a network
                             * bug.*/
                            for(int i=0; i<CLIENT_MAX_ENTITIES; i++) {
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
                                    copy_str(g_entities[i].username, msg_state.username, sizeof(g_entities[i].username));
                                    break;
                                }
                            }
                        }
                    }
                    pthread_mutex_unlock(&g_state_mutex);

                }
            } else if (hdr.type == MSG_ENTITY_UPDATE) {
                /*Compact batch: [int32 count][EntityUpdateRec count].
                 * It only ever carries entities for which the server
                 * ALREADY sent a full MsgState (per-client "seen" bitmap
                 * server-side), so this handler updates x/y/hp of known
                 * ids; an unknown id is ignored by design (the full
                 * state always arrives first). This replaces the old
                 * one-MsgState-per-entity-per-step flood (~1.5 KB each).
                 * count and the total length are sanity-checked BEFORE
                 * the malloc, exactly like the map chunk.*/
                int32_t count = 0;
                if (net_receive_all(g_server_sock, &count, sizeof(count)) > 0 &&
                    count > 0 && count <= 4096 &&
                    hdr.length == (int)(sizeof(count) + (size_t)count * sizeof(EntityUpdateRec))) {
                    EntityUpdateRec *recs = malloc((size_t)count * sizeof(EntityUpdateRec));
                    if (recs) {
                        if (net_receive_all(g_server_sock, recs,
                                            (size_t)count * sizeof(EntityUpdateRec)) > 0) {
                            pthread_mutex_lock(&g_state_mutex);
                            for (int k = 0; k < count; k++) {
                                for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
                                    if (g_entities[i].id == recs[k].entity_id) {
                                        g_entities[i].x = recs[k].x;
                                        g_entities[i].y = recs[k].y;
                                        g_entities[i].hp = recs[k].hp;
                                        g_entities[i].active = true;
                                        break;
                                    }
                                }
                            }
                            pthread_mutex_unlock(&g_state_mutex);
                        } else {
                            /*partial batch: the stream is desynced*/
                            g_running = false;
                        }
                        free(recs);
                    }
                } else {
                    fprintf(stderr, "[NET] Protocol error: bad entity batch (count %d, len %d) — disconnecting.\n",
                            count, hdr.length);
                    g_running = false;
                }
                if (!g_running) break;
            } else if (hdr.type == MSG_MAP_CHUNK) {
                if (net_receive_all(g_server_sock, &msg_chunk, sizeof(MsgMapChunk)) > 0) {
                    /*Sanity-check the chunk BEFORE malloc: width/height
                     * come from the server and drive the allocation — an
                     * invalid (or malicious) value could request a huge
                     * block or make the receive loop spin.*/
                    if (msg_chunk.width <= 0 || msg_chunk.height <= 0 ||
                        msg_chunk.width > 256 || msg_chunk.height > 256 ||
                        msg_chunk.start_x < 0 || msg_chunk.start_y < 0) {
                        fprintf(stderr, "[NET] Protocol error: invalid map chunk dimensions — disconnecting.\n");
                        g_running = false;
                        break;
                    }
                    chunk_size = msg_chunk.width * msg_chunk.height * (int)sizeof(TileType);
                    if (hdr.length != (int)(sizeof(MsgMapChunk) + chunk_size)) {
                        fprintf(stderr, "[NET] Protocol error: map chunk length %d != expected %d — disconnecting.\n",
                                hdr.length, (int)(sizeof(MsgMapChunk) + chunk_size));
                        g_running = false;
                        break;
                    }
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
                        g_map_dirty = true;
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
                    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
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
