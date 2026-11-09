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
 * Server persistence: the state directory and the on-disk formats.
 *
 * The state directory holds everything the server writes; the data
 * directory (JSON datasets) is only ever read:
 *
 *   <state>/world.dat        floors: tiles, traps, crystal respawns
 *   <state>/npcs.dat         NPC records, next entity id, game turns
 *   <state>/artifacts.dat    which unique artifacts already dropped
 *   <state>/saves/<user>.save
 *   <state>/saves/tombstone_<user>_<time>_<x>_<y>.dat
 *   <state>/maps/            DM map dumps and PDFs
 *   <state>/combat_log.json  tactical log (and its .1 rotation)
 *
 *   <state>/server.lock      held (flock) by the running server
 *
 * Every .dat/.save file is a binio container (see binio.h): fields are
 * written one by one in little-endian order, never as raw structs, and no
 * pointer reaches the disk. Readers validate every count and enum; the
 * callers still range-check the values they use as indices.
 */

#ifndef DRAGONGL_PERSIST_H
#define DRAGONGL_PERSIST_H

#include "binio.h"
#include "map.h"
#include "server.h"
#include "server_entities.h"

#include <stdbool.h>
#include <stddef.h>

#define STATE_DIR_MAX 512

/* Format generations. Bump the version when a layout changes: an old file
 * is then refused instead of being misread. */
#define PERSIST_SAVE_MAGIC    0x53474C44u /* "DLGS" */
#define PERSIST_SAVE_VERSION  3
#define PERSIST_WORLD_MAGIC   0x57475244u /* "DRGW" */
#define PERSIST_WORLD_VERSION 4
#define PERSIST_NPCS_MAGIC    0x43504E44u /* "DNPC" */
#define PERSIST_NPCS_VERSION  2
#define PERSIST_TOMB_MAGIC    0x424D5444u /* "DTMB" */
#define PERSIST_TOMB_VERSION  1
#define PERSIST_ART_MAGIC     0x54524144u /* "DART" */
#define PERSIST_ART_VERSION   1

/* --- State directory ------------------------------------------------------- */

/* Pick the state directory: explicit argument, then $DRAGONGL_STATE_DIR,
 * then $XDG_STATE_HOME/dragongl, then $HOME/.local/state/dragongl. Create
 * it (mode 0700, parents included) with its saves/ and maps/ sub-
 * directories, and lock it for this process (a second server on the same
 * directory is refused). Returns false with a message in err on failure. */
bool persist_init_state_dir(const char *explicit_dir, char *err, size_t err_len);
const char *persist_state_dir(void);

/* <state>/<relative>; false (and an empty string) if it does not fit. */
bool state_path(char *out, size_t out_len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Remove *.tmp leftovers of an interrupted write in the state tree. */
void persist_cleanup_tmp(void);

/* --- Formats --------------------------------------------------------------- */
/* Writers encode the snapshot in memory, in the calling thread, and hand
 * the buffer to the writer (writer.h): once writer_start() has run, the
 * write, fsync and rename happen in the writer thread and the call returns
 * true when the job is queued; before that (startup) or after writer_stop()
 * (shutdown) they run synchronously and return false (errno set) if the
 * file could not be written. A failed write keeps the previous file and is
 * logged by the writer. Readers return a BinFileStatus; `why` receives the
 * reason of a BINFILE_INVALID result. */

bool persist_write_player(const char *path, const SaveData *sd);
BinFileStatus persist_read_player(const char *path, SaveData *sd, char *why, size_t why_len);

bool persist_write_world(const char *path, const World *w);
BinFileStatus persist_read_world(const char *path, World *w, char *why, size_t why_len);

/* NPC records: the slots that are active or keep a template (respawning
 * monsters). Each record keeps its slot index. Transient state (active
 * effects, the template pointer) is not stored: the loader re-binds
 * templates from template_idx. */
bool persist_write_npcs(const char *path, const NPC *npcs, int max_npcs, int next_id,
                        int total_turns);
BinFileStatus persist_read_npcs(const char *path, NPC *npcs, int max_npcs, int *next_id,
                                int *total_turns, int *loaded, char *why, size_t why_len);

bool persist_write_tombstone(const char *path, const Tombstone *t);
BinFileStatus persist_read_tombstone(const char *path, Tombstone *t, char *why, size_t why_len);

bool persist_write_flags(const char *path, const bool *flags, int count);
BinFileStatus persist_read_flags(const char *path, bool *flags, int count, char *why,
                                 size_t why_len);

#endif
