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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <termios.h>
#include <unistd.h>
#include "strnum.h"
#include "client_state.h"
#include "net.h"
#include "client_tls.h"
#include "render_vk.h"
#include "dragongl_config.h"
#include "protocol.h"
#include "wire.h"
#include "rules.h"
#include "species.h"
#include "classes.h"
#include "alignment.h"
#include <time.h>

int g_server_sock = -1;
ClientTls *g_tls = NULL;
int g_my_entity_id = -1;
int g_my_x = 0;
int g_my_y = 0;
int g_my_hp = 20, g_my_max_hp = 20;
Entity g_entities[CLIENT_MAX_ENTITIES];
int g_game_h = 8, g_game_m = 0, g_total_turns = 0;
int g_str = 10, g_dex = 10, g_con = 10, g_intel = 10, g_wis = 10, g_cha = 10;
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
char g_eq_head[32] = {0};
char g_eq_neck[32] = {0};
char g_eq_body[32] = {0};
char g_eq_back[32] = {0};
char g_eq_hand_r[32] = {0};
char g_eq_hand_l[32] = {0};
char g_eq_hands[32] = {0};
char g_eq_arm_r[32] = {0};
char g_eq_arm_l[32] = {0};
char g_eq_feet[32] = {0};
char g_eq_ring[10][32] = {{0}};
char g_eq_belt[4][32] = {{0}};
char g_log_lines[MAX_LOG_LINES][256];
int g_log_count = 0;
/*The client holds no World: it renders g_local_map, which the server
 * fills with MSG_MAP_CHUNK.*/
TileType g_local_map[MAP_HEIGHT][MAP_WIDTH];
pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_net_mutex = PTHREAD_MUTEX_INITIALIZER;
/*See the declaration in client_state.h: atomic because the render,
 * net and CLI threads all read/write it.*/
atomic_bool g_running = true;

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

extern void *net_thread_loop(void *arg);
extern void *cli_thread_loop(void *arg);

/*Send one encoded frame. A failed send means the connection is gone:
 * the session ends (the network thread may not notice a dead peer until
 * TCP gives up).*/
static void send_frame(BinWriter *w) {
    bool ok = !w->failed && w->len > 0;
    if (ok) {
        ok = client_tls_send(g_tls, w->data, w->len);
    }
    bw_free(w);
    if (!ok && g_running) {
        printf("\n[NET] Cannot send to the server: connection lost.\n");
        g_running = false;
    }
}

void client_send_move(int dx, int dy) {
    if (g_my_entity_id == -1) {
        return;
    }
    MsgMove m = {dx, dy};
    BinWriter w;
    bw_init(&w);
    if (wire_put_move(&w, &m)) {
        send_frame(&w);
    } else {
        bw_free(&w);
    }
}

void client_send_text_cmd(const char *cmd) {
    MsgTextCmd m;
    copy_str(m.cmd, cmd, sizeof(m.cmd));
    BinWriter w;
    bw_init(&w);
    if (wire_put_text_cmd(&w, &m)) {
        send_frame(&w);
    } else {
        bw_free(&w);
    }
}

void client_log_add(const char *text) {
    pthread_mutex_lock(&g_state_mutex);
    if (g_log_count < MAX_LOG_LINES) {
        copy_str(g_log_lines[g_log_count], text, sizeof(g_log_lines[0]));
        g_log_count++;
    } else {
        // Shift lines up (copy_str guarantees NUL termination)
        for (int i = 0; i < MAX_LOG_LINES - 1; i++) {
            copy_str(g_log_lines[i], g_log_lines[i + 1], sizeof(g_log_lines[0]));
        }
        copy_str(g_log_lines[MAX_LOG_LINES - 1], text, sizeof(g_log_lines[0]));
    }
    pthread_mutex_unlock(&g_state_mutex);
}

/*Prompt for a secret without echoing it when stdin is a terminal. The
 * first word of the line is kept (passwords contain no spaces).*/
