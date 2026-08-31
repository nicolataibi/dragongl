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
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include "client_state.h"

void* cli_thread_loop(void* arg) {
    char buf[256];
    struct pollfd pfd;
    int ret;
    int len;
    
    (void)arg;
    
    printf("> ");
    fflush(stdout);
    
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    
    while (g_running) {
        ret = poll(&pfd, 1, 100);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            if (fgets(buf, sizeof(buf), stdin) != NULL) {
                len = strlen(buf);
                while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
                    buf[len - 1] = '\0';
                    len = len - 1;
                }
                
                if (len > 0) {
                    if (strcmp(buf, "w") == 0) {
                        client_send_move(0, -1);
                    } else if (strcmp(buf, "s") == 0) {
                        client_send_move(0, 1);
                    } else if (strcmp(buf, "a") == 0) {
                        client_send_move(-1, 0);
                    } else if (strcmp(buf, "d") == 0) {
                        client_send_move(1, 0);
                    } else if (strcmp(buf, "quit") == 0) {
                        g_running = false;
                    } else {
                        client_send_text_cmd(buf);
                    }
                }
                
                if (g_running) {
                    printf("> ");
                    fflush(stdout);
                }
            }
        }
    }
    
    return NULL;
}
