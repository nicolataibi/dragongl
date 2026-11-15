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
 * server_traps.h - Traps.
 */

#ifndef SERVER_TRAPS_H
#define SERVER_TRAPS_H

#include "server_entities.h"

/*Detects the traps near the player and fires the one under him.*/
void check_traps(Client *c, Game *game);

/*--- Secret passages ---
 * A straight stretch of room wall with open ground on both sides may hide
 * a passage (one such tile in SECRET_PASSAGE_RARITY, chosen by its
 * position, so there is nothing to save). 'search' or a divination finds
 * it: the wall becomes a door, kept on the saved map. The town has none.*/
#define SECRET_PASSAGE_RARITY 40
bool secret_passage_at(const Floor *f, int floor_id, int x, int y);
/*The perception a search needs to notice one.*/
int secret_passage_dc(int floor_id);
/*Reveals the passages within radius cells of the player whose DC the
 * perception total reaches (perception < 0: every one, as magic does) and
 * tells him; returns how many.*/
int secret_passages_reveal(Game *game, Client *c, int radius, int perception);

#endif /* SERVER_TRAPS_H */