static void read_secret(const char *prompt, char *out, size_t out_len) {
    char line[128];
    struct termios saved, quiet;
    bool tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0;
    printf("%s", prompt);
    fflush(stdout);
    if (tty) {
        quiet = saved;
        quiet.c_lflag &= (tcflag_t)~ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    out[0] = '\0';
    if (fgets(line, sizeof(line), stdin)) {
        char fmt[16];
        snprintf(fmt, sizeof(fmt), "%%%zus", out_len - 1);
        if (sscanf(line, fmt, out) != 1) {
            out[0] = '\0';
        }
    }
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        printf("\n");
    }
    memset(line, 0, sizeof(line));
    if (strlen(out) >= PROTO_SECRET_LEN) {
        printf("Warning: only the first %d characters are sent to the server.\n",
               PROTO_SECRET_LEN - 1);
    }
}

static void print_client_usage(const char *argv0) {
    printf("Usage: %s [--fingerprint <sha256>] [server_host [port]] | -h | -V\n"
           "  server_host    host name, IPv4 or IPv6 address (asked if omitted;\n"
           "                 default 127.0.0.1)\n"
           "  port           TCP port (default 8080)\n"
           "  --fingerprint  SHA-256 fingerprint of the server certificate, as the\n"
           "                 server owner gives it: trust that certificate only\n"
           "The connection is TLS 1.3. A server whose certificate no trusted authority\n"
           "signed is recorded at the first connection in\n"
           "$XDG_CONFIG_HOME/dragongl/known_servers (~/.config/dragongl/known_servers)\n"
           "and refused if its certificate changes later.\n"
           "Environment: DRAGONGL_WINDOW=WxH (window size), DRAGONGL_GPU=<index|name>,\n"
           "             DRAGONGL_VSYNC=0, DRAGONGL_SHADER_DIR=<dir>, VK_INSTANCE_LAYERS\n",
           argv0);
}

/* TLS on the connected socket, and the trust decision told to the user. */
static bool start_tls(const char *host, int port, const char *expected_fp) {
    char why[512];
    ClientTlsTrust trust = TLS_TRUST_KNOWN;
    g_tls =
        client_tls_connect(g_server_sock, host, port, expected_fp, 5000, &trust, why, sizeof(why));
    if (!g_tls) {
        printf("Connection error: %s\n", why);
        return false;
    }
    switch (trust) {
    case TLS_TRUST_FIRST_USE:
        printf("[NET] First connection to %s port %d. Server certificate fingerprint:\n"
               "      %s\n"
               "      Compare it with the one the server owner gives you (the server\n"
               "      logs it at start-up). It is recorded in %s:\n"
               "      from now on a different certificate is refused.\n",
               host, port, client_tls_fingerprint(g_tls), client_tls_known_servers(g_tls));
        break;
    case TLS_TRUST_GIVEN:
        printf("[NET] Server certificate matches the given fingerprint.\n");
        break;
    case TLS_TRUST_AUTHORITY:
        printf("[NET] Server certificate verified for %s.\n", host);
        break;
    case TLS_TRUST_KNOWN:
    default:
        printf("[NET] Known server (%s).\n", client_tls_known_servers(g_tls));
        break;
    }
    return true;
}

