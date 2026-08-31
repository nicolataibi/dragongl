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

#ifndef CLASSES_H
#define CLASSES_H

#include <stdint.h>

typedef enum {
    CLASS_BARBARIAN,
    CLASS_BARD,
    CLASS_CLERIC,
    CLASS_DRUID,
    CLASS_FIGHTER,
    CLASS_MONK,
    CLASS_PALADIN,
    CLASS_RANGER,
    CLASS_ROGUE,
    CLASS_SORCERER,
    CLASS_WARLOCK,
    CLASS_WIZARD,
    CLASS_COUNT
} ClassType;

typedef struct {
    const char* name;
    int hit_die;
    const char* primary_ability;
    int str_save, dex_save, con_save, int_save, wis_save, cha_save;
    const char* description;
} ClassTemplate;

static const ClassTemplate CLASSES[CLASS_COUNT] = {
    [CLASS_BARBARIAN] = { "Barbarian", 12, "Strength", 1, 0, 1, 0, 0, 0, "A fierce warrior of primitive background who can enter a battle rage." },
    [CLASS_BARD]      = { "Bard", 8, "Charisma", 0, 1, 0, 0, 0, 1, "An inspiring magician whose power echoes the music of creation." },
    [CLASS_CLERIC]    = { "Cleric", 8, "Wisdom", 0, 0, 0, 0, 1, 1, "A priestly champion who wields divine magic in service of a higher power." },
    [CLASS_DRUID]     = { "Druid", 8, "Wisdom", 0, 0, 0, 1, 1, 0, "A priest of the Old Faith, wielding the powers of nature and adopting animal forms." },
    [CLASS_FIGHTER]   = { "Fighter", 10, "Str/Dex", 1, 0, 1, 0, 0, 0, "A master of martial combat, skilled with a variety of weapons and armor." },
    [CLASS_MONK]      = { "Monk", 8, "Dex/Wis", 1, 1, 0, 0, 0, 0, "A master of martial arts, harnessing the power of the body in pursuit of physical and spiritual perfection." },
    [CLASS_PALADIN]   = { "Paladin", 10, "Str/Cha", 0, 0, 0, 0, 1, 1, "A holy warrior bound by a sacred oath." },
    [CLASS_RANGER]    = { "Ranger", 10, "Dex/Wis", 1, 1, 0, 0, 0, 0, "A warrior who uses martial prowess and nature magic to combat threats on the edges of civilization." },
    [CLASS_ROGUE]     = { "Rogue", 8, "Dexterity", 0, 1, 0, 1, 0, 0, "A scoundrel who uses stealth and trickery to overcome obstacles and enemies." },
    [CLASS_SORCERER]  = { "Sorcerer", 6, "Charisma", 0, 0, 1, 0, 0, 1, "A spellcaster who draws on inborn magic from a gift or bloodline." },
    [CLASS_WARLOCK]   = { "Warlock", 8, "Charisma", 0, 0, 0, 0, 1, 1, "A wielder of magic that is derived from a bargain with an extraplanar entity." },
    [CLASS_WIZARD]    = { "Wizard", 6, "Intelligence", 0, 0, 0, 1, 1, 0, "A scholarly magic-user capable of manipulating the structures of reality." }
};

#endif // CLASSES_H
