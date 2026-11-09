/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * The command line of dragongl_server.
 */

#ifndef DRAGONGL_SERVER_OPTIONS_H
#define DRAGONGL_SERVER_OPTIONS_H

typedef struct {
    int port;
    const char *password_file;
    const char *dm_password_file;
    const char *state_dir;
    const char *tls_cert;
    const char *tls_key;
} ServerOptions;

/* Parse argv into o (--data-dir goes to g_data_dir). -1 to go on,
 * otherwise the exit status: 0 after --help/--version, 2 on an invalid
 * option. */
int parse_options(int argc, char **argv, ServerOptions *o);

#endif
