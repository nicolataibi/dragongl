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
#include <string.h>

#include "rules.h"

/*Maximum number of spells that a magic book can explicitly list.*/
#define MAX_BOOK_SPELLS 64

typedef enum {
    ITEM_MISC,
    ITEM_WEAPON,
    ITEM_SHIELD,
    ITEM_ARMOR,
    ITEM_HEAD,
    ITEM_NECK,
    ITEM_BACK,
    ITEM_HANDS,
    ITEM_FEET,
    ITEM_RING,
    ITEM_LIGHT_SOURCE,
    ITEM_CONSUMABLE,
    ITEM_FUEL,
    ITEM_AMMO,
    ITEM_BOOK,
    ITEM_BRACELET
} ItemCategory;

typedef enum {
    MAT_NONE,
    MAT_CLOTH,
    MAT_LEATHER,
    MAT_WOOD,
    MAT_BONE,
    MAT_STONE,
    MAT_IRON,
    MAT_STEEL,
    MAT_MITHRIL,
    MAT_GLASS,
    MAT_PAPER
} MaterialType;

/*The ammunition a ranged weapon shoots, or that an ammunition item is.*/
typedef enum { AMMO_NONE = 0, AMMO_ARROW, AMMO_BOLT, AMMO_BULLET, AMMO_NEEDLE } AmmoKind;

/*Dice written "2d4+2" (or a plain number: count 0, the bonus alone).*/
typedef struct {
    int count, sides, bonus;
} ItemDice;

/*What an item does while it is worn or held (a weapon or a staff in a
 * hand), or merely carried when `carried` is set (Ioun stones, the
 * Luckstone, belts: things with no body slot). The "worn" object of
 * data/items.json; LORE.md, "Magic items", tells the rules.
 * Damage types and conditions are bit masks (1 << DamageType,
 * 1 << ConditionType).*/
typedef struct {
    bool any;           /*the item has worn effects at all*/
    bool carried;       /*works from the backpack or the belt too*/
    int ability_set[6]; /*STR..CHA: the score is at least this (0: none)*/
    int ability_add[6]; /*STR..CHA: added to the score...*/
    int ability_max;    /*...up to this (0: no limit)*/
    int ac;             /*armour class*/
    int ac_unarmored;   /*armour class with no armour and no shield*/
    int unarmored_base; /*base armour class with no armour (+ DEX modifier)*/
    int saves;          /*every saving throw*/
    int attack, damage; /*a weapon: its own attack and damage rolls*/
    int ranged_damage;  /*damage of the shots of any ranged weapon*/
    int spell_dc, spell_attack;
    int darkvision; /*sight in the dark, in tiles*/
    uint32_t resist, immune, immune_conditions;
    bool resist_random;      /*one damage type, rolled for each item...*/
    uint32_t resist_choices; /*...among these (0: the d10 table of resistance)*/
    ItemDice regen;          /*hit points regained...*/
    int regen_rounds;        /*...every this many rounds*/
    bool haste, displacement, see_invisible, spell_save_advantage, gas_save_advantage;
    bool feather_fall, evasion, sustenance, crit_immune;
    int immune_spell;        /*a spell that has no effect (-1: none)*/
    ItemDice on_hit;         /*a weapon: extra dice on every hit...*/
    DamageType on_hit_type;  /*...of this type*/
    bool on_hit_charge;      /*...spending a charge (none left: no extra)*/
    ItemDice on_crit;        /*a weapon: extra dice on a critical hit...*/
    DamageType on_crit_type; /*...of this type (MAX_DAMAGE_TYPES: the weapon's)*/
    bool on_crit_temp_hp;    /*...that the wielder gains as temporary HP*/
} ItemWorn;

/*What a consumable does when it is used (drunk, applied, thrown): the
 * "use" object of data/items.json. The timed parts last `rounds` rounds
 * (times the roll of `rounds_dice` when it has one).*/
typedef struct {
    bool any;
    ItemDice heal;
    int temp_hp;
    int rounds;
    ItemDice rounds_dice;
    int attack, damage, ac, saves, darkvision;
    bool haste;
    uint32_t resist;
    bool resist_random;
    uint32_t immune_conditions;
    ConditionType condition; /*a condition (MAX_CONDITIONS: none)...*/
    Ability save_ability;    /*...unless this saving throw (ABILITY_NONE: none)...*/
    int save_dc;             /*...against this DC succeeds*/
    ItemDice harm;           /*damage to the user...*/
    DamageType harm_type;    /*...of this type*/
    uint32_t cure;           /*conditions it ends*/
    bool exhaustion;         /*it ends exhaustion*/
    bool weapon;             /*an oil for the weapon in hand: needs one*/
} ItemUse;

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
    /*The spell of an item: cast at will (no charge, nothing used up)
     * from a worn or held item; each cast spends charge_cost charges;
     * no_recharge: the charges never come back (a necklace's beads).*/
    bool at_will;
    bool no_recharge;
    int charge_cost;
    bool is_scroll; /*read aloud: silence prevents it*/
    bool is_potion; /*drunk or applied: a potion or an oil*/
    ItemWorn worn;
    ItemUse use;
    /*Weapons: a ranged weapon's ammunition and normal range in tiles (5 ft
     * each, "ammunition (range 80/320)" -> 16); a finesse weapon hits with
     * the better of STR and DEX. Ammunition items: their kind.*/
    AmmoKind ammo;
    int range;
    bool finesse;
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
    uint32_t book_class_mask;
    int book_min_level;
    int book_max_level;
    int book_seq;
    int book_spell_count;
    char book_spell_names[MAX_BOOK_SPELLS][64];
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
    bool is_artifact;       //true = legendary unique artifact
    char artifact_name[64]; //proper name of the artifact (e.g. "Void Blade")
    int artifact_str_bonus; //Fixed bonus STR artifact
    int artifact_dex_bonus; //Artifact fixed DEX bonus
    int artifact_con_bonus; //fixed WITH bonus of the artifact
} ItemInstance;

/* Field by field: memcmp() would also compare the padding bytes, which
 * struct copies need not preserve. */
static inline bool item_instance_equal(const ItemInstance *a, const ItemInstance *b) {
    return a->template_idx == b->template_idx && a->durability == b->durability &&
           a->stack_count == b->stack_count && a->is_identified == b->is_identified &&
           a->current_charges == b->current_charges && a->to_hit_bonus == b->to_hit_bonus &&
           a->to_dam_bonus == b->to_dam_bonus && a->ac_bonus == b->ac_bonus &&
           a->quality == b->quality && a->blessing == b->blessing && a->element == b->element &&
           a->is_artifact == b->is_artifact &&
           strncmp(a->artifact_name, b->artifact_name, sizeof(a->artifact_name)) == 0 &&
           a->artifact_str_bonus == b->artifact_str_bonus &&
           a->artifact_dex_bonus == b->artifact_dex_bonus &&
           a->artifact_con_bonus == b->artifact_con_bonus;
}
/*"2d4+2", "1d6" or "15" into dice; false (out untouched) for anything else.*/
bool item_parse_dice(const char *s, ItemDice *out);

extern ItemTemplate *item_database;
extern int item_database_size;

#endif // ITEMS_H
