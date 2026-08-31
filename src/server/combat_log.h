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

#ifndef COMBAT_LOG_H
#define COMBAT_LOG_H

#include <stdbool.h>

//Opens/closes the tactical log file
void clog_init(const char* filepath);
void clog_close(void);

//Log an attack event
void clog_attack(const char* attacker, const char* target,
                 int roll, int bonus, int ac,
                 bool hit, bool crit, int damage);

//Record a saving throw
void clog_save(const char* entity, const char* effect,
               int roll, int modifier, int dc, bool passed);

//Record at spell casting
void clog_spell(const char* caster, const char* spell_name,
                const char* target, int damage_or_heal, bool saved);

// Logs the death of an entity
void clog_death(const char* entity, const char* killer, int floor_id);

#endif // COMBAT_LOG_H
