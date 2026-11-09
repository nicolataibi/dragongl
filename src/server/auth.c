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

#include "auth.h"

#include <errno.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

_Static_assert(AUTH_HASH_LEN == crypto_pwhash_STRBYTES,
               "AUTH_HASH_LEN must match crypto_pwhash_STRBYTES");

static unsigned char g_gate_key[crypto_generichash_KEYBYTES];
static unsigned char g_gate_digest[crypto_generichash_BYTES];
static bool g_gate_set = false;

static char g_dm_hash[AUTH_HASH_LEN];
static bool g_dm_set = false;

bool auth_init(void) {
    return sodium_init() >= 0;
}

void auth_wipe(void *p, size_t n) {
    if (p && n) {
        sodium_memzero(p, n);
    }
}

/* --- Character passwords --------------------------------------------- */

bool auth_password_acceptable(const char *pwd) {
    if (!pwd) {
        return false;
    }
    size_t n = strlen(pwd);
    if (n < 1 || n > AUTH_SECRET_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)pwd[i];
        if (ch < 0x20 || ch == 0x7F) {
            return false;
        }
    }
    return true;
}

bool auth_hash_password(const char *pwd, char out[AUTH_HASH_LEN]) {
    return crypto_pwhash_str(out, pwd, strlen(pwd), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                             crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0;
}

bool auth_verify_password(const char *stored_hash, const char *pwd) {
    /* A stored value that is not NUL-terminated inside its field is
     * corrupt: never hand it to the parser. */
    if (!stored_hash || !memchr(stored_hash, '\0', AUTH_HASH_LEN)) {
        return false;
    }
    return crypto_pwhash_str_verify(stored_hash, pwd, strlen(pwd)) == 0;
}

/* --- Server secrets -------------------------------------------------- */

const char *auth_secret_problem(const char *pwd) {
    size_t n = strlen(pwd);
    if (n < AUTH_SECRET_MIN) {
        return "too short (at least 8 characters)";
    }
    if (n > AUTH_SECRET_MAX) {
        return "too long (at most 63 characters)";
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)pwd[i];
        if (ch <= 0x20 || ch >= 0x7F) {
            return "only printable ASCII without spaces is allowed";
        }
    }
    return NULL;
}

static void gate_digest(const char *pwd, unsigned char out[crypto_generichash_BYTES]) {
    crypto_generichash(out, crypto_generichash_BYTES, (const unsigned char *)pwd, strlen(pwd),
                       g_gate_key, sizeof(g_gate_key));
}

void auth_set_gate_secret(const char *pwd) {
    randombytes_buf(g_gate_key, sizeof(g_gate_key));
    gate_digest(pwd, g_gate_digest);
    g_gate_set = true;
}

bool auth_check_gate(const char *pwd) {
    if (!g_gate_set || !pwd) {
        return false;
    }
    unsigned char d[crypto_generichash_BYTES];
    gate_digest(pwd, d);
    bool ok = sodium_memcmp(d, g_gate_digest, sizeof(d)) == 0;
    sodium_memzero(d, sizeof(d));
    return ok;
}

bool auth_set_dm_secret(const char *pwd) {
    g_dm_set = auth_hash_password(pwd, g_dm_hash);
    return g_dm_set;
}

bool auth_dm_enabled(void) {
    return g_dm_set;
}

bool auth_check_dm(const char *pwd) {
    return g_dm_set && pwd && auth_verify_password(g_dm_hash, pwd);
}

/* --- Loading secrets -------------------------------------------------- */

static void strip_eol(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

int auth_load_secret(const char *file, const char *env_name, char *out, size_t out_len, char *err,
                     size_t err_len) {
    err[0] = '\0';
    out[0] = '\0';
    if (file) {
        FILE *f = fopen(file, "r");
        if (!f) {
            snprintf(err, err_len, "cannot open %s: %s", file, strerror(errno));
            return -1;
        }
        struct stat st;
        bool loose = fstat(fileno(f), &st) == 0 && (st.st_mode & (S_IRWXG | S_IRWXO));
        char line[256];
        bool got = fgets(line, sizeof(line), f) != NULL;
        fclose(f);
        if (!got) {
            snprintf(err, err_len, "%s is empty", file);
            return -1;
        }
        strip_eol(line);
        if (strlen(line) >= out_len) {
            sodium_memzero(line, sizeof(line));
            snprintf(err, err_len, "the secret in %s is too long", file);
            return -1;
        }
        memcpy(out, line, strlen(line) + 1);
        sodium_memzero(line, sizeof(line));
        if (loose) {
            snprintf(err, err_len, "%s is readable by group/others (chmod 600 it)", file);
        }
        return 1;
    }
    char *value = getenv(env_name);
    if (!value || !value[0]) {
        return 0;
    }
    size_t n = strlen(value);
    int rc = 1;
    if (n >= out_len) {
        snprintf(err, err_len, "%s is too long", env_name);
        rc = -1;
    } else {
        memcpy(out, value, n + 1);
    }
    /* Wipe the value where it lives (the initial environment block is
     * what /proc/<pid>/environ shows), then drop the variable. */
    sodium_memzero(value, n);
    unsetenv(env_name);
    return rc;
}
