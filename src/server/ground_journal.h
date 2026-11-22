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
 * ground_journal.h - What was taken from the ground since npcs.dat.
 *
 * A character is saved as soon as he picks up gold or an item from the
 * ground, but the ground (npcs.dat) only at the autosave, once a minute: a
 * crash in between gave the item twice, in the save and on the ground of
 * the last npcs.dat. Each pickup first writes the entity id of what was
 * taken to ground.journal (before the save of the character: the writer
 * performs them in that order); when npcs.dat is written the journal is
 * emptied; at startup what the journal names is taken away from the
 * ground loaded. A crash may then lose what was being picked up, never
 * give it twice.
 */

#ifndef GROUND_JOURNAL_H
#define GROUND_JOURNAL_H

#include "server_entities.h"

#define GROUND_JOURNAL_CAP 4096

/*n (gold or an item lying on the ground) is being taken.*/
void ground_journal_taken(Game *game, const NPC *n);
/*npcs.dat has just been handed to the writer: the journal starts again.*/
void ground_journal_clear(void);
/*At startup, after npcs.dat: what the journal names leaves the ground.
 * Returns how many were taken away.*/
int ground_journal_apply(Game *game);

#endif /* GROUND_JOURNAL_H */
