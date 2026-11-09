/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The command line of dragongl_server (server_options.h).
 */

#include "server_options.h"

#include "dragongl_config.h"
#include "net.h"
#include "server_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_server_usage(const char *argv0) {
    printf("Usage: %s [options]\n"
           "Options:\n"
           "  -P, --port <port>          TCP port to listen on (default: 8080)\n"
           "  -D, --data-dir <dir>       Read-only game data (JSON datasets).\n"
           "                             Default: $DRAGONGL_DATA_DIR, else the\n"
           "                             data/ of the source tree (build tree),\n"
           "                             else " DRAGONGL_PKGDATADIR "/data\n"
           "  -S, --state-dir <dir>      Everything the server writes: world,\n"
           "                             NPCs, saves, logs, DM maps. Default:\n"
           "                             $DRAGONGL_STATE_DIR, else\n"
           "                             $XDG_STATE_HOME/dragongl, else\n"
           "                             ~/.local/state/dragongl\n"
           "      --password-file <f>    Read the server access password from f\n"
           "      --dm-password-file <f> Read the Dungeon Master password from f\n"
           "      --tls-cert <file>      TLS certificate (PEM, with its chain) and\n"
           "      --tls-key <file>       private key. Default: <state>/tls/server.crt\n"
           "                             and server.key, created at the first start\n"
           "                             (self-signed; clients pin its fingerprint)\n"
           "  -h, --help                 Show this help\n"
           "  -V, --version              Show the version\n"
           "\n"
           "Passwords are never accepted on the command line. Without a file the\n"
           "server reads DRAGONGL_SERVER_PASSWORD (required) and DRAGONGL_DM_PASSWORD\n"
           "(optional: without it the DM account is disabled). Each password has\n"
           "8-63 printable ASCII characters, no spaces.\n",
           argv0);
}

/*Returns -1 to go on, otherwise the exit status (0 after --help/--version,
 * 2 on invalid options). --data-dir is stored in g_data_dir.*/
int parse_options(int argc, char **argv, ServerOptions *o) {
    for (int i = 1; i < argc; i++) {
        const char *opt = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(opt, "-h") == 0 || strcmp(opt, "--help") == 0) {
            print_server_usage(argv[0]);
            return 0;
        }
        if (strcmp(opt, "-V") == 0 || strcmp(opt, "--version") == 0) {
            printf("dragongl_server %s\n", DRAGONGL_VERSION);
            return 0;
        }
        if (strcmp(opt, "-p") == 0 || strcmp(opt, "--password") == 0 || strcmp(opt, "-d") == 0 ||
            strcmp(opt, "--dm-password") == 0) {
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
                           strcmp(opt, "--dm-password-file") == 0 ||
                           strcmp(opt, "--tls-cert") == 0 || strcmp(opt, "--tls-key") == 0;
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
            o->port = (int)port;
        } else if (strcmp(opt, "-D") == 0 || strcmp(opt, "--data-dir") == 0) {
            copy_str(g_data_dir, val, sizeof(g_data_dir));
        } else if (strcmp(opt, "-S") == 0 || strcmp(opt, "--state-dir") == 0) {
            o->state_dir = val;
        } else if (strcmp(opt, "--password-file") == 0) {
            o->password_file = val;
        } else if (strcmp(opt, "--tls-cert") == 0) {
            o->tls_cert = val;
        } else if (strcmp(opt, "--tls-key") == 0) {
            o->tls_key = val;
        } else {
            o->dm_password_file = val;
        }
    }
    return -1;
}
