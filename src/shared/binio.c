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

#include "binio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- CRC-32 (IEEE 802.3, polynomial 0xEDB88320) ----------------------------- */

uint32_t crc32_data(const void *data, size_t len) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* --- Writer ------------------------------------------------------------------ */

void bw_init(BinWriter *w) {
    memset(w, 0, sizeof(*w));
}

void bw_free(BinWriter *w) {
    free(w->data);
    memset(w, 0, sizeof(*w));
}

void bw_reset(BinWriter *w) {
    w->len = 0;
    w->failed = false;
}

void bw_bytes(BinWriter *w, const void *p, size_t n) {
    if (w->failed || n == 0)
        return;
    if (w->len + n > w->cap) {
        size_t cap = w->cap ? w->cap : 4096;
        while (cap < w->len + n)
            cap *= 2;
        uint8_t *grown = realloc(w->data, cap);
        if (!grown) {
            w->failed = true;
            return;
        }
        w->data = grown;
        w->cap = cap;
    }
    memcpy(w->data + w->len, p, n);
    w->len += n;
}

void bw_u8(BinWriter *w, uint8_t v) {
    bw_bytes(w, &v, 1);
}

void bw_u16(BinWriter *w, uint16_t v) {
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    bw_bytes(w, b, sizeof(b));
}

void bw_u32(BinWriter *w, uint32_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++)
        b[i] = (uint8_t)(v >> (8 * i));
    bw_bytes(w, b, sizeof(b));
}

void bw_u64(BinWriter *w, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(v >> (8 * i));
    bw_bytes(w, b, sizeof(b));
}

void bw_i32(BinWriter *w, int32_t v) {
    bw_u32(w, (uint32_t)v);
}

void bw_i64(BinWriter *w, int64_t v) {
    bw_u64(w, (uint64_t)v);
}

void bw_bool(BinWriter *w, bool v) {
    bw_u8(w, v ? 1 : 0);
}

void bw_str(BinWriter *w, const char *s, size_t field) {
    if (field == 0)
        return;
    size_t n = s ? strnlen(s, field - 1) : 0;
    bw_bytes(w, s, n);
    static const uint8_t zeros[64] = {0};
    for (size_t left = field - n; left > 0;) {
        size_t chunk = left < sizeof(zeros) ? left : sizeof(zeros);
        bw_bytes(w, zeros, chunk);
        left -= chunk;
    }
}

/* --- Reader ------------------------------------------------------------------ */

void br_init(BinReader *r, const void *data, size_t len) {
    r->data = (const uint8_t *)data;
    r->len = len;
    r->pos = 0;
    r->failed = false;
}

void br_bytes(BinReader *r, void *out, size_t n) {
    if (r->failed || n > r->len - r->pos) {
        r->failed = true;
        memset(out, 0, n);
        return;
    }
    memcpy(out, r->data + r->pos, n);
    r->pos += n;
}

const uint8_t *br_view(BinReader *r, size_t n) {
    if (r->failed || n > r->len - r->pos) {
        r->failed = true;
        return NULL;
    }
    const uint8_t *p = r->data + r->pos;
    r->pos += n;
    return p;
}

uint8_t br_u8(BinReader *r) {
    uint8_t v;
    br_bytes(r, &v, 1);
    return v;
}

uint16_t br_u16(BinReader *r) {
    uint8_t b[2];
    br_bytes(r, b, sizeof(b));
    return (uint16_t)(b[0] | (b[1] << 8));
}

uint32_t br_u32(BinReader *r) {
    uint8_t b[4];
    br_bytes(r, b, sizeof(b));
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--)
        v = (v << 8) | b[i];
    return v;
}

uint64_t br_u64(BinReader *r) {
    uint8_t b[8];
    br_bytes(r, b, sizeof(b));
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | b[i];
    return v;
}

int32_t br_i32(BinReader *r) {
    return (int32_t)br_u32(r);
}

int64_t br_i64(BinReader *r) {
    return (int64_t)br_u64(r);
}