int main(int argc, char **argv) {
    pthread_t net_thread;
    pthread_t cli_thread;
    /*Line-buffered even when redirected: the [VK]/[NET] messages reach a
     *log file as they happen.*/
    setvbuf(stdout, NULL, _IOLBF, 0);
    /*Writing to a connection the server already closed must fail with
     * EPIPE (detected by the network thread), not kill the client.*/
    signal(SIGPIPE, SIG_IGN);
    /*The client generates no world: the map arrives from the server as
     * MSG_MAP_CHUNK packets, so start with solid rock.*/
    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            g_local_map[y][x] = VOXEL_ROCK;
        }
    }

    char server_ip[256] = "127.0.0.1";
    int server_port = 8080;
    bool ip_from_arg = false;
    const char *expected_fp = NULL;
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("dragongl_client %s\n", DRAGONGL_VERSION);
            return 0;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_client_usage(argv[0]);
            return 0;
        }
        if (strcmp(a, "--fingerprint") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --fingerprint needs a value\n", argv[0]);
                return 2;
            }
            expected_fp = argv[++i];
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "%s: unknown option '%s' (see --help)\n", argv[0], a);
            return 2;
        } else if (positional == 0) {
            if (a[0] != '\0') {
                copy_str(server_ip, a, sizeof(server_ip));
                ip_from_arg = true;
            }
            positional++;
        } else if (positional == 1) {
            const char *end = NULL;
            int p = 0;
            if (!parse_int(a, &p, &end) || *end != '\0' || p <= 0 || p > 65535) {
                fprintf(stderr, "Invalid port: %s\n", a);
                return 2;
            }
            server_port = p;
            positional++;
        } else {
            print_client_usage(argv[0]);
            return 2;
        }
    }

    char server_pass[128] = "";
    char username[PROTO_NAME_LEN] = "";
    char password[128] = "";
    int is_new = 0;
    int race_id = 0;
    int class_id = 0;
    char buf[128];

    printf("\n--- Dragon GL Client ---\n");
    if (ip_from_arg) {
        printf("Server host: %s (from command line)\n", server_ip);
    } else {
        printf("Server host [%s]: ", server_ip);
        if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n') {
            sscanf(buf, "%127s", server_ip);
        }
    }
    read_secret("Server Password: ", server_pass, sizeof(server_pass));
    printf("Player Name: ");
    if (fgets(buf, sizeof(buf), stdin)) {
        sscanf(buf, "%31s", username);
    }
    read_secret("Personal Password: ", password, sizeof(password));
    printf("New character? (1=Yes, 0=No):");
    if (fgets(buf, sizeof(buf), stdin)) {
        parse_int(buf, &is_new, NULL);
    }
    /*final_age/height/weight/social: cosmetic flavour rolls only (the
     * server never uses them; the ability scores are rolled server-side).*/
    int final_age = 20, final_height = 170, final_weight = 70, final_social = 50;
    char sex_char = 'M';

    if (is_new) {
        printf("\nAvailable Races:\n");
        for (int i = 0; i < RACE_COUNT; i++) {
            printf("  %2d = %-16s : %s\n", i, RACES[i].name, RACES[i].description);
        }
        printf("Race ID: ");
        if (fgets(buf, sizeof(buf), stdin)) {
            parse_int(buf, &race_id, NULL);
        }
        if (race_id < 0 || race_id >= RACE_COUNT) {
            race_id = 0;
        }

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
                parse_int(buf, &subrace_id, NULL);
            }
            if (subrace_id >= 0 && subrace_id < SUBRACE_COUNT) {
                if (SUBRACES[subrace_id].parent_race != (RaceType)race_id) {
                    subrace_id = -1;
                }
            } else {
                subrace_id = -1;
            }
        }
        g_subrace_id = subrace_id;

        printf("\nAvailable Classes:\n");
        for (int i = 0; i < CLASS_COUNT; i++) {
            printf("  %2d = %-12s : %s\n", i, CLASSES[i].name, CLASSES[i].description);
        }
        printf("Class ID: ");
        if (fgets(buf, sizeof(buf), stdin)) {
            parse_int(buf, &class_id, NULL);
        }
        printf("Sex (M=Male, F=Female): ");
        if (fgets(buf, sizeof(buf), stdin)) {
            if (buf[0] == 'f' || buf[0] == 'F') {
                sex_char = 'F';
            } else {
                sex_char = 'M';
            }
        }
        int alignment = 0;
        printf("Alignment:\n");
        for (int i = 0; i < ALIGN_COUNT; i++) {
            printf("  %d=%-16s", i, ALIGNMENTS[i].name);
            if ((i + 1) % 3 == 0) {
                printf("\n");
            }
        }
        printf("Choice: ");
        if (fgets(buf, sizeof(buf), stdin)) {
            parse_int(buf, &alignment, NULL);
            if (alignment < 0 || alignment >= ALIGN_COUNT) {
                alignment = 0;
            }
        }
        if (class_id < 0 || class_id >= CLASS_COUNT) {
            class_id = 0;
        }
        g_race_id = race_id;
        g_class_id = class_id;
        g_alignment = alignment;

        /*Ability scores are rolled by the SERVER at login: a client could
        * send anything. Only the cosmetic flavour rolls (age, height,
        * weight, social status) are local - the server never uses them.
        * The rolled STR/DEX/CON/INT/WIS/CHA arrive in-game as a
        * [CHARACTER] message right after the welcome.*/
        srand(time(NULL));
        final_age = rules_roll_dice(3, 6) + 15;
        final_height = rules_roll_dice(4, 10) + 140;
        final_weight = rules_roll_dice(4, 10) + 50;
        final_social = rules_roll_dice(1, 100);

        char full_race[64];
        if (g_subrace_id != -1) {
            snprintf(full_race, sizeof(full_race), "%s (%s)", RACES[race_id].name,
                     SUBRACES[g_subrace_id].name);
        } else {
            copy_str(full_race, RACES[race_id].name, sizeof(full_race));
        }

        printf("\n========================================================================\n");
        printf(" Name        : %-22s Age          : %5d\n", username, final_age);
        printf(" Race        : %-22s Height (cm)  : %5d\n", full_race, final_height);
        printf(" Sex         : %-22s Weight (kg)  : %5d\n", sex_char == 'M' ? "Male" : "Female",
               final_weight);
        printf(" Class       : %-22s Social Status: %5d\n", CLASSES[class_id].name, final_social);
        printf("Alignment : %-46s\n", ALIGNMENTS[alignment].name);
        printf(" STR/DEX/CON/INT/WIS/CHA: rolled by the server - see the\n");
        printf(" [CHARACTER] message after login.\n\n");
        printf(" Traits      : %s%s%s\n\n", RACES[race_id].traits, (g_subrace_id != -1 ? ", " : ""),
               (g_subrace_id != -1 ? SUBRACES[g_subrace_id].traits : ""));
        printf("========================================================================\n");
    }

    // --- Final character summary (flavour only: the ability scores are
    // rolled by the server, see the [CHARACTER] line after login) ---
    if (is_new) {
        char full_race_f[64];
        if (g_subrace_id != -1) {
            snprintf(full_race_f, sizeof(full_race_f), "%s (%s)", RACES[race_id].name,
                     SUBRACES[g_subrace_id].name);
        } else {
            copy_str(full_race_f, RACES[race_id].name, sizeof(full_race_f));
        }

        printf("\n===========================================================================\n");
        printf(" Name        : %-23s  Age          : %5d\n", username, final_age);
        printf(" Race        : %-23s  Height       : %5d\n", full_race_f, final_height);
        printf(" Sex         : %-23s  Weight       : %5d\n", sex_char == 'M' ? "Male" : "Female",
               final_weight);
        printf(" Class       : %-23s  Social Status: %5d\n", CLASSES[class_id].name, final_social);
        printf("Alignment   : %-23s\n", ALIGNMENTS[g_alignment].name);
        printf(" Ability scores are rolled by the server: watch for the\n");
        printf(" [CHARACTER] line after you connect.\n");
        printf("===========================================================================\n\n");
    }

    printf("Connecting to server %s port %d...\n", server_ip, server_port);
    char why[160];
    g_server_sock = net_connect_to_server(server_ip, server_port, 5000, why, sizeof(why));
    if (g_server_sock < 0) {
        printf("Connection error: %s\n", why);
        return 1;
    }
    if (!start_tls(server_ip, server_port, expected_fp)) {
        net_close(g_server_sock);
        return 1;
    }

    {
        MsgLogin ml;
        memset(&ml, 0, sizeof(ml));
        copy_str(ml.username, username, sizeof(ml.username));
        copy_str(ml.password, password, sizeof(ml.password));
        copy_str(ml.server_pass, server_pass, sizeof(ml.server_pass));
        /*Creation fields: used by the server only if the character does
         * not exist yet (the ability scores are rolled by the server).*/
        ml.race_id = race_id;
        ml.subrace_id = g_subrace_id;
        ml.class_id = class_id;
        ml.alignment = g_alignment;
        g_race_id = race_id;
        g_class_id = class_id;
        BinWriter w;
        bw_init(&w);
        bool ok = wire_put_login(&w, &ml) && client_tls_send(g_tls, w.data, w.len);
        if (w.data) {
            memset(w.data, 0, w.cap); /* the frame holds the passwords */
        }
        bw_free(&w);
        memset(&ml, 0, sizeof(ml));
        memset(password, 0, sizeof(password));
        memset(server_pass, 0, sizeof(server_pass));
        if (!ok) {
            printf("Connection error: cannot send the login.\n");
            client_tls_close(g_tls);
            net_close(g_server_sock);
            return 1;
        }
    }

    pthread_create(&net_thread, NULL, net_thread_loop, NULL);
    pthread_create(&cli_thread, NULL, cli_thread_loop, NULL);

    /*The window and its render loop; returns when the window is closed,
     *the session ends (g_running) or Vulkan cannot run.*/
    render_vk_start();
    g_running = false;

    pthread_join(cli_thread, NULL);
    pthread_join(net_thread, NULL);
    client_tls_close(g_tls);
    net_close(g_server_sock);
    return 0;
}
