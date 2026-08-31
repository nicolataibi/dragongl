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
Entity g_entities[MAX_NPCS];
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
World g_world;
TileType g_local_map[MAP_HEIGHT][MAP_WIDTH];
pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_net_mutex = PTHREAD_MUTEX_INITIALIZER;
bool g_running = true;
int g_backend = 0; // 0 = GL, 1 = VK

extern void* net_thread_loop(void* arg);
extern void* cli_thread_loop(void* arg);
extern void render_gl_start(void);

void client_send_move(int dx, int dy) {
    MsgHeader hdr;
    MsgMove msg_move;
    
    if (g_my_entity_id == -1) return;
    
    hdr.type = MSG_MOVE;
    hdr.length = sizeof(MsgMove);
    
    msg_move.entity_id = g_my_entity_id;
    msg_move.dx = dx;
    msg_move.dy = dy;
    
    pthread_mutex_lock(&g_net_mutex);
    net_send(g_server_sock, &hdr, sizeof(MsgHeader));
    net_send(g_server_sock, &msg_move, sizeof(MsgMove));
    pthread_mutex_unlock(&g_net_mutex);
}

void client_send_text_cmd(const char *cmd) {
    MsgHeader hdr;
    MsgTextCmd msg_cmd;
    
    hdr.type = MSG_TEXT_CMD;
    hdr.length = sizeof(MsgTextCmd);
    
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
        strncpy(g_log_lines[g_log_count], text, 255);
        g_log_count++;
    } else {
        // Shift lines up
        for (int i = 0; i < MAX_LOG_LINES - 1; i++) {
            strncpy(g_log_lines[i], g_log_lines[i+1], 255);
        }
        strncpy(g_log_lines[MAX_LOG_LINES - 1], text, 255);
    }
    pthread_mutex_unlock(&g_state_mutex);
}

