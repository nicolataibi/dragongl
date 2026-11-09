/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Servers trusted by certificate fingerprint (known_servers.h).
 */

#include "known_servers.h"

#include "net.h"
#include "strnum.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define LINE_MAX_LEN 600

bool known_servers_path(char *out, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int n;
    if (xdg && xdg[0] == '/') {
        n = snprintf(out, cap, "%s/dragongl/known_servers", xdg);
    } else if (home && home[0]) {
        n = snprintf(out, cap, "%s/.config/dragongl/known_servers", home);
    } else {
        return false;
    }
    return n > 0 && (size_t)n < cap;
}

/* Split "host port fp" (fields separated by blanks). False for blank
 * lines, comments and malformed lines. */
static bool parse_line(char *line, char **host, int *port, char **fp) {
    char *hash = strchr(line, '#');
    if (hash) {
        *hash = '\0';
    }
    char *save = NULL;
    char *h = strtok_r(line, " \t\r\n", &save);
    char *p = strtok_r(NULL, " \t\r\n", &save);
    char *f = strtok_r(NULL, " \t\r\n", &save);
    if (!h || !p || !f || strtok_r(NULL, " \t\r\n", &save)) {
        return false;
    }
    const char *end = NULL;
    if (!parse_int(p, port, &end) || *end != '\0') {
        return false;
    }
    *host = h;
    *fp = f;
    return true;
}

KnownServerResult known_servers_check(const char *path, const char *host, int port,
                                      const char *fp_hex, int *line) {
    *line = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        return errno == ENOENT ? KS_UNKNOWN : KS_ERROR;
    }
    char buf[LINE_MAX_LEN];
    KnownServerResult result = KS_UNKNOWN;
    for (int n = 1; fgets(buf, sizeof(buf), f); n++) {
        char *h = NULL, *fp = NULL;
        int p = 0;
        if (!parse_line(buf, &h, &p, &fp) || p != port || strcasecmp(h, host) != 0) {
            continue;
        }
        *line = n;
        result = strcasecmp(fp, fp_hex) == 0 ? KS_MATCH : KS_MISMATCH;
        break;
    }
    fclose(f);
    return result;
}

static bool make_parent_dirs(const char *path, char *why, size_t why_len) {
    char dir[512];
    copy_str(dir, path, sizeof(dir));
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            snprintf(why, why_len, "cannot create %s: %s", dir, strerror(errno));
            return false;
        }
        *p = '/';
    }
    return true;
}

bool known_servers_set(const char *path, const char *host, int port, const char *fp_hex, char *why,
                       size_t why_len) {
    if (strlen(path) >= 480 || !make_parent_dirs(path, why, why_len)) {
        if (strlen(path) >= 480) {
            snprintf(why, why_len, "path too long: %s", path);
        }
        return false;
    }
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    FILE *out = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!out) {
        snprintf(why, why_len, "cannot write %s: %s", tmp, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    bool ok = true;
    FILE *in = fopen(path, "r");
    if (in) {
        /* Every other line is kept as it is, comments included. */
        char buf[LINE_MAX_LEN], copy[LINE_MAX_LEN];
        while (fgets(buf, sizeof(buf), in)) {
            copy_str(copy, buf, sizeof(copy));
            char *h = NULL, *fp = NULL;
            int p = 0;
            if (parse_line(copy, &h, &p, &fp) && p == port && strcasecmp(h, host) == 0) {
                continue;
            }
            ok = ok && fputs(buf, out) >= 0;
        }
        fclose(in);
    } else {
        ok = fputs("# Dragon GL servers trusted by certificate fingerprint (SHA-256).\n"
                   "# Remove a line to accept a new certificate of that server.\n",
                   out) >= 0;
    }
    ok = ok && fprintf(out, "%s %d %s\n", host, port, fp_hex) > 0;
    ok = (fflush(out) == 0) && ok && (fsync(fileno(out)) == 0);
    ok = (fclose(out) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        snprintf(why, why_len, "cannot write %s: %s", path, strerror(errno));
        (void)unlink(tmp);
        return false;
    }
    return true;
}
