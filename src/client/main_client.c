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
#include <unistd.h>
#include "strnum.h"
#include "client_config.h"
#include "client_login.h"
#include "client_state.h"
#include "net.h"
#include "client_tls.h"
#include "render_vk.h"
#include "dragongl_config.h"
#include "protocol.h"
#include "wire.h"

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

static void print_client_usage(const char *argv0) {
    printf("Usage: %s [options] [server_host [port]] | -h | -V\n"
           "  server_host    host name, IPv4 or IPv6 address (asked if omitted;\n"
           "                 default 127.0.0.1)\n"
           "  port           TCP port (default 8080)\n"
           "  --fingerprint  SHA-256 fingerprint of the server certificate, as the\n"
           "                 server owner gives it: trust that certificate only\n"
           "  --ui-scale N   size of the HUD, 0.5 to 4 (default: from the monitor and\n"
           "                 the window; Ctrl+= and Ctrl+- change it while playing)\n"
           "  --print-config write the settings file with every default and key to\n"
           "                 standard output, and exit\n"
           "  --frames N     draw N frames, print the frame times and exit\n"
           "The connection is TLS 1.3. A server whose certificate no trusted authority\n"
           "signed is recorded at the first connection in\n"
           "$XDG_CONFIG_HOME/dragongl/known_servers (~/.config/dragongl/known_servers)\n"
           "and refused if its certificate changes later.\n"
           "Settings and keys: $XDG_CONFIG_HOME/dragongl/client.conf\n"
           "(~/.config/dragongl/client.conf); F1 in the window lists the keys.\n"
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

/*What the command line asks for.*/
typedef struct {
    char host[256];
    int port;
    bool host_given;
    const char *fingerprint;
    float ui_scale;       /*0: not given*/
    unsigned long frames; /*0: until the window is closed*/
    bool print_config;
} ClientArgs;

/*The value of option argv[*i], or NULL (after a message) when missing.*/
static const char *option_value(int argc, char **argv, int *i) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "%s: %s needs a value\n", argv[0], argv[*i]);
        return NULL;
    }
    return argv[++*i];
}

/*One option that takes a value. 0: done; 2: bad value (message printed);
 * -1: not such an option.*/
static int parse_value_option(int argc, char **argv, int *i, ClientArgs *out) {
    const char *a = argv[*i];
    const char *end = NULL;
    int n = 0;
    if (strcmp(a, "--fingerprint") == 0) {
        out->fingerprint = option_value(argc, argv, i);
        return out->fingerprint ? 0 : 2;
    }
    if (strcmp(a, "--ui-scale") == 0) {
        const char *v = option_value(argc, argv, i);
        char *stop = NULL;
        float s = v ? strtof(v, &stop) : 0.0f;
        if (!v || stop == v || *stop != '\0' || !(s >= UI_SCALE_MIN && s <= UI_SCALE_MAX)) {
            fprintf(stderr, "%s: --ui-scale takes a number from 0.5 to 4\n", argv[0]);
            return 2;
        }
        out->ui_scale = ui_scale_clamp(s);
        return 0;
    }
    if (strcmp(a, "--frames") == 0) {
        const char *v = option_value(argc, argv, i);
        if (!v || !parse_int(v, &n, &end) || *end != '\0' || n <= 0) {
            fprintf(stderr, "%s: --frames takes a positive number\n", argv[0]);
            return 2;
        }
        out->frames = (unsigned long)n;
        return 0;
    }
    return -1;
}

/*-1: go on; otherwise the exit status (0 after --help or --version).*/
static int parse_args(int argc, char **argv, ClientArgs *out) {
    int positional = 0;
    memset(out, 0, sizeof(*out));
    copy_str(out->host, "127.0.0.1", sizeof(out->host));
    out->port = 8080;
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
        if (strcmp(a, "--print-config") == 0) {
            out->print_config = true;
            continue;
        }
        int r = parse_value_option(argc, argv, &i, out);
        if (r >= 0) {
            if (r != 0) {
                return r;
            }
            continue;
        }
        const char *end = NULL;
        int p = 0;
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "%s: unknown option '%s' (see --help)\n", argv[0], a);
            return 2;
        }
        if (positional == 0) {
            if (a[0] != '\0') {
                copy_str(out->host, a, sizeof(out->host));
                out->host_given = true;
            }
        } else if (positional == 1) {
            if (!parse_int(a, &p, &end) || *end != '\0' || p <= 0 || p > 65535) {
                fprintf(stderr, "Invalid port: %s\n", a);
                return 2;
            }
            out->port = p;
        } else {
            print_client_usage(argv[0]);
            return 2;
        }
        positional++;
    }
    return -1;
}

