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
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "client_state.h"
#include "net.h"
#include "render_gl.h"
#include "render_vk.h"
#include "protocol.h"
#include "rules.h"
#include "species.h"
#include "classes.h"
#include "alignment.h"
#include <time.h>

int g_server_sock = -1;
int g_my_entity_id = -1;
int g_my_x = 0;
int g_my_y = 0;
int g_my_hp = 20, g_my_max_hp = 20;
Entity g_entities[CLIENT_MAX_ENTITIES];
int g_game_h = 8, g_game_m = 0, g_total_turns = 0;
int g_str=10, g_dex=10, g_con=10, g_intel=10, g_wis=10, g_cha=10;
int g_equipped_mask = 0;
int g_my_level = 1, g_my_xp = 0, g_my_floor = 0;
uint64_t g_my_gold = 0;
int g_my_ac = 10;
int g_vision_radius = 4;
float g_movement_cooldown = 0.2f;
char g_weapon_name[32] = "Unarmed";
char g_armor_name[32] = "None";
int g_to_hit = 0;
int g_to_dmg = 0;
int g_race_id = 0;
int g_subrace_id = -1;
int g_class_id = 0;
int g_alignment = 0;
uint32_t g_bosses_defeated = 0;
uint32_t g_status_icons = 0;
int g_hunger_level = 0;
int g_my_spell_slots[10] = {0};
int g_my_spell_slots_max[10] = {0};
char g_eq_head[32]   = {0};
char g_eq_neck[32]   = {0};
char g_eq_body[32]   = {0};
char g_eq_back[32]   = {0};
char g_eq_hand_r[32] = {0};
char g_eq_hand_l[32] = {0};
char g_eq_hands[32]  = {0};
char g_eq_arm_r[32] = {0};
char g_eq_arm_l[32] = {0};
char g_eq_feet[32]   = {0};
char g_eq_ring[10][32] = {{0}};
char g_eq_belt[4][32] = {{0}};
char g_log_lines[MAX_LOG_LINES][256];
int g_log_count = 0;
/*NOTE: the old `World g_world` client-side copy (~50 MB) was removed:
 * the client renders g_local_map, which the server fills via
 * MSG_MAP_CHUNK — a full local World was never needed.*/
TileType g_local_map[MAP_HEIGHT][MAP_WIDTH];
pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_net_mutex = PTHREAD_MUTEX_INITIALIZER;
/*See the declaration in client_state.h: atomic because the render,
 * net and CLI threads all read/write it (L1).*/
atomic_bool g_running = true;
int g_backend = 0; // 0 = GL, 1 = VK

/*See FrameSnapshot in client_state.h: short lock, then the renderers
 * work on the private copy for the whole frame.*/
void frame_snapshot_acquire(FrameSnapshot *snap) {
    pthread_mutex_lock(&g_state_mutex);
    snap->my_x = g_my_x;
    snap->my_y = g_my_y;
    snap->my_entity_id = g_my_entity_id;
    snap->my_floor = g_my_floor;
    snap->vision_radius = g_vision_radius;
    snap->movement_cooldown = g_movement_cooldown;
    memcpy(snap->map, g_local_map, sizeof(snap->map));
    memcpy(snap->entities, g_entities, sizeof(snap->entities));
    pthread_mutex_unlock(&g_state_mutex);
}

extern void* net_thread_loop(void* arg);
extern void* cli_thread_loop(void* arg);
extern void render_gl_start(void);

void client_send_move(int dx, int dy) {
    MsgHeader hdr = msg_hdr(MSG_MOVE, (int)sizeof(MsgMove));
    MsgMove msg_move;
    
    if (g_my_entity_id == -1) return;
    
    msg_move.entity_id = g_my_entity_id;
    msg_move.dx = dx;
    msg_move.dy = dy;
    
    pthread_mutex_lock(&g_net_mutex);
    net_send(g_server_sock, &hdr, sizeof(MsgHeader));
    net_send(g_server_sock, &msg_move, sizeof(MsgMove));
    pthread_mutex_unlock(&g_net_mutex);
}

