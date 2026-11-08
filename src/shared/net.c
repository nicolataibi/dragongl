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

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Listening socket on every address: IPv6 with IPV6_V6ONLY off (IPv4
 * clients arrive as ::ffff:a.b.c.d), or IPv4 only where the host has no
 * IPv6. Close-on-exec, SO_REUSEADDR. */
static int listen_on(int family, int port) {
    int sock = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return -1;
    int on = 1, off = 0;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_storage ss;
    socklen_t len;
    memset(&ss, 0, sizeof(ss));
    if (family == AF_INET6) {
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
        a6->sin6_family = AF_INET6;
        a6->sin6_addr = in6addr_any;
        a6->sin6_port = htons((uint16_t)port);
        len = sizeof(*a6);
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&ss;
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = htonl(INADDR_ANY);
        a4->sin_port = htons((uint16_t)port);
        len = sizeof(*a4);
    }
    if (bind(sock, (struct sockaddr *)&ss, len) < 0 || listen(sock, SOMAXCONN) < 0) {
        int saved = errno;
        close(sock);
        errno = saved;
        return -1;
    }
    return sock;
}

int net_create_server(int port) {
    int sock = listen_on(AF_INET6, port);
    if (sock < 0 && (errno == EAFNOSUPPORT || errno == EADDRNOTAVAIL))
        sock = listen_on(AF_INET, port);
    if (sock < 0)
        perror("bind");
    return sock;
}

/* Non-blocking connect with a deadline. Returns 0, or an errno value. */
static int connect_with_timeout(int sock, const struct sockaddr *addr, socklen_t len,
                                int timeout_ms) {
    if (connect(sock, addr, len) == 0)
        return 0;
    if (errno != EINPROGRESS)
        return errno;
    struct pollfd pfd = {.fd = sock, .events = POLLOUT};
    int r;
    do {
        r = poll(&pfd, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r == 0)
        return ETIMEDOUT;
    if (r < 0)
        return errno;
    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen) != 0)
        return errno;
    return err;
}

int net_connect_to_server(const char *host, int port, int timeout_ms, char *why,
                          size_t why_len) {
    char service[16];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; /* IPv6 and IPv4, in the resolver's order */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    int gai = getaddrinfo(host, service, &hints, &res);
    if (gai != 0) {
        snprintf(why, why_len, "cannot resolve %s: %s", host, gai_strerror(gai));
        return -1;
    }
    int sock = -1, last_err = 0;
    for (const struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                      ai->ai_protocol);
        if (sock < 0) {
            last_err = errno;
            continue;
        }
        last_err = connect_with_timeout(sock, ai->ai_addr, ai->ai_addrlen, timeout_ms);
        if (last_err == 0)
            break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0) {
        snprintf(why, why_len, "cannot connect to %s port %d: %s", host, port,
                 strerror(last_err ? last_err : ECONNREFUSED));
        return -1;
    }
    /* A server that vanished without closing (power loss, network cut) is
     * noticed after ~90 s instead of never. */
    int on = 1, idle = 60, interval = 10, count = 3;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    why[0] = '\0';
    return sock;
}

void net_set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

/*Send everything on a (possibly non-blocking) socket. A full send buffer
 * (EAGAIN) is waited for with poll(), up to ~2 s in total; any other
 * error (EPIPE, ECONNRESET, ...) fails at once: the peer is gone.*/
bool net_send(int sock, const void *data, int len) {
    const char *buf = (const char *)data;
    int total = 0;
    int waited_ms = 0;
    if (sock < 0 || len < 0)
        return false;
    while (total < len) {
        /*MSG_NOSIGNAL: a peer that already closed yields EPIPE instead of
         * a SIGPIPE that would terminate the process.*/
        ssize_t sent = send(sock, buf + total, (size_t)(len - total), MSG_NOSIGNAL);
        if (sent > 0) {
            total += (int)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waited_ms < 2000) {
            struct pollfd pfd = {.fd = sock, .events = POLLOUT};
            poll(&pfd, 1, 50);
            waited_ms += 50;
            continue;
        }
        return false;
    }
    return true;
}

/*Always-NUL-terminated string copy (see net.h).
 * The NUL goes right after the copied bytes (position n), so a string
 * that SHRINKS between two calls cannot leave the old tail behind —
 * setting only dst[cap-1] would do exactly that.*/
void copy_str(char *dst, const char *src, size_t cap) {
    if (cap == 0)
        return;
    size_t n = 0;
    if (src) {
        n = strlen(src);
        if (n >= cap)
            n = cap - 1;
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

void net_close(int sock) {
    close(sock);
}
