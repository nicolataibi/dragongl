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

#ifndef ITEMS_H
#define ITEMS_H
#include <stdint.h>
#include <stdbool.h>

typedef enum { ITEM_MISC, ITEM_WEAPON, ITEM_SHIELD, ITEM_ARMOR, ITEM_HEAD, ITEM_NECK, ITEM_BACK, ITEM_HANDS, ITEM_FEET, ITEM_RING, ITEM_LIGHT_SOURCE, ITEM_CONSUMABLE, ITEM_FUEL, ITEM_AMMO, ITEM_BOOK, ITEM_BRACELET } ItemCategory;

typedef enum { MAT_NONE, MAT_CLOTH, MAT_LEATHER, MAT_WOOD, MAT_BONE, MAT_STONE, MAT_IRON, MAT_STEEL, MAT_MITHRIL, MAT_GLASS, MAT_PAPER } MaterialType;

typedef struct {
    const char *name;
    const char *description;
    ItemCategory category;
    int str_bonus, dex_bonus, con_bonus, int_bonus, wis_bonus, cha_bonus;
    int damage_dice_count;
    int damage_dice_sides;
    int attack_bonus;
    int ac_bonus;
    int ac_base;
    int max_durability;
    int max_stack;
    bool needs_id;
    int heal_amount;
    int light_radius;
    int duration_turns;
    uint64_t cost;
    float weight;
    const char *plural_name;
    bool is_cursed;
    int cast_spell_idx;
    int max_charges;
    MaterialType material;
    /*--- Magic Books ---
     * book_class_mask : Bitmask classes that can read the book (ClassType).
     * book_min_level : minimum spell level (0-9, for legacy use).
     * book_max_level : maximum spell level (0-9, for legacy use).
     * book_seq : book sequence in the series (0=first, 5=sixth).
     * book_spell_count : number of explicit spells in the book (max 10).
     * book_spell_names : names of spells ordered from lowest to highest level.
     *
     * PG level gate: global_idx = book_seq * 10 + local_location
     * required_level = global_idx / 3 (60 spell / 20 lv = 3/lv)*/
#define MAX_BOOK_SPELLS 64
    uint32_t book_class_mask;
    int      book_min_level;
    int      book_max_level;
    int      book_seq;
    int      book_spell_count;
    char     book_spell_names[MAX_BOOK_SPELLS][64];
} ItemTemplate;

typedef enum { QUALITY_RUSTY = 0, QUALITY_NORMAL, QUALITY_FINE, QUALITY_MASTERWORK } ItemQuality;
typedef enum { BLESS_CURSED = 0, BLESS_NORMAL, BLESS_BLESSED } ItemBlessing;
typedef enum { ELEM_NONE = 0, ELEM_FLAMING, ELEM_FROST, ELEM_SHOCKING, ELEM_POISONOUS } ItemElement;

typedef struct {
    int template_idx;
    int durability;
    int stack_count;
    bool is_identified;
    int current_charges;
    int to_hit_bonus;
    int to_dam_bonus;
    int ac_bonus;
    ItemQuality quality;
    ItemBlessing blessing;
    ItemElement element;
    bool is_artifact;           //true = legendary unique artifact
    char artifact_name[64];     //proper name of the artifact (e.g. "Void Blade")
    int artifact_str_bonus;     //Fixed bonus STR artifact
    int artifact_dex_bonus;     //Artifact fixed DEX bonus
    int artifact_con_bonus;     //fixed WITH bonus of the artifact
} ItemInstance;
extern ItemTemplate* item_database;
extern int item_database_size;

#endif // ITEMS_H