void client_send_text_cmd(const char *cmd) {
    MsgHeader hdr = msg_hdr(MSG_TEXT_CMD, (int)sizeof(MsgTextCmd));
    MsgTextCmd msg_cmd;
    
    memset(&msg_cmd, 0, sizeof(msg_cmd));
    strncpy(msg_cmd.cmd, cmd, sizeof(msg_cmd.cmd) - 1);
    
    pthread_mutex_lock(&g_net_mutex);
    net_send(g_server_sock, &hdr, sizeof(MsgHeader));
    net_send(g_server_sock, &msg_cmd, sizeof(MsgTextCmd));
    pthread_mutex_unlock(&g_net_mutex);
}

void client_log_add(const char *text) {
    pthread_mutex_lock(&g_state_mutex);
    if (g_log_count < MAX_LOG_LINES) {
        copy_str(g_log_lines[g_log_count], text, sizeof(g_log_lines[0]));
        g_log_count++;
    } else {
        // Shift lines up (copy_str guarantees NUL termination)
        for (int i = 0; i < MAX_LOG_LINES - 1; i++) {
            copy_str(g_log_lines[i], g_log_lines[i+1], sizeof(g_log_lines[0]));
        }
        copy_str(g_log_lines[MAX_LOG_LINES - 1], text, sizeof(g_log_lines[0]));
    }
    pthread_mutex_unlock(&g_state_mutex);
}

