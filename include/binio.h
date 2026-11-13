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

/*
 * Portable binary I/O.
 *
 * Every value is written field by field in little-endian order with an
 * explicit width, so a file does not depend on the compiler's struct
 * layout, padding or the host byte order, and never contains a pointer.
 *
 * Container written by binfile_write_atomic():
 *
 *   offset  size  field
 *        0     4  magic            (identifies the kind of file)
 *        4     2  format version
 *        6     2  reserved, 0
 *        8     4  payload length in bytes
 *       12     4  CRC-32 (IEEE) of the payload
 *       16     n  payload
 *
 * Readers reject a wrong magic, version or reserved field, a length that
 * does not match the file, a CRC mismatch, and a payload that is not
 * consumed exactly.
 */

#ifndef DRAGONGL_BINIO_H
#define DRAGONGL_BINIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BINFILE_HEADER_SIZE 16
/* Upper bound for a payload: larger files are rejected before allocation. */
#define BINFILE_MAX_PAYLOAD ((size_t)512 * 1024 * 1024)

uint32_t crc32_data(const void *data, size_t len);

/* --- Writer --------------------------------------------------------------- */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool failed; /* out of memory: every later write is ignored */
} BinWriter;

void bw_init(BinWriter *w);
void bw_free(BinWriter *w);
/* Empty the buffer but keep its memory (a writer reused for every send). */
void bw_reset(BinWriter *w);
void bw_bytes(BinWriter *w, const void *p, size_t n);
void bw_u8(BinWriter *w, uint8_t v);
void bw_u16(BinWriter *w, uint16_t v);
void bw_u32(BinWriter *w, uint32_t v);
void bw_u64(BinWriter *w, uint64_t v);
void bw_i32(BinWriter *w, int32_t v);
void bw_i64(BinWriter *w, int64_t v);
void bw_bool(BinWriter *w, bool v);
/* A fixed-size text field of `field` bytes: the string (truncated so that a
 * NUL always fits) followed by zero padding. */
void bw_str(BinWriter *w, const char *s, size_t field);

/* --- Reader --------------------------------------------------------------- */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    bool failed; /* read past the end, or an invalid value */
} BinReader;

void br_init(BinReader *r, const void *data, size_t len);
void br_bytes(BinReader *r, void *out, size_t n);
/* The next n bytes in place (NULL, reader failed, if fewer remain). */
const uint8_t *br_view(BinReader *r, size_t n);
uint8_t br_u8(BinReader *r);
uint16_t br_u16(BinReader *r);
uint32_t br_u32(BinReader *r);
uint64_t br_u64(BinReader *r);
int32_t br_i32(BinReader *r);
int64_t br_i64(BinReader *r);
bool br_bool(BinReader *r);                         /* anything but 0/1 marks the reader failed */
void br_str(BinReader *r, char *out, size_t field); /* always NUL-terminated */
/* A count that must lie in [0, max]; otherwise the reader fails. */
uint32_t br_count(BinReader *r, uint32_t max);
/* True if no error occurred and every byte was consumed. */
bool br_done(const BinReader *r);

/* --- Files ---------------------------------------------------------------- */
/* Write header + payload to <path>.tmp, fsync, rename over <path>, fsync
 * the directory. On any error the temporary file is removed, the previous
 * file is kept, and false is returned with errno set. */
bool binfile_write_atomic(const char *path, uint32_t magic, uint16_t version,
                          const BinWriter *payload);

#ifdef DRAGONGL_FAULT_INJECTION
/* Test builds only (tests/integration): the process dies, as if killed,
 * when binfile_write_atomic() reaches this point of its sequence. */
typedef enum {
    BINFILE_CRASH_NONE = 0,
    BINFILE_CRASH_TMP_CREATED,  /* <path>.tmp exists and is empty */
    BINFILE_CRASH_HEADER,       /* header written, no payload */
    BINFILE_CRASH_HALF_PAYLOAD, /* header and half of the payload */
    BINFILE_CRASH_WRITTEN,      /* everything written, before fsync */
    BINFILE_CRASH_SYNCED,       /* fsync done, before the rename */
    BINFILE_CRASH_RENAMED,      /* renamed, before the directory fsync */
    BINFILE_CRASH_POINTS
} BinFileCrashPoint;
extern BinFileCrashPoint binfile_crash_point;
#endif

typedef enum {
    BINFILE_OK = 1,
    BINFILE_MISSING = 0, /* the file does not exist */
    BINFILE_INVALID = -1 /* unreadable, wrong kind/version, corrupt */
} BinFileStatus;

/* Read and verify a container. On BINFILE_OK *payload is a malloc'ed
 * buffer (free it) of *len bytes. `why` (may be NULL) receives a short
 * reason when the result is BINFILE_INVALID. */
BinFileStatus binfile_read(const char *path, uint32_t magic, uint16_t version, uint8_t **payload,
                           size_t *len, char *why, size_t why_len);

#endif
