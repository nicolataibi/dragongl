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
 * Background writer: the disk side of every save.
 *
 * The game loop takes the snapshot (persist_encode_*() into a BinWriter,
 * in memory) and hands the buffer to writer_submit(); one thread performs
 * binfile_write_atomic() (write, fsync, rename, fsync of the directory),
 * so a slow or busy disk never stalls a game tick.
 *
 *  - Jobs for different paths are independent; jobs for the same path are
 *    performed in submission order.
 *  - A job still waiting in the queue is REPLACED by a newer one for the
 *    same path (the newer snapshot supersedes it): the queue never holds
 *    more than one pending job per file, whatever the disk speed.
 *  - writer_wait_for(path) returns once nothing is queued or in progress
 *    for that path: a reader that needs the latest contents (a login right
 *    after a logout) calls it before opening the file.
 *  - A failed write keeps the previous file (binfile_write_atomic) and is
 *    logged by the thread; the next save of that file retries.
 *
 * Until writer_start() (and after writer_stop()) every call runs
 * synchronously in the caller: startup, shutdown, tools and tests need no
 * thread.
 */

#ifndef DRAGONGL_WRITER_H
#define DRAGONGL_WRITER_H

#include "binio.h"

#include <stdbool.h>
#include <stdint.h>

bool writer_start(void);
/* Perform every queued job, then stop the thread. Returns false if any
 * job failed while draining. */
bool writer_stop(void);
bool writer_running(void);

/* Store the payload at path (atomically) with the given container magic
 * and version. Takes ownership of w's buffer (w is left empty). Returns
 * the result of the write when it runs synchronously; true once queued. */
bool writer_submit(const char *path, uint32_t magic, uint16_t version, BinWriter *w);

/* Remove path (a missing file is not an error); ordered like a write. */
bool writer_submit_unlink(const char *path);

/* Block until no job for path is queued or in progress. */
void writer_wait_for(const char *path);

/* Failed jobs since start (for tests and the shutdown report). */
unsigned writer_failures(void);

#endif