bool br_bool(BinReader *r) {
    uint8_t v = br_u8(r);
    if (v > 1)
        r->failed = true;
    return v == 1;
}

void br_str(BinReader *r, char *out, size_t field) {
    if (field == 0)
        return;
    br_bytes(r, out, field);
    out[field - 1] = '\0';
}

uint32_t br_count(BinReader *r, uint32_t max) {
    uint32_t n = br_u32(r);
    if (n > max) {
        r->failed = true;
        return 0;
    }
    return n;
}

bool br_done(const BinReader *r) {
    return !r->failed && r->pos == r->len;
}

/* --- Files -------------------------------------------------------------------- */

static void put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* fsync the directory that holds `path`, so the rename itself is durable. */
static void fsync_parent_dir(const char *path) {
    char dir[4096];
    size_t n = strnlen(path, sizeof(dir) - 1);
    memcpy(dir, path, n);
    dir[n] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash == dir)
        dir[1] = '\0';
    else if (slash)
        *slash = '\0';
    else
        strcpy(dir, ".");
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
}

bool binfile_write_atomic(const char *path, uint32_t magic, uint16_t version,
                          const BinWriter *payload) {
    if (payload->failed || payload->len > BINFILE_MAX_PAYLOAD) {
        errno = ENOMEM;
        return false;
    }
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return false;
    }
    uint8_t hdr[BINFILE_HEADER_SIZE];
    put_le32(hdr, magic);
    hdr[4] = (uint8_t)version;
    hdr[5] = (uint8_t)(version >> 8);
    hdr[6] = hdr[7] = 0;
    put_le32(hdr + 8, (uint32_t)payload->len);
    put_le32(hdr + 12, crc32_data(payload->data, payload->len));

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    bool ok = write_all(fd, hdr, sizeof(hdr)) &&
              write_all(fd, payload->data, payload->len) && fsync(fd) == 0;
    int saved = errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        saved = errno;
    }
    if (ok && rename(tmp, path) != 0) {
        ok = false;
        saved = errno;
    }
    if (!ok) {
        unlink(tmp);
        errno = saved;
        return false;
    }
    fsync_parent_dir(path);
    return true;
}

static BinFileStatus invalid(char *why, size_t why_len, const char *reason) {
    if (why && why_len)
        snprintf(why, why_len, "%s", reason);
    return BINFILE_INVALID;
}

BinFileStatus binfile_read(const char *path, uint32_t magic, uint16_t version,
                           uint8_t **payload, size_t *len, char *why, size_t why_len) {
    *payload = NULL;
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (errno == ENOENT)
            return BINFILE_MISSING;
        return invalid(why, why_len, strerror(errno));
    }
    uint8_t hdr[BINFILE_HEADER_SIZE];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return invalid(why, why_len, "truncated header");
    }
    if (get_le32(hdr) != magic) {
        fclose(f);
        return invalid(why, why_len, "not a file of this kind (magic)");
    }
    uint16_t file_version = (uint16_t)(hdr[4] | (hdr[5] << 8));
    if (file_version != version || hdr[6] != 0 || hdr[7] != 0) {
        fclose(f);
        char msg[96];
        snprintf(msg, sizeof(msg), "format version %u, expected %u", file_version, version);
        return invalid(why, why_len, msg);
    }
    size_t n = get_le32(hdr + 8);
    uint32_t crc = get_le32(hdr + 12);
    if (n > BINFILE_MAX_PAYLOAD) {
        fclose(f);
        return invalid(why, why_len, "payload too large");
    }
    uint8_t *buf = malloc(n ? n : 1);
    if (!buf) {
        fclose(f);
        return invalid(why, why_len, "out of memory");
    }
    bool size_ok = fread(buf, 1, n, f) == n && fgetc(f) == EOF;
    fclose(f);
    if (!size_ok) {
        free(buf);
        return invalid(why, why_len, "length does not match the file");
    }
    if (crc32_data(buf, n) != crc) {
        free(buf);
        return invalid(why, why_len, "checksum mismatch");
    }
    *payload = buf;
    *len = n;
    return BINFILE_OK;
}