int main(int argc, char **argv) {
    pthread_t net_thread;
    pthread_t cli_thread;
    /* Initialize Dungeon 1000x1000 */
    world_init(&g_world);
     
     
     
    
    if (argc < 2) {
        printf("Usage: %s [gl|vk]\n", argv[0]);
        return 1;
    }
    
    int x, y;
    for (y = 0; y < MAP_HEIGHT; y++) {
        for (x = 0; x < MAP_WIDTH; x++) {
            g_local_map[y][x] = g_world.floors[0].map.data[0][y][x];
        }
    }
    
    char server_ip[64] = "127.0.0.1";
    char server_pass[64] = "";
    char username[32] = "";
    char password[32] = "";
    int is_new = 0;
    int race_id = 0;
    int class_id = 0;
    char buf[128];

    printf("\n--- DND GL Client ---\n");
    printf("Server IP [%s]: ", server_ip);
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n') {
        sscanf(buf, "%63s", server_ip);
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
    int final_str=10, final_dex=10, final_con=10, final_int=10, final_wis=10, final_cha=10;
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

        srand(time(NULL));
        char conf = 'n';
        while (conf != 's' && conf != 'S') {
            int base_str = rules_roll_dice(3, 6);
            int base_dex = rules_roll_dice(3, 6);
            int base_con = rules_roll_dice(3, 6);
            int base_int = rules_roll_dice(3, 6);
            int base_wis = rules_roll_dice(3, 6);
            int base_cha = rules_roll_dice(3, 6);
            
            final_str = base_str + RACES[race_id].str_bonus;
            final_dex = base_dex + RACES[race_id].dex_bonus;
            final_con = base_con + RACES[race_id].con_bonus;
            final_int = base_int + RACES[race_id].int_bonus;
            final_wis = base_wis + RACES[race_id].wis_bonus;
            final_cha = base_cha + RACES[race_id].cha_bonus;

            if (g_subrace_id != -1) {
                final_str += SUBRACES[g_subrace_id].str_bonus;
                final_dex += SUBRACES[g_subrace_id].dex_bonus;
                final_con += SUBRACES[g_subrace_id].con_bonus;
                final_int += SUBRACES[g_subrace_id].int_bonus;
                final_wis += SUBRACES[g_subrace_id].wis_bonus;
                final_cha += SUBRACES[g_subrace_id].cha_bonus;
            }
            
            final_age = rules_roll_dice(3, 6) + 15;
            final_height = rules_roll_dice(4, 10) + 140;
            final_weight = rules_roll_dice(4, 10) + 50;
            final_social = rules_roll_dice(1, 100);
            
            int to_hit = rules_get_modifier(final_str);
            int to_dmg = rules_get_modifier(final_str);
            int to_ac = rules_get_modifier(final_dex);
            int tot_ac = 10 + to_ac;
            
            char full_race[64];
            if (g_subrace_id != -1) {
                snprintf(full_race, sizeof(full_race), "%s (%s)", RACES[race_id].name, SUBRACES[g_subrace_id].name);
            } else {
                strncpy(full_race, RACES[race_id].name, sizeof(full_race));
            }

            printf("\n========================================================================\n");
            printf(" Name        : %-22s Age          : %5d  STR : %4d\n", username, final_age, final_str);
            printf(" Race        : %-22s Height (cm)  : %5d  INT : %4d\n", full_race, final_height, final_int);
            printf(" Sex         : %-22s Weight (kg)  : %5d  WIS : %4d\n", sex_char == 'M' ? "Male" : "Female", final_weight, final_wis);
            printf(" Class       : %-22s Social Status: %5d  DEX : %4d\n", CLASSES[class_id].name, final_social, final_dex);
            printf("Alignment : %-46s CON : %4d\n", ALIGNMENTS[alignment].name, final_con);
            printf("                                                             CHR : %4d\n\n", final_cha);
            printf(" Traits      : %s%s%s\n\n", RACES[race_id].traits, 
                   (g_subrace_id != -1 ? ", " : ""),
                   (g_subrace_id != -1 ? SUBRACES[g_subrace_id].traits : ""));
            printf(" + To Hit    : %6d\n", to_hit);
            printf(" + To Damage : %6d\n", to_dmg);
            printf(" + To AC     : %6d\n", to_ac);
            printf("   Total AC  : %6d\n", tot_ac);
            printf("========================================================================\n");
            
            printf("Confirm these statistics? (y = Yes, n = Reroll): ");
            if (fgets(buf, sizeof(buf), stdin)) {
                conf = buf[0];
                if (conf == 'y' || conf == 'Y') conf = 's'; // internal 's' for Yes
            }
        }
    }

    // --- Final character summary ---
    if (is_new) {
        int con_mod  = rules_get_modifier(final_con);
        int str_mod  = rules_get_modifier(final_str);
        int dex_mod  = rules_get_modifier(final_dex);
        int int_mod  = rules_get_modifier(final_int);
        int wis_mod  = rules_get_modifier(final_wis);
        int max_hp   = 20 + con_mod; // Same as server
        int total_ac = 10 + dex_mod;
        int to_hit_f = str_mod;
        int to_dmg_f = str_mod;

        char full_race_f[64];
        if (g_subrace_id != -1) {
            snprintf(full_race_f, sizeof(full_race_f), "%s (%s)", RACES[race_id].name, SUBRACES[g_subrace_id].name);
        } else {
            strncpy(full_race_f, RACES[race_id].name, sizeof(full_race_f));
        }

        printf("\n");
        printf("===========================================================================\n");
        printf(" Name        : %-23s  Age          : %5d  STR : %4d\n", username, final_age, final_str);
        printf(" Race        : %-23s  Height       : %5d  INT : %4d\n", full_race_f, final_height, final_int);
        printf(" Sex         : %-23s  Weight       : %5d  WIS : %4d\n", sex_char == 'M' ? "Male" : "Female", final_weight, final_wis);
        printf(" Class       : %-23s  Social Status: %5d  DEX : %4d\n", CLASSES[class_id].name, final_social, final_dex);
        printf("Alignment : %-23s CON : %4d\n", ALIGNMENTS[g_alignment].name, final_con);
        printf("                                                             CHR : %4d\n", final_cha);
        printf("\n");
        printf(" + To Hit    : %6d       Level      : %7d    Max Hit Points : %6d\n", to_hit_f, 1, max_hp);
        printf(" + To Damage : %6d       Experience : %7d    Cur Hit Points : %6d\n", to_dmg_f, 0, max_hp);
        printf(" + To AC     : %6d       Max Exp    : %7d    Max Mana       : %6d\n", dex_mod, 0, 0);
        printf("   Total AC  : %6d       Exp to Adv.: %7d    Cur Mana       : %6d\n", total_ac, 1000, 0);
        printf("                            Gold       : %7d\n", 1000);
        printf("\n");
        printf("                         (Miscellaneous Abilities)\n");
        printf(" Fighting    : %-12s  Stealth     : %-12s  Perception  : %-12s\n", 
               str_mod > 2 ? "Very Good" : (str_mod > 0 ? "Good" : "Fair"),
               dex_mod > 2 ? "Excellent" : (dex_mod > 0 ? "Good" : "Fair"),
               int_mod > 2 ? "Sharp" : (int_mod > 0 ? "Good" : "Poor"));
        printf(" Bows/Throw  : %-12s  Disarming   : %-12s  Searching   : %-12s\n",
               dex_mod > 1 ? "Good" : "Fair",
               dex_mod > 1 ? "Good" : "Poor",
               wis_mod > 1 ? "Good" : "Poor");
        printf(" Saving Throw: %-12s  Magic Device: %-12s  Infra-Vision: %2d feet\n",
               con_mod > 1 ? "Good" : "Fair",
               int_mod > 1 ? "Good" : "Fair",
               (strstr(RACES[race_id].traits, "Darkvision") || (g_subrace_id != -1 && strstr(SUBRACES[g_subrace_id].traits, "Darkvision"))) ? 60 : 0);
        printf("===========================================================================\n\n");
    }

    printf("Connecting to server %s...\n", server_ip);
    g_server_sock = net_connect_to_server(server_ip, 8080);
    
    if (g_server_sock < 0) {
        printf("Connection error.\n");
        return 1;
    }
    
    {
        MsgHeader login_hdr;
        MsgLogin msg_log;
        memset(&msg_log, 0, sizeof(msg_log));
        strncpy(msg_log.username, username, 31);
        strncpy(msg_log.password, password, 31);
        strncpy(msg_log.server_pass, server_pass, 31);
        msg_log.is_new_char = is_new;
        msg_log.race_id = race_id;
        msg_log.subrace_id = g_subrace_id;
        msg_log.class_id = class_id;
        msg_log.str = final_str; msg_log.dex = final_dex; msg_log.con = final_con;
        msg_log.intel = final_int; msg_log.wis = final_wis; msg_log.cha = final_cha;
        msg_log.age = final_age; msg_log.height = final_height; msg_log.weight = final_weight;
        msg_log.social_class = final_social; msg_log.alignment = g_alignment;
        g_race_id = race_id;
        g_class_id = class_id;
        
        login_hdr.type = MSG_LOGIN;
        login_hdr.length = sizeof(MsgLogin);
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
