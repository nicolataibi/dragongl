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
 * ground_journal.c - What was taken from the ground since npcs.dat (see
 * ground_journal.h).
 */

#include "ground_journal.h"
#include "persist.h"
#include "server_internal.h"
#include "server_log.h"
#include "server_world.h"

#include <stdint.h>

static int32_t g_ids[GROUND_JOURNAL_CAP];
static int g_count;

static bool journal_path(char *out, size_t cap) {
    return state_path(out, cap, "ground.journal");
}

static void journal_write(void) {
    char path[STATE_DIR_MAX + 32];
    if (journal_path(path, sizeof(path)) && !persist_write_ids(path, g_ids, g_count)) {
        server_log("SAVE", "WARNING: ground.journal could not be written.");
    }
}

void ground_journal_taken(Game *game, const NPC *n) {
    if (n->entity_id <= 0) {
        return;
    }
    if (g_count >= GROUND_JOURNAL_CAP) {
        /*Full: the ground itself is saved, and the journal starts again.*/
        if (save_world_state(game)) {
            ground_journal_clear();
        }
        if (g_count >= GROUND_JOURNAL_CAP) {
            return;
        }
    }
    g_ids[g_count++] = n->entity_id;
    journal_write();
}

void ground_journal_clear(void) {
    g_count = 0;
    journal_write();
}

int ground_journal_apply(Game *game) {
    char path[STATE_DIR_MAX + 32], why[96];
    int n = 0;
    if (!journal_path(path, sizeof(path))) {
        return 0;
    }
    BinFileStatus st = persist_read_ids(path, g_ids, GROUND_JOURNAL_CAP, &n, why, sizeof(why));
    if (st == BINFILE_INVALID) {
        server_log("SAVE", "WARNING: ground.journal unreadable (%s).", why);
    }
    int taken = 0;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < MAX_NPCS; k++) {
            NPC *npc = &game->npcs[k];
            if (npc->active && npc->entity_id == g_ids[i] &&
                (npc->archetype == ARCH_GOLD || npc->archetype == ARCH_TREASURE)) {
                npc->active = false;
                npc->respawn_timer = -1;
                taken++;
                break;
            }
        }
    }
    /*Kept until npcs.dat is written again: another crash before it finds
     * the same ground in npcs.dat, and must take the same away.*/
    g_count = n;
    return taken;
}