static void config_warning(int line, const char *message, void *user) {
    (void)user;
    if (line > 0) {
        fprintf(stderr, "client.conf, line %d: %s\n", line, message);
    } else {
        fprintf(stderr, "client.conf: %s\n", message);
    }
}

/*Sends MSG_LOGIN and wipes every copy of the passwords.*/
static bool send_login(LoginAnswers *who) {
    MsgLogin ml;
    memset(&ml, 0, sizeof(ml));
    copy_str(ml.username, who->username, sizeof(ml.username));
    copy_str(ml.password, who->password, sizeof(ml.password));
    copy_str(ml.server_pass, who->server_pass, sizeof(ml.server_pass));
    /*Creation fields: used by the server only if the character does not
     * exist yet (the ability scores are rolled by the server).*/
    ml.race_id = who->race_id;
    ml.subrace_id = who->subrace_id;
    ml.class_id = who->class_id;
    ml.alignment = who->alignment;
    BinWriter w;
    bw_init(&w);
    bool ok = wire_put_login(&w, &ml) && client_tls_send(g_tls, w.data, w.len);
    if (w.data) {
        memset(w.data, 0, w.cap); /* the frame holds the passwords */
    }
    bw_free(&w);
    memset(&ml, 0, sizeof(ml));
    memset(who->password, 0, sizeof(who->password));
    memset(who->server_pass, 0, sizeof(who->server_pass));
    return ok;
}

/*Connects, starts TLS and logs in. False (after a message) on failure.*/
static bool connect_and_login(const ClientArgs *args, LoginAnswers *who) {
    char why[160];
    printf("Connecting to server %s port %d...\n", args->host, args->port);
    g_server_sock = net_connect_to_server(args->host, args->port, 5000, why, sizeof(why));
    if (g_server_sock < 0) {
        printf("Connection error: %s\n", why);
        return false;
    }
    if (!start_tls(args->host, args->port, args->fingerprint)) {
        net_close(g_server_sock);
        return false;
    }
    if (!send_login(who)) {
        printf("Connection error: cannot send the login.\n");
        client_tls_close(g_tls);
        net_close(g_server_sock);
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    static ClientArgs args;
    static ClientConfig config;
    static LoginAnswers who;
    pthread_t net_thread;
    pthread_t cli_thread;
    /*Line-buffered even when redirected: the [VK]/[NET] messages reach a
     *log file as they happen.*/
    setvbuf(stdout, NULL, _IOLBF, 0);
    /*Writing to a connection the server already closed must fail with
     * EPIPE (detected by the network thread), not kill the client.*/
    signal(SIGPIPE, SIG_IGN);

    int status = parse_args(argc, argv, &args);
    if (status >= 0) {
        return status;
    }
    if (client_config_load(&config, config_warning, NULL) < 0) {
        fprintf(stderr, "client.conf cannot be read: using the defaults.\n");
        client_config_defaults(&config);
    }
    if (args.ui_scale > 0.0f) {
        config.ui_scale = args.ui_scale;
    }
    if (args.print_config) {
        client_config_print(&config, stdout);
        return 0;
    }

    /*The client generates no world: the map arrives from the server as
     * MSG_MAP_CHUNK packets, so start with solid rock.*/
    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            g_local_map[y][x] = VOXEL_ROCK;
        }
    }

    login_prompt(&who, args.host, sizeof(args.host), !args.host_given);
    g_race_id = who.race_id;
    g_subrace_id = who.subrace_id;
    g_class_id = who.class_id;
    g_alignment = who.alignment;
    if (!connect_and_login(&args, &who)) {
        return 1;
    }

    pthread_create(&net_thread, NULL, net_thread_loop, NULL);
    pthread_create(&cli_thread, NULL, cli_thread_loop, NULL);

    /*The window and its render loop; returns when the window is closed,
     *the session ends (g_running) or Vulkan cannot run.*/
    render_vk_start(&config, args.frames);
    g_running = false;

    pthread_join(cli_thread, NULL);
    pthread_join(net_thread, NULL);
    client_tls_close(g_tls);
    net_close(g_server_sock);
    return 0;
}
