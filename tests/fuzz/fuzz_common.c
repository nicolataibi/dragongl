/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * Helpers shared by the libFuzzer targets (fuzz_common.h).
 */

#include "fuzz_common.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *fuzz_seed_dir(void) {
    const char *dir = getenv("DGL_FUZZ_SEEDS");
    return (dir && *dir) ? dir : NULL;
}

void fuzz_write_seed(const char *dir, const char *name, int prefix, const void *data, size_t n) {
    char path[512];
    (void)mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    bool ok = true;
    if (prefix >= 0) {
        ok = fputc(prefix, f) != EOF;
    }
    ok = ok && (n == 0 || fwrite(data, 1, n, f) == n);
    if (fclose(f) != 0 || !ok) {
        perror(path);
        exit(1);
    }
}

const char *fuzz_tmp_dir(void) {
    static char dir[256];
    if (dir[0]) {
        return dir;
    }
    const char *base = getenv("TMPDIR");
    snprintf(dir, sizeof(dir), "%s/dragongl-fuzz-XXXXXX", (base && *base) ? base : "/tmp");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        exit(1);
    }
    return dir;
}