int main(int argc, char **argv) {
    pthread_t net_thread;
    pthread_t cli_thread;
    /*The client does NOT generate a world at startup: the old
     * world_init() built all 101 floors + traps (~73 MB, seconds of CPU)
     * only to copy floor 0 into g_local_map. The map arrives from the
     * server as MSG_MAP_CHUNK packets, so start with solid rock.*/
    for (int y = 0; y < MAP_HEIGHT; y++)
        for (int x = 0; x < MAP_WIDTH; x++)
            g_local_map[y][x] = VOXEL_ROCK;

    if (argc < 2) {
        printf("Usage: %s [gl|vk] [server_ip] [port]\n", argv[0]);
        return 1;
    }

    char server_ip[64] = "127.0.0.1";
    int server_port = 8080;
    bool ip_from_arg = false;
    if (argc >= 3 && argv[2][0] != '\0') {
        copy_str(server_ip, argv[2], sizeof(server_ip));
        ip_from_arg = true;
    }
    if (argc >= 4) {
        int p = atoi(argv[3]);
        if (p > 0 && p < 65536)
            server_port = p;
    }

    char server_pass[64] = "";
    char username[32] = "";
    char password[32] = "";
    int is_new = 0;
    int race_id = 0;
    int class_id = 0;
    char buf[128];

    printf("\n--- DND GL Client ---\n");
    if (ip_from_arg) {
        printf("Server IP: %s (from command line)\n", server_ip);
    } else {
        printf("Server IP [%s]: ", server_ip);
        if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n') {
            sscanf(buf, "%63s", server_ip);
        }
    }
    printf("Server Password: ");
    if (fgets(buf, sizeof(buf), stdin)) {
        sscanf(buf, "%63s", server_pass);
    }
    printf("Player Name: ");
    if (fgets(buf, sizeof(buf), stdin)) {
        sscanf(buf, "%31s", username);
    }
    printf("Personal Password: ");
    if (fgets(buf, sizeof(buf), stdin)) {
        sscanf(buf, "%31s", password);
    }
    printf("New character? (1=Yes, 0=No):");
    if (fgets(buf, sizeof(buf), stdin)) {
        sscanf(buf, "%d", &is_new);
    }
    /*final_age/height/weight/social: cosmetic flavour rolls only (the
     * server never uses them; the ability scores are rolled server-side,
     * M9).*/
    int final_age=20, final_height=170, final_weight=70, final_social=50;
    char sex_char = 'M';

    if (is_new) {
        printf("\nAvailable Races:\n");
        for (int i=0; i<RACE_COUNT; i++) {
            printf("  %2d = %-16s : %s\n", i, RACES[i].name, RACES[i].description);
        }
        printf("Race ID: ");
        if (fgets(buf, sizeof(buf), stdin)) {
            sscanf(buf, "%d", &race_id);
        }
        if (race_id < 0 || race_id >= RACE_COUNT) race_id = 0;

        int subrace_id = -1;
        bool has_subraces = false;
        printf("\nSubraces available for %s:\n", RACES[race_id].name);
        for (int i = 0; i < SUBRACE_COUNT; i++) {
            if (SUBRACES[i].parent_race == (RaceType)race_id) {
                printf("  %2d = %-16s : %s\n", i, SUBRACES[i].name, SUBRACES[i].description);
                has_subraces = true;
            }
        }
        if (has_subraces) {
            printf("Choose Subrace (ID) or -1 for none: ");
            if (fgets(buf, sizeof(buf), stdin)) {
                sscanf(buf, "%d", &subrace_id);
            }
            if (subrace_id >= 0 && subrace_id < SUBRACE_COUNT) {
                if (SUBRACES[subrace_id].parent_race != (RaceType)race_id) subrace_id = -1;
            } else {
                subrace_id = -1;
            }
        }
        g_subrace_id = subrace_id;

        printf("\nAvailable Classes:\n");
        for (int i=0; i<CLASS_COUNT; i++) {
            printf("  %2d = %-12s : %s\n", i, CLASSES[i].name, CLASSES[i].description);
        }
        printf("Class ID: ");
        if (fgets(buf, sizeof(buf), stdin)) {
            sscanf(buf, "%d", &class_id);
        }
        printf("Sex (M=Male, F=Female): ");
        if (fgets(buf, sizeof(buf), stdin)) {
            if (buf[0] == 'f' || buf[0] == 'F') sex_char = 'F';
            else sex_char = 'M';
        }
        int alignment = 0;
        printf("Alignment:\n");
        for (int i=0; i<ALIGN_COUNT; i++) {
            printf("  %d=%-16s", i, ALIGNMENTS[i].name);
            if ((i+1)%3 == 0) printf("\n");
        }
        printf("Choice: ");
        if (fgets(buf, sizeof(buf), stdin)) {
            sscanf(buf, "%d", &alignment);
            if (alignment < 0 || alignment >= ALIGN_COUNT) alignment = 0;
        }
        if (class_id < 0 || class_id >= CLASS_COUNT) class_id = 0;
        g_race_id = race_id;
        g_class_id = class_id;
        g_alignment = alignment;

        /*Ability scores are rolled by the SERVER at login (M9): the old
        * local 3d6 + reroll loop meant an edited client simply sent
        * 18/18/18/18/18/18. Only the cosmetic flavour rolls (age, height,
        * weight, social status) are local - the server never uses them.
        * The rolled STR/DEX/CON/INT/WIS/CHA arrive in-game as a
        * [CHARACTER] message right after the welcome.*/
        srand(time(NULL));
        final_age    = rules_roll_dice(3, 6) + 15;
        final_height = rules_roll_dice(4, 10) + 140;
        final_weight = rules_roll_dice(4, 10) + 50;
        final_social = rules_roll_dice(1, 100);

        char full_race[64];
        if (g_subrace_id != -1) {
            snprintf(full_race, sizeof(full_race), "%s (%s)",
                     RACES[race_id].name, SUBRACES[g_subrace_id].name);
        } else {
            strncpy(full_race, RACES[race_id].name, sizeof(full_race));
        }

        printf("\n========================================================================\n");
        printf(" Name        : %-22s Age          : %5d\n", username, final_age);
        printf(" Race        : %-22s Height (cm)  : %5d\n", full_race, final_height);
        printf(" Sex         : %-22s Weight (kg)  : %5d\n", sex_char == 'M' ? "Male" : "Female", final_weight);
        printf(" Class       : %-22s Social Status: %5d\n", CLASSES[class_id].name, final_social);
        printf("Alignment : %-46s\n", ALIGNMENTS[alignment].name);
        printf(" STR/DEX/CON/INT/WIS/CHA: rolled by the server - see the\n");
        printf(" [CHARACTER] message after login.\n\n");
        printf(" Traits      : %s%s%s\n\n", RACES[race_id].traits,
               (g_subrace_id != -1 ? ", " : ""),
               (g_subrace_id != -1 ? SUBRACES[g_subrace_id].traits : ""));
        printf("========================================================================\n");
    }

    // --- Final character summary (flavour only: the ability scores are
    // rolled by the server, see the [CHARACTER] line after login) ---
    if (is_new) {
        char full_race_f[64];
        if (g_subrace_id != -1) {
            snprintf(full_race_f, sizeof(full_race_f), "%s (%s)",
                     RACES[race_id].name, SUBRACES[g_subrace_id].name);
        } else {
            strncpy(full_race_f, RACES[race_id].name, sizeof(full_race_f));
        }

        printf("\n===========================================================================\n");
        printf(" Name        : %-23s  Age          : %5d\n", username, final_age);
        printf(" Race        : %-23s  Height       : %5d\n", full_race_f, final_height);
        printf(" Sex         : %-23s  Weight       : %5d\n", sex_char == 'M' ? "Male" : "Female", final_weight);
        printf(" Class       : %-23s  Social Status: %5d\n", CLASSES[class_id].name, final_social);
        printf("Alignment   : %-23s\n", ALIGNMENTS[g_alignment].name);
        printf(" Ability scores are rolled by the server: watch for the\n");
        printf(" [CHARACTER] line after you connect.\n");
        printf("===========================================================================\n\n");
    }


    printf("Connecting to server %s:%d...\n", server_ip, server_port);
    g_server_sock = net_connect_to_server(server_ip, server_port);
    
    if (g_server_sock < 0) {
        printf("Connection error.\n");
        return 1;
    }
    
    {
        MsgHeader login_hdr = msg_hdr(MSG_LOGIN, (int)sizeof(MsgLogin));
        MsgLogin msg_log;
        memset(&msg_log, 0, sizeof(msg_log));
        strncpy(msg_log.username, username, 31);
        strncpy(msg_log.password, password, 31);
        strncpy(msg_log.server_pass, server_pass, 31);
        msg_log.is_new_char = is_new;
        msg_log.race_id = race_id;
        msg_log.subrace_id = g_subrace_id;
        msg_log.class_id = class_id;
        /*Stats are rolled server-side (M9): send zeros - the
        * server ignores them for new characters and never
        * reads them for existing ones.*/
        msg_log.str = 0; msg_log.dex = 0; msg_log.con = 0;
        msg_log.intel = 0; msg_log.wis = 0; msg_log.cha = 0;
        msg_log.age = final_age; msg_log.height = final_height; msg_log.weight = final_weight;
        msg_log.social_class = final_social; msg_log.alignment = g_alignment;
        g_race_id = race_id;
        g_class_id = class_id;
        
        net_send(g_server_sock, &login_hdr, sizeof(MsgHeader));
        net_send(g_server_sock, &msg_log, sizeof(MsgLogin));
    }
    
    net_set_nonblocking(g_server_sock);
    
    pthread_create(&net_thread, NULL, net_thread_loop, NULL);
    pthread_create(&cli_thread, NULL, cli_thread_loop, NULL);
    
    if (strcmp(argv[1], "gl") == 0) {
        printf("Starting OpenGL backend...\n");
        g_backend = 0;
        render_gl_start();
    } else if (strcmp(argv[1], "vk") == 0) {
        g_backend = 1;
        render_vk_start();
        // Loop handled inside render_vk_start until window close
        g_running = false; // exit after rendering loop finishes
    } else {
        printf("Backend unknown. Usage: gl or vk.\n");
        g_running = false;
    }
    
    pthread_join(cli_thread, NULL);
    pthread_join(net_thread, NULL);
    
    net_close(g_server_sock);
    /* Cleanup Dungeon */
     
    
    return 0;
}
