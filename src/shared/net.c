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

#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int net_create_server(int port) {
    int sock;
    struct sockaddr_in addr;
    int opt;
    
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    
    opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }
    
    if (listen(sock, 10) < 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

int net_connect_to_server(const char *ip, int port) {
    int sock;
    struct sockaddr_in addr;
    
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

void net_set_nonblocking(int sock) {
    int flags;
    
    flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

bool net_send(int sock, const void *data, int len) {
    int total = 0;
    int retries = 0;
    int max_retries = 500;
    const char *buf = (const char *)data;
    
    while (total < len && retries < max_retries) {
        int sent = send(sock, buf + total, len - total, 0);
        if (sent > 0) {
            total += sent;
            retries = 0;
        } else if (sent == 0) {
            return false;
        } else {
            retries++;
            usleep(10000);
        }
    }
    
    return total == len;
}

int net_receive(int sock, void *buffer, int max_len) {
    int bytes;
    
    bytes = recv(sock, buffer, max_len, 0);
    
    return bytes;
}

void net_close(int sock) {
    close(sock);
}

/*Read exactly 'len' bytes on a non-blocking socket,
   trying again with short pauses up to a maximum of 500 iterations (~5 sec).*/
int net_receive_all(int sock, void *buffer, int len) {
    int total = 0;
    int retries = 0;
    int max_retries = 500;
    char *buf = (char *)buffer;
    
    while (total < len && retries < max_retries) {
        int bytes = recv(sock, buf + total, len - total, 0);
        if (bytes > 0) {
            total = total + bytes;
            retries = 0;
        } else if (bytes == 0) {
            /* connection closed */
            return -1;
        } else {
            /* EAGAIN / EWOULDBLOCK */
            retries = retries + 1;
            usleep(10000);
        }
    }
    
    if (total == len) {
        return total;
    }
    return -1;
}

