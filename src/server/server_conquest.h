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
 * server_conquest.h - The end of the game (LORE.md, The Conquest of the
 * Dungeon): the boss of the last floor defeated, its stairs down lead the
 * character back to the town as a legend. The dungeon is closed to him and
 * nothing he does after is saved.
 */

#ifndef SERVER_CONQUEST_H
#define SERVER_CONQUEST_H

#include "server_entities.h"

#include <stddef.h>

/*True once the character has conquered the dungeon.*/
static inline bool client_conquered(const Client *c) {
    return c->conquered_at != 0;
}

/*The end: the victory told to the character and announced to every
 * player, the epilogue (lines "[EPILOGUE] ..." that the client shows on a
 * screen of their own), the character back in the town, saved one last
 * time with the date of the conquest.*/
void conquest_complete(Game *game, Client *c);

/*What a conquered character is told when he tries what is closed to him.*/
void conquest_refuse(const Client *c);

/*The date of a conquest, "YYYY-MM-DD" (UTC).*/
void conquest_date(int64_t at, char *out, size_t cap);

#endif /* SERVER_CONQUEST_H */
