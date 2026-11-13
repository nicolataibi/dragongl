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

/*data_internal.h - What the data loaders share between their files.*/

#ifndef DATA_INTERNAL_H
#define DATA_INTERNAL_H

#include <stdbool.h>

#include "cJSON.h"

/*Case-insensitive strstr.*/
const char *data_strcasestr(const char *haystack, const char *needle);
/*The parsed document of a JSON file, or NULL after the message; the caller
 * deletes it.*/
cJSON *parse_json_file(const char *filepath);
/*Index of the spell in spell_database, or -1.*/
int get_spell_idx_by_name(const char *name);
/*Loads data/items.json into item_database (the spells must be loaded).*/
bool load_items_json(const char *filepath);

#endif /* DATA_INTERNAL_H */
