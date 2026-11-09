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

#ifndef DATA_LOADER_H
#define DATA_LOADER_H

#include <stdbool.h>
#include <stdint.h>

/* Load bestiary.json, spells.json and items.json from data_dir. False if
 * any of them cannot be read or parsed (the reason is on stderr): the
 * server refuses to start without its data. */
bool init_data_loaders(const char *data_dir);

//Frees memory allocated by loaders
void cleanup_data_loaders(void);

/* CRC-32 of the monster names of the loaded bestiary, in order (computed
 * by init_data_loaders). Saved NPCs
 * refer to their template by index: npcs.dat stores this value and is
 * refused by a bestiary where a monster was added, removed or moved. */
uint32_t bestiary_fingerprint(void);

#endif // DATA_LOADER_H
