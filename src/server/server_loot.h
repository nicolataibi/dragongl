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
 * server_loot.h - Loot and the unique artifacts.
 */

#ifndef SERVER_LOOT_H
#define SERVER_LOOT_H

#include "server_entities.h"

/*Which artifacts already dropped (each drops once in the life of a world).*/
void artifacts_load(void);
void artifacts_save(void);
/*The loot of a dead monster, by its archetype and floor, next to the killer.*/
void drop_loot_from_monster(Client *c, NPC *killer);

#endif /* SERVER_LOOT_H */
